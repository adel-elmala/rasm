#include "renderer/grid.h"

#include "spdlog/spdlog.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace rasm
{
    namespace
    {
        constexpr uint32_t MAX_GRID_SUBDIVISIONS = 1000;
        // A nanometre to a million kilometres: a float holds the cell, and the cells to any point of a planet fit a double.
        constexpr double MIN_GRID_CELL = 1e-9;
        constexpr double MAX_GRID_CELL = 1e9;
        // Whole cells a double counts exactly; a camera farther out than this from the grid's origin sees no grid.
        constexpr double MAX_GRID_CELLS = 9007199254740992.0;

        bool isFinite(const glm::vec4 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
        }

        // The whole cells of a coordinate modulo period (fmod is exact), and the rest.
        void splitCells(double cells, double period, uint32_t &residue, float &fraction)
        {
            const double whole = std::floor(cells);
            fraction = static_cast<float>(cells - whole);
            // The rest rounds to 1 just under a whole cell; the next cell is as good.
            if (fraction >= 1.0f)
                fraction = std::nextafter(1.0f, 0.0f);
            double rest = std::fmod(whole, period);
            if (rest < 0.0)
                rest += period;
            residue = static_cast<uint32_t>(rest);
        }
    }

    // The checks are written so that NaN fails them.
    bool checkGrid(const Grid &grid)
    {
        const bool finite = std::isfinite(grid.origin.x) && std::isfinite(grid.origin.y) && std::isfinite(grid.origin.z) &&
                            isFinite(glm::vec4(grid.orientation.x, grid.orientation.y, grid.orientation.z, grid.orientation.w)) &&
                            isFinite(grid.minorColor) && isFinite(grid.majorColor) && isFinite(grid.xAxisColor) && isFinite(grid.zAxisColor);
        if (!finite || glm::length(grid.orientation) < 1e-6f)
        {
            spdlog::error("A grid needs a finite origin and colors, and a non-zero orientation.");
            return false;
        }
        if (!(grid.cellSize >= MIN_GRID_CELL && grid.cellSize <= MAX_GRID_CELL))
        {
            spdlog::error("A grid needs a cellSize from {} to {}, got {}.", MIN_GRID_CELL, MAX_GRID_CELL, grid.cellSize);
            return false;
        }
        if (grid.subdivisions < 2 || grid.subdivisions > MAX_GRID_SUBDIVISIONS)
        {
            spdlog::error("A grid needs 2 to {} subdivisions, got {}.", MAX_GRID_SUBDIVISIONS, grid.subdivisions);
            return false;
        }
        if (!(grid.extent.x >= 0.0f) || !(grid.extent.y >= 0.0f) || std::isinf(grid.extent.x) || std::isinf(grid.extent.y))
        {
            spdlog::error("A grid's extent must be finite and 0 or more, got {} x {}.", grid.extent.x, grid.extent.y);
            return false;
        }
        if (!(grid.lineWidth >= 0.0f) || !(grid.majorLineWidth >= 0.0f) || !(grid.fadeDistance >= 0.0f) || std::isinf(grid.lineWidth) ||
            std::isinf(grid.majorLineWidth) || std::isinf(grid.fadeDistance))
        {
            spdlog::error("A grid's line widths and fadeDistance must be finite and 0 or more.");
            return false;
        }
        return true;
    }

    // In doubles: the view's origin on the plane, as whole cells modulo period and the rest, where period is the
    // largest power of subdivisions in 32 bits. Every level's size divides period, so each level's line positions come
    // out of the residue exactly, however far the origin is from the grid's.
    gfx::GridData makeGridData(const Grid &grid, const glm::dvec3 &origin)
    {
        gfx::GridData data{};
        if (!grid.enabled)
            return data;

        const glm::dquat turn = glm::normalize(glm::dquat(grid.orientation));
        const glm::dvec3 axisX = turn * glm::dvec3(1.0, 0.0, 0.0);
        const glm::dvec3 axisZ = turn * glm::dvec3(0.0, 0.0, 1.0);
        const glm::dvec3 normal = turn * glm::dvec3(0.0, 1.0, 0.0);
        const glm::dvec3 offset = origin - grid.origin;

        uint64_t period = 1;
        uint32_t levelCount = 0;
        while (period * grid.subdivisions <= std::numeric_limits<uint32_t>::max())
        {
            period *= grid.subdivisions;
            ++levelCount;
        }

        const double cellSize = grid.cellSize;
        const double x = glm::dot(offset, axisX) / cellSize;
        const double z = glm::dot(offset, axisZ) / cellSize;
        if (!(std::abs(x) < MAX_GRID_CELLS && std::abs(z) < MAX_GRID_CELLS))
            return data;
        data.axisX = glm::vec3(axisX);
        data.axisZ = glm::vec3(axisZ);
        data.normal = glm::vec3(normal);
        data.height = static_cast<float>(glm::dot(offset, normal));
        data.cellSize = static_cast<float>(grid.cellSize);
        data.subdivisions = static_cast<float>(grid.subdivisions);
        splitCells(x, static_cast<double>(period), data.residueX, data.fractionX);
        splitCells(z, static_cast<double>(period), data.residueZ, data.fractionZ);
        data.originCells = glm::vec2(static_cast<float>(x), static_cast<float>(z));
        data.extent = glm::vec2(glm::dvec2(grid.extent) / cellSize);
        data.minorColor = grid.minorColor;
        data.majorColor = grid.majorColor;
        data.lineWidth = grid.lineWidth;
        data.majorLineWidth = grid.majorLineWidth;
        data.fadeDistance = grid.fadeDistance;
        data.levelCount = levelCount;
        data.period = static_cast<uint32_t>(period);
        if (grid.axes)
        {
            data.xAxisColor = grid.xAxisColor;
            data.zAxisColor = grid.zAxisColor;
        }
        return data;
    }
}
