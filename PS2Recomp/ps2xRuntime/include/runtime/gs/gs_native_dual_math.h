// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gs_native_geometry.h"
#include <cfenv>

namespace gsnative
{
// Compile the implementation with strict floating-point semantics. One scope
// covers a batch of CPU fallbacks; the normal GPU draw does not use this path.
class DualFloatScope
{
    fenv_t saved{};
    uint32_t csr = 0;
    bool savedOk = false;

  public:
    bool ready = false;
    DualFloatScope();
    ~DualFloatScope();
    DualFloatScope(const DualFloatScope &) = delete;
    DualFloatScope &operator=(const DualFloatScope &) = delete;
};
struct DualRawVertex
{
    std::array<uint32_t, 3> stq{}, xyz{};
};
// Requires an active DualFloatScope and already validated packet dimensions.
DualRawVertex dualRawVertex(const DualPacket &packet, unsigned vertex, unsigned field);
// Version 2 packets retain source geometry and only the resolved CPU tail.
// Fill their unresolved words before sending them through an ordinary GS path.
void materializeDualPacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes);
} // namespace gsnative
