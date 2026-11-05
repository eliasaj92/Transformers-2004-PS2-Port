// SPDX-License-Identifier: GPL-3.0-or-later
// Accelerated-only grass emission boundary experiment. Not a live bypass.
#include "common.h"
#include "runtime/gs/gs_native_grass_vu.h"
#include "runtime/gs/gs_native_grass_math.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <deque>

bool TF_GRASS_PROLOGUE(StaticVU &, uint8_t *, uint32_t, uint64_t, bool &);
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
bool deferGeometry = false;
std::array<uint8_t, 1216> restoredPacket;
uint64_t deferredPackets = 0;
unsigned prologueGuardChecks = 0;

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

void verifyPrologueGuards(const StaticVU &v, uint8_t *data, uint32_t size, uint64_t end)
{
    auto candidate = std::make_unique<StaticVU>();
    for (unsigned test = 0; test < 15; ++test)
    {
        *candidate = v;
        auto &c = *candidate;
        uint64_t budget = end;
        uint32_t bytes = size;
        switch (test)
        {
        case 0: budget = c.m_cycle + 262; break;
        case 1: c.m_state.branchPending = true; break;
        case 2: c.m_stopRequested = true; break;
        case 3: c.m_state.dBitEnabled = true; break;
        case 4: c.m_state.tBitEnabled = true; break;
        case 5: c.m_state.ebit = true; break;
        case 6: c.m_state.haltAfterDelaySlot = true; break;
        case 7: c.m_state.status &= ~0x3c0u; break;
        case 8: bytes = 8192; break;
        case 9: c.m_viReady[5] = c.m_cycle + 1; break;
        case 10: c.m_vfReady[9][0] = c.m_cycle + 1; break;
        case 11: case 12: case 13: case 14:
            c.m_state.vf[9][3] = test == 11 ? 0.0f : test == 12 ? -0.0f :
                test == 13 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
            c.m_state.vf[16][3] = c.m_state.vf[17][3] = 0.0f;
            break;
        }
        unsigned ordinaryCalls = 0, tailCalls = 0;
        const auto ordinary = [&](auto &, auto *, auto, auto, auto &) { ++ordinaryCalls; return true; };
        const auto tail = [&](auto &, auto *, auto, auto, auto &) { ++tailCalls; return true; };
        const auto stateBefore = c.m_state;
        const auto selected = tf_gpu_grass::stats().selected;
        const auto cycle = c.m_cycle;
        bool ended = false;
        require(tf_gpu_grass::prologue(c, data, bytes, budget, ended, ordinary, tail) && ordinaryCalls == 1 &&
                    !tailCalls && !ended && c.m_cycle == cycle && !std::memcmp(&stateBefore, &c.m_state, sizeof(stateBefore)) &&
                    tf_gpu_grass::stats().selected == selected,
                "Grass prologue failed to decline unsupported control/depth without mutation");
        ++prologueGuardChecks;
    }
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
    if (const char *option = std::getenv("PS2X_GRASS_DEFER"))
    {
        require(!std::strcmp(option, "1") && !gpuGeometry.empty() && !inputFile.is_open(),
                "Deferred grass replay requires authenticated GPU geometry and no new capture");
        deferGeometry = true;
        tf_gpu_grass::install(0x74c982c0, true);
    }
}

