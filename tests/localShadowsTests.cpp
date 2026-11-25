#include "renderer/localShadows.h"

#include "doctest/doctest.h"
#include "glm/gtc/matrix_transform.hpp"

#include <cmath>
#include <cstdint>

namespace
{
    constexpr float DISTANCE = 20.0f;

    // Where Vulkan samples direction r of a cube: its face, and the face's texel coordinates (s, t) from 0 to 1, from
    // the spec's cube map face table, written out on its own here.
    void vulkanCubeLookup(const glm::vec3 &r, uint32_t &face, glm::vec2 &st)
    {
        const glm::vec3 a = glm::abs(r);
        float sc = 0.0f;
        float tc = 0.0f;
        float ma = 0.0f;
        if (a.x >= a.y && a.x >= a.z)
        {
            face = r.x >= 0.0f ? 0 : 1;
            sc = r.x >= 0.0f ? -r.z : r.z;
            tc = -r.y;
            ma = a.x;
        }
        else if (a.y >= a.z)
        {
            face = r.y >= 0.0f ? 2 : 3;
            sc = r.x;
            tc = r.y >= 0.0f ? r.z : -r.z;
            ma = a.y;
        }
        else
        {
            face = r.z >= 0.0f ? 4 : 5;
            sc = r.z >= 0.0f ? r.x : -r.x;
            tc = -r.y;
            ma = a.z;
        }
        st = glm::vec2(sc / ma + 1.0f, tc / ma + 1.0f) * 0.5f;
    }

    // Where a face's camera draws world point p: texel coordinates from 0 to 1 (t = 0 is the image's top row, clip y
    // is up) and the reversed-Z depth.
    glm::vec3 drawnAt(const glm::mat4 &viewProjection, const glm::vec3 &p)
    {
        const glm::vec4 clip = viewProjection * glm::vec4(p, 1.0f);
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, ndc.z};
    }
}

TEST_CASE("Each cube face's camera draws a direction where Vulkan samples it")
{
    const glm::vec3 light(1.0f, 2.0f, -3.0f);
    const glm::mat4 projection = rasm::getCubeFaceProjection(DISTANCE);
    const glm::vec3 directions[] = {{1.0f, 0.3f, -0.6f}, {-1.0f, -0.2f, 0.5f}, {0.4f, 1.0f, 0.7f}, {-0.3f, -1.0f, 0.2f}, {0.6f, -0.5f, 1.0f}, {-0.1f, 0.8f, -1.0f}, {0.9f, -0.9f, 0.1f}};
    for (const glm::vec3 &direction : directions)
    {
        uint32_t face = 0;
        glm::vec2 st;
        vulkanCubeLookup(direction, face, st);
        const glm::vec3 drawn = drawnAt(projection * glm::mat4(rasm::getCubeFaceView(light, face)), light + direction * 5.0f);
        CHECK(drawn.x == doctest::Approx(st.x).epsilon(1e-4));
        CHECK(drawn.y == doctest::Approx(st.y).epsilon(1e-4));
        CHECK(drawn.z > 0.0f);
        CHECK(drawn.z < 1.0f);
    }
}

TEST_CASE("A cube face's depth is 1 at the near plane and 0 at the shadow's distance, along the face's axis")
{
    const glm::vec3 light(0.0f);
    const glm::mat4 faceView = rasm::getCubeFaceProjection(DISTANCE) * glm::mat4(rasm::getCubeFaceView(light, 2));
    CHECK(drawnAt(faceView, glm::vec3(0.0f, DISTANCE, 0.0f)).z == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(drawnAt(faceView, glm::vec3(0.0f, DISTANCE * 0.005f, 0.0f)).z == doctest::Approx(1.0f).epsilon(1e-5));
    // A point off the axis has the depth of its distance along the axis.
    CHECK(drawnAt(faceView, glm::vec3(3.0f, 5.0f, -2.0f)).z == doctest::Approx(drawnAt(faceView, glm::vec3(0.0f, 5.0f, 0.0f)).z));
}

TEST_CASE("Every cube face's view is mirrored")
{
    for (uint32_t face = 0; face < 6; ++face)
        CHECK(glm::determinant(glm::mat3(rasm::getCubeFaceView(glm::vec3(4.0f, -1.0f, 2.0f), face))) == doctest::Approx(-1.0f));
}

TEST_CASE("A spot light's map is centred on its axis and holds its whole cone")
{
    const glm::vec3 position(-8.0f, 8.0f, 6.0f);
    const glm::vec3 direction = glm::normalize(glm::vec3(1.0f, -1.0f, -1.0f));
    const float outerConeAngle = glm::radians(30.0f);
    const glm::mat4 view = rasm::getSpotShadowView(position, direction);
    const glm::mat4 viewProjection = rasm::getSpotShadowProjection(outerConeAngle, DISTANCE) * view;
    CHECK(glm::determinant(glm::mat3(view)) == doctest::Approx(1.0f));

    const glm::vec3 centre = drawnAt(viewProjection, position + direction * 10.0f);
    CHECK(centre.x == doctest::Approx(0.5f));
    CHECK(centre.y == doctest::Approx(0.5f));

    // Rays on the cone's edge, all around it, land inside the map; the ones beside the axis touch its sides.
    const glm::vec3 side = glm::normalize(glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(side, direction);
    for (int i = 0; i < 16; ++i)
    {
        const float around = glm::radians(22.5f * static_cast<float>(i));
        const glm::vec3 edge = std::cos(outerConeAngle) * direction + std::sin(outerConeAngle) * (std::cos(around) * side + std::sin(around) * up);
        const glm::vec3 drawn = drawnAt(viewProjection, position + edge * 10.0f);
        CHECK(drawn.x >= -1e-4f);
        CHECK(drawn.x <= 1.0f + 1e-4f);
        CHECK(drawn.y >= -1e-4f);
        CHECK(drawn.y <= 1.0f + 1e-4f);
    }
    CHECK(drawnAt(viewProjection, position + (std::cos(outerConeAngle) * direction + std::sin(outerConeAngle) * side) * 10.0f).x == doctest::Approx(1.0f).epsilon(1e-4));
}
