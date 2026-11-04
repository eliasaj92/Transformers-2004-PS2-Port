// SPDX-License-Identifier: GPL-3.0-or-later
// Compare the accelerated package against private captured outputs; no interpreter oracle.
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <memory>

bool ps2Vu1StaticPrepare(const uint8_t*, uint32_t, const PS2Memory*, uint64_t, uint32_t);
bool ps2Vu1StaticRun(int, void*, bool, uint8_t*, uint8_t*, GS&, PS2Memory*, uint32_t, uint32_t, uint32_t, uint32_t, void*);
uint64_t ps2Vu1StaticFallbackCalls();
#ifdef TF_REPLAY_GRASS_TAIL
void grassReplayInitialize();
void grassReplayFinish();
bool grassTailCapture(uint32_t, const uint8_t*, uint32_t, bool, bool);
#endif
namespace fs = std::filesystem;
namespace {
std::vector<uint8_t> packets;
struct Dump {
    std::array<uint32_t, 4> header;
    std::array<uint8_t, 16384> code, data;
    VU1State state;
    explicit Dump(const fs::path& path) {
        static_assert(sizeof(state) == 664);
        std::ifstream file(path, std::ios::binary);
        file.read(reinterpret_cast<char*>(header.data()), sizeof(header));
        file.read(reinterpret_cast<char*>(code.data()), code.size());
        file.read(reinterpret_cast<char*>(data.data()), data.size());
        file.read(reinterpret_cast<char*>(&state), sizeof(state));
        if (!file || file.peek() != EOF || header[3] != sizeof(state))
            throw std::runtime_error("Malformed private VU capture: " + path.string());
        if ((header[0] & 0x7fffffffu) >= code.size() || (header[0] & 7) || header[1] >= 1024 || header[2] >= 1024)
            throw std::runtime_error("Invalid VU entry or data address: " + path.string());
    }
};
std::vector<uint8_t> readBytes(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > 64*1024*1024) throw std::runtime_error("Fixture exceeds 64 MiB");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!file) throw std::runtime_error("Cannot read fixture: " + path.string());
    return bytes;
}
bool meaningfulStateEqual(const VU1State& a, const VU1State& b) {
    static_assert(offsetof(VU1State, top) == 640 && offsetof(VU1State, branchTarget) == 652 &&
                  offsetof(VU1State, branchDelay) == 656);
    const auto* x = reinterpret_cast<const uint8_t*>(&a);
    const auto* y = reinterpret_cast<const uint8_t*>(&b);
    // Windows x64 ABI padding only; cycles and every semantic field remain compared.
    for (size_t offset = 0; offset < sizeof(a); ++offset) {
        if (offset == 638 || offset == 639 || (offset >= 649 && offset <= 651) || offset >= 660) continue;
        if (x[offset] != y[offset]) return false;
    }
    return true;
}
void applyObservedExternalWrites(VU1State& carry, const VU1State& before, const VU1State& previousPost) {
    // A VIF/host write can affect one component. Compare semantic fields, not
    // padding or whole vectors, which could accidentally erase a perturbation.
    const auto adopt = [](auto& target, const auto& pre, const auto& post) {
        if (std::memcmp(&pre, &post, sizeof(pre))) std::memcpy(&target, &pre, sizeof(pre));
    };
    for (unsigned reg = 0; reg < 32; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            adopt(carry.vf[reg][lane], before.vf[reg][lane], previousPost.vf[reg][lane]);
    for (unsigned reg = 0; reg < 16; ++reg) adopt(carry.vi[reg], before.vi[reg], previousPost.vi[reg]);
    for (unsigned lane = 0; lane < 4; ++lane) adopt(carry.acc[lane], before.acc[lane], previousPost.acc[lane]);
#define ADOPT(field) adopt(carry.field, before.field, previousPost.field)
    ADOPT(q); ADOPT(p); ADOPT(i); ADOPT(r); ADOPT(pc); ADOPT(mac); ADOPT(clip); ADOPT(status); ADOPT(cycles);
    ADOPT(ebit); ADOPT(haltAfterDelaySlot); ADOPT(dBitEnabled); ADOPT(tBitEnabled);
    ADOPT(stoppedByD); ADOPT(stoppedByT); ADOPT(top); ADOPT(itop);
    ADOPT(branchPending); ADOPT(branchTarget); ADOPT(branchDelay);
#undef ADOPT
}
uint32_t word(const std::array<uint8_t, 16384>& data, size_t offset) {
    uint32_t value; std::memcpy(&value, data.data()+offset, 4); return value;
}
uint64_t fingerprint(const uint8_t* data, size_t bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
    return hash;
}
bool grass(const Dump& dump) {
    if ((dump.header[0] != 0x80000010u && dump.header[0] != 0x800000f8u) || word(dump.data, 64) != 1) return false;
    const uint32_t start = (word(dump.data, 80)&0xffffu)*8;
    return start <= 16384-3872 && fingerprint(dump.code.data()+start, 64) == 0x2ad8a73a9581a552ull &&
        fingerprint(dump.code.data()+start, 3872) == 0xbbe99809fbf7361eull;
}
bool controlEqual(const VU1State& a, const VU1State& b) {
    return a.pc == b.pc && a.cycles == b.cycles && a.ebit == b.ebit && a.haltAfterDelaySlot == b.haltAfterDelaySlot &&
        a.stoppedByD == b.stoppedByD && a.stoppedByT == b.stoppedByT && a.branchPending == b.branchPending &&
        a.branchTarget == b.branchTarget && a.branchDelay == b.branchDelay && a.top == b.top && a.itop == b.itop;
}
}

