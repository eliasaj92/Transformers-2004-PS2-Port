// SPDX-License-Identifier: GPL-3.0-or-later
// Deferred guest-memory checks using private accelerated source/GIF captures.
#include "tf_reflection_memory.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace
{
constexpr unsigned MaxBytes = 4480;
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
std::vector<uint8_t> read(const char *path)
{
    const auto bytes = std::filesystem::file_size(path);
    require(bytes && bytes <= 128 * 1024 * 1024, "Invalid private reflection fixture size");
    std::vector<uint8_t> result(size_t(bytes), 0);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char *>(result.data()), result.size());
    require(bool(file), "Cannot read reflection fixture");
    return result;
}
void poison(uint8_t *data, unsigned base, unsigned count, unsigned tail)
{
    for (unsigned section = 0; section < 4; ++section)
        for (unsigned vertex = 0; vertex < count - tail; ++vertex)
        {
            auto *v = data + (base + section * (4 + 3 * count) + vertex * 3) * 16;
            std::memset(v, 0xcd, section ? 12 : 16);
            std::memset(v + 16, 0xcd, 32);
        }
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        require(argc == 3, "usage: tf_reflection_memory <private-sources.bin> <private-packets.bin>");
        const auto sources = read(argv[1]), packets = read(argv[2]);
        const size_t count = sources.size() / sizeof(gsnative::DualPacket);
        require(count && sources.size() == count * sizeof(gsnative::DualPacket) && packets.size() == count * MaxBytes,
                "Reflection fixture framing mismatch");
        uint64_t preserved = 0, partial = 0, projected = 0, overlaps = 0, rejected = 0;
        auto memory = std::make_unique<tf_native::ReflectionDeferredMemory>();
        for (size_t index = 0; index < count; ++index)
        {
            auto p = std::make_shared<gsnative::DualPacket>();
            std::memcpy(p.get(), sources.data() + index * sizeof(*p), sizeof(*p));
            require(gsnative::reflectionSourceValid(*p), "Invalid reflection source");
            const auto *gif = packets.data() + index * MaxBytes;
            const unsigned bytes = 4 * (4 + 3 * p->vertices) * 16;
            constexpr unsigned base = 128, tail = 3;
            std::array<uint8_t, 16384> data, expected;
            data.fill(0x5a);
            std::memcpy(data.data() + (base - 4) * 16, gif, bytes);
            expected = data;
            poison(data.data(), base, p->vertices, tail);
            memory->record(data.data(), base, tail, p);
            const auto initial = memory->dirtyQwords;
            require(initial == (p->vertices - tail) * 12, "Incomplete reflection ownership");
            if (!initial)
                continue;
            const auto beforeProjected = memory->projectedVertices, beforeLit = memory->litVertices;
            memory->readW(data.data());
            for (unsigned qword = 0; qword < 1024; ++qword)
                require(!std::memcmp(data.data() + qword * 16 + 12, expected.data() + qword * 16 + 12, 4),
                        "Source-only W restoration changed a retained lane");
            require(memory->dirtyQwords == initial && memory->projectedVertices == beforeProjected &&
                        memory->litVertices == beforeLit,
                    "W preservation recalculated geometry");
            const unsigned qword = base + unsigned(index % 4) * (4 + 3 * p->vertices) + unsigned(index % 3);
            memory->overwriteXYZ(data.data(), qword, 1);
            const uint32_t xyz[] = {0x12345678u, 0x00002468u, 0x9abcdef0u};
            std::memcpy(data.data() + qword * 16, xyz, 12);
            std::memcpy(expected.data() + qword * 16, xyz, 12);
            memory->overwrite(qword + 3, 1);
            std::memset(data.data() + (qword + 3) * 16, 0x37, 16);
            std::memset(expected.data() + (qword + 3) * 16, 0x37, 16);
            memory->read(data.data());
            require(data == expected && !memory->dirtyQwords, "Partial/full overwrite corrupted reflection memory");
            ++partial;

            // An overlapping packet must preserve old W before its accelerated
            // writes, then release obsolete geometry without corrupting new data.
            data.fill(0x6b);
            std::memcpy(data.data() + (base - 4) * 16, gif, bytes);
            expected = data;
            poison(data.data(), base, p->vertices, tail);
            memory->record(data.data(), base, tail, p);
            bool declined = false;
            try
            {
                memory->record(data.data(), base + 5, tail, p);
            }
            catch (const std::runtime_error &)
            {
                declined = true;
            }
            require(declined && memory->dirtyQwords == initial, "Unpreserved W overwrite was accepted");
            ++rejected;
            memory->readW(data.data());
            std::memcpy(data.data() + (base + 1) * 16, gif, bytes);
            std::memcpy(expected.data() + (base + 1) * 16, gif, bytes);
            poison(data.data(), base + 5, p->vertices, tail);
            memory->record(data.data(), base + 5, tail, p);
            memory->read(data.data());
            require(data == expected && !memory->dirtyQwords, "Overlapping reflection records corrupted guest data");
            ++overlaps;

            // Fully overwritten packets incur no lighting/projection work.
            std::memcpy(data.data() + (base - 4) * 16, gif, bytes);
            poison(data.data(), base, p->vertices, tail);
            memory->record(data.data(), base, tail, p);
            const auto checkpoint = memory->projectedVertices;
            memory->overwriteXYZ(data.data(), base - 4, bytes / 16);
            require(!memory->dirtyQwords && memory->projectedVertices == checkpoint,
                    "Replacement unnecessarily calculated reflection vertices");
        }
        preserved = memory->preservedW;
        projected = memory->projectedVertices;
        std::printf("REFLECTION_MEMORY packets=%zu preserved_w=%llu partial_writes=%llu overlaps=%llu rejected=%llu "
                    "lit_vertices=%llu projected_vertices=%llu dirty=%u PASS\n",
                    count, preserved, partial, overlaps, rejected, memory->litVertices, projected, memory->dirtyQwords);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "REFLECTION_MEMORY FAIL %s\n", e.what());
        return 1;
    }
}
