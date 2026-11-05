// SPDX-License-Identifier: GPL-3.0-or-later
#include "renderer.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace tfnative {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " failed: HRESULT 0x" << std::hex << uint32_t(hr);
        throw std::runtime_error(message.str());
    }
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = type;
    result.CreationNodeMask = result.VisibleNodeMask = 1;
    return result;
}
D3D12_RESOURCE_DESC bufferDescription(uint64_t size) {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = result.DepthOrArraySize = result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}
D3D12_RESOURCE_DESC textureDescription(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = width;
    result.Height = height;
    result.DepthOrArraySize = result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Format = format;
    result.Flags = flags;
    return result;
}
bool finite(const Matrix& matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](float f) { return std::isfinite(f); });
}
} // namespace

struct Renderer::Impl {
    struct GpuMesh {
        ComPtr<ID3D12Resource> vertices, indices;
        D3D12_VERTEX_BUFFER_VIEW vb{};
        D3D12_INDEX_BUFFER_VIEW ib{};
        uint32_t indexCount = 0;
    };
    uint32_t width, height;
    std::string adapter;
    Statistics stats;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    HANDLE event = nullptr;
    uint64_t submitted = 0;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap, dsvHeap;
    UINT srvStride = 0;
    static constexpr UINT MaxTextures = 256;
    ComPtr<ID3D12Resource> color, depth;
    std::vector<GpuMesh> meshes;
    std::vector<ComPtr<ID3D12Resource>> textures;
    std::vector<Material> materials;

    ~Impl() {
        // Destruction cannot throw. On a removed device, resources are still
        // released; on success all queued uses complete before releasing them.
        if (fence && queue && event && fence->GetCompletedValue() < submitted &&
            SUCCEEDED(fence->SetEventOnCompletion(submitted, event)))
            WaitForSingleObject(event, 30000);
        if (event) CloseHandle(event);
    }
    void wait() {
        if (fence->GetCompletedValue() < submitted) {
            check(fence->SetEventOnCompletion(submitted, event), "SetEventOnCompletion");
            require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU fence timed out");
        }
        check(device->GetDeviceRemovedReason(), "D3D12 device");
    }
    void begin() {
        wait(); // one in-flight frame in this correctness prototype
        check(allocator->Reset(), "Reset allocator");
        check(commands->Reset(allocator.Get(), nullptr), "Reset command list");
    }
    void submit() {
        check(commands->Close(), "Close command list");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++submitted), "Signal fence");
    }
    void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER value{};
        value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        value.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        commands->ResourceBarrier(1, &value);
    }
    ComPtr<ID3D12Resource> makeBuffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        ComPtr<ID3D12Resource> resource;
        auto h = heap(type);
        auto description = bufferDescription(size);
        check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &description, state,
                                              nullptr, IID_PPV_ARGS(&resource)), "Create buffer");
        return resource;
    }
    ComPtr<ID3D12Resource> uploadBuffer(const void* bytes, size_t size, D3D12_RESOURCE_STATES state) {
        auto resource = makeBuffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto upload = makeBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        D3D12_RANGE empty{};
        check(upload->Map(0, &empty, &mapped), "Map upload");
        std::memcpy(mapped, bytes, size);
        upload->Unmap(0, nullptr);
        begin();
        commands->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, size);
        barrier(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
        submit();
        wait(); // temporary upload may now be released
        stats.assetUploadBytes += size;
        return resource;
    }
    ComPtr<ID3D12DescriptorHeap> makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible) {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = type;
        description.NumDescriptors = count;
        description.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> result;
        check(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&result)), "Create descriptor heap");
        return result;
    }
};

