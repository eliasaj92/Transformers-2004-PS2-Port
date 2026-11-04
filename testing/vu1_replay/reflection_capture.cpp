// SPDX-License-Identifier: GPL-3.0-or-later
// Capture private accelerated boundaries or qualify paired CPU tails. The
// replacement experiment uses authenticated offline GPU output, never live readback.
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

bool reflectionOriginal(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &, unsigned);
bool reflectionProjectionOriginal(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &, unsigned);
bool sr798(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &);
bool sr801(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &);

namespace
{
std::ofstream file;
std::ofstream projectionFile;
uint64_t captured = 0;
uint64_t projected = 0;
std::vector<uint8_t> projectionGpu, projectionRaw, referencePackets, candidatePackets;
uint64_t projectionVertex = 0, projectionReplaced = 0, projectionSkipped = 0;
uint64_t poisonedVertices = 0;
unsigned projectionMode = 0, projectionBase = 0, projectionCount = 0, projectionSkip = 0;
unsigned deferredReflectionBase = 0, deferredReflectionCount = 0;
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
    const char *projectionGpuPath = std::getenv("TF_REFLECTION_PROJECT_GPU");
    const char *projectionRawPath = std::getenv("TF_REFLECTION_PROJECT_RAW");
    if (projectionGpuPath || projectionRawPath)
    {
        if (!projectionGpuPath || !projectionRawPath || std::getenv("TF_REFLECTION_PROJECT_CAPTURE") ||
            !std::getenv("TF_REFLECTION_GPU_CAPTURE"))
            throw std::runtime_error("Projection tail requires paired GPU inputs and the reflection tail mode");
        projectionGpu = read(projectionGpuPath, 48 + 65536 * 320);
        projectionRaw = read(projectionRawPath, 10000 * recordBytes);
        if (projectionGpu.size() < 48 || word(projectionGpu.data()) != 0x52504750 ||
            word(projectionGpu.data() + 4) != 1 || word(projectionGpu.data() + 12) != 320 ||
            projectionGpu.size() != 48 + uint64_t(word(projectionGpu.data() + 8)) * 320 ||
            projectionRaw.size() % recordBytes)
            throw std::runtime_error("Malformed reflection projection GPU/raw capture");
    }
    if (const char *projectionPath = std::getenv("TF_REFLECTION_PROJECT_CAPTURE"))
    {
        if (!std::filesystem::path(projectionPath).is_absolute() || std::filesystem::exists(projectionPath))
            throw std::runtime_error("Use a fresh absolute private reflection projection path");
        projectionFile.open(projectionPath, std::ios::binary);
        if (!projectionFile)
            throw std::runtime_error("Cannot open reflection projection capture");
    }
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
    if (!projectionGpu.empty())
    {
        if (!projectionReplaced || !poisonedVertices || deferredReflectionCount || projected != captured ||
            projected * recordBytes != projectionRaw.size() || 48 + projectionVertex * 320 != projectionGpu.size())
            throw std::runtime_error("Incomplete paired reflection projection tail experiment");
        std::fprintf(
            stderr,
            "REFLECTION projection_tail calls=%llu replaced=%llu skipped_vertices=%llu poisoned_vertices=%llu\n",
            projected, projectionReplaced, projectionSkipped, poisonedVertices);
    }
    if (projectionFile.is_open())
    {
        projectionFile.close();
        if (!projectionFile || projected != captured)
            throw std::runtime_error("Incomplete paired reflection projection capture");
        std::fprintf(stderr, "REFLECTION projected=%llu\n", projected);
    }
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
        if (n <= 4 || v.m_xgkick.active || (!projectionGpu.empty() && (n == 17 || n == 18 || word(data + 21 * 16))))
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
        if (!projectionGpu.empty())
        {
            // Catch accidental reads of skipped lighting work by the manager
            // or projection tail. The ordinary accelerated shadow below gets
            // the authenticated GPU values back; the candidate stays poisoned
            // until the projection packet is restored at its final boundary.
            deferredReflectionBase = base;
            deferredReflectionCount = n - 4;
            std::memset(data + base * 16, 0xcd, deferredReflectionCount * 48);
            poisonedVertices += deferredReflectionCount;
        }
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

bool sc46(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, unsigned relative)
{
    if (!projectionFile.is_open() && projectionGpu.empty())
        return reflectionProjectionOriginal(v, data, size, end, ended, relative);
    if (relative != 0 || size != 16384 || !v.m_activeMemory || !v.m_activeMemory->vu1Code ||
        v.m_state.pc > 16384 - 1728 || projected + 1 != captured ||
        fingerprint(v.m_activeMemory->vu1Code + v.m_state.pc, 1728) != 0x6ca5df87990a9276ull)
        throw std::runtime_error("Unexpected reflection projection entry or program identity");
    const uint32_t metadata[4] = {0x5250524au, 1, uint32_t(projected), size};
    const auto before = v.m_state;
    const uint64_t cycle = v.m_cycle;
    const unsigned target = (uint16_t(v.m_state.vi[15]) * 8u) & 16383u;
    std::array<uint8_t, 16384> input;
    std::memcpy(input.data(), data, size);
    if (!projectionGpu.empty())
    {
        if ((projected + 1) * recordBytes > projectionRaw.size() ||
            std::memcmp(projectionRaw.data() + projected * recordBytes, metadata, sizeof(metadata)))
            throw std::runtime_error("Projection tail differs from its authenticated GPU source");
        const auto *expectedInput = projectionRaw.data() + projected * recordBytes + 32 + 664;
        const unsigned deferredStart = deferredReflectionBase * 16;
        const unsigned deferredEnd = deferredStart + deferredReflectionCount * 48;
        for (unsigned i = 0; i < size; ++i)
            if (data[i] != expectedInput[i] && !(deferredReflectionCount && i >= deferredStart && i < deferredEnd))
                throw std::runtime_error("Projection input changed outside deferred reflection geometry");
        VU1State expectedState;
        std::memcpy(&expectedState, projectionRaw.data() + projected * recordBytes + 32, sizeof(expectedState));
        if (!sameState(v.m_state, expectedState))
            throw std::runtime_error("Deferred reflection changed the manager's projection entry state");
        const unsigned n = uint16_t(v.m_state.vi[10]), base = uint16_t(v.m_state.vi[1]);
        if (!n || n > 28 || base < 4 || base - 4 + (4 + 3 * n) * 4 > 1024 ||
            48 + (projectionVertex + n) * 320 > projectionGpu.size())
            throw std::runtime_error("Projection tail source range is outside qualification");
        ++projected;
        if (n <= 4 || n == 17 || n == 18 || v.m_xgkick.active || word(data + 21 * 16))
        {
            if (deferredReflectionCount)
                throw std::runtime_error("Deferred reflection reached an unsupported projection path");
            projectionVertex += n;
            return reflectionProjectionOriginal(v, data, size, end, ended, relative);
        }
        static auto candidate = std::make_unique<StaticVU>();
        static std::array<uint8_t, 16384> scratch;
        *candidate = v;
        scratch = input;
        auto &tail = *candidate;
        tail.m_activeVuData = scratch.data();
        projectionBase = base;
        projectionCount = n;
        projectionSkip = 0;
        referencePackets.clear();
        candidatePackets.clear();
        projectionMode = 2;
        bool tailEnded = false;
        if (!reflectionProjectionOriginal(tail, scratch.data(), size, end, tailEnded, relative) || tailEnded)
            throw std::runtime_error("Accelerated reflection projection tail declined");
        projectionMode = 1;
        if (deferredReflectionCount)
        {
            const auto *lighting = gpu.data() + 48 + projectionVertex * 48;
            if (base != deferredReflectionBase || deferredReflectionCount != n - 4 ||
                std::memcmp(expectedInput + deferredStart, lighting, deferredReflectionCount * 48))
                throw std::runtime_error("Deferred reflection does not match its paired GPU lighting result");
            std::memcpy(data + deferredStart, lighting, deferredReflectionCount * 48);
        }
        const bool result = reflectionProjectionOriginal(v, data, size, end, ended, relative);
        projectionMode = 0;
        if (!result || ended || v.m_state.pc != target || !sameState(v.m_state, tail.m_state) ||
            v.m_cycle != tail.m_cycle || std::memcmp(scratch.data(), data, size) ||
            referencePackets != candidatePackets)
        {
            std::fprintf(stderr, "PROJECTION mismatch call=%llu n=%u skipped=%u cycles=%llu/%llu packets=%zu/%zu\n",
                         projected, n, projectionSkip, v.m_cycle, tail.m_cycle, referencePackets.size(),
                         candidatePackets.size());
            const auto *a = reinterpret_cast<const uint8_t *>(&v.m_state),
                       *b = reinterpret_cast<const uint8_t *>(&tail.m_state);
            for (unsigned i = 0; i < 660; i += 4)
                if (word(a + i) != word(b + i))
                    std::fprintf(stderr, "state offset=%u expected=%08x actual=%08x\n", i, word(a + i), word(b + i));
            unsigned differences = 0;
            for (unsigned i = 0; i < size; i += 4)
                if (word(data + i) != word(scratch.data() + i) && differences++ < 12)
                    std::fprintf(stderr, "memory offset=%u expected=%08x actual=%08x\n", i, word(data + i),
                                 word(scratch.data() + i));
            throw std::runtime_error("Projection tail changed state, cycles, memory or ordered GIF");
        }
        v = tail;
        v.m_activeVuData = data;
        projectionVertex += n;
        ++projectionReplaced;
        projectionSkipped += projectionSkip;
        deferredReflectionCount = 0;
        return result;
    }
    const bool result = reflectionProjectionOriginal(v, data, size, end, ended, relative);
    if (!result || ended || v.m_state.pc != target || v.m_stopRequested)
        throw std::runtime_error("Reflection projection has unfinished accelerated execution");
    const uint64_t timing[2] = {cycle, v.m_cycle};
    auto append = [&](const void *source, size_t bytes) {
        projectionFile.write(static_cast<const char *>(source), bytes);
    };
    append(metadata, sizeof(metadata));
    append(timing, sizeof(timing));
    append(&before, sizeof(before));
    append(input.data(), input.size());
    append(&v.m_state, sizeof(v.m_state));
    append(data, size);
    if (!projectionFile)
        throw std::runtime_error("Cannot write reflection projection capture");
    ++projected;
    return result;
}

bool reflectionProjectionFirst(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended)
{
    if (projectionMode == 2)
    {
        if (v.m_state.vi[13] > 0)
        {
            if (v.m_state.vi[10] != 16 || v.m_state.vi[13] > 12)
                throw std::runtime_error("Unexpected first reflection projection block");
            v.advanceTo(v.m_cycle + 425u);
            for (unsigned i = 3; i <= 6; ++i)
                v.m_state.vi[i] = int16_t(v.m_state.vi[i] + 48);
            v.m_state.vi[10] = v.m_state.vi[13];
            v.m_state.vi[13] = 0;
            v.m_state.vi[8] = v.m_state.vi[9];
            projectionSkip += 16;
        }
        const unsigned skip = v.m_state.vi[10] > 4 ? v.m_state.vi[10] - 4 : 0;
        if (skip)
        {
            v.advanceTo(v.m_cycle + skip * 26u);
            for (unsigned i = 3; i <= 6; ++i)
                v.m_state.vi[i] = int16_t(v.m_state.vi[i] + skip * 3u);
            v.m_state.vi[10] -= skip;
            v.m_state.vi[8] = int16_t(uint16_t(v.m_state.vi[8]) << skip);
            projectionSkip += skip;
        }
    }
    return sr798(v, data, size, end, ended);
}

bool reflectionProjectionEnd(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended)
{
    if (projectionMode == 2)
    {
        const auto *rawOutput =
            projectionGpu.data() + 48 + uint64_t(word(projectionGpu.data() + 8)) * 128 + projectionVertex * 192;
        for (unsigned vertex = 0; vertex < projectionSkip; ++vertex)
            for (unsigned section = 0; section < 4; ++section)
            {
                auto *destination = data + (projectionBase + section * (4 + 3 * projectionCount) + vertex * 3) * 16;
                const auto *source = rawOutput + (vertex * 4 + section) * 48;
                std::memcpy(destination, source, 12);
                std::memcpy(destination + 16, source + 16, 32);
                if (section == 0)
                {
                    // Projection leaves STQ.w untouched. Its previous lighting
                    // value is just original T, so no GPU readback is needed.
                    const auto *original = raw.data() + (projected - 1) * recordBytes + 32 + 664;
                    std::memcpy(destination + 12, original + (projectionBase + vertex * 3) * 16 + 4, 4);
                }
            }
    }
    return sr801(v, data, size, end, ended);
}

bool reflectionTailCapture(uint32_t path, const uint8_t *data, uint32_t count, bool drain, bool direct)
{
    if (!projectionMode)
        return false;
    auto &out = projectionMode == 1 ? referencePackets : candidatePackets;
    const uint32_t metadata[] = {path, count, uint32_t(drain), uint32_t(direct)};
    const auto *header = reinterpret_cast<const uint8_t *>(metadata);
    out.insert(out.end(), header, header + sizeof(metadata));
    out.insert(out.end(), data, data + count);
    return projectionMode == 2;
}
