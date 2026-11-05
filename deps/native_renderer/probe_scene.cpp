// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe_scene.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace tfnative;
using namespace DirectX;
namespace {
struct Asset {
    Mesh mesh;
    Texture texture;
};
Asset load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open native asset");
    const auto size = input.tellg();
    input.seekg(0);
    char magic[8]{};
    uint32_t header[6]{};
    input.read(magic, 8).read(reinterpret_cast<char*>(header), sizeof(header));
    const auto [vertices, indices, width, height, flags, reserved] =
        std::array<uint32_t, 6>{header[0], header[1], header[2], header[3], header[4], header[5]};
    const uint64_t expected = 32 + uint64_t(vertices) * sizeof(Vertex) + uint64_t(indices) * 4 + uint64_t(width) * height * 4;
    if (!input || std::memcmp(magic, "TFNMESH1", 8) || vertices == 0 || vertices > 1000000 ||
        indices == 0 || indices > 3000000 || indices % 3 || width == 0 || height == 0 ||
        width > 8192 || height > 8192 || flags || reserved || uint64_t(size) != expected)
        throw std::runtime_error("Invalid or unsupported native asset");
    Asset asset;
    asset.mesh.vertices.resize(vertices); asset.mesh.indices.resize(indices);
    asset.texture = {width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    input.read(reinterpret_cast<char*>(asset.mesh.vertices.data()), vertices * sizeof(Vertex));
    input.read(reinterpret_cast<char*>(asset.mesh.indices.data()), indices * 4);
    input.read(reinterpret_cast<char*>(asset.texture.rgba.data()), asset.texture.rgba.size());
    if (!input) throw std::runtime_error("Truncated native asset");
    return asset;
}
Matrix matrix(FXMMATRIX value) {
    XMFLOAT4X4 stored;
    XMStoreFloat4x4(&stored, value);
    Matrix result;
    std::memcpy(result.data(), &stored, sizeof(stored));
    return result;
}
template <typename Container> void array(std::ostream& out, const Container& values) {
    out << '[';
    bool first = true;
    for (auto value : values) { if (!first) out << ','; out << value; first = false; }
    out << ']';
}
void write(const std::filesystem::path& path, const void* bytes, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(bytes), size);
    if (!out) throw std::runtime_error("Cannot write validation output");
}
} // namespace

