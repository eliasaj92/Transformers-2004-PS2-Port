// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic qualification of compute -> native GS draw. No game data.
// One final image download is diagnostic only; vertex output stays on the GPU.
#include "tf_hwgs.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/Common/GSVertex.h"
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace
{
void check(HRESULT hr, const char *where)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: %08lx", where, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}
auto buffer(ID3D12Device *device, uint32_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
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
        "Create geometry buffer");
    return result;
}
constexpr char shader[] = R"(
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer vertices : register(u0);
cbuffer Constants : register(b0) {
    float4 column0, column1, column2, column3;
    uint color, count, pad0, pad1;
};
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(id.x>=count) return;
    const float4 p=asfloat(source.Load4(id.x*16));
    precise float4 transformed=column3;
    transformed=transformed+column2*p.z;
    transformed=transformed+column1*p.y;
    transformed=transformed+column0*p.x;
    const uint2 xy=uint2(transformed.xy*16.0f);
    // GSVertex: ST, RGBAQ, packed XY, Z, UV, FOG. All output is GPU-written.
    vertices.Store4(id.x*32,uint4(0,0,color,asuint(1.0f)));
    vertices.Store4(id.x*32+16,uint4((xy.x&65535)|(xy.y<<16),1,0,0));
}
)";
struct Constants
{
    float columns[16] = {16, 0, 0, 0, 0, 16, 0, 0, 0, 0, 1, 0, 32, 32, 0, 1};
    uint32_t color = 0, count = 3, pad0 = 0, pad1 = 0;
};
static_assert(sizeof(Constants) == 80 && sizeof(GSVertex) == 32);
} // namespace

