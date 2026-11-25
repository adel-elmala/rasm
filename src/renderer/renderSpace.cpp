#include "renderer/renderSpace.h"

#include "glm/gtc/matrix_transform.hpp"

namespace rasm
{
    glm::vec3 toRenderPosition(const glm::dvec3 &worldPosition, const glm::dvec3 &origin)
    {
        return glm::vec3(worldPosition - origin);
    }

    glm::dvec3 toWorldPosition(const glm::vec3 &renderPosition, const glm::dvec3 &origin)
    {
        return origin + glm::dvec3(renderPosition);
    }

    SplitPosition splitPosition(const glm::dvec3 &position)
    {
        const glm::vec3 high = glm::vec3(position);
        return {.high = high, .low = glm::vec3(position - glm::dvec3(high))};
    }

    void writeWorldRows(const glm::dmat4 &world, glm::vec4 rows[3], glm::vec3 &translationLow)
    {
        const SplitPosition translation = splitPosition(glm::dvec3(world[3]));
        for (int row = 0; row < 3; ++row)
            rows[row] = glm::vec4(static_cast<float>(world[0][row]), static_cast<float>(world[1][row]), static_cast<float>(world[2][row]), translation.high[row]);
        translationLow = translation.low;
    }

    void writePreviousRows(const glm::dmat4 &previous, const glm::dmat4 &world, glm::vec4 rows[3])
    {
        const glm::vec3 moved = glm::vec3(glm::dvec3(previous[3]) - glm::dvec3(world[3]));
        for (int row = 0; row < 3; ++row)
            rows[row] = glm::vec4(static_cast<float>(previous[0][row]), static_cast<float>(previous[1][row]), static_cast<float>(previous[2][row]), moved[row]);
    }

    // A render-space point p is the world point p + origin.
    glm::mat4 toRenderMatrix(const glm::dmat4 &worldMatrix, const glm::dvec3 &origin)
    {
        return glm::mat4(worldMatrix * glm::translate(glm::dmat4(1.0), origin));
    }

    // A point p of the first space is p + from - to in the second.
    glm::vec4 moveRenderPlane(const glm::vec4 &plane, const glm::dvec3 &from, const glm::dvec3 &to)
    {
        const double w = static_cast<double>(plane.w) + glm::dot(glm::dvec3(glm::vec3(plane)), to - from);
        return glm::vec4(glm::vec3(plane), static_cast<float>(w));
    }
}
