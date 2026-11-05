// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace gsnative
{
enum class GeometryKind : uint32_t
{
    DualTransform = 0,
    GrassSprites = 1,
    Reflection = 2
};
// Owned companion to one PATH1 packet. Version 1 contains complete raw GIF;
// version 2 resolves only the CPU tail and requires source materialization for
// ordinary fallback. Final EE/GS latches are resolved in both versions.
// No guest pointers cross queues.
struct alignas(16) DualPacket
{
    uint32_t version = 1, vertices = 0, varyingDepth = 0;
    GeometryKind kind = GeometryKind::DualTransform;
    uint64_t ticket = 0, generation = 0;
    std::array<uint8_t, 28 * 32> inputs{};
    std::array<uint8_t, 144> constants{};
};
// Grass companions use eight sprites and the first 256 input bytes for their
// emitter constants/scatter. Version 2 retains sprites 6,7 in the raw GIF.
// The legacy type/API name and 1072-byte queue layout remain compatible.
// Reflection uses at most 22 position/UV/normal source records. Its eleven
// lighting vectors occupy inputs[720..895]; constants hold two matrices,
// draw opacity and restart masks. No guest pointers or ABI size change.
static_assert(sizeof(DualPacket) == 1072 && std::is_trivially_copyable_v<DualPacket>);
using DualPacketPtr = std::shared_ptr<const DualPacket>;

// Scoped only around the existing arbiter callback. Queues explicitly own the
// companion; this is not an asynchronous pointer/hash side channel.
inline thread_local DualPacketPtr current;
struct Scope
{
    DualPacketPtr previous;
    explicit Scope(DualPacketPtr value) : previous(std::move(current)) { current = std::move(value); }
    ~Scope() { current = std::move(previous); }
};
}