extern "C" TF_HWGS_API int tf_gs_gpu_geometry_probe(uint32_t *painted, char *error, uint32_t capacity)
{
    bool passed = false;
    if (painted)
        *painted = 0;
    kzgs::RunOnGSThread([&] {
        try
        {
            if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12)
                throw std::runtime_error("GPU geometry probe requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            auto *device = gs->GetDevice();
            wil::com_ptr_nothrow<ID3DBlob> code, diagnostics, serialized;
            const HRESULT compiled =
                D3DCompile(shader, sizeof(shader) - 1, "native-geometry-probe", nullptr, nullptr, "main", "cs_5_0",
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                           0, code.put(), diagnostics.put());
            if (FAILED(compiled))
                throw std::runtime_error(diagnostics ? static_cast<const char *>(diagnostics->GetBufferPointer())
                                                     : "Geometry shader compilation failed");
            D3D12_ROOT_PARAMETER parameters[3]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[2].Constants.Num32BitValues = 20;
            D3D12_ROOT_SIGNATURE_DESC root{};
            root.NumParameters = 3;
            root.pParameters = parameters;
            diagnostics.reset();
            check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), diagnostics.put()),
                  "Serialize geometry root signature");
            wil::com_ptr_nothrow<ID3D12RootSignature> signature;
            check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                              IID_PPV_ARGS(signature.put())),
                  "Create geometry root signature");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = signature.get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            wil::com_ptr_nothrow<ID3D12PipelineState> pipeline;
            check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline.put())),
                  "Create geometry compute pipeline");
            auto input =
                buffer(device, 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
            auto green = buffer(device, 96, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto red = buffer(device, 96, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            constexpr float points[] = {-1, -1, 0, 1, 1, -1, 0, 1, 0, 1, 0, 1};
            void *mapped = nullptr;
            const D3D12_RANGE empty{0, 0};
            check(input->Map(0, &empty, &mapped), "Map input positions");
            std::memcpy(mapped, points, sizeof(points));
            const D3D12_RANGE written{0, sizeof(points)};
            input->Unmap(0, &written);
            // This diagnostic waits only after all draws; protect resources on failures too.
            struct TargetOwner
            {
                GSDevice12 *device;
                GSTexture *target;
                ~TargetOwner()
                {
                    device->WaitForGPUIdle();
                    if (target)
                        device->Recycle(target);
                }
            } target{gs, gs->CreateRenderTarget(64, 64, GSTexture::Format::Color, true, false)};
            if (!target.target)
                throw std::runtime_error("Create geometry probe target failed");
            const uint16_t indices[] = {0, 1, 2};
            GSHWDrawConfig draw{};
            draw.rt = target.target;
            draw.verts = nullptr;
            draw.indices = indices;
            draw.nverts = 3;
            draw.nindices = 3;
            draw.indices_per_prim = 3;
            draw.topology = GSHWDrawConfig::Topology::Triangle;
            draw.scissor = draw.drawarea = GSVector4i(0, 0, 64, 64);
            draw.vs.iip = 1;
            draw.ps.iip = 1;
            draw.ps.tfx = 4;
            draw.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
            draw.cb_vs.vertex_scale = GSVector2(2.0f / (64 * 16), 2.0f / (64 * 16));
            draw.cb_vs.vertex_offset = GSVector2(1, 1);
            draw.cb_vs.max_depth = 0xffffffffu;
            const auto dispatch = [&](ID3D12Resource *output, uint32_t color, float xOffset) {
                gs->EndRenderPass();
                auto *command = gs->GetCommandList().list4.get();
                Constants constants;
                constants.color = color;
                constants.columns[12] += xOffset;
                command->SetComputeRootSignature(signature.get());
                command->SetPipelineState(pipeline.get());
                command->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
                command->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
                command->SetComputeRoot32BitConstants(2, 20, &constants, 0);
                command->Dispatch(1, 1, 1);
                D3D12_RESOURCE_BARRIER barriers[2]{};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barriers[0].UAV.pResource = output;
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition.pResource = output;
                barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                command->ResourceBarrier(2, barriers);
                gs->InvalidateCachedState();
                const D3D12_VERTEX_BUFFER_VIEW view{output->GetGPUVirtualAddress(), 96, 32};
                if (!gs->RenderHWFromGPU(draw, view))
                    throw std::runtime_error("Native GPU vertex draw rejected");
            };
            dispatch(green.get(), 0x80008000u, 0);
            // Reject an unsupported expansion without replacing the active state.
            draw.vs.expand = GSHWDrawConfig::VSExpand::Sprite;
            const D3D12_VERTEX_BUFFER_VIEW view{green->GetGPUVirtualAddress(), 96, 32};
            if (gs->RenderHWFromGPU(draw, view))
                throw std::runtime_error("Unsupported expansion accepted");
            draw.vs.expand = GSHWDrawConfig::VSExpand::None;
            const uint16_t invalidIndices[] = {0, 1, 3};
            draw.indices = invalidIndices;
            if (gs->RenderHWFromGPU(draw, view))
                throw std::runtime_error("Out-of-range GPU vertex index accepted");
            draw.indices = indices;
            auto shortView = view;
            shortView.SizeInBytes = 64;
            if (gs->RenderHWFromGPU(draw, shortView))
                throw std::runtime_error("Short GPU vertex buffer accepted");
            dispatch(red.get(), 0x80000080u, 8);
            // Verify return to ordinary GS input after native draws. This uses the
            // same enhanced renderer, not a separate reference executable.
            std::array<GSVertex, 3> ordinary{};
            const uint16_t xy[3][2] = {{4, 4}, {12, 4}, {8, 12}};
            for (size_t i = 0; i < ordinary.size(); ++i)
            {
                const uint32_t words[] = {
                    0, 0, 0x80800000u, 0x3f800000u, uint32_t(xy[i][0] * 16) | (uint32_t(xy[i][1] * 16) << 16), 1, 0, 0};
                std::memcpy(&ordinary[i], words, sizeof(GSVertex));
            }
            draw.verts = ordinary.data();
            static_cast<GSDevice *>(gs)->RenderHW(draw);
            // A single image download checks ordered drawing. Neither vertex buffer is mapped.
            auto download = gs->CreateDownloadTexture(64, 64, GSTexture::Format::Color);
            if (!download)
                throw std::runtime_error("Create diagnostic image download failed");
            const GSVector4i area(0, 0, 64, 64);
            download->CopyFromTexture(area, target.target, area, 0);
            download->Flush();
            std::array<uint8_t, 64 * 64 * 4> pixels{};
            if (!download->ReadTexels(area, pixels.data(), 64 * 4))
                throw std::runtime_error("Read diagnostic image failed");
            uint32_t count = 0, greenCount = 0, blueCount = 0;
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                if (pixels[i] >= 64 && pixels[i + 1] < 16 && pixels[i + 2] < 16)
                    ++count;
                if (pixels[i] < 16 && pixels[i + 1] >= 64 && pixels[i + 2] < 16)
                    ++greenCount;
                if (pixels[i] < 16 && pixels[i + 1] < 16 && pixels[i + 2] >= 64)
                    ++blueCount;
            }
            const size_t center = (32 * 64 + 32) * 4;
            if (count < 400 || count > 600 || greenCount < 100 || blueCount < 20 || blueCount > 45 ||
                pixels[center] < 64 || pixels[center + 1] >= 16)
                throw std::runtime_error("GPU transform/draw ordering image mismatch");
            if (painted)
                *painted = count;
            std::fprintf(stderr,
                         "[hwgs:geometry] GPU transform -> GS draw PASS; native_draws=2 red=%u green=%u "
                         "ordinary_blue=%u native_CPU_vertices=null\n",
                         count, greenCount, blueCount);
            passed = true;
        }
        catch (const std::exception &e)
        {
            if (error && capacity)
                std::snprintf(error, capacity, "%s", e.what());
            std::fprintf(stderr, "[hwgs:geometry] %s\n", e.what());
        }
    });
    return passed ? 1 : 0;
}
