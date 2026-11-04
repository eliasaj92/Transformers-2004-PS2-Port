// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_grass_packets.h"
#include <cstring>

namespace tf_native {
static_assert(sizeof(GrassPacket) == 1216);
namespace {
bool layout(const GrassPacket& packet) {
    constexpr unsigned offsets[] = {0, 3, 5, 38, 41, 43};
    constexpr uint64_t registers[] = {0xe, 2, 0x4124, 0xe, 2, 0x4124};
    constexpr unsigned loops[] = {2, 1, 8, 2, 1, 8};
    for (unsigned tag = 0; tag < 6; ++tag) {
        uint64_t low, high;
        std::memcpy(&low, packet[offsets[tag]].data(), 8);
        std::memcpy(&high, packet[offsets[tag]].data()+2, 8);
        if ((low & 0x7fff) != loops[tag] || ((low >> 58) & 3) != 0 ||
            (low >> 60) != (tag % 3 == 2 ? 4u : 1u) || high != registers[tag]) return false;
    }
    return true;
}
}

bool prepareGrassPackets(std::span<const uint8_t, 16384> data, uint32_t itop,
                         uint32_t constants, GrassPacketTemplates& output) {
    // Two adjacent 76-qword buffers start 85 qwords after ITOP.
    if (itop > 1024-237 || constants > 1024-23) return false;
    std::memcpy(output.empty.data(), data.data()+1023*16, 16);
    if ((output.empty[0] & 0x7fff) != 0) return false;
    for (unsigned buffer = 0; buffer < 2; ++buffer) {
        auto& packet = output.buffers[buffer];
        std::memcpy(packet.data(), data.data()+(itop+85+buffer*76)*16, sizeof(packet));
        for (unsigned field = 0; field < 2; ++field) {
            // The context supplies three tags, two A+D writes and initial ST.
            std::memcpy(packet.data()+field*38, data.data()+(constants+11+field*6)*16, 96);
            for (unsigned sprite = 0; sprite < 8; ++sprite) {
                auto& st = packet[7+field*38+sprite*4];
                if (sprite & 1) st[0] = st[1] = 0x3f000000u; // Center.
                else {
                    st[0] = sprite == 2 || sprite == 4 ? 0x3f800000u : 0;
                    st[1] = sprite == 4 || sprite == 6 ? 0x3f800000u : 0;
                }
                st[2] = 0x3f800000u;
                // The shader writes XYZ only here. Preserve the input W bits,
                // even though packed ST does not consume this padding lane.
            }
        }
        if (!layout(packet)) return false;
    }
    return true;
}

void writeGrassPacket(const GrassPacketTemplates& templates, const GrassBatch& batch,
                      std::size_t index, GrassPacket& output) {
    output = templates.buffers[index & 1];
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned sprite = 0; sprite < 8; ++sprite) {
            const unsigned base = 6+field*38+sprite*4;
            output[base] = batch.positions[field][sprite*2];
            output[base+2] = batch.colors[sprite];
            output[base+3] = batch.positions[field][sprite*2+1];
        }
}
}
