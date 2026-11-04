// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace gsnative
{
// Owned companion to one PATH1 packet. Version 1 contains complete raw GIF;
// version 2 resolves only the CPU tail and requires source materialization for
// ordinary fallback. Final EE/GS latches are resolved in both versions.
// No guest pointers cross queues.
struct alignas(16) DualPacket
{
    uint32_t version = 1, vertices = 0, varyingDepth = 0, reserved = 0;
    uint64_t ticket = 0, generation = 0;
    std::array<uint8_t, 28 * 32> inputs{};
    std::array<uint8_t, 144> constants{};
};
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