bool grassPrologueProbe(StaticVU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended)
{
    if (!deferGeometry)
        return TF_GRASS_PROLOGUE(v, data, size, end, ended);
    require(!emitterPending, "Nested grass prologue");
    if (!prologueGuardChecks)
        verifyPrologueGuards(v, data, size, end);
    const auto input = tf_gpu_grass::inputs(v, data, 0);
    const auto selected = tf_gpu_grass::stats().selected;
    const bool overlapping = v.m_xgkick.active;
    const bool result = tf_gpu_grass::prologue(v, data, size, end, ended, TF_GRASS_PROLOGUE, TF_GRASS_LOOP);
    if (tf_gpu_grass::stats().selected == selected)
        return result;
    require(result && captured < gpuInputs.size() / 256 &&
                !std::memcmp(gpuInputs.data() + captured * 256, input.data(), 256),
            "Grass prologue differs from actual GPU upload");
    emitterInput = input;
    emitterPending = true;
    ++calls;
    active += overlapping;
    ++replaced;
    return result;
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
    emitterInput = tf_gpu_grass::inputs(v, data);
    emitterPending = true;
    if (!gpuGeometry.empty())
    {
        require(captured < gpuInputs.size() / 256 &&
                    !std::memcmp(gpuInputs.data() + captured * 256, emitterInput.data(), 256),
                "Grass replay differs from actual GPU upload");
        if (deferGeometry)
            require(tf_gpu_grass::loop(v, data, size, end, ended, TF_GRASS_LOOP), "Runtime grass loop declined");
        else
        {
            tf_gpu_grass::skipMiddlePairs(v, data);
            require(TF_GRASS_LOOP(v, data, size, end, ended), "Accelerated grass tail declined");
        }
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
    tf_gpu_grass::skipMiddlePairs(c, scratch.data());
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
    if (deferGeometry)
        require(tf_gpu_grass::epilogue(v, data, size, end, ended, TF_GRASS_EPILOGUE),
                "Runtime grass epilogue declined");
    else
        require(TF_GRASS_EPILOGUE(v, data, size, end, ended), "Accelerated grass epilogue declined");
    require(emitterPending, "Missing grass emitter inputs");
    const unsigned base = (uint16_t(v.m_state.vi[2]) - 6u) & 1023u;
    require(base <= 1024 - 76, "Wrapping grass packet");
    auto *packet = data + base * 16;
    if (!gpuGeometry.empty() && !deferGeometry)
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
    if (deferGeometry)
    {
        // Deliberately invalid unresolved words reveal accidental guest reads.
        // Ordered GS packets and explicit CPU reads resolve from their source.
        for (unsigned field = 0; field < 2; ++field)
            for (unsigned sprite = 0; sprite < 6; ++sprite)
            {
                auto *p = packet + (6 + field * 38 + sprite * 4) * 16;
                std::memset(p, 0xcd, 16);
                std::memset(p + 32, 0xcd, 32);
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

bool grassTailCapture(uint32_t path, const uint8_t *&data, uint32_t count, bool drain, bool direct)
{
    if (auto owner = deferGeometry ? tf_gpu_grass::packet(path, data, count) : nullptr)
    {
        require(!captureMode && path == 1 && drain && !direct, "Deferred grass packet routing changed");
        const uint64_t captureIndex = owner->ticket - 1;
        std::memcpy(restoredPacket.data(), data, count);
        gsnative::materializePacket(*owner, restoredPacket.data(), count);
        const auto *raw = gpuGeometry.data() + 48 + captureIndex * 640;
        for (unsigned field = 0; field < 2; ++field)
            for (unsigned sprite = 0; sprite < 8; ++sprite)
            {
                const auto *p = restoredPacket.data() + (6 + field * 38 + sprite * 4) * 16;
                const unsigned offsets[] = {0, 48, 32};
                const unsigned referenceOffsets[] = {field * 256 + sprite * 32, field * 256 + sprite * 32 + 16,
                                                     512 + sprite * 16};
                for (unsigned part = 0; part < 3; ++part)
                    for (unsigned lane = 0; lane < 4; ++lane)
                        if (word(p + offsets[part] + lane * 4) != word(raw + referenceOffsets[part] + lane * 4))
                        {
                            std::fprintf(stderr,
                                         "GRASS_DEFER mismatch packet=%llu field=%u sprite=%u part=%u lane=%u "
                                         "actual=%08x expected=%08x\n",
                                         captureIndex, field, sprite, part, lane, word(p + offsets[part] + lane * 4),
                                         word(raw + referenceOffsets[part] + lane * 4));
                            require(false, "Deferred grass packet differs from actual GPU output");
                        }
            }
        data = restoredPacket.data();
        ++deferredPackets;
    }
    if (!captureMode)
        return false;
    auto &out = captureMode == 1 ? referencePackets : candidatePackets;
    const uint32_t metadata[] = {path, count, uint32_t(drain), uint32_t(direct)};
    const auto *h = reinterpret_cast<const uint8_t *>(metadata);
    out.insert(out.end(), h, h + 16);
    out.insert(out.end(), data, data + count);
    return captureMode == 2;
}

void grassReplayBegin(const void *owner, const uint8_t *code, uint8_t *data, uint64_t generation)
{
    if (deferGeometry)
        tf_gpu_grass::beginCall(owner, code, data, generation);
}
void grassReplayResolve(bool transferred, bool fellBack)
{
    if (deferGeometry)
        tf_gpu_grass::endCall(transferred, fellBack);
}

void grassReplayFinish()
{
    require(calls && calls == captured && !emitterPending, "Incomplete grass emission replay");
    require(gpuGeometry.empty() || captured == gpuInputs.size() / 256, "Unused GPU grass results");
    if (deferGeometry)
    {
        const auto deferred = tf_gpu_grass::stats();
        require(deferredPackets == captured && !deferred.dirtyQwords && deferred.attached == captured,
                "Incomplete deferred grass ownership");
        std::printf("GRASS_DEFER packets=%llu recorded=%llu overwritten_qwords=%llu materialized_sprites=%llu "
                    "materialized_qwords=%llu dirty=%u\n",
                    deferredPackets, deferred.recorded, deferred.overwrittenQwords, deferred.materializedSprites,
                    deferred.materializedQwords, deferred.dirtyQwords);
        std::printf("GRASS_PROLOGUE skipped=%llu guard_checks=%u\n", deferred.skippedPrologues, prologueGuardChecks);
        std::printf("GRASS_REUSE records=%llu\n", deferred.reusedRecords);
    }
    std::printf("GRASS_TAIL calls=%llu active_transfers=%llu emitted=%llu gpu_replaced=%llu skipped_iterations=%llu\n",
                calls, active, captured, replaced, replaced * 2);
}
