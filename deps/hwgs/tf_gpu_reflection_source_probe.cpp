// SPDX-License-Identifier: GPL-3.0-or-later
// Private accelerated captures qualify source fallback and conservative bounds.
#include "tf_hwgs.h"
#include "tf_gpu_dual_bounds.h"
#include "runtime/gs/gs_native_reflection_math.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <xmmintrin.h>

extern "C" TF_HWGS_API int tf_gs_reflection_source_probe(const gsnative::DualPacket *packet, const void *lighting,
                                                         const uint8_t *gif, uint32_t bytes, void *expanded,
                                                         uint32_t expandedBytes, uint64_t *stats, char *error,
                                                         uint32_t errorBytes)
{
    const auto require = [](bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto word = [](const uint8_t *p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    };
    try
    {
        require(packet && lighting && gif && expanded && stats, "Missing source probe buffers");
        require(gsnative::reflectionSourceValid(*packet) && packet->version == 2 &&
                    bytes == 4 * (4 + packet->vertices * 3) * 16,
                "Malformed source probe packet");
        std::fill_n(stats, 8, 0);
        const uint32_t before = _mm_getcsr();
        std::vector<uint8_t> restored(gif, gif + bytes);
        for (unsigned section = 0; section < 4; ++section)
            for (unsigned vertex = 0; vertex < packet->vertices; ++vertex)
            {
                auto *v = restored.data() + (section * (4 + packet->vertices * 3) + 4 + vertex * 3) * 16;
                std::memset(v, 0xa5, section ? 12 : 16);
                std::memset(v + 16, 0x5a, 32);
            }
        gsnative::materializeReflectionPacket(*packet, restored.data(), bytes);
        for (unsigned i = 0; i < bytes; i += 4)
            stats[0] += word(gif + i) != word(restored.data() + i);
        {
            gsnative::DualFloatScope floating;
            require(floating.ready, "Could not select VU floating point environment");
            for (unsigned vertex = 0; vertex < packet->vertices; ++vertex)
            {
                const auto lit = gsnative::reflectionLitVertex(*packet, vertex);
                const auto *actual = reinterpret_cast<const uint8_t *>(&lit);
                const auto *expected = static_cast<const uint8_t *>(lighting) + vertex * 48;
                for (unsigned i = 0; i < 48; i += 4)
                    stats[1] += word(actual + i) != word(expected + i);
            }
        }
        require(_mm_getcsr() == before, "Fallback changed caller FP environment");
        require(gsnative::expandReflectionInputs(*packet, static_cast<uint8_t *>(expanded), expandedBytes),
                "Could not expand source packet");
        std::array<tf_gpu::DualDrawBounds, 4> bounds;
        uint32_t nodes = 0, leaves = 0;
        const auto start = std::chrono::steady_clock::now();
        const bool accepted = tf_gpu::projectReflectionBounds(*packet, bounds, &nodes, &leaves);
        stats[2] = accepted;
        stats[3] = nodes;
        stats[4] = leaves;
        stats[7] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        require(_mm_getcsr() == before, "Bounds changed caller FP environment");
        if (accepted)
            for (unsigned section = 0; section < 4; ++section)
                for (unsigned vertex = 0; vertex < packet->vertices; ++vertex)
                {
                    const auto *v = gif + (section * (4 + packet->vertices * 3) + 4 + vertex * 3) * 16;
                    const auto &b = bounds[section];
                    const double q = std::bit_cast<float>(word(v + 8));
                    for (unsigned axis = 0; axis < 3; ++axis)
                    {
                        const auto xyz = axis < 2 ? word(v + 32 + axis * 4) & 65535u : (word(v + 40) >> 4) & 0xffffffu;
                        const double stq = std::bit_cast<float>(word(v + axis * 4));
                        require(xyz >= b.xyzMin[axis] && xyz <= b.xyzMax[axis], "Bounds exclude packed XYZ");
                        require(stq >= b.stq[axis].low && stq <= b.stq[axis].high, "Bounds exclude STQ");
                        if (axis < 2)
                            require(stq / q >= b.texture[axis].low && stq / q <= b.texture[axis].high,
                                    "Bounds exclude texture coordinate");
                        stats[5] += 2 + (axis < 2);
                    }
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const auto color = word(v + 16 + channel * 4) & 255;
                        require(color >= b.colorMin[channel] && color <= b.colorMax[channel], "Bounds exclude color");
                        ++stats[5];
                    }
                }
        // Decline malformed/unsupported inputs before any array access.
        for (unsigned mutation = 0; mutation < 6; ++mutation)
        {
            auto invalid = *packet;
            if (mutation == 0)
                invalid.vertices = gsnative::ReflectionMaxVertices + 1;
            if (mutation == 1)
                invalid.version = 0;
            if (mutation == 2)
                invalid.kind = gsnative::GeometryKind::DualTransform;
            if (mutation == 3)
                invalid.constants[140] = 1;
            if (mutation == 4)
            {
                uint32_t nan = 0x7fc00000;
                std::memcpy(invalid.inputs.data(), &nan, 4);
            }
            if (mutation == 5)
            {
                float opacity = -1;
                std::memcpy(invalid.constants.data() + 128, &opacity, 4);
            }
            require(!tf_gpu::projectReflectionBounds(invalid, bounds), "Invalid bounds input accepted");
            ++stats[6];
        }
        require(stats[0] == 0 && stats[1] == 0, "CPU source fallback differs from accelerated capture");
        return 1;
    }
    catch (const std::exception &e)
    {
        if (error && errorBytes)
            std::snprintf(error, errorBytes, "%s", e.what());
        return 0;
    }
}
