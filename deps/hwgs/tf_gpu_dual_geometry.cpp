// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_shaders.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX11/D3D.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

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
    explicit Pipeline(GSDevice12 &device, bool grouped = false, bool grass = false, bool reflection = false,
                      bool reflectionCull = false)
        : gs(device)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        check(gs.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              "Cannot query dual-transform arithmetic support");
        if (!options.DoublePrecisionFloatShaderOps)
            throw std::runtime_error("Native dual geometry requires double-precision shader arithmetic");
        wil::com_ptr_nothrow<ID3DBlob> code, error, serialized;
        const D3D_SHADER_MACRO macros[] = {{"TF_GPU_DUAL_GROUPED", "1"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO grassMacros[] = {
            {"TF_GPU_GRASS_NATIVE", "1"}, {"TF_GPU_GRASS_CULL", "1"}, {nullptr, nullptr}};
        if (reflection)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
            check(gs.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)),
                  "Cannot query reflection shader support");
            if (model.HighestShaderModel < D3D_SHADER_MODEL_6_5)
                throw std::runtime_error("Native reflection geometry requires shader model 6.5");
            const std::string shader = std::string("#define TF_GPU_REFLECTION_PROJECT 1\n") +
                (reflectionCull ? "#define TF_GPU_REFLECTION_CULL 1\n" : "") + tf_gpu_reflection_shader;
            code = D3D::CompileShaderDXIL(D3D::ShaderType::Compute, D3D::ShaderModel::SM65, false, shader);
            if (!code)
                throw std::runtime_error("Native reflection DXIL compilation failed");
        }
        else
        {
            const char *shader = grass ? tf_gpu_grass_emit_shader : tf_gpu_dual_transform_shader;
            const HRESULT compiled =
                D3DCompile(shader, std::strlen(shader), grass ? "grass-emitter" : "dual-transform",
                           grass ? grassMacros : (grouped ? macros : nullptr), nullptr, "main", "cs_5_0",
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                           0, code.put(), error.put());
            if (FAILED(compiled))
                throw std::runtime_error(error ? static_cast<const char *>(error->GetBufferPointer())
                                               : "Dual shader compilation failed");
        }
        D3D12_ROOT_PARAMETER parameters[11]{};
        const unsigned srvs = (grass || reflection) ? 1 : (grouped ? 9 : 2);
        for (unsigned i = 0; i < srvs; ++i)
        {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[i].Descriptor.ShaderRegister = i;
        }
        parameters[srvs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[srvs + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[srvs + 1].Constants.Num32BitValues = grass ? 2 : 1;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = srvs + 2;
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
    void *mappedUpload = nullptr;
    bool vertexState = false;
    Page(GSDevice12 &device, const DualGeometry *pool, uint32_t uploadBytes, uint32_t outputBytes)
        : gs(device), owner(pool), uploadCapacity(uploadBytes), outputCapacity(outputBytes)
    {
        upload = makeBuffer(gs, uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                            D3D12_RESOURCE_FLAG_NONE);
        output = makeBuffer(gs, outputBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RANGE noReads{0, 0};
        check(upload->Map(0, &noReads, &mappedUpload), "Cannot map native geometry upload page");
    }
    ~Page()
    {
        upload->Unmap(0, nullptr);
        // Retaining these through the current list also covers earlier lists.
        // Neither release nor pool exhaustion waits for GPU completion.
        if (fence > gs.GetCompletedFenceValue())
        {
            gs.DeferObjectDestruction(upload.get());
            gs.DeferObjectDestruction(output.get());
        }
    }
};

struct DualGeometry::Source
{
    GSDevice12 &gs;
    const DualGeometry *owner;
    wil::com_ptr_nothrow<ID3D12Resource> buffer;
    std::vector<uint32_t> firsts;
    std::vector<DualAttributes> attributes;
    uint32_t vertices = 0, capacity = 0;
    uint64_t fence = 0;
    Source(GSDevice12 &device, const DualGeometry *pool) : gs(device), owner(pool)
    {
    }
    ~Source()
    {
        if (buffer && fence > gs.GetCompletedFenceValue())
            gs.DeferObjectDestruction(buffer.get());
    }
};

DualGeometry::DualGeometry(GSDevice12 &device, uint64_t budget, uint64_t meshBudget)
    : m_device(device), m_pipeline(std::make_unique<Pipeline>(device)), m_budget(budget), m_meshBudget(meshBudget)
{
}
DualGeometry::~DualGeometry() = default;

std::shared_ptr<DualGeometry::Page> DualGeometry::acquirePage(uint32_t uploadBytes, uint32_t outputBytes)
{
    std::shared_ptr<Page> page;
    const auto completed = m_device.GetCompletedFenceValue();
    // Start after the last selected page instead of repeatedly scanning the
    // same in-flight prefix. A full pass still checks every candidate once.
    for (std::size_t visited = 0, count = m_pages.size(); visited < count; ++visited)
    {
        if (m_pageCursor >= count)
            m_pageCursor = 0;
        const auto &candidate = m_pages[m_pageCursor++];
        ++m_stats.pageProbes;
        if (candidate.use_count() == 1 && candidate->fence <= completed && candidate->uploadCapacity >= uploadBytes &&
            candidate->outputCapacity >= outputBytes)
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
            if (item->use_count() == 1 && (*item)->fence <= completed)
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
        m_pageCursor = m_pages.size();
        m_stats.bytes += bytes;
        ++m_stats.allocations;
    }
    return page;
}

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
    auto page = acquirePage((constantsOffset + batches * 144 + 65535) & ~65535u, (vertices * 64 + 65535) & ~65535u);
    if (!page)
        return {};
    // acquirePage admits only unretained pages whose previous GPU use ended.
    // These CPU writes finish before dispatch/command-list submission.
    std::memcpy(page->mappedUpload, source, size_t(vertices) * 32);
    std::memcpy(static_cast<uint8_t *>(page->mappedUpload) + constantsOffset, transforms, size_t(batches) * 144);
    m_stats.sourceUploadBytes += uint64_t(vertices) * 32;
    m_stats.constantUploadBytes += uint64_t(batches) * 144;
    const auto sourceAddress = page->upload->GetGPUVirtualAddress();
    return dispatch(std::move(page), sourceAddress, vertices, constantsOffset);
}

DualGeometry::Mesh DualGeometry::uploadMesh(const void *inputs, uint32_t vertices, uint32_t batches)
{
    if (!inputs || !vertices || vertices > 1024 * 1024 || !batches || batches > 65536)
        return {};
    const auto *data = static_cast<const uint8_t *>(inputs);
    auto source = std::make_shared<Source>(m_device, this);
    for (uint32_t i = 0; i < vertices; ++i)
    {
        const uint32_t batch = word(data + i * 32 + 12);
        if (batch == source->firsts.size() && batch < batches)
            source->firsts.push_back(i);
        if (source->firsts.empty() || batch != source->firsts.size() - 1 || i - source->firsts.back() >= 28)
            return {};
    }
    if (source->firsts.size() != batches)
        return {};
    source->attributes.resize(batches);
    for (uint32_t i = 0; i < batches; ++i)
    {
        const uint32_t first = source->firsts[i], end = i + 1 == batches ? vertices : source->firsts[i + 1];
        if (!summarizeDualInputs(data + first * 32, end - first, source->attributes[i]))
            return {};
    }
    const uint32_t capacity = (vertices * 32 + 65535) & ~65535u;
    if (capacity > m_meshBudget)
        return {};
    for (auto item = m_sources.begin(); m_stats.meshBytes > m_meshBudget - capacity && item != m_sources.end();)
    {
        if (item->use_count() == 1 && (*item)->fence <= m_device.GetCompletedFenceValue())
        {
            m_stats.meshBytes -= (*item)->capacity;
            item = m_sources.erase(item);
        }
        else
            ++item;
    }
    if (m_stats.meshBytes > m_meshBudget - capacity)
        return {};
    source->buffer = makeBuffer(m_device, capacity, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_FLAG_NONE);
    auto staging = makeBuffer(m_device, capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                              D3D12_RESOURCE_FLAG_NONE);
    void *mapped = nullptr;
    const D3D12_RANGE empty{0, 0}, written{0, size_t(vertices) * 32};
    check(staging->Map(0, &empty, &mapped), "Cannot map native mesh staging upload");
    std::memcpy(mapped, inputs, written.End);
    staging->Unmap(0, &written);
    // Finish all potentially throwing host allocations before queuing GPU use.
    m_sources.push_back(source);
    m_device.EndRenderPass();
    auto *command = m_device.GetCommandList().list4.get();
    command->CopyBufferRegion(source->buffer.get(), 0, staging.get(), 0, written.End);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source->buffer.get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command->ResourceBarrier(1, &barrier);
    source->vertices = vertices;
    source->capacity = capacity;
    source->fence = m_device.GetCurrentFenceValue();
    m_device.DeferObjectDestruction(staging.get());
    m_stats.meshBytes += capacity;
    m_stats.sourceUploadBytes += written.End;
    ++m_stats.meshUploads;
    Mesh mesh;
    mesh.source = std::move(source);
    return mesh;
}

DualGeometry::Batch DualGeometry::prepare(const Mesh &mesh, const void *constants, uint32_t batches)
{
    if (!mesh.source || mesh.source->owner != this || !constants || batches != mesh.source->firsts.size())
        return {};
    const auto *transforms = static_cast<const uint8_t *>(constants);
    for (uint32_t i = 0; i < batches; ++i)
        if (word(transforms + i * 144 + 140) != mesh.source->firsts[i])
            return {};
    const uint32_t vertices = mesh.source->vertices;
    auto page = acquirePage((batches * 144 + 65535) & ~65535u, (vertices * 64 + 65535) & ~65535u);
    if (!page)
        return {};
    std::memcpy(page->mappedUpload, constants, size_t(batches) * 144);
    m_stats.constantUploadBytes += uint64_t(batches) * 144;
    auto result = dispatch(std::move(page), mesh.source->buffer->GetGPUVirtualAddress(), vertices, 0);
    mesh.source->fence = m_device.GetCurrentFenceValue();
    return result;
}

DualGeometry::Batch DualGeometry::prepareGroup(const Mesh *meshes, const void *constants, uint32_t count)
{
    if (!meshes || !constants || !count || count > 8)
        return {};
    const auto *transforms = static_cast<const uint8_t *>(constants);
    std::array<uint8_t, 8 * 160> records{};
    uint64_t sources[8]{};
    uint32_t vertices = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &source = meshes[i].source;
        if (!source || source->owner != this || source->firsts.size() != 1 || source->vertices > 28 ||
            word(transforms + i * 144 + 140) != 0)
            return {};
        auto *record = records.data() + i * 160;
        std::memcpy(record, transforms + i * 144, 144);
        std::memcpy(record + 144, &source->vertices, 4);
        std::memcpy(record + 148, &vertices, 4);
        vertices += source->vertices;
        sources[i] = source->buffer->GetGPUVirtualAddress();
    }
    if (!m_groupedPipeline)
        m_groupedPipeline = std::make_unique<Pipeline>(m_device, true);
    auto page = acquirePage(65536, 65536);
    if (!page)
        return {};
    std::memcpy(page->mappedUpload, records.data(), count * 160);
    m_stats.constantUploadBytes += count * 160;
    auto result = dispatch(std::move(page), sources[0], vertices, 0, sources, count);
    for (unsigned i = 0; i < count; ++i)
        meshes[i].source->fence = m_device.GetCurrentFenceValue();
    return result;
}

