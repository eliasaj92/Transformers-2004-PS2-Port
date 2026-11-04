// SPDX-License-Identifier: GPL-3.0-or-later
// Offline arithmetic qualification. Readback here is diagnostic only.
#include "tf_hwgs.h"
#include "tf_gpu_shaders.h"
#include "tf_gpu_dual_bounds.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cfenv>

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
        "Create grass-emitter buffer");
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

static int grassProbe(const void *inputs, uint32_t count, void *output, uint32_t outputCapacity, char *error,
                      uint32_t errorCapacity, bool native)
{
    const uint32_t inputStride = native ? 320 : 256, outputStride = native ? 1024 : 640;
    bool passed = false;
    if (!inputs || !output || !count || count > 65536 || outputCapacity < uint64_t(count) * outputStride)
    {
        if (error && errorCapacity)
            std::snprintf(error, errorCapacity, "Invalid grass-emitter probe buffers");
        return 0;
    }
    kzgs::RunOnGSThread([&] {
        try
        {
            if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12)
                throw std::runtime_error("Grass-emitter probe requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            auto *device = gs->GetDevice();
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
                  "Query double precision support");
            if (!options.DoublePrecisionFloatShaderOps)
                throw std::runtime_error("Grass-emitter qualification requires double precision shader operations");
            wil::com_ptr_nothrow<ID3DBlob> code, diagnostics, serialized;
            const D3D_SHADER_MACRO macros[] = {{"TF_GPU_GRASS_NATIVE", "1"}, {nullptr, nullptr}};
            const HRESULT compiled =
                D3DCompile(tf_gpu_grass_emit_shader, sizeof(tf_gpu_grass_emit_shader) - 1, "grass-emitter",
                           native ? macros : nullptr, nullptr, "main", "cs_5_0",
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                           0, code.put(), diagnostics.put());
            if (FAILED(compiled))
                throw std::runtime_error(diagnostics ? static_cast<const char *>(diagnostics->GetBufferPointer())
                                                     : "Shader compilation failed");
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
                  "Serialize grass-emitter signature");
            wil::com_ptr_nothrow<ID3D12RootSignature> signature;
            check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                              IID_PPV_ARGS(signature.put())),
                  "Create grass-emitter signature");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = signature.get();
            desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            wil::com_ptr_nothrow<ID3D12PipelineState> pipeline;
            check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline.put())),
                  "Create grass-emitter pipeline");
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
            command->Dispatch((count * 8 + 63) / 64, 1, 1);
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

extern "C" TF_HWGS_API int tf_gs_gpu_grass_emit_probe(const void *inputs, uint32_t count, void *output,
                                                      uint32_t outputCapacity, char *error, uint32_t errorCapacity)
{
    return grassProbe(inputs, count, output, outputCapacity, error, errorCapacity, false);
}
extern "C" TF_HWGS_API int tf_gs_gpu_grass_native_probe(const void *inputs, uint32_t count, void *output,
                                                        uint32_t outputCapacity, char *error, uint32_t errorCapacity)
{
    return grassProbe(inputs, count, output, outputCapacity, error, errorCapacity, true);
}

extern "C" TF_HWGS_API int tf_gs_grass_bounds_probe(const void *inputs, uint32_t count, const void *expected,
                                                    uint64_t *stats, char *error, uint32_t capacity)
{
    if (!inputs || !expected || !stats || !count || count > 65536)
        return 0;
    const auto word = [](const uint8_t *data) {
        uint32_t value;
        std::memcpy(&value, data, 4);
        return value;
    };
    for (unsigned packet = 0; packet < count; ++packet)
    {
        std::array<tf_gpu::DualDrawBounds, 2> bounds;
        const uint32_t saved = _mm_getcsr();
        const int rounding = std::fegetround();
        const bool accepted = tf_gpu::projectGrassBounds(static_cast<const uint8_t *>(inputs) + packet * 320, bounds);
        if (_mm_getcsr() != saved || std::fegetround() != rounding)
        {
            if (error && capacity)
                std::snprintf(error, capacity, "Grass bounds changed the caller's float environment");
            return 0;
        }
        ++stats[accepted ? 0 : 1];
        if (!accepted)
            continue;
        for (unsigned field = 0; field < 2; ++field)
            for (unsigned vertex = 0; vertex < 16; ++vertex)
            {
                const auto &b = bounds[field];
                const auto *v = static_cast<const uint8_t *>(expected) + packet * 1024 + field * 512 + vertex * 32;
                const uint32_t position[] = {word(v + 16) & 65535u, word(v + 16) >> 16, word(v + 20)};
                bool valid = true;
                for (unsigned axis = 0; axis < 3; ++axis)
                    valid &= position[axis] >= b.xyzMin[axis] && position[axis] <= b.xyzMax[axis];
                if (vertex & 1)
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const unsigned color = (word(v + 8) >> (channel * 8)) & 255;
                        valid &= color >= b.colorMin[channel] && color <= b.colorMax[channel];
                    }
                if (!valid)
                {
                    if (error && capacity)
                        std::snprintf(error, capacity, "Grass bounds missed packet %u field %u vertex %u", packet,
                                      field, vertex);
                    return 0;
                }
                ++stats[2];
            }
    }
    return stats[0] ? 1 : 0;
}
