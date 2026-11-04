// SPDX-License-Identifier: GPL-3.0-or-later
// Offline arithmetic qualification. Readback here is diagnostic only.
#include "tf_hwgs.h"
#include "tf_gpu_shaders.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX11/D3D.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
void check(HRESULT result, const char *operation)
{
    if (FAILED(result))
    {
        char message[192];
        std::snprintf(message, sizeof(message), "%s failed: %08lx", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

auto createBuffer(ID3D12Device *device, uint32_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                  D3D12_RESOURCE_FLAGS flags)
{
    wil::com_ptr_nothrow<ID3D12Resource> result;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    check(
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(result.put())),
        "Create reflection buffer");
    return result;
}

void upload(ID3D12Resource *buffer, const void *source, uint32_t bytes)
{
    void *mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    check(buffer->Map(0, &empty, &mapped), "Map source geometry");
    std::memcpy(mapped, source, bytes);
    const D3D12_RANGE written{0, bytes};
    buffer->Unmap(0, &written);
}
} // namespace

static int reflectionProbe(const void *inputs, uint32_t count, void *output, uint32_t outputCapacity, char *error,
                           uint32_t errorCapacity, bool projection)
{
    const uint32_t inputStride = projection ? 368 : 224, outputStride = projection ? 320 : 48;
    bool passed = false;
    if (!inputs || !output || !count || count > 65536 || outputCapacity < uint64_t(count) * outputStride)
    {
        if (error && errorCapacity)
            std::snprintf(error, errorCapacity, "Invalid reflection probe buffers");
        return 0;
    }
    kzgs::RunOnGSThread([&] {
        try
        {
            if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12)
                throw std::runtime_error("Reflection probe requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            auto *device = gs->GetDevice();
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
                  "Query double precision support");
            if (!options.DoublePrecisionFloatShaderOps)
                throw std::runtime_error("Reflection qualification requires double precision shader operations");
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
            check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)),
                  "Query reflection shader model");
            if (model.HighestShaderModel < D3D_SHADER_MODEL_6_5)
                throw std::runtime_error("Reflection qualification requires shader model 6.5");
            // FXC's optimized output loses vector components in this kernel.
            // Use the renderer's existing optimized DXC path, qualified below.
            const std::string shader =
                std::string(projection ? "#define TF_GPU_REFLECTION_PROJECT 1\n#define TF_GPU_REFLECTION_CAPTURE 1\n"
                                       : "") +
                tf_gpu_reflection_shader;
            auto code = D3D::CompileShaderDXIL(D3D::ShaderType::Compute, D3D::ShaderModel::SM65, false, shader);
            if (!code)
                throw std::runtime_error("Reflection DXIL compilation failed");
            wil::com_ptr_nothrow<ID3DBlob> diagnostics, serialized;
            D3D12_ROOT_PARAMETER parameters[3]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[2].Constants.Num32BitValues = 1;
            D3D12_ROOT_SIGNATURE_DESC root{};
            root.NumParameters = 3;
            root.pParameters = parameters;
            diagnostics.reset();
            check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), diagnostics.put()),
                  "Serialize reflection signature");
            wil::com_ptr_nothrow<ID3D12RootSignature> signature;
            check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                              IID_PPV_ARGS(signature.put())),
                  "Create reflection signature");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = signature.get();
            desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            wil::com_ptr_nothrow<ID3D12PipelineState> pipeline;
            check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline.put())),
                  "Create reflection pipeline");
            auto source = createBuffer(device, count * inputStride, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
            auto result =
                createBuffer(device, count * outputStride, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = createBuffer(device, count * outputStride, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
            struct LifetimeGuard
            {
                GSDevice12 *device;
                ~LifetimeGuard()
                {
                    device->WaitForGPUIdle();
                }
            } lifetime{gs};
            upload(source.get(), inputs, count * inputStride);
            gs->EndRenderPass();
            auto *command = gs->GetCommandList().list4.get();
            command->SetComputeRootSignature(signature.get());
            command->SetPipelineState(pipeline.get());
            command->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
            command->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
            command->SetComputeRoot32BitConstant(2, count, 0);
            command->Dispatch((count + 63) / 64, 1, 1);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = result.get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            command->ResourceBarrier(1, &barrier);
            command->CopyBufferRegion(readback.get(), 0, result.get(), 0, count * outputStride);
            gs->InvalidateCachedState();
            gs->ExecuteCommandList(true);
            void *mapped = nullptr;
            const D3D12_RANGE range{0, count * outputStride};
            check(readback->Map(0, &range, &mapped), "Read diagnostic transformed vertices");
            std::memcpy(output, mapped, count * outputStride);
            const D3D12_RANGE empty{0, 0};
            readback->Unmap(0, &empty);
            passed = true;
        }
        catch (const std::exception &e)
        {
            if (error && errorCapacity)
                std::snprintf(error, errorCapacity, "%s", e.what());
        }
    });
    return passed ? 1 : 0;
}

extern "C" TF_HWGS_API int tf_gs_gpu_reflection_probe(const void *inputs, uint32_t count, void *output,
                                                      uint32_t outputCapacity, char *error, uint32_t errorCapacity)
{
    return reflectionProbe(inputs, count, output, outputCapacity, error, errorCapacity, false);
}

extern "C" TF_HWGS_API int tf_gs_gpu_reflection_projection_probe(const void *inputs, uint32_t count, void *output,
                                                                 uint32_t outputCapacity, char *error,
                                                                 uint32_t errorCapacity)
{
    return reflectionProbe(inputs, count, output, outputCapacity, error, errorCapacity, true);
}