DualGeometry::Batch DualGeometry::prepareGrass(const void *inputs, uint32_t count, const GrassRasterState *raster,
                                               bool fieldMajor)
{
    if (!inputs || !count || count > 65536)
        return {};
    if (raster)
        for (uint32_t i = 0; i < count; ++i)
            for (unsigned field = 0; field < 2; ++field)
                if (raster[i].offset[field][2] || uint32_t(raster[i].offset[field][3]) > 1)
                    return {};
    if (!m_grassPipeline)
        m_grassPipeline = std::make_unique<Pipeline>(m_device, false, true);
    auto page = acquirePage((count * 384 + 65535) & ~65535u, (count * 1024 + 65535) & ~65535u);
    if (!page)
        return {};
    const GrassRasterState disabled{};
    for (uint32_t i = 0; i < count; ++i)
    {
        auto *destination = static_cast<uint8_t *>(page->mappedUpload) + i * 384;
        std::memcpy(destination, static_cast<const uint8_t *>(inputs) + i * 320, 320);
        std::memcpy(destination + 320, raster ? &raster[i] : &disabled, 64);
    }
    m_stats.constantUploadBytes += uint64_t(count) * 384;
    const auto address = page->upload->GetGPUVirtualAddress();
    return dispatch(std::move(page), address, count * 16, 0, nullptr, 0, true, fieldMajor);
}

