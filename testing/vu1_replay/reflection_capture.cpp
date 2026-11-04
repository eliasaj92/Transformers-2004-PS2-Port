// SPDX-License-Identifier: GPL-3.0-or-later
// Observe a private accelerated chain without changing its execution.
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

bool reflectionOriginal(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &, unsigned);

namespace
{
std::ofstream file;
uint64_t captured = 0;
std::vector<uint8_t> gpu, raw;
uint64_t vertexIndex = 0, replaced = 0, skipped = 0, overlapping = 0;
constexpr unsigned recordBytes = 32 + 2 * (664 + 16384);
uint32_t word(const void *data)
{
    uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}
std::vector<uint8_t> read(const char *path, size_t limit)
{
    if (!path || !std::filesystem::path(path).is_absolute())
        throw std::runtime_error("Use absolute private reflection input paths");
    const auto size = std::filesystem::file_size(path);
    if (!size || size > limit)
        throw std::runtime_error("Reflection diagnostic input exceeds its limit");
    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(result.data()), result.size());
    if (!input)
        throw std::runtime_error("Cannot read reflection diagnostic input");
    return result;
}
bool sameState(const VU1State &a, const VU1State &b)
{
    const auto *x = reinterpret_cast<const uint8_t *>(&a), *y = reinterpret_cast<const uint8_t *>(&b);
    for (unsigned i = 0; i < sizeof(a); ++i)
    {
        // ABI padding only. Every semantic register, flag and cycle is compared.
        if (i == 638 || i == 639 || (i >= 649 && i <= 651) || i >= 660)
            continue;
        if (x[i] != y[i])
            return false;
    }
    return true;
}
uint64_t fingerprint(const uint8_t *data, size_t bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
void write(const void *data, size_t bytes)
{
    file.write(static_cast<const char *>(data), bytes);
    if (!file)
        throw std::runtime_error("Cannot write reflection capture");
}
} // namespace

