// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_scene_capture.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX12/GSTexture12.h"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace tf_capture
{
namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
FILE *create(const std::filesystem::path &path)
{
    FILE *file = nullptr;
    require(!_wfopen_s(&file, path.c_str(), L"wbx") && file, "Cannot create fresh private scene capture");
    return file;
}
} // namespace

struct SceneCapture::State
{
    struct Row
    {
        TfGpuFrame frame;
        uint64_t hostUs;
    };
    std::filesystem::path prefix;
    uint64_t first, last, stride = 0, bytes = 0;
    uint32_t width = 0, height = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    wil::com_ptr_nothrow<ID3D12Resource> buffer;
    std::vector<Row> rows;
    bool finished = false;
};

SceneCapture::SceneCapture(const char *prefix, uint64_t firstTick, uint64_t lastTick) : state(std::make_unique<State>())
{
    require(prefix && *prefix && lastTick >= firstTick && lastTick - firstTick < 64,
            "Scene capture requires a private prefix and at most 64 consecutive fields");
    state->prefix = std::filesystem::u8path(prefix);
    require(state->prefix.is_absolute() && std::filesystem::is_directory(state->prefix.parent_path()),
            "Scene capture requires an existing absolute private directory");
    require(!std::filesystem::exists(state->prefix.wstring() + L".rgba") &&
                !std::filesystem::exists(state->prefix.wstring() + L".json"),
            "Scene capture output already exists");
    state->first = firstTick;
    state->last = lastTick;
    state->rows.reserve(size_t(lastTick - firstTick + 1));
    std::fprintf(stderr, "[hwgs:scene-capture] fields=%llu:%llu diagnostic readback; not a performance measurement\n",
                 firstTick, lastTick);
}
SceneCapture::~SceneCapture() = default;

void SceneCapture::record(GSDevice12 &device, GSTexture12 &texture, const TfGpuFrame &frame)
{
    auto &s = *state;
    if (frame.tick < s.first || frame.tick > s.last)
        return;
    require(!s.finished && frame.tick == s.first + s.rows.size(), "Scene capture missed or repeated a field");
    require(s.rows.empty() || frame.sequence > s.rows.back().frame.sequence,
            "Scene capture publication sequence did not advance");
    require(frame.width && frame.height && frame.width == uint32_t(texture.GetWidth()) &&
                frame.height == uint32_t(texture.GetHeight()) && texture.GetDXGIFormat() == DXGI_FORMAT_R8G8B8A8_UNORM,
            "Unsupported scene capture texture");
    if (!s.buffer)
    {
        s.width = frame.width;
        s.height = frame.height;
        auto desc = texture.GetResource()->GetDesc();
        require(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1,
                "Scene capture requires a resolved 2D output");
        desc.Width = s.width;
        desc.Height = s.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        uint64_t size = 0;
        device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &s.footprint, nullptr, nullptr, &size);
        s.stride =
            (size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        s.bytes = s.stride * (s.last - s.first + 1);
        require(s.stride && s.bytes <= 1536ull * 1024 * 1024, "Scene capture exceeds its bounded readback budget");
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = s.bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        require(SUCCEEDED(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                      IID_PPV_ARGS(s.buffer.put()))),
                "Cannot allocate scene capture readback");
    }
    require(frame.width == s.width && frame.height == s.height, "Scene capture dimensions changed within the window");
    device.EndRenderPass();
    texture.CommitClear();
    const auto oldState = texture.GetResourceState();
    texture.TransitionToState(GSTexture12::ResourceState::CopySrc);
    texture.SetUseFenceCounter(device.GetCurrentFenceValue());
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = texture.GetResource();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = s.buffer.get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = s.footprint;
    destination.PlacedFootprint.Offset = s.rows.size() * s.stride;
    const D3D12_BOX area{0, 0, 0, s.width, s.height, 1};
    device.GetCommandList().list4->CopyTextureRegion(&destination, 0, 0, 0, &source, &area);
    texture.TransitionToState(oldState);
    const auto now =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    s.rows.push_back({frame, uint64_t(now)});
}

