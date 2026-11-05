// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_reflection_memory.h"
#include <cstring>
#include <stdexcept>

namespace tf_native
{
namespace
{
void range(unsigned first, unsigned count)
{
    if (first > 1024 || count > 1024 - first)
        throw std::runtime_error("Deferred reflection access outside VU memory");
}
} // namespace
void ReflectionDeferredMemory::discard(unsigned qword)
{
    auto &cell = cells[qword];
    if (cell.slot == 255)
        return;
    auto &batch = batches[cell.slot];
    if (!--batch.remaining)
        batch.source.reset();
    cell = {};
    --dirtyQwords;
}
void ReflectionDeferredMemory::overwrite(unsigned first, unsigned count)
{
    range(first, count);
    if (!dirtyQwords)
        return;
    for (unsigned qword = first; qword < first + count; ++qword)
    {
        overwrittenQwords += cells[qword].slot != 255;
        discard(qword);
    }
}
void ReflectionDeferredMemory::readW(uint8_t *data, unsigned first, unsigned count)
{
    range(first, count);
    if (!data)
        throw std::runtime_error("Missing deferred reflection memory");
    if (!dirtyQwords)
        return;
    for (unsigned qword = first; qword < first + count; ++qword)
    {
        auto &cell = cells[qword];
        if (cell.slot == 255 || !(cell.mask & 8))
            continue;
        const auto &batch = batches[cell.slot];
        uint32_t value;
        if (!cell.kind)
            std::memcpy(&value, batch.source->inputs.data() + cell.vertex * 32 + 28, 4);
        else if (cell.kind == 1)
            value = batch.alpha;
        else
            value = gsnative::reflectionRestartFlags(*batch.source, cell.vertex);
        std::memcpy(data + qword * 16 + 12, &value, 4);
        cell.mask &= 7;
        ++preservedW;
    }
}
void ReflectionDeferredMemory::overwriteXYZ(uint8_t *data, unsigned first, unsigned count)
{
    readW(data, first, count);
    overwrite(first, count);
}
void ReflectionDeferredMemory::read(uint8_t *data, unsigned first, unsigned count)
{
    range(first, count);
    if (!data)
        throw std::runtime_error("Missing deferred reflection memory");
    if (!dirtyQwords)
        return;
    const unsigned last = first + count;
    while (first < last && cells[first].slot == 255)
        ++first;
    if (first == last)
        return;
    gsnative::DualFloatScope floating;
    if (!floating.ready)
        throw std::runtime_error("Cannot restore deferred reflection geometry");
    for (unsigned qword = first; qword < last; ++qword)
    {
        const auto cell = cells[qword];
        if (cell.slot == 255)
            continue;
        auto &batch = batches[cell.slot];
        if (!batch.litReady[cell.vertex])
        {
            batch.lit[cell.vertex] = gsnative::reflectionLitVertex(*batch.source, cell.vertex);
            batch.litReady.set(cell.vertex);
            ++litVertices;
        }
        const unsigned index = cell.vertex * 4 + cell.section;
        if (!batch.projectedReady[index])
        {
            batch.projected[index] =
                gsnative::reflectionProjectVertex(*batch.source, batch.lit[cell.vertex], cell.vertex, cell.section);
            batch.projectedReady.set(index);
            ++projectedVertices;
        }
        const auto &result = batch.projected[index];
        const auto *value = cell.kind == 0   ? result.stq.data()
                            : cell.kind == 1 ? result.color.data()
                                             : result.xyz.data();
        std::memcpy(data + qword * 16, value, 12);
        if (cell.mask & 8)
            std::memcpy(data + qword * 16 + 12, value + 3, 4);
        ++materializedQwords;
        discard(qword);
    }
}
void ReflectionDeferredMemory::record(uint8_t *data, unsigned base, unsigned resolvedTail,
                                      gsnative::DualPacketPtr source)
{
    if (!data || !source || !gsnative::reflectionSourceValid(*source) || source->version != 2 || resolvedTail < 3 ||
        resolvedTail > source->vertices || base < 4 || base - 4 + 4 * (4 + 3 * source->vertices) > 1024)
        throw std::runtime_error("Invalid deferred reflection record");
    const unsigned first = base - 4, qwords = 4 * (4 + 3 * source->vertices);
    for (unsigned qword = first; qword < first + qwords; ++qword)
        if (cells[qword].slot != 255 && (cells[qword].mask & 8))
            throw std::runtime_error("Reflection overwrite did not preserve old W lanes before execution");
    overwrite(first, qwords);
    const unsigned unresolved = source->vertices - resolvedTail;
    if (!unresolved)
        return;
    unsigned slot = 0;
    while (slot < batches.size() && batches[slot].remaining)
        ++slot;
    if (slot == batches.size())
    {
        read(data);
        slot = 0;
    }
    auto &batch = batches[slot];
    batch.source = std::move(source);
    batch.litReady.reset();
    batch.projectedReady.reset();
    // The last projected color is already resolved by the accelerated CPU
    // tail. Its alpha is constant across every vertex and section.
    std::memcpy(&batch.alpha, data + (base + (batch.source->vertices - 1) * 3 + 1) * 16 + 12, 4);
    batch.remaining = 12 * unresolved;
    dirtyQwords += batch.remaining;
    ++recorded;
    for (unsigned section = 0; section < 4; ++section)
        for (unsigned vertex = 0; vertex < unresolved; ++vertex)
            for (unsigned kind = 0; kind < 3; ++kind)
            {
                const unsigned qword = base + section * (4 + 3 * batch.source->vertices) + vertex * 3 + kind;
                cells[qword] = {uint8_t(slot), uint8_t(vertex), uint8_t(section), uint8_t(kind),
                                uint8_t(kind || !section ? 15 : 7)};
            }
}
} // namespace tf_native