Renderer::Renderer(uint32_t width, uint32_t height, const std::wstring& adapterName,
                   const std::filesystem::path& shader, bool debugLayer) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    require(width && height && width <= 8192 && height <= 8192, "Invalid target size");
    p.width = width; p.height = height;
    if (debugLayer) {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12 debug layer requested but unavailable");
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create DXGI factory");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        auto hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        check(hr, "Enumerate hardware adapters");
        DXGI_ADAPTER_DESC1 description{};
        check(candidate->GetDesc1(&description), "Get adapter description");
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            std::wstring(description.Description).find(adapterName) == std::wstring::npos) continue;
        check(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&p.device)), "Create D3D12 device");
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
        require(bytes > 1, "Invalid adapter name");
        std::vector<char> utf8(size_t(bytes), '\0');
        require(WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, utf8.data(), bytes, nullptr, nullptr) == bytes,
                "Cannot encode adapter name");
        p.adapter = utf8.data();
        break;
    }
    require(bool(p.device), "Requested hardware adapter not found (software fallback is disabled)");
    if (debugLayer) check(p.device.As(&p.info), "Get validation queue");
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(p.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&p.queue)), "Create graphics queue");
    check(p.device->CreateCommandAllocator(queue.Type, IID_PPV_ARGS(&p.allocator)), "Create allocator");
    check(p.device->CreateCommandList(0, queue.Type, p.allocator.Get(), nullptr, IID_PPV_ARGS(&p.commands)), "Create command list");
    check(p.commands->Close(), "Close initial list");
    check(p.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence)), "Create fence");
    p.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(p.event != nullptr, "Create fence event failed");
    p.rtvHeap = p.makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    p.dsvHeap = p.makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    p.srvHeap = p.makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Impl::MaxTextures, true);
    p.srvStride = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    auto color = textureDescription(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    check(p.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &color,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&p.color)), "Create color target");
    auto depth = textureDescription(width, height, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clearDepth{};
    clearDepth.Format = DXGI_FORMAT_D32_FLOAT;
    clearDepth.DepthStencil.Depth = 1.0f;
    check(p.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &depth,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearDepth, IID_PPV_ARGS(&p.depth)), "Create depth target");
    p.device->CreateRenderTargetView(p.color.Get(), nullptr, p.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    p.device->CreateDepthStencilView(p.depth.Get(), nullptr, p.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 36};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxAnisotropy = 1;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized, errors;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "Serialize root signature");
    check(p.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&p.root)), "Create root signature");
    auto compile = [&](const char* entry, const char* target) {
        ComPtr<ID3DBlob> binary, diagnostics;
        auto hr = D3DCompileFromFile(shader.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                    0, &binary, &diagnostics);
        if (FAILED(hr) && diagnostics) throw std::runtime_error(std::string(
            static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()));
        check(hr, "Compile native shader");
        return binary;
    };
    auto vs = compile("vsMain", "vs_5_0"), ps = compile("psMain", "ps_5_0");
    D3D12_INPUT_ELEMENT_DESC input[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = p.root.Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.InputLayout = {input, 3};
    pipeline.SampleMask = UINT_MAX;
    auto& blend = pipeline.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pipeline.DepthStencilState.StencilReadMask = pipeline.DepthStencilState.StencilWriteMask = 0xff;
    pipeline.DepthStencilState.FrontFace = pipeline.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipeline.SampleDesc.Count = 1;
    check(p.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&p.pipeline)), "Create native mesh pipeline");
}