void SceneCapture::finish(GSDevice12 &device)
{
    auto &s = *state;
    if (s.finished)
        return;
    // An incomplete diagnostic also retires queued copies before error cleanup.
    device.WaitForGPUIdle();
    require(s.buffer && s.rows.size() == s.last - s.first + 1, "Scene capture did not cover its complete field window");
    const D3D12_RANGE range{0, size_t(s.bytes)};
    void *mapped = nullptr;
    require(SUCCEEDED(s.buffer->Map(0, &range, &mapped)), "Cannot map completed scene capture");
    FILE *raw = nullptr;
    try
    {
        raw = create(s.prefix.wstring() + L".rgba");
    }
    catch (...)
    {
        const D3D12_RANGE empty{};
        s.buffer->Unmap(0, &empty);
        throw;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < s.rows.size(); ++i)
        for (uint32_t y = 0; ok && y < s.height; ++y)
        {
            const auto *row = static_cast<const uint8_t *>(mapped) + i * s.stride + y * s.footprint.Footprint.RowPitch;
            ok = std::fwrite(row, 4, s.width, raw) == s.width;
        }
    const D3D12_RANGE empty{};
    s.buffer->Unmap(0, &empty);
    ok &= std::fclose(raw) == 0;
    require(ok, "Cannot write complete scene capture pixels");
    FILE *meta = create(s.prefix.wstring() + L".json");
    ok = std::fprintf(meta,
                      "{\n  \"version\": 1, \"format\": \"RGBA8\", \"width\": %u, \"height\": %u,\n"
                      "  \"scope\": \"Complete GPU output images before host scaling; diagnostic timing only\",\n"
                      "  \"frames\": [\n",
                      s.width, s.height) >= 0;
    for (size_t i = 0; i < s.rows.size(); ++i)
    {
        const auto &r = s.rows[i];
        ok &= std::fprintf(meta,
                           "    {\"index\": %zu, \"sequence\": %llu, \"tick\": %llu, \"scene\": %u, \"display_fbp\": "
                           "%u, \"host_us\": %llu}%s\n",
                           i, r.frame.sequence, r.frame.tick, r.frame.scene, r.frame.display_fbp, r.hostUs,
                           i + 1 == s.rows.size() ? "" : ",") >= 0;
    }
    ok &= std::fputs("  ]\n}\n", meta) >= 0;
    ok &= std::fclose(meta) == 0;
    require(ok, "Cannot write complete scene capture metadata");
    std::fprintf(stderr, "[hwgs:scene-capture] frames=%zu image=%ux%u bytes=%llu complete=1\n", s.rows.size(), s.width,
                 s.height, uint64_t(s.rows.size()) * s.width * s.height * 4);
    s.finished = true;
    s.buffer.reset();
}

namespace
{
std::unique_ptr<SceneCapture> fromEnvironment()
{
    const char *prefix = std::getenv("PS2X_TF_SCENE_CAPTURE");
    if (!prefix || !*prefix)
        return {};
    const char *range = std::getenv("PS2X_TF_SCENE_RANGE");
    require(range && *range, "PS2X_TF_SCENE_CAPTURE requires PS2X_TF_SCENE_RANGE=first:last");
    const std::string_view value(range);
    const auto colon = value.find(':');
    require(colon != value.npos, "Invalid scene capture field range");
    uint64_t first = 0, last = 0;
    const auto left = std::from_chars(value.data(), value.data() + colon, first);
    const auto right = std::from_chars(value.data() + colon + 1, value.data() + value.size(), last);
    require(left.ec == std::errc{} && left.ptr == value.data() + colon && right.ec == std::errc{} &&
                right.ptr == value.data() + value.size(),
            "Invalid scene capture field range");
    return std::make_unique<SceneCapture>(prefix, first, last);
}
std::unique_ptr<SceneCapture> &capture()
{
    static auto value = fromEnvironment();
    return value;
}
} // namespace
void record(GSDevice12 &device, GSTexture12 &texture, const TfGpuFrame &frame)
{
    if (auto &value = capture())
        value->record(device, texture, frame);
}
void finish(GSDevice12 &device)
{
    if (auto &value = capture())
    {
        try
        {
            value->finish(device);
        }
        catch (...)
        {
            value.reset();
            throw;
        }
        value.reset();
    }
}
} // namespace tf_capture
