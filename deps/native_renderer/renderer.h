// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ID3D12Resource;
struct ID3D12Fence;

namespace tfnative {

// These are engine assets and owned scene values, never guest addresses or GIF
// packets. Handles remain valid for the lifetime of their Renderer.
using MeshId = uint32_t;
using TextureId = uint32_t;
using MaterialId = uint32_t;
using Matrix = std::array<float, 16>; // row-major; row-vector multiplication

struct Vertex {
    float position[3];
    float uv[2];
    float color[4];
};
static_assert(sizeof(Vertex) == 36);

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // complete triangle list, not VU-sized pieces
};
struct Texture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // linear RGBA8, already decoded by the asset loader
};
struct Material {
    TextureId texture = 0;
    std::array<float, 4> baseColor{1, 1, 1, 1};
    // First supported pass: opaque, vertex-color modulation, bilinear repeat.
    // Other render states must remain on the enhanced renderer until supported.
};
struct Instance {
    MeshId mesh = 0;
    MaterialId material = 0;
    Matrix model{};
    std::array<float, 4> tint{1, 1, 1, 1};
};
struct SceneSnapshot {
    Matrix viewProjection{};
    std::array<float, 4> clearColor{0.025f, 0.035f, 0.055f, 1};
    std::vector<Instance> instances;
};
struct Statistics {
    uint64_t meshes = 0, textures = 0, materials = 0;
    uint64_t assetUploadBytes = 0, renderedFrames = 0, meshDraws = 0;
};
struct GpuOutput {
    // Borrowed until the next render/readback/destruction. Color is in
    // PIXEL_SHADER_RESOURCE state after readyFence reaches readyValue. A future
    // host integration must explicitly arrange device/resource ownership.
    ID3D12Resource* color = nullptr;
    ID3D12Fence* readyFence = nullptr;
    uint64_t readyValue = 0;
};
struct Readback {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
};

class Renderer {
public:
    // No window, swapchain, emulator, guest memory or GS frontend is created.
    // The probe deliberately requires the requested hardware adapter, no WARP.
    Renderer(uint32_t width, uint32_t height, const std::wstring& adapterName,
             const std::filesystem::path& shader, bool debugLayer);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    MeshId createMesh(const Mesh& mesh);
    TextureId createTexture(const Texture& texture);
    MaterialId createMaterial(const Material& material);
    GpuOutput render(const SceneSnapshot& scene);
    Readback readbackForValidation(); // explicit diagnostic; not the render path
    const Statistics& statistics() const;
    std::string adapterName() const;
    void requireNoValidationErrors() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace tfnative
