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

    glm::mat4 cameraWorld(const glm::vec3 &position, float yaw)
    {
        return glm::translate(glm::mat4(1.0f), position) * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
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
    const glm::mat4 world = cameraWorld(glm::vec3(3.0f, 2.0f, -7.0f), 0.8f);
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
                    const glm::vec4 corner = world * glm::vec4(x * tanY * ASPECT * depth, y * tanY * depth, -depth, 1.0f);
                    const glm::vec4 clip = cascade.projection * cascade.view * corner;
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
    const rasm::ShadowCascade a = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::vec3(0.0f), 0.0f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    const rasm::ShadowCascade b = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::vec3(0.0f), 1.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    CHECK(a.projection[0][0] == doctest::Approx(b.projection[0][0]));
    CHECK(a.projection[2][2] == doctest::Approx(b.projection[2][2]));
}

TEST_CASE("Moving the camera moves the cascade by whole texels")
{
    const rasm::ShadowCascade a = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::vec3(0.0f), 0.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    const rasm::ShadowCascade b = rasm::fitShadowCascade(cameraProjection(), cameraWorld(glm::vec3(0.37f, 0.0f, 1.21f), 0.3f), lightDirection(), 5.0f, 20.0f, MAP_SIZE, DISTANCE);
    // The view's translation, in light-space units, changes by a whole number of texels in x and y.
    const float texel = 2.0f / (a.projection[0][0] * static_cast<float>(MAP_SIZE));
    for (int axis = 0; axis < 2; ++axis)
    {
        const float texels = (b.view[3][axis] - a.view[3][axis]) / texel;
        CHECK(std::abs(texels - std::round(texels)) < 1e-2f);
    }
}
