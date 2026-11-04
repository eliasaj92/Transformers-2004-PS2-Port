// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_native_seam_probe.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "debug_counters.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ps2_native_seam_probe {
bool enabled = false;
namespace {
constexpr uint32_t kWriter = 0x002688f0, kWriterEnd = 0x00268ba0;
constexpr uint32_t kAllocator = 0x00221cc0;
constexpr uint64_t kMaxRecords = 250000;
PS2Runtime::RecompiledFunction originalWriter = nullptr, originalAllocator = nullptr;

uint64_t fingerprint(const uint8_t* bytes, uint32_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}
bool ramSpan(uint32_t address, uint32_t bytes) {
    // Display-list allocations use the EE's 0x20000000 uncached RAM alias.
    // Reject scratchpad/MMIO and out-of-RAM physical spans before dereferencing.
    const uint32_t physical = address & 0x1fffffffu;
    const bool ramSegment = address < 0x40000000u || (address >= 0x80000000u && address < 0xc0000000u);
    return ramSegment && physical >= 0x100000 && bytes <= PS2_RAM_SIZE && physical <= PS2_RAM_SIZE - bytes;
}
uint32_t read32(const uint8_t* ram, uint32_t address) {
    uint32_t value = 0;
    if (ramSpan(address, 4)) std::memcpy(&value, ram + (address & 0x1fffffffu), 4);
    return value;
}
struct Trace {
    FILE* file = nullptr;
    std::mutex mutex;
    uint64_t first = 0, last = 120, sequence = 0;
    ~Trace() { if (file) std::fclose(file); }
    bool inRange() const {
        const auto vs = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
        return vs >= first && vs <= last;
    }
    void row(const char* event, uint64_t eeVs, uint32_t caller, uint32_t object,
             uint32_t address, uint32_t qwords, uint32_t destination,
             uint32_t opcode, uint32_t pc, uint32_t top, uint32_t itop,
             uint64_t hash, uint64_t codeHash, uint32_t cycle = 0, uint32_t mode = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        if (sequence >= kMaxRecords) return;
        std::fprintf(file, "%s,%llu,%llu,%llu,%08x,%08x,%08x,%u,%u,%02x,%08x,%u,%u,%016llx,%016llx,%u,%u\n",
            event, static_cast<unsigned long long>(++sequence),
            static_cast<unsigned long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(eeVs), caller, object, address, qwords,
            destination, opcode, pc, top, itop, static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(codeHash), cycle, mode);
        if (sequence % 4096 == 0) std::fflush(file);
        if (sequence == kMaxRecords)
            std::fprintf(stderr, "[native-seam] trace truncated at %llu records\n",
                         static_cast<unsigned long long>(kMaxRecords));
    }
};
Trace trace;

// EE-thread state only. Keep pending calls across scheduler yields; a host
// function returning is not necessarily the guest method returning.
struct WriterCall {
    uint32_t stack, caller, object, block = 0, qwords, destination;
};
std::vector<WriterCall> pending;

void writer(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    if (ctx->pc == kWriter && trace.inRange() && pending.size() < 64) {
        const uint32_t object = GPR_U32(ctx, 4);
        pending.push_back({GPR_U32(ctx, 29), GPR_U32(ctx, 31), object, 0,
                           read32(ram, object + 8), read32(ram, object + 12)});
    }
    originalWriter(ram, ctx, runtime);
    for (size_t i = pending.size(); i-- > 0;) {
        const auto call = pending[i];
        if (ctx->pc != call.caller || GPR_U32(ctx, 29) != call.stack) continue;
        if (call.qwords == 56 && ramSpan(call.block, 56 * 16))
            trace.row("constants", runtime->eeScheduler().currentVSyncTick(), call.caller,
                      call.object, call.block, 56, call.destination, 0, kWriter, 0, 0,
                      fingerprint(ram + (call.block & 0x1fffffffu), 56 * 16), 0);
        else
            trace.row("invalid_constants", runtime->eeScheduler().currentVSyncTick(), call.caller,
                      call.object, call.block, call.qwords, call.destination, 0, kWriter, 0, 0, 0, 0);
        pending.erase(pending.begin() + i);
    }
}
void allocate(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t caller = GPR_U32(ctx, 31), stack = GPR_U32(ctx, 29);
    const uint32_t bytes = GPR_U32(ctx, 4), destination = GPR_U32(ctx, 5);
    originalAllocator(ram, ctx, runtime); // Verified leaf: emits CNT/STCYCL/UNPACK.
    if (ctx->pc != caller || GPR_U32(ctx, 29) != stack) return;
    const uint32_t block = GPR_U32(ctx, 2);
    if (caller == 0x00268910 && !pending.empty() && pending.back().stack == stack + 0x30)
        pending.back().block = block;
    if (trace.inRange())
        trace.row("allocation", runtime->eeScheduler().currentVSyncTick(), caller, 0,
                  block, bytes / 16, destination, 0, kAllocator, 0, 0, 0, 0);
}
}

