// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_dual_memory.h"
#include <cstring>
#include <stdexcept>

namespace tf_native
{
void DualDeferredMemory::discard(unsigned qword)
{
    auto &cell = cells[qword];
    if (cell.slot == 255)
        return;
    auto &batch = batches[cell.slot];
    if (!--batch.remaining)
        batch.source.reset();
    cell.slot = 255;
    --dirtyQwords;
}
void DualDeferredMemory::overwrite(unsigned first, unsigned count)
{
    if (first > 1024 || count > 1024 - first)
        throw std::runtime_error("Deferred geometry write outside VU memory");
    if (!dirtyQwords)
        return;
    for (unsigned qword = first; qword < first + count; ++qword)
    {
        overwrittenQwords += cells[qword].slot != 255;
        discard(qword);
    }
}
void DualDeferredMemory::read(uint8_t *data, unsigned first, unsigned count)
{
    if (!data || first > 1024 || count > 1024 - first)
        throw std::runtime_error("Deferred geometry read outside VU memory");
    if (!dirtyQwords)
        return;
    gsnative::DualFloatScope floating;
    if (!floating.ready)
        throw std::runtime_error("Cannot restore deferred VU geometry");
    for (unsigned qword = first; qword < first + count; ++qword)
    {
        const auto cell = cells[qword];
        if (cell.slot == 255)
            continue;
        auto &batch = batches[cell.slot];
        const unsigned index = cell.vertex * 2 + cell.field;
        if (!batch.computed[index])
        {
            batch.cache[index] = gsnative::dualRawVertex(*batch.source, cell.vertex, cell.field);
            batch.computed.set(index);
            ++materializedVertices;
        }
        const auto &result = batch.cache[index];
        std::memcpy(data + qword * 16, cell.xyz ? result.xyz.data() : result.stq.data(), 12);
        ++materializedQwords;
        discard(qword);
    }
}
void DualDeferredMemory::record(uint8_t *data, uint32_t itop, uint32_t resolvedTail, gsnative::DualPacketPtr source)
{
    if (!source || source->vertices < 3 || source->vertices > 28 || resolvedTail > source->vertices || itop < 3 ||
        itop + 6 * source->vertices + 5 > 1023)
        throw std::runtime_error("Invalid deferred VU geometry record");
    // Every word in these output packets has been replaced or retained from a
    // previously resolved input. In particular, new colors may overlap an old
    // batch's STQ/XYZ; leaving that ownership behind would corrupt later reads.
    overwrite(itop - 3, 6 * source->vertices + 8);
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
    batch.computed.reset();
    batch.remaining = 4 * unresolved;
    dirtyQwords += batch.remaining;
    ++recorded;
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned vertex = 0; vertex < unresolved; ++vertex)
        {
            const unsigned qword = itop + 1 + field * (3 * batch.source->vertices + 4) + vertex * 3;
            cells[qword] = {uint8_t(slot), uint8_t(vertex), uint8_t(field), 0};
            cells[qword + 2] = {uint8_t(slot), uint8_t(vertex), uint8_t(field), 1};
        }
}
} // namespace tf_native
