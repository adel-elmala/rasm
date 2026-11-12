#include "rasm/engine.h"

#include "doctest/doctest.h"

#include <cmath>
#include <limits>

namespace
{
    constexpr float NEAR_Z = 0.1f;
    constexpr float FAR_Z = 100.0f;
    constexpr float TOLERANCE = 1e-4f;

    bool isNear(const glm::vec3 &a, const glm::vec3 &b)
    {
        return glm::length(a - b) < TOLERANCE;
    }

    rasm::CameraProjection perspective(float aspect = 1.5f)
    {
        return rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                      .perspective = {.fovY = 60.0f, .aspect = aspect, .nearZ = NEAR_Z, .farZ = FAR_Z}};
    }

    rasm::CameraController freeFly(float speed)
    {
        return rasm::CameraController{.type = rasm::CameraController::Type::FREE_FLY,
                                      .freeFly = {.yaw = 0.0f, .pitch = 0.0f, .speed = speed, .lookSensitivity = 1.0f},
                                      .easingSpeed = 0.0f};
    }

    rasm::CameraController trackBall(float radius, const glm::vec3 &center)
    {
        return rasm::CameraController{.type = rasm::CameraController::Type::TRACK_BALL,
                                      .trackBall = {.radius = radius, .center = center, .orbitSensitivity = 1.0f},
                                      .easingSpeed = 0.0f};
    }

    float projectedDepth(const glm::mat4 &projection, float viewZ)
    {
        glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, viewZ, 1.0f);
        return clip.z / clip.w;
    }

    // Zero window dimensions make the constructor return before it touches a window or the GPU, which leaves
    // the CPU-side camera API usable.
    rasm::EngineConfig headlessConfig()
    {
        return rasm::EngineConfig{};
    }
}

TEST_CASE("the view matrix moves the camera to the origin")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(1.0f, 2.0f, 3.0f);
    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.7f);
    auto camera = engine.createCamera(perspective(), transform);

    glm::mat4 view = engine.getViewMatrix(camera);

    CHECK(isNear(glm::vec3(view * glm::vec4(transform.position, 1.0f)), glm::vec3(0.0f)));
    CHECK(isNear(glm::vec3(view * glm::vec4(transform.position + transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f), 1.0f)),
                 glm::vec3(0.0f, 0.0f, -1.0f)));
}

TEST_CASE("perspective projection maps depth to reversed [1, 0]")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});

    glm::mat4 projection = engine.getProjectionMatrix(camera);

    CHECK(projectedDepth(projection, -NEAR_Z) == doctest::Approx(1.0f).epsilon(TOLERANCE));
    CHECK(projectedDepth(projection, -FAR_Z) == doctest::Approx(0.0f).epsilon(TOLERANCE));
}

TEST_CASE("an infinite perspective projection approaches depth 0 and never reaches it")
{
    rasm::Engine engine(headlessConfig());
    auto projection = perspective();
    projection.perspective.farZ = 0.0f;
    auto camera = engine.createCamera(projection, rasm::Transform{});

    glm::mat4 matrix = engine.getProjectionMatrix(camera);

    CHECK(projectedDepth(matrix, -NEAR_Z) == doctest::Approx(1.0f).epsilon(TOLERANCE));
    CHECK(projectedDepth(matrix, -10.0f * NEAR_Z) == doctest::Approx(0.1f).epsilon(TOLERANCE));
    CHECK(projectedDepth(matrix, -1.0e6f) > 0.0f);
    CHECK(projectedDepth(matrix, -1.0e6f) < 1.0e-6f);
}

TEST_CASE("a farZ of infinity is the same infinite projection as 0")
{
    rasm::Engine engine(headlessConfig());
    auto zeroFar = perspective();
    zeroFar.perspective.farZ = 0.0f;
    auto infiniteFar = perspective();
    infiniteFar.perspective.farZ = std::numeric_limits<float>::infinity();

    CHECK(engine.getProjectionMatrix(engine.createCamera(infiniteFar, rasm::Transform{})) ==
          engine.getProjectionMatrix(engine.createCamera(zeroFar, rasm::Transform{})));
}

