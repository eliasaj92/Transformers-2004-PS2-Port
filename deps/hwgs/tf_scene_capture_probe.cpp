// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_scene_capture.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX12/GSTexture12.h"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

extern "C" TF_HWGS_API int tf_gs_scene_capture_probe(const char *prefix, char *error, uint32_t capacity)
{
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        // Keep queued resources alive until the catch path has drained the GPU.
        std::unique_ptr<tf_capture::SceneCapture> capture;
        std::unique_ptr<GSTexture12> texture;
        auto *device =
            g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12 ? GSDevice12::GetInstance() : nullptr;
        try
        {
            if (!device)
                throw std::runtime_error("Scene capture diagnostic requires D3D12");
            constexpr unsigned width = 225, height = 129; // Force padded GPU readback rows.
            texture.reset(
                static_cast<GSTexture12 *>(device->CreateTexture(width, height, 1, GSTexture::Format::Color)));
            if (!texture)
                throw std::runtime_error("Cannot allocate diagnostic texture");
            capture = std::make_unique<tf_capture::SceneCapture>(prefix, 1, 4);
            std::vector<uint8_t> pixels(width * height * 4);
            for (unsigned tick = 0; tick < 6; ++tick)
            {
                const unsigned phase = tick == 3 ? 2 : tick; // One deliberate duplicated picture.
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = 0; x < width; ++x)
                    {
                        auto *p = pixels.data() + (y * width + x) * 4;
                        p[0] = uint8_t(x + phase * 17);
                        p[1] = uint8_t(y + phase * 11);
                        p[2] = uint8_t((x ^ y) + phase * 7);
                        p[3] = 255;
                    }
                if (!texture->Update(GSVector4i(0, 0, width, height), pixels.data(), width * 4))
                    throw std::runtime_error("Cannot upload synthetic scene pixels");
                TfGpuFrame frame{};
                frame.width = width;
                frame.height = height;
                frame.sequence = tick + 100;
                frame.tick = tick;
                frame.scene = 7;
                frame.display_fbp = (tick & 1) * 14;
                capture->record(*device, *texture, frame);
                if (!device->ExecuteCommandList(GSDevice12::WaitType::None))
                    throw std::runtime_error("Cannot submit synthetic scene capture");
            }
            capture->finish(*device);
            passed = true;
        }
        catch (const std::exception &ex)
        {
            if (device)
                device->WaitForGPUIdle();
            if (error && capacity)
                std::snprintf(error, capacity, "%s", ex.what());
        }
    });
    return passed ? 1 : 0;
}
