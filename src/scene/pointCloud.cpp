#include "rasm/pointCloud.h"
#include "assets/meshBuild.h"
#include "scene/pointCloudInternal.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace rasm
{
    namespace
    {
        constexpr float QUANTIZATION_STEPS = 65535.0f;

        // The 16 bits of v spread to every third bit of 48.
        uint64_t spreadBits(uint32_t v)
        {
            uint64_t x = v & 0xFFFFu;
            x = (x | x << 32) & 0x001F00000000FFFFull;
            x = (x | x << 16) & 0x001F0000FF0000FFull;
            x = (x | x << 8) & 0x100F00F00F00F00Full;
            x = (x | x << 4) & 0x10C30C30C30C30C3ull;
            x = (x | x << 2) & 0x1249249249249249ull;
            return x;
        }

        uint32_t reverseBits(uint32_t value, uint32_t bits)
        {
            uint32_t reversed = 0;
            for (uint32_t i = 0; i < bits; ++i)
                reversed |= ((value >> i) & 1u) << (bits - 1 - i);
            return reversed;
        }

        uint16_t quantize(float value, float first, float step)
        {
            if (!(step > 0.0f))
                return 0;
            return static_cast<uint16_t>(std::clamp(std::round((value - first) / step), 0.0f, QUANTIZATION_STEPS));
        }
    }

    uint64_t spreadMortonBits(uint32_t v)
    {
        uint64_t x = v & 0x1FFFFFu;
        x = (x | x << 32) & 0x1F00000000FFFFull;
        x = (x | x << 16) & 0x1F0000FF0000FFull;
        x = (x | x << 8) & 0x100F00F00F00F00Full;
        x = (x | x << 4) & 0x10C30C30C30C30C3ull;
        x = (x | x << 2) & 0x1249249249249249ull;
        return x;
    }

    uint32_t compactMortonBits(uint64_t x)
    {
        x &= 0x1249249249249249ull;
        x = (x ^ (x >> 2)) & 0x10C30C30C30C30C3ull;
        x = (x ^ (x >> 4)) & 0x100F00F00F00F00Full;
        x = (x ^ (x >> 8)) & 0x1F0000FF0000FFull;
        x = (x ^ (x >> 16)) & 0x1F00000000FFFFull;
        x = (x ^ (x >> 32)) & 0x1FFFFFull;
        return static_cast<uint32_t>(x);
    }

    // The cell size at which the points take half as many cells as there are points: the cells of each octree level of
    // their cube are counted at once along a Morton order, and the size between the two levels around that count
    // follows the growth from one to the next (4 for a surface, 8 for a volume). For a random sample of a surface that is
    // 1.2 times its mean spacing (the square root of its area over its points), for a grid of step h 1.4 h: a little
    // more than the mean, as a random sample has gaps wider than it.
    float measurePointSpacing(const glm::vec3 *positions, uint32_t count)
    {
        if (positions == nullptr || count < 2)
            return 0.0f;
        glm::vec3 low = positions[0];
        glm::vec3 high = positions[0];
        for (uint32_t i = 1; i < count; ++i)
        {
            low = glm::min(low, positions[i]);
            high = glm::max(high, positions[i]);
        }
        const float extent = std::max(std::max(high.x - low.x, high.y - low.y), high.z - low.z);
        if (!(extent > 0.0f) || !std::isfinite(extent))
            return 0.0f;
        const float scale = static_cast<float>((1u << MORTON_LEVELS) - 1) / extent;
        std::vector<uint64_t> codes(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const glm::vec3 cell = glm::clamp((positions[i] - low) * scale, glm::vec3(0.0f), glm::vec3(static_cast<float>((1u << MORTON_LEVELS) - 1)));
            codes[i] = spreadMortonBits(static_cast<uint32_t>(cell.x)) | spreadMortonBits(static_cast<uint32_t>(cell.y)) << 1 | spreadMortonBits(static_cast<uint32_t>(cell.z)) << 2;
        }
        std::sort(codes.begin(), codes.end());
        // cells[L]: the cells of level L the points take (level L splits the cube into 2^L cells a side).
        uint64_t cells[MORTON_LEVELS + 1] = {};
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t first = 0;
            if (i > 0)
            {
                const uint64_t differ = codes[i] ^ codes[i - 1];
                first = differ == 0 ? MORTON_LEVELS + 1 : MORTON_LEVELS - static_cast<uint32_t>(std::bit_width(differ) - 1) / 3;
            }
            for (uint32_t level = first; level <= MORTON_LEVELS; ++level)
                ++cells[level];
        }
        const double target = 0.5 * static_cast<double>(count);
        for (uint32_t level = 0; level < MORTON_LEVELS; ++level)
        {
            if (static_cast<double>(cells[level + 1]) < target)
                continue;
            const double size = static_cast<double>(extent) / static_cast<double>(1u << level);
            const double growth = std::log2(static_cast<double>(cells[level + 1]) / static_cast<double>(cells[level]));
            return static_cast<float>(growth > 0.0 ? size * std::pow(static_cast<double>(cells[level]) / target, 1.0 / growth) : size);
        }
        return extent / static_cast<float>(1u << MORTON_LEVELS);
    }

    // Each prefix of the order is an even sample: the points sorted along a Morton curve, then taken at the bit-reversed
    // positions of the next power of two (0, P/2, P/4, 3P/4, ...), so the first 2^k are every P/2^k-th along the curve.
    MeshBuild buildPointCloudChunk(const PointCloudChunkDesc &desc)
    {
        if (desc.positions == nullptr || desc.count == 0 || desc.count > MAX_POINT_CHUNK_POINTS)
        {
            spdlog::error("buildPointCloudChunk: a chunk holds 1 to {} points; this one has {}{}.", MAX_POINT_CHUNK_POINTS, desc.count, desc.positions == nullptr ? " and no positions" : "");
            return {};
        }
        const uint32_t count = desc.count;
        glm::vec3 boxMin = desc.positions[0];
        glm::vec3 boxMax = desc.positions[0];
        float scalarMin = 0.0f;
        float scalarMax = 0.0f;
        for (uint32_t i = 0; i < count; ++i)
        {
            const glm::vec3 &p = desc.positions[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || (desc.scalars != nullptr && !std::isfinite(desc.scalars[i])))
            {
                spdlog::error("buildPointCloudChunk: point {} is not finite.", i);
                return {};
            }
            boxMin = glm::min(boxMin, p);
            boxMax = glm::max(boxMax, p);
            if (desc.scalars != nullptr)
            {
                scalarMin = i == 0 ? desc.scalars[i] : std::min(scalarMin, desc.scalars[i]);
                scalarMax = i == 0 ? desc.scalars[i] : std::max(scalarMax, desc.scalars[i]);
            }
        }
        const glm::vec3 boxStep = (boxMax - boxMin) / QUANTIZATION_STEPS;
        const float scalarStep = (scalarMax - scalarMin) / QUANTIZATION_STEPS;

        std::vector<uint16_t> quantized(static_cast<size_t>(count) * 3);
        std::vector<uint64_t> codes(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            for (int axis = 0; axis < 3; ++axis)
                quantized[i * 3 + axis] = quantize(desc.positions[i][axis], boxMin[axis], boxStep[axis]);
            codes[i] = spreadBits(quantized[i * 3]) | spreadBits(quantized[i * 3 + 1]) << 1 | spreadBits(quantized[i * 3 + 2]) << 2;
        }
        std::vector<uint32_t> sorted(count);
        std::iota(sorted.begin(), sorted.end(), 0u);
        std::stable_sort(sorted.begin(), sorted.end(), [&codes](uint32_t a, uint32_t b)
                         { return codes[a] < codes[b]; });

        MeshBuild build;
        build.data = std::make_unique<MeshBuild::Data>();
        std::vector<uint16_t> &order = build.data->pointOrder;
        order.reserve(count);
        const uint32_t bits = static_cast<uint32_t>(std::countr_zero(std::bit_ceil(count)));
        for (uint32_t i = 0; i < (1u << bits); ++i)
        {
            const uint32_t j = reverseBits(i, bits);
            if (j < count)
                order.push_back(static_cast<uint16_t>(sorted[j]));
        }

        const bool colors = desc.colors != nullptr;
        const bool scalars = desc.scalars != nullptr;
        const uint32_t positionWords = gfx::POINT_CHUNK_HEADER_WORDS + 2 * count;
        const uint32_t words = positionWords + (colors ? (count + 1) / 2 * 2 : 0);
        const gfx::PointChunkHeader header{.boxMin = boxMin,
                                           .pointCount = count,
                                           .boxStep = boxStep,
                                           .flags = (colors ? gfx::POINT_CHUNK_COLORS : 0u) | (scalars ? gfx::POINT_CHUNK_SCALARS : 0u),
                                           .scalarMin = scalarMin,
                                           .scalarStep = scalarStep,
                                           .colorOffset = colors ? positionWords : 0u,
                                           .spacing = desc.spacing > 0.0f && std::isfinite(desc.spacing) ? desc.spacing : measurePointSpacing(desc.positions, count)};
        std::vector<uint32_t> &data = build.data->raw.points;
        data.assign(words, 0u);
        std::memcpy(data.data(), &header, sizeof(header));
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t i = order[k];
            const uint32_t scalar = scalars ? quantize(desc.scalars[i], scalarMin, scalarStep) : 0u;
            data[gfx::POINT_CHUNK_HEADER_WORDS + 2 * k] = quantized[i * 3] | static_cast<uint32_t>(quantized[i * 3 + 1]) << 16;
            data[gfx::POINT_CHUNK_HEADER_WORDS + 2 * k + 1] = quantized[i * 3 + 2] | scalar << 16;
            if (colors)
                data[positionWords + k] = desc.colors[i];
        }
        build.data->raw.pointCount = count;
        return build;
    }
}