DualGeometry::Batch DualGeometry::prepareReflection(const void *inputs, uint32_t count)
{
    if (!inputs || !count || count > 65536)
        return {};
    if (!m_reflectionPipeline)
        m_reflectionPipeline = std::make_unique<Pipeline>(m_device, false, false, true);
    auto page = acquirePage((count * 368 + 65535) & ~65535u, (count * 128 + 65535) & ~65535u);
    if (!page)
        return {};
    std::memcpy(page->mappedUpload, inputs, size_t(count) * 368);
    m_stats.sourceUploadBytes += uint64_t(count) * 48;
    m_stats.constantUploadBytes += uint64_t(count) * 320;
    const auto address = page->upload->GetGPUVirtualAddress();
    return dispatch(std::move(page), address, count * 2, 0, nullptr, 0, false, false, true);
}

DualGeometry::Batch DualGeometry::prepareReflectionCulled(const void *inputs, uint32_t count,
                                                         const ReflectionRasterState &raster)
{
    if (!inputs || count < 3 || count > 22)
        return {};
    for (unsigned section = 0; section < 4; ++section)
        if (raster.offset[section][2] || raster.offset[section][3] ||
            raster.scissor[section][0] >= raster.scissor[section][2] ||
            raster.scissor[section][1] >= raster.scissor[section][3])
            return {};
    if (!m_reflectionCullPipeline)
        m_reflectionCullPipeline = std::make_unique<Pipeline>(m_device, false, false, true, true);
    auto page = acquirePage((count * 368 + 128 + 65535) & ~65535u, ((count - 2) * 384 + 65535) & ~65535u);
    if (!page)
        return {};
    std::memcpy(page->mappedUpload, inputs, size_t(count) * 368);
    std::memcpy(static_cast<uint8_t *>(page->mappedUpload) + count * 368, &raster, sizeof(raster));
    m_stats.sourceUploadBytes += uint64_t(count) * 48;
    m_stats.constantUploadBytes += uint64_t(count) * 320 + 128;
    const auto address = page->upload->GetGPUVirtualAddress();
    return dispatch(std::move(page), address, (count - 2) * 6, 0, nullptr, 0, false, false, true, count);
}

