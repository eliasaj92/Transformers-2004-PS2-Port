// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_hwgs.h"
#include <cstdint>
#include <memory>

class GSDevice12;
class GSTexture12;

namespace tf_capture
{
// Bounded diagnostic capture. Copies complete output images into a readback
// buffer on the GPU; maps/writes them only after the capture has ended.
// This is separate from ordinary direct presentation and never re-uploads pixels.
class SceneCapture
{
  public:
    SceneCapture(const char *prefix, uint64_t firstTick, uint64_t lastTick);
    ~SceneCapture();
    void record(GSDevice12 &device, GSTexture12 &texture, const TfGpuFrame &frame);
    void finish(GSDevice12 &device);

  private:
    struct State;
    std::unique_ptr<State> state;
};
void record(GSDevice12 &device, GSTexture12 &texture, const TfGpuFrame &frame);
void finish(GSDevice12 &device);
} // namespace tf_capture