TEST_CASE("a perspective camera with a bad near or far plane is rejected")
{
    rasm::Engine engine(headlessConfig());
    const float badNear[] = {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    const float badFar[] = {NEAR_Z * 0.5f, NEAR_Z, -FAR_Z, -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};

    for (float nearZ : badNear)
    {
        auto projection = perspective();
        projection.perspective.nearZ = nearZ;
        CHECK_FALSE(engine.createCamera(projection, rasm::Transform{}).isValid());
    }
    for (float farZ : badFar)
    {
        auto projection = perspective();
        projection.perspective.farZ = farZ;
        CHECK_FALSE(engine.createCamera(projection, rasm::Transform{}).isValid());
    }
}

TEST_CASE("an orthographic camera without a finite far plane past the near plane is rejected")
{
    rasm::Engine engine(headlessConfig());
    const float badFar[] = {0.0f, NEAR_Z, std::numeric_limits<float>::infinity()};

    for (float farZ : badFar)
    {
        rasm::CameraProjection projection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                                          .orthographic = {.left = -2.0f, .right = 2.0f, .bottom = -1.0f, .top = 1.0f, .nearZ = NEAR_Z, .farZ = farZ}};
        CHECK_FALSE(engine.createCamera(projection, rasm::Transform{}).isValid());
    }
}

TEST_CASE("setCameraProjection keeps the old projection when the new one is rejected")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});
    auto projection = perspective();
    projection.perspective.nearZ = 0.0f;

    engine.setCameraProjection(camera, projection);

    CHECK(engine.getCameraProjection(camera).perspective.nearZ == doctest::Approx(NEAR_Z));
}

TEST_CASE("orthographic projection maps depth to reversed [1, 0]")
{
    rasm::Engine engine(headlessConfig());
    rasm::CameraProjection projection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                                      .orthographic = {.left = -2.0f, .right = 2.0f, .bottom = -1.0f, .top = 1.0f, .nearZ = NEAR_Z, .farZ = FAR_Z}};
    auto camera = engine.createCamera(projection, rasm::Transform{});

    glm::mat4 matrix = engine.getProjectionMatrix(camera);

    CHECK(projectedDepth(matrix, -NEAR_Z) == doctest::Approx(1.0f).epsilon(TOLERANCE));
    CHECK(projectedDepth(matrix, -FAR_Z) == doctest::Approx(0.0f).epsilon(TOLERANCE));
}

TEST_CASE("an aspect override leaves the camera unchanged")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(1.5f), rasm::Transform{});
    const float xScale = engine.getProjectionMatrix(camera)[0][0];

    CHECK(engine.getProjectionMatrix(camera, 3.0f)[0][0] == doctest::Approx(xScale / 2.0f));
    CHECK(engine.getCameraProjection(camera).perspective.aspect == doctest::Approx(1.5f));
}

TEST_CASE("updateCameraAspectRatio keeps an orthographic camera's width and center")
{
    rasm::Engine engine(headlessConfig());
    rasm::CameraProjection projection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                                      .orthographic = {.left = 2.0f, .right = 6.0f, .bottom = 0.0f, .top = 2.0f, .nearZ = NEAR_Z, .farZ = FAR_Z}};
    auto camera = engine.createCamera(projection, rasm::Transform{});

    engine.updateCameraAspectRatio(1.0f);

    auto result = engine.getCameraProjection(camera).orthographic;
    CHECK(result.left == doctest::Approx(2.0f));
    CHECK(result.right == doctest::Approx(6.0f));
    CHECK(result.bottom == doctest::Approx(-1.0f));
    CHECK(result.top == doctest::Approx(3.0f));
}