bool DualGeometry::bounds(const Mesh &mesh, const void *constants, uint32_t batches, uint32_t firstChunk,
                          uint32_t chunks, std::array<DualDrawBounds, 2> &result) const
{
    result = {};
    if (!mesh.source || mesh.source->owner != this || !constants || batches != mesh.source->firsts.size() ||
        firstChunk >= batches || !chunks || chunks > batches - firstChunk)
        return false;
    const auto *transforms = static_cast<const uint8_t *>(constants);
    const auto *first = transforms + firstChunk * 144;
    DualAttributes attributes;
    for (uint32_t i = firstChunk; i < firstChunk + chunks; ++i)
    {
        if (std::memcmp(first, transforms + i * 144, 132) || word(transforms + i * 144 + 140) != mesh.source->firsts[i])
            return false;
        mergeDualAttributes(attributes, mesh.source->attributes[i]);
    }
    if (!projectDualBounds(attributes, first, word(first + 128), result[0]))
        return false;
    if (std::memcmp(first, first + 64, 64) == 0)
    {
        result[1] = result[0];
        return true;
    }
    return projectDualBounds(attributes, first + 64, word(first + 128), result[1]);
}

DualGeometry::Batch DualGeometry::dispatch(std::shared_ptr<Page> page, uint64_t sourceAddress, uint32_t vertices,
                                           uint32_t constantsOffset, const uint64_t *groupSources, uint32_t groups,
                                           bool grass, bool fieldMajor, bool reflection, uint32_t reflectionCullCount)
{
    m_device.EndRenderPass();
    auto *command = m_device.GetCommandList().list4.get();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = page->output.get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (page->vertexState)
    {
        barrier.Transition.StateBefore =
            (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        command->ResourceBarrier(1, &barrier);
    }
    const auto &pipeline =
        reflection ? (reflectionCullCount ? m_reflectionCullPipeline : m_reflectionPipeline)
                   : (grass ? m_grassPipeline : (groups ? m_groupedPipeline : m_pipeline));
    const unsigned sources = groups ? 8 : 1;
    command->SetComputeRootSignature(pipeline->signature.get());
    command->SetPipelineState(pipeline->pipeline.get());
    for (unsigned i = 0; i < sources; ++i)
        command->SetComputeRootShaderResourceView(i, groups && i < groups ? groupSources[i] : sourceAddress);
    if (!grass && !reflection)
        command->SetComputeRootShaderResourceView(sources, page->upload->GetGPUVirtualAddress() + constantsOffset);
    const unsigned outputSlot = (grass || reflection) ? 1 : sources + 1;
    const uint32_t count = reflection ? (reflectionCullCount ? reflectionCullCount : vertices / 2)
                                     : (grass ? vertices / 16 : (groups ? groups : vertices));
    command->SetComputeRootUnorderedAccessView(outputSlot, page->output->GetGPUVirtualAddress());
    command->SetComputeRoot32BitConstant(outputSlot + 1, count, 0);
    if (grass)
        command->SetComputeRoot32BitConstant(outputSlot + 1, fieldMajor, 1);
    command->Dispatch(((reflection ? count : (grass ? count * 8 : (groups ? groups * 32 : vertices))) + 63) / 64, 1, 1);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter =
        (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command->ResourceBarrier(1, &barrier);
    page->vertexState = true;
    page->fence = m_device.GetCurrentFenceValue();
    m_device.InvalidateCachedState();
    ++m_stats.dispatches;
    m_stats.groupedDispatches += groups != 0;
    m_stats.groupedMeshes += groups;
    m_stats.grassDispatches += grass;
    m_stats.grassBatches += grass ? count : 0;
    m_stats.reflectionDispatches += reflection;
    m_stats.reflectionVertices += reflection ? count : 0;
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
