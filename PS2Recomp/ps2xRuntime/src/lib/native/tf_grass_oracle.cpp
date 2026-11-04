// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_grass_oracle.h"
#include "tf_grass_geometry.h"
#include "tf_grass_packets.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace tf_grass_oracle {
bool enabled = false;
namespace {
using Clock = std::chrono::steady_clock;
uint64_t fingerprint(const uint8_t* data, uint32_t bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < bytes; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
    return hash;
}
uint32_t word(const uint8_t* data, unsigned qword, unsigned lane = 0) {
    uint32_t value;
    std::memcpy(&value, data + qword*16 + lane*4, 4);
    return value;
}
tf_native::GrassVector vector(const uint8_t* data, unsigned qword) {
    tf_native::GrassVector value;
    std::memcpy(value.data(), data + qword*16, 16);
    return value;
}
struct Stats {
    std::atomic<uint64_t> matched{0}, declined{0}, groups{0}, empty{0}, nativeNs{0}, referenceNs{0};
    void print(const char* why) const {
        const uint64_t native = nativeNs.load(), reference = referenceNs.load();
        std::fprintf(stderr, "[grass-oracle] %s matched=%llu declined=%llu groups=%llu empty=%llu native_ms=%llu.%03llu observed_reference_ms=%llu.%03llu (reference includes comparison overhead)\n",
            why, static_cast<unsigned long long>(matched.load()), static_cast<unsigned long long>(declined.load()),
            static_cast<unsigned long long>(groups.load()), static_cast<unsigned long long>(empty.load()),
            static_cast<unsigned long long>(native/1000000), static_cast<unsigned long long>((native/1000)%1000),
            static_cast<unsigned long long>(reference/1000000), static_cast<unsigned long long>((reference/1000)%1000));
        std::fflush(stderr);
    }
};
Stats stats;
struct Call {
    bool active = false;
    size_t next = 0;
    std::vector<tf_native::GrassBatch> batches;
    tf_native::GrassPacketTemplates packets;
    Clock::time_point referenceStart;
};
thread_local Call call;

[[noreturn]] void mismatch(const char* reason) {
    std::fprintf(stderr, "[grass-oracle] MISMATCH %s group=%zu expected_groups=%zu after_matched=%llu\n",
        reason, call.next, call.batches.size(), static_cast<unsigned long long>(stats.matched.load()));
    std::fflush(stderr);
    throw std::runtime_error("Native grass geometry comparison failed");
}
}

void install(uint32_t crc) {
    const char* option = std::getenv("PS2X_TF_GRASS_VERIFY");
    if (!option || !*option || std::strcmp(option, "0") == 0) return;
    if (std::strcmp(option, "1") != 0 || crc != 0x74c982c0 || enabled)
        throw std::runtime_error("PS2X_TF_GRASS_VERIFY=1 requires the supported SLUS-20668 executable, installed once");
    enabled = true;
    std::fprintf(stderr, "[grass-oracle] complete packet comparison enabled; accelerated VU/GS execution remains authoritative\n");
}

bool begin(uint32_t pc, bool continuing, uint32_t itop, const uint8_t* code, const uint8_t* data) {
    if (!continuing || (pc != 0x10 && pc != 0xf8) || word(data, 4) != 1) return false;
    const uint32_t start = (word(data, 5) & 0xffffu)*8, constants = word(data, 5, 1) & 0xffffu;
    if (start > 16384-3872 || fingerprint(code + start, 64) != 0x2ad8a73a9581a552ull ||
        fingerprint(code + start, 3872) != 0xbbe99809fbf7361eull) return false;
    if (call.active) throw std::runtime_error("Nested native grass comparison");
    if (constants > 1024-56 || itop > 1024-243 || word(data, itop, 1) != 3) { ++stats.declined; return false; }
    const unsigned count = word(data, itop) & 0x7fff;
    if (count < 2 || count > 28) { ++stats.declined; return false; }
    if (!tf_native::prepareGrassPackets(std::span<const uint8_t, 16384>(data, 16384), itop, constants, call.packets)) {
        ++stats.declined; return false;
    }
    tf_native::GrassContext context;
    context.clipScale = vector(data, constants); context.clipOffset = vector(data, constants+1);
    context.billboardOffset = vector(data, constants+2);
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned column = 0; column < 4; ++column) context.transform[field][column] = vector(data, constants+3+field*4+column);
    const auto density = vector(data, constants+23);
    context.depthBias = density[0]; context.density = density[2]; context.distanceScale = density[3];
    for (unsigned sample = 0; sample < 32; ++sample) context.scatter[sample] = vector(data, constants+24+sample);
    std::array<tf_native::GrassVertex, 28> vertices;
    for (unsigned vertex = 0; vertex < count; ++vertex) {
        vertices[vertex].position = vector(data, itop+3+vertex*3);
        vertices[vertex].position[3] = 1.0f;
        for (unsigned lane = 0; lane < 4; ++lane) vertices[vertex].color[lane] = word(data, itop+2+vertex*3, lane);
    }
    const tf_native::GrassStrip strip{std::span(vertices.data(), count),
        {static_cast<uint16_t>(word(data, itop+count*3+4)), static_cast<uint16_t>(word(data, itop+count*3+4, 1))}};
    tf_native::GrassStats modelStats;
    const auto startTime = Clock::now();
    const bool accepted = tf_native::buildGrassGeometry(context, strip, call.batches, modelStats);
    stats.nativeNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-startTime).count(), std::memory_order_relaxed);
    if (!accepted) {
        if (++stats.declined <= 8) std::fprintf(stderr, "[grass-oracle] native input declined at itop=%u count=%u\n", itop, count);
        return false;
    }
    call.next = 0; call.active = true; call.referenceStart = Clock::now();
    return true;
}

void gif(uint32_t path, const uint8_t* data, uint32_t bytes, bool drain, bool directHl) {
    if (!call.active) return; // Later EE replay has no active VU invocation.
    if (path != 1 || !drain || directHl || !data || bytes < 16) mismatch("packet metadata");
    if (call.batches.empty()) {
        if (call.next || bytes != 16 || std::memcmp(data, call.packets.empty.data(), 16)) mismatch("empty packet");
        ++stats.empty;
    } else {
        if (bytes != sizeof(tf_native::GrassPacket) || call.next >= call.batches.size()) mismatch("packet count/size");
        tf_native::GrassPacket expected;
        tf_native::writeGrassPacket(call.packets, call.batches[call.next], call.next, expected);
        if (std::memcmp(data, expected.data(), sizeof(expected))) mismatch("complete GIF bytes");
    }
    ++call.next;
}

void end(bool needsTransfer, bool fellBack) {
    if (!call.active) throw std::runtime_error("Missing native grass comparison begin");
    if (needsTransfer || fellBack) mismatch("incomplete accelerated reference");
    if (call.next != (call.batches.empty() ? 1 : call.batches.size())) mismatch("missing emitted packets");
    call.active = false;
    stats.referenceNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-call.referenceStart).count(), std::memory_order_relaxed);
    stats.groups.fetch_add(call.batches.size(), std::memory_order_relaxed);
    if ((++stats.matched & 1023) == 0) stats.print("progress");
}

void finish() {
    if (enabled) stats.print("final");
}
}