TEST_CASE("an update without input keeps a new trackball camera's pose")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(1.0f, 2.0f, 3.0f);
    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.7f);
    auto camera = engine.createCamera(perspective(), transform);

    engine.updateCamera(camera, rasm::CameraInput{.deltaTime = 0.016f});

    CHECK(isNear(engine.getCameraTransform(camera).position, transform.position));
}

TEST_CASE("the trackball wheel zooms toward the center and stops there")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(0.0f, 0.0f, 5.0f);
    auto camera = engine.createCamera(perspective(), transform);
    engine.setCameraController(camera, trackBall(5.0f, glm::vec3(0.0f)));

    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = 2.0f});
    CHECK(isNear(engine.getCameraTransform(camera).position, glm::vec3(0.0f, 0.0f, 3.0f)));

    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = 10.0f});
    CHECK(isNear(engine.getCameraTransform(camera).position, glm::vec3(0.0f)));
    CHECK(engine.getCameraController(camera).trackBall.radius == doctest::Approx(0.0f));
}

TEST_CASE("a trackball drag keeps the camera on the orbit sphere")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(2.0f, 1.0f, 4.0f);
    auto camera = engine.createCamera(perspective(), transform);
    const glm::vec3 center(2.0f, 1.0f, 0.0f);
    engine.setCameraController(camera, trackBall(4.0f, center));

    engine.updateCamera(camera, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.3f, 0.2f)});

    auto result = engine.getCameraTransform(camera);
    CHECK_FALSE(isNear(result.position, transform.position));
    CHECK(glm::length(result.position - center) == doctest::Approx(4.0f));
}

TEST_CASE("free-fly movement is scaled by speed and frame time")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});
    engine.setCameraController(camera, freeFly(4.0f));

    engine.updateCamera(camera, rasm::CameraInput{.deltaTime = 0.5f, .forward = true});

    CHECK(isNear(engine.getCameraTransform(camera).position, glm::vec3(0.0f, 0.0f, -2.0f)));
}

TEST_CASE("free-fly diagonal movement is not faster")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});
    engine.setCameraController(camera, freeFly(4.0f));

    engine.updateCamera(camera, rasm::CameraInput{.deltaTime = 0.5f, .forward = true, .right = true});

    CHECK(glm::length(engine.getCameraTransform(camera).position) == doctest::Approx(2.0f));
}

TEST_CASE("free-fly pitch stops short of the poles")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});
    engine.setCameraController(camera, freeFly(1.0f));

    engine.updateCamera(camera, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.0f, 10.0f)});

    CHECK(engine.getCameraController(camera).freeFly.pitch == doctest::Approx(glm::radians(89.0f)));
}

TEST_CASE("switching to free-fly keeps the view direction")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.5f);
    auto camera = engine.createCamera(perspective(), transform);
    const glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);

    engine.setCameraController(camera, freeFly(1.0f));
    engine.updateCamera(camera, rasm::CameraInput{});

    CHECK(isNear(engine.getCameraTransform(camera).rotation * glm::vec3(0.0f, 0.0f, -1.0f), forward));
}

TEST_CASE("easing moves part of the way and converges")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});
    auto controller = freeFly(1.0f);
    controller.easingSpeed = 10.0f;
    engine.setCameraController(camera, controller);

    engine.updateCamera(camera, rasm::CameraInput{.deltaTime = 0.1f, .forward = true});
    const float expected = 0.1f * (1.0f - std::exp(-1.0f));
    CHECK(engine.getCameraTransform(camera).position.z == doctest::Approx(-expected));

    for (int i = 0; i < 100; ++i)
    {
        engine.updateCamera(camera, rasm::CameraInput{.deltaTime = 0.1f});
    }
    CHECK(engine.getCameraTransform(camera).position.z == doctest::Approx(-0.1f));
}

TEST_CASE("an invalid camera handle is rejected")
{
    rasm::Engine engine(headlessConfig());

    CHECK(engine.getViewMatrix(rasm::CameraHandle{}) == glm::mat4(1.0f));
    CHECK(engine.getProjectionMatrix(rasm::CameraHandle{}) == glm::mat4(1.0f));
}
