// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_shaders.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace tf_gpu
{
namespace
{
void check(HRESULT result, const char *operation)
{
    if (FAILED(result))
        throw std::runtime_error(operation);
}
uint32_t word(const uint8_t *address)
{
    uint32_t value;
    std::memcpy(&value, address, 4);
    return value;
}
auto makeBuffer(GSDevice12 &gs, uint32_t bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                D3D12_RESOURCE_FLAGS flags)
{
    wil::com_ptr_nothrow<ID3D12Resource> resource;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    check(gs.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                  IID_PPV_ARGS(resource.put())),
          "Cannot allocate native dual geometry buffer");
    return resource;
}
} // namespace

struct DualGeometry::Pipeline
{
    GSDevice12 &gs;
    wil::com_ptr_nothrow<ID3D12RootSignature> signature;
    wil::com_ptr_nothrow<ID3D12PipelineState> pipeline;
    explicit Pipeline(GSDevice12 &device) : gs(device)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        check(gs.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              "Cannot query dual-transform arithmetic support");
        if (!options.DoublePrecisionFloatShaderOps)
            throw std::runtime_error("Native dual geometry requires double-precision shader arithmetic");
        wil::com_ptr_nothrow<ID3DBlob> code, error, serialized;
        const HRESULT compiled =
            D3DCompile(tf_gpu_dual_transform_shader, sizeof(tf_gpu_dual_transform_shader) - 1, "dual-transform",
                       nullptr, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                       code.put(), error.put());
        if (FAILED(compiled))
            throw std::runtime_error(error ? static_cast<const char *>(error->GetBufferPointer())
                                           : "Dual shader compilation failed");
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor.ShaderRegister = 1;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[3].Constants.Num32BitValues = 1;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 4;
        root.pParameters = parameters;
        error.reset();
        check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), error.put()),
              "Cannot serialize dual geometry root signature");
        check(gs.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(signature.put())),
              "Cannot create dual geometry root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = signature.get();
        desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        check(gs.GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline.put())),
              "Cannot create dual geometry pipeline");
    }
    ~Pipeline()
    {
        gs.DeferObjectDestruction(pipeline.get());
        gs.DeferObjectDestruction(signature.get());
    }
};