void tfnative::runMeshProbe(Renderer& renderer, const std::filesystem::path& assetPath,
                             const std::filesystem::path& output) {
    if (std::filesystem::exists(output)) throw std::runtime_error("Use a fresh validation directory");
    auto asset = load(assetPath);
    constexpr uint32_t Width = 1920, Height = 1080;
    const auto mesh = renderer.createMesh(asset.mesh);
    const auto texture = renderer.createTexture(asset.texture);
    const auto material = renderer.createMaterial({texture});

    // The native API rejects malformed handles/indices before changing GPU state.
    unsigned rejections = 0;
    Mesh invalid = asset.mesh;
    invalid.indices[0] = UINT32_MAX;
    try { renderer.createMesh(invalid); } catch (const std::runtime_error&) { ++rejections; }
    try { renderer.createMaterial({0}); } catch (const std::runtime_error&) { ++rejections; }
    SceneSnapshot invalidScene;
    invalidScene.viewProjection = matrix(XMMatrixIdentity());
    invalidScene.instances.push_back({0, material, matrix(XMMatrixIdentity())});
    try { renderer.render(invalidScene); } catch (const std::runtime_error&) { ++rejections; }
    if (rejections != 3) throw std::runtime_error("Invalid asset/scene input was accepted");

    XMVECTOR low = XMVectorReplicate(1.0e30f), high = XMVectorReplicate(-1.0e30f);
    for (const auto& vertex : asset.mesh.vertices) {
        auto position = XMVectorSet(vertex.position[0], vertex.position[1], vertex.position[2], 0);
        low = XMVectorMin(low, position); high = XMVectorMax(high, position);
    }
    auto center = XMVectorScale(XMVectorAdd(low, high), 0.5f);
    const float radius = XMVectorGetX(XMVector3Length(XMVectorSubtract(high, low))) * 0.5f;
    if (!(radius > 0 && std::isfinite(radius))) throw std::runtime_error("Invalid mesh bounds");
    auto recenter = XMMatrixTranslationFromVector(XMVectorNegate(center));
    // Destroy all caller-owned asset data before drawing. Only resource handles
    // and transforms survive; no frame can re-import or read the source mesh.
    asset = {}; invalid = {};
    const auto uploadBytes = renderer.statistics().assetUploadBytes;
    std::filesystem::create_directories(output);
    std::ofstream report(output / "probe.json");
    report << std::setprecision(9) << "{\n\"adapter\":\"" << renderer.adapterName() << "\","
           << "\n\"width\":" << Width << ",\"height\":" << Height
           << ",\"debug_layer\":" << (renderer.validationEnabled() ? "true" : "false")
           << ",\"host_device\":" << (renderer.usesHostDevice() ? "true" : "false")
           << ",\"invalid_inputs_rejected\":" << rejections << ",\n\"frames\":[\n";
    const char* names[]{"asset", "camera_and_model", "depth_overlap", "near_clip"};
    for (unsigned frame = 0; frame < 4; ++frame) {
        SceneSnapshot scene;
        auto eye = XMVectorSet(1.65f * radius, -2.0f * radius, 1.4f * radius, 0);
        if (frame == 1) eye = XMVectorSet(-1.9f * radius, -1.8f * radius, 0.9f * radius, 0);
        const float nearPlane = frame == 3 ? XMVectorGetX(XMVector3Length(eye)) - 0.1f * radius : 0.1f * radius;
        auto view = XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0, 0, 1, 0));
        auto projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(42.0f), float(Width) / Height, nearPlane, 20 * radius);
        scene.viewProjection = matrix(view * projection);
        auto model = recenter;
        if (frame == 1) model = model * XMMatrixRotationZ(0.65f) * XMMatrixTranslation(0, 0.1f * radius, 0);
        scene.instances.push_back({mesh, material, matrix(model)});
        if (frame == 2) {
            // Submit the farther, red-tinted copy LAST. It must still be hidden
            // behind the original where their projected surfaces overlap.
            auto away = XMVectorScale(XMVector3Normalize(XMVectorNegate(eye)), 0.8f * radius);
            away = XMVectorAdd(away, XMVectorSet(0.3f * radius, 0, 0, 0));
            scene.instances.push_back({mesh, material, matrix(recenter * XMMatrixTranslationFromVector(away)), {1, 0.35f, 0.3f, 1}});
        }
        const auto gpu = renderer.render(scene);
        if (!gpu.color || !gpu.readyFence || !gpu.readyValue) throw std::runtime_error("Missing GPU output");
        auto pixels = renderer.readbackForValidation();
        write(output / (std::string(names[frame]) + ".rgba"), pixels.rgba.data(), pixels.rgba.size());
        write(output / (std::string(names[frame]) + ".depth32f"), pixels.depth.data(), pixels.depth.size() * sizeof(float));
        if (renderer.statistics().assetUploadBytes != uploadBytes) throw std::runtime_error("Frame re-uploaded an asset");
        if (frame) report << ",\n";
        report << "{\"name\":\"" << names[frame] << "\",\"view_projection\":";
        array(report, scene.viewProjection);
        report << ",\"clear\":"; array(report, scene.clearColor);
        report << ",\"instances\":[";
        for (size_t i = 0; i < scene.instances.size(); ++i) {
            if (i) report << ',';
            report << "{\"model\":"; array(report, scene.instances[i].model);
            report << ",\"tint\":"; array(report, scene.instances[i].tint); report << '}';
        }
        report << "],\"asset_upload_bytes_after_frame\":" << renderer.statistics().assetUploadBytes << '}';
    }
    renderer.requireNoValidationErrors();
    const auto& stats = renderer.statistics();
    report << "\n],\"statistics\":{\"meshes\":" << stats.meshes << ",\"textures\":" << stats.textures
           << ",\"materials\":" << stats.materials << ",\"asset_upload_bytes\":" << stats.assetUploadBytes
           << ",\"rendered_frames\":" << stats.renderedFrames << ",\"mesh_draws\":" << stats.meshDraws << "}}\n";
    report.close();
    if (!report) throw std::runtime_error("Cannot write probe report");
    std::cout << "Native D3D12 mesh probe: " << renderer.adapterName() << ", 1920x1080, "
              << stats.renderedFrames << " diagnostic frames, " << stats.meshDraws
              << " complete-mesh draws, " << uploadBytes << " bytes uploaded once.\n";
}
