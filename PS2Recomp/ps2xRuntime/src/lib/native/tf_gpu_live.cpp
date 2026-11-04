// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_live.h"
#include "runtime/ps2_vu1.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace tf_gpu_live
{
namespace
{
uint32_t word(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t wide(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
void put(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
uint64_t hash(const uint8_t *p, unsigned bytes)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
std::atomic<uint64_t> selected{0}, attached{0}, declined{0};
struct Pending
{
    std::shared_ptr<gsnative::DualPacket> packet;
    bool emitted = false;
};
thread_local Pending pending;
struct Identity
{
    const void *owner = nullptr;
    uint64_t generation = 0;
    uint32_t dual = 0, clip = 0;
    bool valid = false;
};
thread_local Identity identity;
}
void install(uint32_t crc)
{
    const char *p = std::getenv("PS2X_TF_GPU_DUAL");
    if (!p || !*p || !std::strcmp(p, "0")) return;
    if (std::strcmp(p, "1") || crc != 0x74c982c0 || enabled)
        throw std::runtime_error("PS2X_TF_GPU_DUAL=1 requires the supported SLUS-20668 executable");
    enabled = true;
    std::fprintf(stderr, "[gpu-dual] ordered live renderer qualification enabled; static VU still produces fallback state\n");
}
bool begin(uint32_t pc, bool continuing, uint32_t itop, uint64_t generation,
           const void *owner, const uint8_t *code, const uint8_t *data, const VU1State &state)
{
    if (!continuing || (pc != 0x10 && pc != 0xf8) || word(data + 64) != 2 || itop < 3 || itop > 1000)
        return false;
    const uint32_t count = word(data + itop * 16) & 0x7fff, context = word(data + 84);
    if (count < 3 || count > 28 || itop + 6 * count + 5 > 1023 || context > 1009 ||
        word(data + itop * 16 + 4) != 3 || wide(data + 21 * 16) || wide(data + 21 * 16 + 8))
        return false;
    const uint32_t dual = word(data + 80), clip = word(data + 96);
    if (dual > (16384 - 1712) / 8 || clip > (16384 - 3888) / 8) return false;
    if (identity.owner != owner || identity.generation != generation || identity.dual != dual || identity.clip != clip)
    {
        identity = {owner, generation, dual, clip,
            hash(code, 1104) == 0xf84e7059a7f93d26ull && hash(code + dual * 8, 1712) == 0xd5cdafb0be7f3d85ull &&
            hash(code + clip * 8, 3888) == 0xeea480a125c824a3ull};
    }
    if (!identity.valid) return false;
    if (pending.packet) throw std::runtime_error("Nested GPU geometry qualification");
    auto p = std::make_shared<gsnative::DualPacket>();
    p->vertices = count;
    p->generation = generation;
    std::memcpy(p->constants.data(), state.vf[1], 64);
    std::memcpy(p->constants.data() + 64, data + context * 16, 64);
    std::memcpy(p->constants.data() + 128, data + itop * 16 + 8, 4);
    std::memcpy(p->constants.data() + 132, data + (itop + 3 * count + 4) * 16, 8);
    for (unsigned i = 0; i < 33; ++i)
        if (!std::isfinite(std::bit_cast<float>(word(p->constants.data() + 4 * i)))) { ++declined; return false; }
    for (unsigned i = 0; i < count; ++i)
    {
        const auto *source = data + (itop + 1 + 3 * i) * 16;
        auto *input = p->inputs.data() + i * 32;
        std::memcpy(input, source + 32, 12);
        std::memcpy(input + 16, source, 12);
        uint32_t color = 0;
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            const uint32_t c = word(source + 16 + lane * 4);
            if (c > 255) { ++declined; return false; }
            color |= c << (lane * 8);
        }
        put(input + 28, color);
        for (unsigned lane = 0; lane < 3; ++lane)
            if (!std::isfinite(std::bit_cast<float>(word(input + lane * 4))) ||
                !std::isfinite(std::bit_cast<float>(word(input + 16 + lane * 4)))) { ++declined; return false; }
    }
    p->ticket = ++selected;
    pending = {std::move(p), false};
    return true;
}
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes)
{
    if (!pending.packet || pending.emitted) return {};
    pending.emitted = true;
    const auto &p = pending.packet;
    const unsigned count = p->vertices, fieldBytes = (4 + 3 * count) * 16;
    if (path != 1 || bytes != 2 * fieldBytes) { ++declined; return {}; }
    for (unsigned field = 0; field < 2; ++field)
    {
        const uint8_t *f = data + field * fieldBytes;
        const uint64_t state = wide(f), tag = wide(f + 48);
        if ((state & 32767) != 2 || (state >> 58 & 3) || (state >> 60) != 1 || wide(f + 8) != 14 ||
            (tag & 32767) != count || (tag >> 58 & 3) || (tag >> 60) != 3 || wide(f + 56) != 0x412 ||
            (tag >> 47 & 7) != 4 || !(tag & (1ull << 46))) { ++declined; return {}; }
        uint32_t firstDepth = 0;
        bool haveDepth = false, varies = false;
        for (unsigned i = 0; i < count; ++i)
        {
            const uint8_t *v = f + 64 + i * 48;
            uint32_t rgba = 0;
            for (unsigned lane = 0; lane < 4; ++lane) rgba |= (word(v + 16 + lane * 4) & 255) << (lane * 8);
            if (rgba != word(p->inputs.data() + i * 32 + 28)) { ++declined; return {}; }
            const uint32_t mask = word(p->constants.data() + 132 + (i / 16) * 4);
            const bool skip = ((mask << (i % 16)) & 32768) != 0;
            if (((word(v + 44) >> 15) & 1) != skip || (i < 2 && !skip)) { ++declined; return {}; }
            if (i < 2 || skip) continue;
            for (unsigned corner = i - 2; corner <= i; ++corner)
            {
                const uint32_t z = (word(f + 64 + corner * 48 + 40) >> 4) & 0xffffff;
                if (!haveDepth) { firstDepth = z; haveDepth = true; }
                else varies |= z != firstDepth;
            }
        }
        if (varies) p->varyingDepth |= 1u << field;
    }
    ++attached;
    return p;
}
void end(bool transferred, bool fellBack)
{
    if (!pending.packet) return;
    if (transferred || fellBack || !pending.emitted)
        throw std::runtime_error("Qualified GPU geometry call changed its accelerated control flow");
    pending = {};
}
void finish()
{
    if (enabled) std::fprintf(stderr, "[gpu-dual] selected=%llu attached=%llu declined=%llu; VU bypass=0\n",
        static_cast<unsigned long long>(selected.load()), static_cast<unsigned long long>(attached.load()),
        static_cast<unsigned long long>(declined.load()));
}
}
