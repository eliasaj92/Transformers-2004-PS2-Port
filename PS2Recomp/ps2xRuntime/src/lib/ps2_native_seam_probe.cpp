// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_native_seam_probe.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_vu1.h"
#include "debug_counters.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ps2_native_seam_probe {
bool enabled = false;
bool captureEnabled = false;
namespace {
constexpr uint32_t kWriter = 0x002688f0, kWriterEnd = 0x00268ba0;
constexpr uint32_t kAllocator = 0x00221cc0;
constexpr uint32_t kMeshCall = 0x0026f790;
constexpr uint32_t kVisibleMesh = 0x00260a70, kVisibleMeshEnd = 0x00260c80;
constexpr uint64_t kMaxRecords = 250000;
PS2Runtime::RecompiledFunction originalWriter = nullptr, originalAllocator = nullptr;
PS2Runtime::RecompiledFunction originalMeshCall = nullptr;
PS2Runtime::RecompiledFunction originalVisibleMesh = nullptr;

// All derived bytes remain in an explicitly chosen private directory. Bound
// total output as well as call count; fail visibly rather than accept truncation.
std::string capturePrefix;
uint32_t captureLimit = 256;
bool captureGrassOnly = false;
std::atomic<uint32_t> captureCount{0};
std::atomic<uint64_t> captureBytes{0};
constexpr uint64_t kMaxCaptureBytes = 512ull * 1024 * 1024;
FILE* createCapture(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wbx");
    if (!file) throw std::runtime_error("Cannot exclusively create native-seam capture: " + path);
    return file;
}
void writeCapture(FILE* file, const void* data, size_t bytes) {
    if (captureBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes > kMaxCaptureBytes)
        throw std::runtime_error("Native-seam capture exceeded its 512 MiB limit");
    if (std::fwrite(data, 1, bytes, file) != bytes)
        throw std::runtime_error("Cannot write native-seam capture");
}
template <typename Writer> void captureFile(const std::string& path, Writer write) {
    FILE* file = createCapture(path);
    try { write(file); }
    catch (...) { std::fclose(file); throw; }
    if (std::fclose(file) != 0) throw std::runtime_error("Cannot finish native-seam capture: " + path);
}
void vuDump(const std::string& path, uint32_t word0, uint32_t top, uint32_t itop,
            const uint8_t* code, const uint8_t* data, const VU1State& state) {
    captureFile(path, [&](FILE* file) {
        const uint32_t header[] = {word0, top, itop, uint32_t(sizeof(VU1State))};
        writeCapture(file, header, sizeof(header));
        writeCapture(file, code, 16384);
        writeCapture(file, data, 16384);
        writeCapture(file, &state, sizeof(state));
    });
}
struct VuCapture {
    uint32_t ticket = 0, word0 = 0, top = 0, itop = 0, packets = 0;
    FILE* gifFile = nullptr;
    std::string base;
    ~VuCapture() { if (gifFile) std::fclose(gifFile); }
};
thread_local VuCapture vuCapture;

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
             uint64_t hash, uint64_t codeHash, uint32_t cycle = 0, uint32_t mode = 0,
             uint32_t parent = 0, uint64_t contextHash = 0, uint32_t capture = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        if (sequence >= kMaxRecords) return;
        std::fprintf(file, "%s,%llu,%llu,%llu,%08x,%08x,%08x,%u,%u,%02x,%08x,%u,%u,%016llx,%016llx,%u,%u,%08x,%016llx,%u\n",
            event, static_cast<unsigned long long>(++sequence),
            static_cast<unsigned long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(eeVs), caller, object, address, qwords,
            destination, opcode, pc, top, itop, static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(codeHash), cycle, mode, parent,
            static_cast<unsigned long long>(contextHash), capture);
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
    uint32_t stack, caller, object, block = 0, qwords, destination, parent;
};
std::vector<WriterCall> pending;
struct Context { uint64_t hash = 0, eeVs = 0, serial = 0; uint32_t parent = 0, object = 0, stack = 0; };
Context latestContext; // EE thread only; adjacency is not proof of VU ownership.
bool meshCaptured = false;
struct MeshCall { uint32_t stack, caller, object, list; uint64_t contextSerial; };
std::vector<MeshCall> pendingMeshes;

