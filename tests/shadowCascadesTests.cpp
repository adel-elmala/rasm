#include "renderer/shadowCascades.h"

#include "doctest/doctest.h"
#include "glm/gtc/matrix_transform.hpp"

#include <cmath>

namespace
{
    constexpr float NEAR_Z = 0.1f;
    constexpr float FOV_Y = glm::radians(60.0f);
    constexpr float ASPECT = 16.0f / 9.0f;
    constexpr uint32_t MAP_SIZE = 1024;
    constexpr float DISTANCE = 50.0f;

    glm::vec3 lightDirection()
    {
        return glm::normalize(glm::vec3(-0.5f, -1.0f, -0.35f));
    }

    // Reversed-Z, like the engine's cameras.
    glm::mat4 cameraProjection()
    {
        return glm::perspective(FOV_Y, ASPECT, 100.0f, NEAR_Z);
    }

    glm::dmat4 cameraWorld(const glm::dvec3 &position, float yaw)
    {
        return glm::translate(glm::dmat4(1.0), position) * glm::dmat4(glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    }

    // The cascade's view translation moves by a whole number of texels in the light's x and y.
    void checkWholeTexels(const rasm::ShadowCascade &a, const rasm::ShadowCascade &b)
    {
        const double texel = 2.0 / (static_cast<double>(a.projection[0][0]) * static_cast<double>(MAP_SIZE));
        for (int axis = 0; axis < 2; ++axis)
        {
            const double texels = (b.view[3][axis] - a.view[3][axis]) / texel;
            CHECK(std::abs(texels - std::round(texels)) < 1e-2);
        }
    }
}

TEST_CASE("Shadow splits grow and end at the distance")
{
    float splits[4] = {};
    rasm::getShadowSplits(NEAR_Z, DISTANCE, 0.7f, 4, splits);
    CHECK(splits[0] > NEAR_Z);
    for (int i = 1; i < 4; ++i)
        CHECK(splits[i] > splits[i - 1]);
    CHECK(splits[3] == DISTANCE);

    // splitLambda 0 gives even slices.
    rasm::getShadowSplits(NEAR_Z, DISTANCE, 0.0f, 4, splits);
    for (int i = 0; i < 4; ++i)
        CHECK(splits[i] == doctest::Approx(NEAR_Z + (DISTANCE - NEAR_Z) * static_cast<float>(i + 1) / 4.0f));
}

TEST_CASE("A cascade holds its whole slice of the view")
{
    float splits[4] = {};
    rasm::getShadowSplits(NEAR_Z, DISTANCE, 0.7f, 4, splits);
    const glm::dmat4 world = cameraWorld(glm::dvec3(3.0, 2.0, -7.0), 0.8f);
    const float tanY = std::tan(FOV_Y * 0.5f);
    for (int i = 0; i < 4; ++i)
    {
        const float nearDepth = i == 0 ? NEAR_Z : splits[i - 1];
        const rasm::ShadowCascade cascade = rasm::fitShadowCascade(cameraProjection(), world, lightDirection(), nearDepth, splits[i], MAP_SIZE, DISTANCE);
        for (float depth : {nearDepth, splits[i]})
        {
            for (float x : {-1.0f, 1.0f})
            {
                for (float y : {-1.0f, 1.0f})
                {
                    const glm::dvec4 corner = world * glm::dvec4(x * tanY * ASPECT * depth, y * tanY * depth, -depth, 1.0f);
                    const glm::vec4 clip = glm::vec4(glm::dmat4(cascade.projection) * cascade.view * corner);
                    CHECK(std::abs(clip.x / clip.w) <= 1.0f);
                    CHECK(std::abs(clip.y / clip.w) <= 1.0f);
                    CHECK(clip.z / clip.w >= 0.0f);
                    CHECK(clip.z / clip.w <= 1.0f);
                }
            }
        }
    }
}

TEST_CASE("Turning the camera keeps the cascade's size")
{
    const rasm::ShadowCascade a = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::dvec3(0.0), 0.0f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    const rasm::ShadowCascade b = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::dvec3(0.0), 1.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    CHECK(a.projection[0][0] == doctest::Approx(b.projection[0][0]));
    CHECK(a.projection[2][2] == doctest::Approx(b.projection[2][2]));
}

TEST_CASE("Moving the camera moves the cascade by whole texels")
{
    const rasm::ShadowCascade a = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::dvec3(0.0), 0.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    const rasm::ShadowCascade b = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::dvec3(0.37, 0.0, 1.21), 0.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    checkWholeTexels(a, b);
}

// The grid is the world's, in doubles: 6400 km from the origin a float could not hold a texel of 1 cm.
TEST_CASE("Far from the origin, moving the camera still moves the cascade by whole texels")
{
    const glm::dvec3 far(4197000.0, 168000.0, 4786000.0);
    const rasm::ShadowCascade a = rasm::fitShadowCascade(cameraProjection(), cameraWorld(far, 0.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    const rasm::ShadowCascade b = rasm::fitShadowCascade(cameraProjection(), cameraWorld(far + glm::dvec3(0.37, 0.0, 1.21), 0.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    checkWholeTexels(a, b);
}

TEST_CASE("An orthographic camera with its near plane behind it splits evenly")
{
    float splits[4] = {};
    rasm::getShadowSplits(-10.0f, DISTANCE, 0.7f, 4, splits);
    for (int i = 0; i < 4; ++i)
        CHECK(splits[i] == doctest::Approx(-10.0f + (DISTANCE + 10.0f) * static_cast<float>(i + 1) / 4.0f));
}