void reflectionReplayInitialize()
{
    const char *gpuPath = std::getenv("TF_REFLECTION_GPU_CAPTURE");
    const char *rawPath = std::getenv("TF_REFLECTION_GPU_RAW");
    const char *path = std::getenv("TF_REFLECTION_CAPTURE");
    if (gpuPath || rawPath)
    {
        if (!gpuPath || !rawPath || path)
            throw std::runtime_error("Choose either reflection capture or the GPU tail experiment");
        gpu = read(gpuPath, 48 + 65536 * 48);
        raw = read(rawPath, 10000 * recordBytes);
        if (gpu.size() < 48 || word(gpu.data()) != 0x52464750 || word(gpu.data() + 4) != 1 ||
            word(gpu.data() + 12) != 48 || gpu.size() != 48 + uint64_t(word(gpu.data() + 8)) * 48 ||
            raw.size() % recordBytes)
            throw std::runtime_error("Malformed reflection GPU/raw capture");
        // The Python driver verifies hashes and provenance. Each source memory
        // image is also compared with the actual accelerated entry below.
        return;
    }
    if (!path || !std::filesystem::path(path).is_absolute() || std::filesystem::exists(path))
        throw std::runtime_error("Use a fresh absolute private reflection capture path");
    file.open(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open reflection capture");
}
void reflectionReplayFinish()
{
    if (!captured)
        throw std::runtime_error("No reflection chains in this accelerated capture");
    if (!gpu.empty())
    {
        if (!replaced || 48 + vertexIndex * 48 != gpu.size() || captured * recordBytes != raw.size())
            throw std::runtime_error("Incomplete GPU reflection tail experiment");
        std::fprintf(stderr, "REFLECTION tail calls=%llu replaced=%llu overlapping=%llu skipped_vertices=%llu\n",
                     captured, replaced, overlapping, skipped);
        return;
    }
    file.close();
    if (!file)
        throw std::runtime_error("Cannot close reflection capture");
    std::fprintf(stderr, "REFLECTION captured=%llu\n", captured);
}

bool sc40(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, unsigned relative)
{
    if (relative != 0 || size != 16384 || !v.m_activeMemory || !v.m_activeMemory->vu1Code ||
        v.m_state.pc > 16384 - 1248 ||
        fingerprint(v.m_activeMemory->vu1Code + v.m_state.pc, 1248) != 0xf72677cdf8f26fdcull)
        throw std::runtime_error("Unexpected reflection entry or program identity");
    if (captured >= 10000)
        throw std::runtime_error("Reflection capture limit exceeded");
    static_assert(sizeof(VU1State) == 664 && offsetof(VU1State, vi) == 512);
    const uint32_t metadata[4] = {0x5245464cu, 1, uint32_t(captured), size};
    const auto before = v.m_state;
    const uint64_t cycle = v.m_cycle;
    const unsigned target = (uint16_t(v.m_state.vi[15]) * 8u) & 16383u;
    std::array<uint8_t, 16384> input;
    std::memcpy(input.data(), data, size);
    if (!gpu.empty())
    {
        const unsigned n = uint16_t(v.m_state.vi[10]), base = uint16_t(v.m_state.vi[1]);
        const uint64_t record = captured++;
        if (!n || n > 28 || base + 3 * n + 14 > 1024 || (record + 1) * recordBytes > raw.size() ||
            std::memcmp(raw.data() + record * recordBytes, metadata, sizeof(metadata)) ||
            std::memcmp(raw.data() + record * recordBytes + 32 + 664, data, size))
            throw std::runtime_error("Reflection tail differs from its authenticated GPU source");
        const auto *output = gpu.data() + 48 + vertexIndex * 48;
        vertexIndex += n;
        if (48 + vertexIndex * 48 > gpu.size())
            throw std::runtime_error("Reflection tail exhausted the GPU output");
        if (n <= 4 || v.m_xgkick.active)
        {
            overlapping += v.m_xgkick.active;
            return reflectionOriginal(v, data, size, end, ended, relative);
        }
        static auto candidate = std::make_unique<StaticVU>();
        static std::array<uint8_t, 16384> scratch;
        *candidate = v;
        scratch = input;
        auto &tail = *candidate;
        tail.m_activeVuData = scratch.data();
        tail.advanceTo(tail.m_cycle + (n - 4) * 41u);
        tail.m_state.vi[1] = int16_t(base + (n - 4) * 3u);
        tail.m_state.vi[10] = 4;
        bool tailEnded = false;
        if (!reflectionOriginal(tail, scratch.data(), size, end, tailEnded, relative) || tailEnded)
            throw std::runtime_error("Accelerated reflection tail declined");
        tail.m_state.vi[10] = n;
        // Offline only: inject actual shader results from the qualified capture.
        // A live path must retain these vertices on the GPU and defer VU memory.
        for (unsigned i = 0; i < n - 4; ++i)
            std::memcpy(scratch.data() + (base + i * 3u) * 16, output + i * 48, 48);
        const bool result = reflectionOriginal(v, data, size, end, ended, relative);
        if (!result || ended || v.m_state.pc != target || !sameState(v.m_state, tail.m_state) ||
            v.m_cycle != tail.m_cycle || std::memcmp(scratch.data(), data, size))
            throw std::runtime_error("Reflection tail changed retained state, cycles or restored memory");
        // Carry the candidate's real pending pipeline state through the rest of
        // the call. The outer replay still checks all memory and ordered GIF.
        v = tail;
        v.m_activeVuData = data;
        ++replaced;
        skipped += n - 4;
        return result;
    }
    const bool result = reflectionOriginal(v, data, size, end, ended, relative);
    if (!result || ended || v.m_state.pc != target || v.m_stopRequested)
        throw std::runtime_error("Reflection capture has unfinished accelerated execution");
    const uint64_t timing[2] = {cycle, v.m_cycle};
    write(metadata, sizeof(metadata));
    write(timing, sizeof(timing));
    write(&before, sizeof(before));
    write(input.data(), input.size());
    write(&v.m_state, sizeof(v.m_state));
    write(data, size);
    ++captured;
    return result;
}