struct DualGeometry::Page
{
    GSDevice12 &gs;
    const DualGeometry *owner;
    wil::com_ptr_nothrow<ID3D12Resource> upload, output;
    uint32_t uploadCapacity, outputCapacity;
    uint64_t fence = 0;
    bool vertexState = false;
    Page(GSDevice12 &device, const DualGeometry *pool, uint32_t uploadBytes, uint32_t outputBytes)
        : gs(device), owner(pool), uploadCapacity(uploadBytes), outputCapacity(outputBytes)
    {
        upload = makeBuffer(gs, uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                            D3D12_RESOURCE_FLAG_NONE);
        output = makeBuffer(gs, outputBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    ~Page()
    {
        // Retaining these through the current list also covers earlier lists.
        // Neither release nor pool exhaustion waits for GPU completion.
        if (fence > gs.GetCompletedFenceValue())
        {
            gs.DeferObjectDestruction(upload.get());
            gs.DeferObjectDestruction(output.get());
        }
    }
};

DualGeometry::DualGeometry(GSDevice12 &device, uint64_t budget)
    : m_device(device), m_pipeline(std::make_unique<Pipeline>(device)), m_budget(budget)
{
}
DualGeometry::~DualGeometry() = default;

DualGeometry::Batch DualGeometry::prepare(const void *inputs, uint32_t vertices, const void *constants,
                                          uint32_t batches)
{
    if (!inputs || !constants || !vertices || vertices > 1024 * 1024 || !batches || batches > 65536)
        return {};
    const auto *source = static_cast<const uint8_t *>(inputs);
    const auto *transforms = static_cast<const uint8_t *>(constants);
    for (uint32_t i = 0; i < vertices; ++i)
    {
        const uint32_t batch = word(source + i * 32 + 12);
        if (batch >= batches)
            return {};
        const uint32_t first = word(transforms + batch * 144 + 140);
        if (first > i || i - first >= 28)
            return {};
    }
    const uint32_t constantsOffset = (vertices * 32 + 255) & ~255u;
    const uint32_t uploadBytes = (constantsOffset + batches * 144 + 65535) & ~65535u;
    const uint32_t outputBytes = (vertices * 64 + 65535) & ~65535u;
    std::shared_ptr<Page> page;
    for (const auto &candidate : m_pages)
    {
        if (candidate.use_count() == 1 && candidate->fence <= m_device.GetCompletedFenceValue() &&
            candidate->uploadCapacity >= uploadBytes && candidate->outputCapacity >= outputBytes)
        {
            page = candidate;
            ++m_stats.reuses;
            break;
        }
    }
    if (!page)
    {
        const uint64_t bytes = uint64_t(uploadBytes) + outputBytes;
        if (bytes > m_budget)
            return {};
        // A sequence of growing meshes must not strand the budget in smaller,
        // completed allocations. Never evict a retained or in-flight batch.
        for (auto item = m_pages.begin(); m_stats.bytes > m_budget - bytes && item != m_pages.end();)
        {
            if (item->use_count() == 1 && (*item)->fence <= m_device.GetCompletedFenceValue())
            {
                m_stats.bytes -= uint64_t((*item)->uploadCapacity) + (*item)->outputCapacity;
                item = m_pages.erase(item);
            }
            else
                ++item;
        }
        if (m_stats.bytes > m_budget - bytes)
            return {};
        page = std::make_shared<Page>(m_device, this, uploadBytes, outputBytes);
        m_pages.push_back(page);
        m_stats.bytes += bytes;
        ++m_stats.allocations;
    }
    void *mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    check(page->upload->Map(0, &empty, &mapped), "Cannot map dual geometry source upload");
    std::memcpy(mapped, source, size_t(vertices) * 32);
    std::memcpy(static_cast<uint8_t *>(mapped) + constantsOffset, transforms, size_t(batches) * 144);
    const D3D12_RANGE written{0, constantsOffset + size_t(batches) * 144};
    page->upload->Unmap(0, &written);
    m_device.EndRenderPass();
    auto *command = m_device.GetCommandList().list4.get();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = page->output.get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (page->vertexState)
    {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        command->ResourceBarrier(1, &barrier);
    }
    command->SetComputeRootSignature(m_pipeline->signature.get());
    command->SetPipelineState(m_pipeline->pipeline.get());
    command->SetComputeRootShaderResourceView(0, page->upload->GetGPUVirtualAddress());
    command->SetComputeRootShaderResourceView(1, page->upload->GetGPUVirtualAddress() + constantsOffset);
    command->SetComputeRootUnorderedAccessView(2, page->output->GetGPUVirtualAddress());
    command->SetComputeRoot32BitConstant(3, vertices, 0);
    command->Dispatch((vertices + 63) / 64, 1, 1);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    command->ResourceBarrier(1, &barrier);
    page->vertexState = true;
    page->fence = m_device.GetCurrentFenceValue();
    m_device.InvalidateCachedState();
    ++m_stats.dispatches;
    Batch result;
    result.page = std::move(page);
    result.vertices = vertices;
    return result;
}

bool DualGeometry::draw(const Batch &batch, GSHWDrawConfig &config, uint32_t firstSource, uint32_t sourceCount)
{
    if (!batch.page || batch.page->owner != this || firstSource > batch.vertices || !sourceCount ||
        sourceCount > batch.vertices - firstSource || config.nverts != sourceCount * 2)
        return false;
    const D3D12_VERTEX_BUFFER_VIEW view{batch.page->output->GetGPUVirtualAddress() + uint64_t(firstSource) * 64,
                                        sourceCount * 64, 32};
    const bool accepted = m_device.RenderHWFromGPU(config, view);
    // RenderHW may roll the command list internally. Retain through the final
    // list, rather than through only the compute dispatch's original fence.
    batch.page->fence = m_device.GetCurrentFenceValue();
    m_stats.draws += accepted;
    return accepted;
}
} // namespace tf_gpu