void writer(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    if (ctx->pc == kWriter && trace.inRange() && pending.size() < 64) {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t stack = GPR_U32(ctx, 29), caller = GPR_U32(ctx, 31);
        const uint32_t parent = caller == 0x23aaf4 || caller == 0x23ab24 ? read32(ram, stack + 0x30) : 0;
        pending.push_back({stack, caller, object, 0,
                           read32(ram, object + 8), read32(ram, object + 12), parent});
    }
    originalWriter(ram, ctx, runtime);
    for (size_t i = pending.size(); i-- > 0;) {
        const auto call = pending[i];
        if (ctx->pc != call.caller || GPR_U32(ctx, 29) != call.stack) continue;
        if (call.qwords == 56 && ramSpan(call.block, 56 * 16)) {
            latestContext = {fingerprint(ram + (call.block & 0x1fffffffu), 56 * 16),
                             runtime->eeScheduler().currentVSyncTick(), latestContext.serial + 1,
                             call.parent, call.object, call.stack};
            trace.row("constants", runtime->eeScheduler().currentVSyncTick(), call.caller,
                      call.object, call.block, 56, call.destination, 0, kWriter, 0, 0,
                      latestContext.hash, 0, 0, 0, call.parent);
        }
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
void observeMesh(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime, uint32_t source,
                 uint32_t caller, uint32_t list, uint32_t tag, bool adjacentGrass) {
    const uint64_t eeVs = runtime->eeScheduler().currentVSyncTick();
    uint32_t capture = 0;
    if (captureEnabled && !meshCaptured && adjacentGrass && ramSpan(list, 64)) {
        meshCaptured = true;
        capture = 1;
        // This is an EE-thread snapshot at the submission boundary, not a
        // globally atomic dump of every emulated device or its queues.
        captureFile(capturePrefix + "-ee.bin", [&](FILE* file) {
            const uint32_t header[] = {0x3145534e, 1, PS2_RAM_SIZE, PS2_SCRATCHPAD_SIZE,
                                       caller, list, tag, ctx->pc};
            uint32_t registers[32];
            for (unsigned reg = 0; reg < 32; ++reg) registers[reg] = GPR_U32(ctx, reg);
            writeCapture(file, header, sizeof(header));
            writeCapture(file, registers, sizeof(registers));
            writeCapture(file, ram, PS2_RAM_SIZE);
            writeCapture(file, runtime->memory().getScratchpad(), PS2_SCRATCHPAD_SIZE);
        });
    }
    trace.row("mesh_call", eeVs, caller, adjacentGrass ? latestContext.object : 0,
              list, 0, tag, 0, source, 0, 0,
              ramSpan(list, 64) ? fingerprint(ram + (list & 0x1fffffffu), 64) : 0,
              0, 0, 0, adjacentGrass ? latestContext.parent : 0,
              adjacentGrass ? latestContext.hash : 0, capture);
}
void meshCall(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t list = GPR_U32(ctx, 4), caller = GPR_U32(ctx, 31), stack = GPR_U32(ctx, 29);
    originalMeshCall(ram, ctx, runtime); // Verified leaf: appends CALL/MSCAL(0)/FLUSH.
    if (ctx->pc != caller || GPR_U32(ctx, 29) != stack || !trace.inRange()) return;
    const bool adjacentGrass = caller == 0x270e24 && latestContext.parent == 0x270e1c &&
        latestContext.eeVs == runtime->eeScheduler().currentVSyncTick();
    observeMesh(ram, ctx, runtime, kMeshCall, caller, list, GPR_U32(ctx, 8), adjacentGrass);
}
void visibleMesh(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    if (ctx->pc == kVisibleMesh && trace.inRange() && pendingMeshes.size() < 64) {
        const uint32_t object = GPR_U32(ctx, 5);
        pendingMeshes.push_back({GPR_U32(ctx, 29), GPR_U32(ctx, 31), object,
                                 read32(ram, object + 0x14), latestContext.serial});
    }
    originalVisibleMesh(ram, ctx, runtime);
    for (size_t i = pendingMeshes.size(); i-- > 0;) {
        const auto call = pendingMeshes[i];
        if (ctx->pc != call.caller || GPR_U32(ctx, 29) != call.stack) continue;
        uint32_t cursor = 0;
        std::memcpy(&cursor, runtime->memory().getScratchpad() + 0xd30, 4);
        const uint32_t tag = cursor - 16;
        // A culled object does not append this tag. Require a completed context
        // update from inside this invocation and validate the actual CALL.
        const bool adjacentGrass = latestContext.serial > call.contextSerial &&
            latestContext.parent == 0x260c04 && latestContext.stack == call.stack - 0xa0 &&
            latestContext.eeVs == runtime->eeScheduler().currentVSyncTick();
        if (adjacentGrass && ramSpan(tag, 16) && read32(ram, tag) == 0x50000000 &&
            read32(ram, tag + 4) == call.list && read32(ram, tag + 8) == 0x14000000 &&
            read32(ram, tag + 12) == 0x11000000)
            observeMesh(ram, ctx, runtime, kVisibleMesh, call.caller, call.list, tag, true);
        pendingMeshes.erase(pendingMeshes.begin() + i);
    }
}
}

void install(PS2Runtime& runtime, uint32_t crc) {
    const char* path = std::getenv("PS2X_TF_SEAM_TRACE");
    if (!path || !*path) return;
    if (crc != 0x74c982c0 || !runtime.hasFunction(kWriter) || !runtime.hasFunction(kAllocator) ||
        !runtime.hasFunction(kMeshCall) || !runtime.hasFunction(kVisibleMesh))
        throw std::runtime_error("Native seam probe requires the supported SLUS-20668 executable");
    if (enabled) throw std::runtime_error("Native seam probe already installed");
    if (const char* prefix = std::getenv("PS2X_TF_SEAM_CAPTURE")) {
        if (!*prefix) throw std::runtime_error("PS2X_TF_SEAM_CAPTURE requires a fresh private filename prefix");
        capturePrefix = prefix;
        if (const char* family = std::getenv("PS2X_TF_SEAM_FAMILY")) {
            if (std::strcmp(family, "grass") == 0) captureGrassOnly = true;
            else if (std::strcmp(family, "all") != 0)
                throw std::runtime_error("PS2X_TF_SEAM_FAMILY requires all or grass");
        }
        if (const char* count = std::getenv("PS2X_TF_SEAM_CALLS")) {
            unsigned value = 0;
            char extra = 0;
            if (std::sscanf(count, "%u%c", &value, &extra) != 1 || value < 1 || value > 6000)
                throw std::runtime_error("PS2X_TF_SEAM_CALLS requires 1..6000");
            captureLimit = value;
        }
        captureEnabled = true;
    }
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
    std::fprintf(trace.file, "event,sequence,present_vs,ee_vs,caller,object,address,qwords,destination,opcode,pc,top,itop,hash,code_hash,cycle,mode,parent,context_hash,capture\n");
    originalWriter = runtime.lookupFunction(kWriter);
    originalAllocator = runtime.lookupFunction(kAllocator);
    originalMeshCall = runtime.lookupFunction(kMeshCall);
    originalVisibleMesh = runtime.lookupFunction(kVisibleMesh);
    pending.reserve(64);
    pendingMeshes.reserve(64);
    for (uint32_t pc = kWriter; pc < kWriterEnd; pc += 4)
        if (runtime.hasFunction(pc) && runtime.lookupFunction(pc) == originalWriter)
            runtime.replaceFunction(pc, &writer);
    for (uint32_t pc = kVisibleMesh; pc < kVisibleMeshEnd; pc += 4)
        if (runtime.hasFunction(pc) && runtime.lookupFunction(pc) == originalVisibleMesh)
            runtime.replaceFunction(pc, &visibleMesh);
    runtime.replaceFunction(kAllocator, &allocate);
    runtime.replaceFunction(kMeshCall, &meshCall);
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

uint32_t vuBegin(uint32_t pc, bool continuing, uint32_t top, uint32_t itop,
                 const uint8_t* code, const uint8_t* data, const VU1State& state) {
    if (!captureEnabled || !trace.inRange() || captureCount.load(std::memory_order_relaxed) >= captureLimit) return 0;
    if (captureGrassOnly) {
        // Read owned VU memory on the execution thread, not VIF's producer.
        // Select a complete known grass stage, independent of its code address.
        uint32_t stages = 0, entry = 0;
        std::memcpy(&stages, data + 4*16, 4);
        std::memcpy(&entry, data + 5*16, 4);
        const uint32_t start = (entry & 0xffffu) * 8;
        if (!continuing || (pc != 0x10 && pc != 0xf8) || stages != 1 || start > 16384 - 3872 ||
            fingerprint(code + start, 64) != 0x2ad8a73a9581a552ull ||
            fingerprint(code + start, 3872) != 0xbbe99809fbf7361eull) return 0;
    }
    if (vuCapture.ticket) throw std::runtime_error("Nested native-seam VU capture");
    const uint32_t ticket = captureCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ticket > captureLimit) return 0;
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "-vu-%06u-", ticket);
    vuCapture.base = capturePrefix + suffix;
    vuCapture.word0 = pc | (continuing ? 0x80000000u : 0);
    vuCapture.top = top; vuCapture.itop = itop; vuCapture.packets = 0;
    vuDump(vuCapture.base + "pre.bin", vuCapture.word0, top, itop, code, data, state);
    vuCapture.gifFile = createCapture(vuCapture.base + "gif.bin");
    vuCapture.ticket = ticket;
    trace.row("vu_begin", 0, 0, 0, 0, 0, 0, continuing ? 0x17 : 0x14, pc, top, itop,
              fingerprint(data, 16384), fingerprint(code, 16384), 0, 0, 0, 0, ticket);
    return ticket;
}
void vuEnd(uint32_t ticket, const uint8_t* code, const uint8_t* data,
           const VU1State& state, bool needsTransfer, bool fellBack) {
    if (!ticket) return;
    if (ticket != vuCapture.ticket) throw std::runtime_error("Mismatched native-seam VU capture");
    vuCapture.ticket = 0;
    FILE* file = vuCapture.gifFile;
    vuCapture.gifFile = nullptr;
    if (std::fclose(file) != 0) throw std::runtime_error("Cannot finish native-seam GIF capture");
    vuDump(vuCapture.base + "post.bin", vuCapture.word0, vuCapture.top, vuCapture.itop, code, data, state);
    // Destination counts packets; cycle/mode mark pipeline transfer/fallback.
    // Those cases are not self-contained replacement fixtures.
    trace.row("vu_end", 0, 0, 0, 0, 0, vuCapture.packets, 0, state.pc, state.top, state.itop,
              fingerprint(data, 16384), fingerprint(code, 16384), needsTransfer, fellBack, 0, 0, ticket);
}
void gif(uint32_t path, const uint8_t* data, uint32_t bytes, bool drainImmediately, bool path2DirectHl) {
    if (!vuCapture.ticket || !data || !bytes) return;
    const uint32_t header[] = {path, bytes, uint32_t(drainImmediately), uint32_t(path2DirectHl)};
    writeCapture(vuCapture.gifFile, header, sizeof(header));
    writeCapture(vuCapture.gifFile, data, bytes);
    ++vuCapture.packets;
}
}