void install(PS2Runtime& runtime, uint32_t crc) {
    const char* path = std::getenv("PS2X_TF_SEAM_TRACE");
    if (!path || !*path) return;
    if (crc != 0x74c982c0 || !runtime.hasFunction(kWriter) || !runtime.hasFunction(kAllocator))
        throw std::runtime_error("Native seam probe requires the supported SLUS-20668 executable");
    if (enabled) throw std::runtime_error("Native seam probe already installed");
    if (const char* range = std::getenv("PS2X_TF_SEAM_RANGE")) {
        unsigned long long first = 0, last = 0;
        char extra = 0;
        if (std::sscanf(range, "%llu:%llu%c", &first, &last, &extra) != 2 || first > last || last - first > 600)
            throw std::runtime_error("PS2X_TF_SEAM_RANGE requires first:last, at most 600 fields apart");
        trace.first = first; trace.last = last;
    }
    // Exclusive creation keeps a repeated diagnostic invocation from erasing evidence.
    trace.file = std::fopen(path, "wbx");
    if (!trace.file) throw std::runtime_error("Cannot create PS2X_TF_SEAM_TRACE (use a fresh private path)");
    std::fprintf(trace.file, "event,sequence,present_vs,ee_vs,caller,object,address,qwords,destination,opcode,pc,top,itop,hash,code_hash,cycle,mode\n");
    originalWriter = runtime.lookupFunction(kWriter);
    originalAllocator = runtime.lookupFunction(kAllocator);
    pending.reserve(64);
    for (uint32_t pc = kWriter; pc < kWriterEnd; pc += 4)
        if (runtime.hasFunction(pc) && runtime.lookupFunction(pc) == originalWriter)
            runtime.replaceFunction(pc, &writer);
    runtime.replaceFunction(kAllocator, &allocate);
    enabled = true;
    std::fprintf(stderr, "[native-seam] observation only, presenter fields %llu:%llu, limit %llu records\n",
                 static_cast<unsigned long long>(trace.first), static_cast<unsigned long long>(trace.last),
                 static_cast<unsigned long long>(kMaxRecords));
}

void unpack(uint32_t command, uint32_t destination, const uint8_t* payload,
            uint32_t bytes, uint32_t cycle, uint32_t mode) {
    if (!trace.inRange() || bytes != 56 * 16) return;
    trace.row("unpack56", 0, 0, 0, 0, 56, destination, (command >> 24) & 0x7f,
              0, 0, 0, fingerprint(payload, bytes), 0, cycle, mode);
}
void launch(uint32_t opcode, uint32_t pc, uint32_t top, uint32_t itop, const uint8_t* code) {
    if (!trace.inRange()) return;
    // Only code is read. Reading live VU data/registers here would require a
    // worker fence. MSCNT resumes saved execution; its VIF command has no entry PC.
    const uint64_t hash = code && pc <= 0x4000 - 64 ? fingerprint(code + pc, 64) : 0;
    trace.row(opcode == 0x17 ? "mscnt" : "mscal", 0, 0, 0, 0, 0, 0, opcode,
              pc, top, itop, 0, hash);
}
void upload(uint32_t destination, const uint8_t* payload, uint32_t bytes) {
    if (!trace.inRange()) return;
    trace.row("mpg", 0, 0, 0, 0, bytes / 16, destination, 0x4a, 0, 0, 0,
              fingerprint(payload, bytes), 0);
}
}
