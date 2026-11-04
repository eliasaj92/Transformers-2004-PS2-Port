// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_grass_memory.h"
#include <cstring>
#include <stdexcept>

namespace tf_native
{
void GrassDeferredMemory::discard(unsigned qword)
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
void GrassDeferredMemory::overwrite(unsigned first, unsigned count)
{
    if (first > 1024 || count > 1024 - first)
        throw std::runtime_error("Deferred grass write outside VU memory");
    if (!dirtyQwords)
        return;
    for (unsigned qword = first; qword < first + count; ++qword)
    {
        overwrittenQwords += cells[qword].slot != 255;
        discard(qword);
    }
}
void GrassDeferredMemory::read(uint8_t *data, unsigned first, unsigned count)
{
    if (!data || first > 1024 || count > 1024 - first)
        throw std::runtime_error("Deferred grass read outside VU memory");
    if (!dirtyQwords)
        return;
    gsnative::DualFloatScope floating;
    if (!floating.ready)
        throw std::runtime_error("Cannot restore deferred grass VU memory");
    for (unsigned qword = first; qword < first + count; ++qword)
    {
        const auto cell = cells[qword];
        if (cell.slot == 255)
            continue;
        auto &batch = batches[cell.slot];
        const unsigned index = cell.field * 8 + cell.sprite;
        if (!batch.computed[index])
        {
            batch.cache[index] = gsnative::grassRawSprite(batch.source->inputs.data(), cell.sprite, cell.field);
            batch.computed.set(index);
            ++materializedSprites;
        }
        const auto &raw = batch.cache[index];
        const auto &words = cell.kind == 1 ? raw.color : raw.xyz[cell.kind == 2 ? 1 : 0];
        std::memcpy(data + qword * 16, words.data(), 16);
        ++materializedQwords;
        discard(qword);
    }
}
void GrassDeferredMemory::record(uint8_t *data, unsigned base, gsnative::DualPacketPtr source)
{
    if (!data || base > 1024 - 76 || !source || source->kind != gsnative::GeometryKind::GrassSprites ||
        source->version != 2 || source->vertices != 8)
        throw std::runtime_error("Invalid deferred grass record");
    overwrite(base, 76);
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
    batch.remaining = 36;
    dirtyQwords += batch.remaining;
    ++recorded;
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned sprite = 0; sprite < 6; ++sprite)
        {
            const unsigned qword = base + 6 + field * 38 + sprite * 4;
            cells[qword] = {uint8_t(slot), uint8_t(sprite), uint8_t(field), 0};
            cells[qword + 2] = {uint8_t(slot), uint8_t(sprite), uint8_t(field), 1};
            cells[qword + 3] = {uint8_t(slot), uint8_t(sprite), uint8_t(field), 2};
        }
}
} // namespace tf_native
