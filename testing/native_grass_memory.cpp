// SPDX-License-Identifier: GPL-3.0-or-later
// Compare handwritten fallback/memory ownership with enhanced captured output.
#include "tf_grass_memory.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
std::vector<uint8_t> read(const std::filesystem::path &path)
{
    const auto bytes = std::filesystem::file_size(path);
    require(bytes && bytes <= 128 * 1024 * 1024, "Invalid private grass fixture size");
    std::vector<uint8_t> result(static_cast<size_t>(bytes));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char *>(result.data()), result.size());
    require(bool(file), "Cannot read grass fixture");
    return result;
}
void poison(uint8_t *packet)
{
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned sprite = 0; sprite < 6; ++sprite)
        {
            auto *p = packet + (6 + field * 38 + sprite * 4) * 16;
            std::memset(p, 0xcd, 16);
            std::memset(p + 32, 0xcd, 32);
        }
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        require(argc == 3, "usage: tf_grass_memory <private-inputs.bin> <private-packets.bin>");
        const auto inputs = read(argv[1]), packets = read(argv[2]);
        const size_t count = inputs.size() / 256;
        require(count && inputs.size() == count * 256 && packets.size() == count * 1216,
                "Grass fixture framing mismatch");
        uint64_t rawSprites = 0, partialWrites = 0, discarded = 0;
        for (size_t index = 0; index < count; ++index)
        {
            auto p = std::make_shared<gsnative::DualPacket>();
            p->kind = gsnative::GeometryKind::GrassSprites;
            p->version = 2;
            p->vertices = 8;
            std::memcpy(p->inputs.data(), inputs.data() + index * 256, 256);
            const auto *reference = packets.data() + index * 1216;
            {
                gsnative::DualFloatScope floating;
                require(floating.ready, "Cannot establish VU float mode");
                for (unsigned field = 0; field < 2; ++field)
                    for (unsigned sprite = 0; sprite < 8; ++sprite)
                    {
                        const auto raw = gsnative::grassRawSprite(p->inputs.data(), sprite, field);
                        const auto *v = reference + (6 + field * 38 + sprite * 4) * 16;
                        require(!std::memcmp(v, raw.xyz[0].data(), 16) && !std::memcmp(v + 32, raw.color.data(), 16) &&
                                    !std::memcmp(v + 48, raw.xyz[1].data(), 16),
                                "CPU grass materializer differs from accelerated captured geometry");
                        ++rawSprites;
                    }
            }
            std::array<uint8_t, 16384> data, expected;
            data.fill(0x5a);
            constexpr unsigned base = 128;
            std::memcpy(data.data() + base * 16, reference, 1216);
            expected = data;
            poison(data.data() + base * 16);
            auto restored = data;
            gsnative::materializePacket(*p, restored.data() + base * 16, 1216);
            require(restored == expected, "Packet fallback changed retained CPU output or framing");
            tf_native::GrassDeferredMemory memory;
            memory.record(data.data(), base, p);
            require(memory.dirtyQwords == 36, "Grass ownership did not cover all skipped qwords");
            // A masked XYZ write must resolve W first, even though all three
            // position lanes are then overwritten. Use alternating fields.
            const unsigned xyz = base + 14 + unsigned(index & 1) * 38;
            memory.read(data.data(), xyz, 1);
            const uint32_t newXYZ[3] = {0x1234, 0x5678, 0x9abc};
            std::memcpy(data.data() + xyz * 16, newXYZ, 12);
            std::memcpy(expected.data() + xyz * 16, newXYZ, 12);
            ++partialWrites;
            // A complete qword replacement must discard old ownership rather
            // than letting a later read overwrite the new value.
            const unsigned color = xyz + 2;
            memory.overwrite(color, 1);
            std::memset(data.data() + color * 16, 0x37, 16);
            std::memset(expected.data() + color * 16, 0x37, 16);
            memory.read(data.data());
            require(data == expected && !memory.dirtyQwords && memory.materializedQwords == 35 &&
                        memory.overwrittenQwords == 1,
                    "Partial/full writes changed grass data or failed to release ownership");
            discarded += memory.overwrittenQwords;
        }
        std::printf("GRASS_MEMORY packets=%zu raw_sprites=%llu partial_writes=%llu discarded_qwords=%llu PASS\n", count,
                    rawSprites, partialWrites, discarded);
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "GRASS_MEMORY FAIL %s\n", exception.what());
        return 1;
    }
}