void GS::processGIFPacket(const uint8_t*, uint32_t) {
    throw std::runtime_error("Unexpected unframed GS output");
}
void PS2Memory::submitGifPacket(GifPathId path, const uint8_t* bytes, uint32_t count, bool drain, bool direct) {
#ifdef TF_REPLAY_GRASS_TAIL
    if (grassTailCapture(uint32_t(path), bytes, count, drain, direct)) return;
#endif
    if (packets.size()+count+16 > 64*1024*1024) throw std::runtime_error("Captured replay exceeds 64 MiB");
    const uint32_t metadata[] = {uint32_t(path), count, uint32_t(drain), uint32_t(direct)};
    const auto* prefix = reinterpret_cast<const uint8_t*>(metadata);
    packets.insert(packets.end(), prefix, prefix+sizeof(metadata));
    packets.insert(packets.end(), bytes, bytes+count);
}

#include "dual_replay.h"

int main(int argc, char** argv) {
    try {
#ifdef TF_REPLAY_GRASS_TAIL
        grassReplayInitialize();
#endif
        if (argc != 3 && argc != 4 && argc != 8) throw std::runtime_error("usage: tf_vu1_replay <private-pre-file-list> <fresh-report.csv> [restore-grass-vf-mask-hex | --gpu-dual selection inputs batches capture]");
        std::unique_ptr<DualReplay> dual;
        if (argc == 8) {
            if (std::string(argv[3]) != "--gpu-dual") throw std::runtime_error("Unknown replay mode");
            dual = std::make_unique<DualReplay>(argv[4], argv[5], argv[6], argv[7]);
        }
        uint32_t restoreMask = 0;
        if (argc == 4) {
            const std::string text(argv[3]);
            const auto parsed = std::from_chars(text.data(), text.data()+text.size(), restoreMask, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || !restoreMask || (restoreMask & 31))
                throw std::runtime_error("Register experiment requires a nonzero hexadecimal mask of VF5..VF31");
        }
        _putenv_s("PS2X_VU1_FAST", "1");
        _putenv_s("PS2X_VU1_RUN_TO_END", "0");
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        if (fs::exists(argv[2])) throw std::runtime_error("Use a fresh private report path");
        std::ifstream list(argv[1]);
        std::ofstream report(argv[2]);
        if (!list || !report) throw std::runtime_error("Cannot open fixture list or report");
        report << "fixture,data,state,code,packets,control,guest_cycles,host_cycles,restored_grass,gpu_dual\n";
        std::array<uint8_t, 16384> resident{};
        std::array<uint8_t, 80000> transfer{};
        PS2Memory memory;
        memory.vu1Code = resident.data();
        GS gs;
        size_t total = 0, failures = 0;
        size_t restored = 0, differentState = 0;
        VU1State carry{}, previousPost{};
        std::array<uint8_t, 16384> carriedData{}, previousData{};
        fs::path previousPath;
        uint64_t bytes = 0;
        std::string name;
        while (std::getline(list, name)) {
            if (!name.empty() && name.back() == '\r') name.pop_back();
            if (name.empty()) continue;
            if (!name.ends_with("-pre.bin")) throw std::runtime_error("Expected a -pre.bin fixture");
            const std::string stem = name.substr(0, name.size()-7);
            Dump before(name), expected(stem+"post.bin");
            if ((restoreMask || dual) && total) {
                // Require consecutive tickets. The caller must also verify an
                // unfiltered capture: filenames cannot reveal omitted programs.
                const auto currentPath = fs::path(name);
                const auto ticket = [](const fs::path& p) {
                    const auto s = p.filename().string();
                    const auto start = s.rfind("-vu-");
                    if (start == std::string::npos || s.size() != start+18) throw std::runtime_error("Invalid capture ticket name");
                    unsigned value = 0;
                    const auto parsed = std::from_chars(s.data()+start+4, s.data()+start+10, value);
                    if (parsed.ec != std::errc{} || parsed.ptr != s.data()+start+10) throw std::runtime_error("Invalid capture ticket number");
                    return std::pair{s.substr(0, start), value};
                };
                const auto currentTicket = ticket(currentPath), previousTicket = ticket(previousPath);
                if (currentPath.parent_path() != previousPath.parent_path() || currentTicket.first != previousTicket.first ||
                    currentTicket.second != previousTicket.second+1)
                    throw std::runtime_error("Carried-state replay requires consecutive broad-capture fixtures");
                // Apply observed changes made outside the preceding VU call.
                // Equal-value external writes are not observable in snapshots;
                // retaining a perturbation there is conservative, not a proof of liveness.
                applyObservedExternalWrites(carry, before.state, previousPost);
                if (dual) {
                    for (size_t offset = 0; offset < 16384; offset += 4)
                        if (std::memcmp(before.data.data() + offset, previousData.data() + offset, 4))
                            std::memcpy(carriedData.data() + offset, before.data.data() + offset, 4);
                }
            } else { carry = before.state; carriedData = before.data; }
            const auto expectedPackets = readBytes(stem+"gif.bin");
            if (resident != before.code) { resident = before.code; ++memory.gen1; }
            const bool continuing = (before.header[0] & 0x80000000u) != 0;
            const uint32_t entry = before.header[0] & 0x7fffffffu;
            if (!ps2Vu1StaticPrepare(resident.data(), resident.size(), &memory, memory.gen1, entry))
                throw std::runtime_error("Accelerated package does not admit fixture: " + name);
            auto actual = before;
            if (restoreMask || dual) actual.state = carry;
            if (dual) actual.data = carriedData;
            const auto callInput = actual.state;
            const auto dataInput = actual.data;
            packets.clear();
            const uint64_t fallback = ps2Vu1StaticFallbackCalls();
            ULONG64 start = 0, finish = 0;
            if (!QueryThreadCycleTime(GetCurrentThread(), &start)) throw std::runtime_error("Cannot read thread cycles");
            std::bitset<1024> geometry;
            const bool native = dual && dual->replace(name, actual, memory, geometry);
            const bool pending = !native && ps2Vu1StaticRun(0, &actual.state, continuing, resident.data(), actual.data.data(),
                gs, &memory, entry, before.header[1], before.header[2], 40000000, transfer.data());
            if (!QueryThreadCycleTime(GetCurrentThread(), &finish)) throw std::runtime_error("Cannot read thread cycles");
            if (pending || ps2Vu1StaticFallbackCalls() != fallback)
                throw std::runtime_error("Replay transferred pending work or fell back: " + name);
            bool dataOk = actual.data == expected.data;
            if (dual) {
                dataOk = true;
                for (size_t offset = 0; offset < 16384; offset += 4) {
                    if (geometry[offset / 16] && offset % 16 != 12) continue;
                    const bool wrote = std::memcmp(dataInput.data() + offset, actual.data.data() + offset, 4) ||
                        std::memcmp(before.data.data() + offset, expected.data.data() + offset, 4);
                    if (wrote && std::memcmp(actual.data.data() + offset, expected.data.data() + offset, 4)) dataOk = false;
                }
            }
            const bool stateOk = meaningfulStateEqual(actual.state, expected.state);
            const bool codeOk = resident == expected.code, packetsOk = packets == expectedPackets;
            const bool controlOk = controlEqual(actual.state, expected.state);
            differentState += !stateOk;
            const bool restore = restoreMask && grass(before);
            report << fs::path(name).filename().string() << ',' << dataOk << ',' << stateOk << ',' << codeOk << ','
                   << packetsOk << ',' << controlOk << ',' << actual.state.cycles-before.state.cycles << ',' << finish-start << ',' << restore << ',' << native << '\n';
            if (!report) throw std::runtime_error("Cannot write replay report");
            bytes += packets.size(); ++total;
            if (!dataOk || (!restoreMask && !dual && !stateOk) || !codeOk || !packetsOk || !controlOk) {
                ++failures;
                std::fprintf(stderr, "FAIL %s data=%d state=%d code=%d packets=%d control=%d\n", fs::path(name).filename().string().c_str(),
                    dataOk, stateOk, codeOk, packetsOk, controlOk);
                if (!dataOk) {
                    std::fprintf(stderr, "  differing VU data qwords:");
                    unsigned count = 0;
                    for (unsigned qword = 0; qword < 1024; ++qword)
                        if (std::memcmp(actual.data.data()+qword*16, expected.data.data()+qword*16, 16)) {
                            if (count < 16) std::fprintf(stderr, " %u", qword);
                            ++count;
                        }
                    std::fprintf(stderr, " (total %u)\n", count);
                }
                if (restoreMask || dual) break; // First affected successor is the actionable boundary.
            }
            if (restore) {
                ++restored;
                for (unsigned reg = 5; reg < 32; ++reg)
                    if (restoreMask & (1u << reg)) std::memcpy(actual.state.vf[reg], callInput.vf[reg], 16);
            }
            carry = actual.state; previousPost = expected.state; previousPath = name;
            carriedData = actual.data; previousData = expected.data;
            if ((total & 255) == 0) { std::printf("REPLAY progress fixtures=%zu failures=%zu\n", total, failures); std::fflush(stdout); }
        }
        if (!total) throw std::runtime_error("Empty fixture list");
        if (dual && !failures) dual->finish();
#ifdef TF_REPLAY_GRASS_TAIL
        if (!failures) grassReplayFinish();
#endif
        std::printf("REPLAY final mode=%s fixtures=%zu failures=%zu packet_bytes=%llu fallback=0 pending=0 restore_mask=%08x restored_calls=%zu differing_states=%zu gpu_dual_calls=%zu\n",
                    dual ? "gpu-dual-bypass" : restoreMask ? "register-experiment" : "exact", total, failures,
                    static_cast<unsigned long long>(bytes), restoreMask, restored, differentState, dual ? dual->replaced() : 0);
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "REPLAY ERROR: %s\n", error.what()); return 1;
    }
}