Renderer::~Renderer() = default;
MeshId Renderer::createMesh(const Mesh& mesh) {
    auto& p = *impl_;
    require(!mesh.vertices.empty() && !mesh.indices.empty() && mesh.indices.size() % 3 == 0, "Invalid triangle mesh");
    require(mesh.vertices.size() <= UINT_MAX / sizeof(Vertex) && mesh.indices.size() <= UINT_MAX / sizeof(uint32_t), "Mesh exceeds D3D12 buffer-view size");
    for (auto index : mesh.indices) require(index < mesh.vertices.size(), "Mesh index outside vertices");
    for (const auto& v : mesh.vertices) {
        for (float f : v.position) require(std::isfinite(f), "Nonfinite position");
        for (float f : v.uv) require(std::isfinite(f), "Nonfinite UV");
        for (float f : v.color) require(std::isfinite(f), "Nonfinite vertex color");
    }
    Impl::GpuMesh gpu;
    gpu.vertices = p.uploadBuffer(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    gpu.indices = p.uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t), D3D12_RESOURCE_STATE_INDEX_BUFFER);
    gpu.vb = {gpu.vertices->GetGPUVirtualAddress(), UINT(mesh.vertices.size() * sizeof(Vertex)), sizeof(Vertex)};
    gpu.ib = {gpu.indices->GetGPUVirtualAddress(), UINT(mesh.indices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
    gpu.indexCount = UINT(mesh.indices.size());
    p.meshes.push_back(std::move(gpu));
    ++p.stats.meshes;
    return UINT(p.meshes.size());
}
TextureId Renderer::createTexture(const Texture& texture) {
    auto& p = *impl_;
    require(texture.width && texture.height && texture.width <= 8192 && texture.height <= 8192 &&
            texture.rgba.size() == uint64_t(texture.width) * texture.height * 4, "Invalid RGBA8 texture");
    require(p.textures.size() < Impl::MaxTextures, "Native texture descriptor heap exhausted");
    auto description = textureDescription(texture.width, texture.height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> gpu;
    check(p.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &description,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu)), "Create albedo texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 size = 0;
    p.device->GetCopyableFootprints(&description, 0, 1, 0, &layout, nullptr, nullptr, &size);
    auto upload = p.makeBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    D3D12_RANGE empty{};
    check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "Map texture upload");
    for (uint32_t y = 0; y < texture.height; ++y)
        std::memcpy(mapped + layout.Offset + size_t(y) * layout.Footprint.RowPitch,
                    texture.rgba.data() + size_t(y) * texture.width * 4, size_t(texture.width) * 4);
    upload->Unmap(0, nullptr);
    p.begin();
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = layout;
    dst.pResource = gpu.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    p.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    p.barrier(gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    p.submit(); p.wait();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = description.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    auto handle = p.srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += p.textures.size() * p.srvStride;
    p.device->CreateShaderResourceView(gpu.Get(), &view, handle);
    p.textures.push_back(std::move(gpu));
    ++p.stats.textures;
    p.stats.assetUploadBytes += texture.rgba.size();
    return UINT(p.textures.size());
}
MaterialId Renderer::createMaterial(const Material& material) {
    auto& p = *impl_;
    require(material.texture && material.texture <= p.textures.size(), "Invalid material texture handle");
    for (float f : material.baseColor) require(std::isfinite(f), "Nonfinite material color");
    p.materials.push_back(material);
    ++p.stats.materials;
    return UINT(p.materials.size());
}
GpuOutput Renderer::render(const SceneSnapshot& scene) {
    auto& p = *impl_;
    require(finite(scene.viewProjection), "Nonfinite camera matrix");
    for (float f : scene.clearColor) require(std::isfinite(f), "Nonfinite clear color");
    // Validate before resetting the command list so rejection leaves it usable.
    for (const auto& instance : scene.instances) {
        require(instance.mesh && instance.mesh <= p.meshes.size(), "Invalid mesh handle");
        require(instance.material && instance.material <= p.materials.size(), "Invalid material handle");
        require(finite(instance.model), "Nonfinite instance matrix");
        for (float f : instance.tint) require(std::isfinite(f), "Nonfinite instance tint");
    }
    p.begin();
    p.barrier(p.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = p.rtvHeap->GetCPUDescriptorHandleForHeapStart(), dsv = p.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    p.commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    p.commands->ClearRenderTargetView(rtv, scene.clearColor.data(), 0, nullptr);
    p.commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(p.width), float(p.height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(p.width), LONG(p.height)};
    p.commands->RSSetViewports(1, &viewport);
    p.commands->RSSetScissorRects(1, &scissor);
    p.commands->SetPipelineState(p.pipeline.Get());
    p.commands->SetGraphicsRootSignature(p.root.Get());
    ID3D12DescriptorHeap* heaps[]{p.srvHeap.Get()};
    p.commands->SetDescriptorHeaps(1, heaps);
    p.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const auto& instance : scene.instances) {
        const auto& mesh = p.meshes[instance.mesh - 1];
        const auto& material = p.materials[instance.material - 1];
        float constants[36];
        std::copy(scene.viewProjection.begin(), scene.viewProjection.end(), constants);
        std::copy(instance.model.begin(), instance.model.end(), constants + 16);
        for (unsigned i = 0; i < 4; ++i) constants[32 + i] = material.baseColor[i] * instance.tint[i];
        p.commands->SetGraphicsRoot32BitConstants(1, 36, constants, 0);
        auto texture = p.srvHeap->GetGPUDescriptorHandleForHeapStart();
        texture.ptr += uint64_t(material.texture - 1) * p.srvStride;
        p.commands->SetGraphicsRootDescriptorTable(0, texture);
        p.commands->IASetVertexBuffers(0, 1, &mesh.vb);
        p.commands->IASetIndexBuffer(&mesh.ib);
        p.commands->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
        ++p.stats.meshDraws;
    }
    p.barrier(p.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    p.submit();
    ++p.stats.renderedFrames;
    return {p.color.Get(), p.fence.Get(), p.submitted};
}
Readback Renderer::readbackForValidation() {
    auto& p = *impl_;
    require(p.stats.renderedFrames != 0, "No native frame to read back");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{};
    UINT64 sizes[2]{};
    ComPtr<ID3D12Resource> buffers[2];
    ID3D12Resource* targets[]{p.color.Get(), p.depth.Get()};
    for (unsigned i = 0; i < 2; ++i) {
        auto description = targets[i]->GetDesc();
        p.device->GetCopyableFootprints(&description, 0, 1, 0, &layouts[i], nullptr, nullptr, &sizes[i]);
        buffers[i] = p.makeBuffer(sizes[i], D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    p.begin();
    D3D12_RESOURCE_STATES states[]{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE};
    for (unsigned i = 0; i < 2; ++i) {
        p.barrier(targets[i], states[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = targets[i]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = buffers[i].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = layouts[i];
        p.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        p.barrier(targets[i], D3D12_RESOURCE_STATE_COPY_SOURCE, states[i]);
    }
    p.submit(); p.wait();
    Readback result{p.width, p.height, std::vector<uint8_t>(size_t(p.width) * p.height * 4),
                    std::vector<float>(size_t(p.width) * p.height)};
    uint8_t* destinations[]{result.rgba.data(), reinterpret_cast<uint8_t*>(result.depth.data())};
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read{0, SIZE_T(sizes[i])}, noWrite{};
        check(buffers[i]->Map(0, &read, reinterpret_cast<void**>(&mapped)), "Map validation readback");
        for (uint32_t y = 0; y < p.height; ++y)
            std::memcpy(destinations[i] + size_t(y) * p.width * 4,
                        mapped + layouts[i].Offset + size_t(y) * layouts[i].Footprint.RowPitch, size_t(p.width) * 4);
        buffers[i]->Unmap(0, &noWrite);
    }
    requireNoValidationErrors();
    return result;
}
const Statistics& Renderer::statistics() const { return impl_->stats; }
std::string Renderer::adapterName() const { return impl_->adapter; }
void Renderer::requireNoValidationErrors() const {
    const auto& p = *impl_;
    if (!p.info) return;
    for (UINT64 i = 0; i < p.info->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        check(p.info->GetMessage(i, nullptr, &size), "Get validation size");
        std::vector<uint8_t> storage(size);
        auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        check(p.info->GetMessage(i, message, &size), "Get validation message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(std::string("D3D12 validation: ") + message->pDescription);
    }
}
} // namespace tfnative
