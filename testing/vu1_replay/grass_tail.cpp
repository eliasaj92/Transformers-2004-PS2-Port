// SPDX-License-Identifier: GPL-3.0-or-later
// Accelerated-only grass emission boundary experiment. Not a live bypass.
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

bool TF_GRASS_LOOP(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &);
bool TF_GRASS_EPILOGUE(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &);

namespace
{
std::vector<uint8_t> referencePackets, candidatePackets, gpuInputs, gpuGeometry;
unsigned captureMode = 0;
uint64_t calls = 0, active = 0, captured = 0, replaced = 0;
bool emitterPending = false;
std::array<uint8_t, 256> emitterInput;
std::ofstream inputFile, packetFile;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
uint32_t word(const void *p)
{
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}
std::vector<uint8_t> read(const std::filesystem::path &path)
{
    const auto size = std::filesystem::file_size(path);
    require(size <= 64 * 1024 * 1024, "Grass fixture exceeds 64 MiB");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    require(bool(file), "Cannot read grass fixture");
    return bytes;
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

// The final accelerated iteration overwrites all retained results except the
// previous pair's second-field clipping flags. Reconstruct only those flags;
// the GPU performs the skipped coordinate, reciprocal and color calculations.
void skipMiddlePairs(StaticVU &v, const uint8_t *data)
{
    const auto add = [](__m128 x, __m128 y) { return _mm_add_ps(x, y); };
    const auto scale = [](__m128 x, float y) { return _mm_mul_ps(x, _mm_set1_ps(y)); };
    float weights[4];
    std::memcpy(weights, data + ((uint16_t(v.m_state.vi[5]) + 1u) & 1023u) * 16u, 16);
    const __m128 edge = _mm_loadu_ps(v.m_state.vf[18]);
    __m128 point = add(_mm_loadu_ps(v.m_state.vf[10]), scale(edge, weights[0]));
    point = add(add(point, scale(edge, weights[2])), scale(_mm_loadu_ps(v.m_state.vf[19]), weights[3]));
    const auto flags = [&](const __m128 p) {
        const __m128 clip = add(_mm_mul_ps(_mm_loadu_ps(v.m_state.vf[2]), _mm_shuffle_ps(p, p, 255)),
                                _mm_mul_ps(_mm_loadu_ps(v.m_state.vf[1]), p));
        float f[4];
        _mm_storeu_ps(f, clip);
        uint32_t bits = 0;
        for (unsigned i = 0; i < 3; ++i)
        {
            if (f[i] > std::fabs(f[3]))
                bits |= 1u << (i * 2);
            if (f[i] < -std::fabs(f[3]))
                bits |= 2u << (i * 2);
        }
        return bits;
    };
    const uint32_t retained = flags(point) & flags(add(point, _mm_loadu_ps(v.m_state.vf[3])));
    // Advances the real pending transfer and pipeline calendar, not just the
    // cycle counter. The following accelerated iteration retains normal timing.
    v.advanceTo(v.m_cycle + 118);
    v.m_state.vi[10] = int16_t(retained);
    v.m_state.vi[12] = 63;
    v.m_state.vi[5] = int16_t(v.m_state.vi[5] + 2);
    v.m_state.vi[8] = int16_t(v.m_state.vi[8] + 16);
    v.m_state.vi[9] = 2;
    v.m_state.branchTarget = v.m_state.pc;
    v.m_state.branchDelay = 0;
}
} // namespace

void grassReplayInitialize()
{
    if (const char *prefix = std::getenv("PS2X_GRASS_CAPTURE"))
    {
        const std::filesystem::path a = std::string(prefix) + "-inputs.bin", b = std::string(prefix) + "-packets.bin";
        require(!std::filesystem::exists(a) && !std::filesystem::exists(b), "Use fresh grass capture paths");
        inputFile.open(a, std::ios::binary);
        packetFile.open(b, std::ios::binary);
        require(bool(inputFile) && bool(packetFile), "Cannot create grass captures");
    }
    const char *gpu = std::getenv("PS2X_GRASS_GPU_CAPTURE"), *inputs = std::getenv("PS2X_GRASS_GPU_INPUTS");
    require(bool(gpu) == bool(inputs), "Specify both grass GPU capture and its uploaded inputs");
    if (gpu)
    {
        gpuGeometry = read(gpu);
        gpuInputs = read(inputs);
        require(gpuGeometry.size() >= 48 && word(gpuGeometry.data()) == 0x47524750 &&
                    word(gpuGeometry.data() + 4) == 1 && word(gpuGeometry.data() + 12) == 640,
                "Invalid grass GPU capture header");
        const uint32_t count = word(gpuGeometry.data() + 8);
        require(count && count <= 65536 && gpuGeometry.size() == 48 + uint64_t(count) * 640 &&
                    gpuInputs.size() == uint64_t(count) * 256,
                "Invalid grass GPU capture lengths");
        // The Python driver verifies SHA-256 provenance. Every record is also
        // checked byte-for-byte against its actual VU inputs below.
    }
}

bool grassTailProbe(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended)
{
    ++calls;
    active += v.m_xgkick.active;
    require(!emitterPending, "Nested grass emitter");
    require(size == 16384 && v.m_state.vi[9] == 6 && end >= v.m_cycle && end - v.m_cycle >= 177 &&
                !v.m_state.branchPending && !v.m_state.ebit && !v.m_state.haltAfterDelaySlot && !v.m_stopRequested &&
                !v.m_state.dBitEnabled && !v.m_state.tBitEnabled && (v.m_state.status & 0x3c0) == 0x3c0,
            "Grass tail entry is outside the qualified control state");
    const unsigned base = (v.m_state.pc - 2536u) & 16383u;
    require(base <= 16384 - 3872 && v.m_activeMemory && v.m_activeMemory->vu1Code &&
                fingerprint(v.m_activeMemory->vu1Code + base, 3872) == 0xbbe99809fbf7361eull,
            "Grass program identity changed");
    constexpr unsigned registers[] = {1, 2, 3, 9, 16, 17, 10, 18, 19, 22, 20, 21};
    for (unsigned i = 0; i < 12; ++i)
        std::memcpy(emitterInput.data() + i * 16, v.m_state.vf[registers[i]], 16);
    for (unsigned i = 0; i < 4; ++i)
        std::memcpy(emitterInput.data() + 192 + i * 16, data + ((uint16_t(v.m_state.vi[5]) + i - 1u) & 1023u) * 16, 16);
    emitterPending = true;
    if (!gpuGeometry.empty())
    {
        require(captured < gpuInputs.size() / 256 &&
                    !std::memcmp(gpuInputs.data() + captured * 256, emitterInput.data(), 256),
                "Grass replay differs from actual GPU upload");
        skipMiddlePairs(v, data);
        require(TF_GRASS_LOOP(v, data, size, end, ended), "Accelerated grass tail declined");
        ++replaced;
        return true;
    }

    // First qualify state and overlapping transfers independently of GPU
    // arithmetic. No speed claim: this mode retains the complete memory oracle.
    static thread_local auto candidate = std::make_unique<StaticVU>();
    static thread_local std::array<uint8_t, 16384> scratch;
    *candidate = v;
    std::memcpy(scratch.data(), data, size);
    auto &c = *candidate;
    referencePackets.clear();
    candidatePackets.clear();
    captureMode = 1;
    require(TF_GRASS_LOOP(v, data, size, end, ended), "Accelerated grass reference declined");
    captureMode = 2;
    c.m_activeVuData = scratch.data();
    skipMiddlePairs(c, scratch.data());
    bool candidateEnded = false;
    require(TF_GRASS_LOOP(c, scratch.data(), size, end, candidateEnded), "Accelerated grass tail declined");
    captureMode = 0;
    require(candidateEnded == ended && referencePackets == candidatePackets, "Grass tail changed pending GIF transfer");
    static_assert(sizeof(VU1State) == 664 && offsetof(VU1State, branchDelay) == 656);
    const auto *a = reinterpret_cast<const uint8_t *>(&v.m_state), *b = reinterpret_cast<const uint8_t *>(&c.m_state);
    for (unsigned i = 0; i < sizeof(VU1State); ++i)
    {
        if (i == 638 || i == 639 || (i >= 649 && i <= 651) || i >= 660)
            continue; // Windows x64 ABI padding only.
        require(a[i] == b[i], "Grass tail changed retained VU state");
    }
    c.m_activeVuData = data;
    v = c;
    return true;
}

bool grassEpilogueProbe(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended)
{
    require(TF_GRASS_EPILOGUE(v, data, size, end, ended), "Accelerated grass epilogue declined");
    require(emitterPending, "Missing grass emitter inputs");
    const unsigned base = (uint16_t(v.m_state.vi[2]) - 6u) & 1023u;
    require(base <= 1024 - 76, "Wrapping grass packet");
    auto *packet = data + base * 16;
    if (!gpuGeometry.empty())
    {
        const auto *raw = gpuGeometry.data() + 48 + captured * 640;
        for (unsigned field = 0; field < 2; ++field)
            for (unsigned sprite = 0; sprite < 8; ++sprite)
            {
                const unsigned address = (6 + field * 38 + sprite * 4) * 16;
                std::memcpy(packet + address, raw + field * 256 + sprite * 32, 16);
                std::memcpy(packet + address + 48, raw + field * 256 + sprite * 32 + 16, 16);
                std::memcpy(packet + address + 32, raw + 512 + sprite * 16, 16);
            }
    }
    if (inputFile.is_open())
    {
        inputFile.write(reinterpret_cast<const char *>(emitterInput.data()), emitterInput.size());
        packetFile.write(reinterpret_cast<const char *>(packet), 1216);
        require(bool(inputFile) && bool(packetFile), "Cannot write grass capture");
    }
    emitterPending = false;
    ++captured;
    return true;
}

bool grassTailCapture(uint32_t path, const uint8_t *data, uint32_t count, bool drain, bool direct)
{
    if (!captureMode)
        return false;
    auto &out = captureMode == 1 ? referencePackets : candidatePackets;
    const uint32_t metadata[] = {path, count, uint32_t(drain), uint32_t(direct)};
    const auto *h = reinterpret_cast<const uint8_t *>(metadata);
    out.insert(out.end(), h, h + 16);
    out.insert(out.end(), data, data + count);
    return captureMode == 2;
}

void grassReplayFinish()
{
    require(calls && calls == captured && !emitterPending, "Incomplete grass emission replay");
    require(gpuGeometry.empty() || captured == gpuInputs.size() / 256, "Unused GPU grass results");
    std::printf("GRASS_TAIL calls=%llu active_transfers=%llu emitted=%llu gpu_replaced=%llu skipped_iterations=%llu\n",
                calls, active, captured, replaced, replaced * 2);
}
