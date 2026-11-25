#include "rasm/engine.h"

#include "doctest/doctest.h"

#include <algorithm>
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
        return rasm::EngineConfig{.cacheDir = ""};
    }
}

TEST_CASE("the view matrix moves the camera to the origin")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(1.0f, 2.0f, 3.0f);
    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.7f);
    auto camera = engine.createCamera(perspective(), transform);

    glm::dmat4 view = engine.getViewMatrix(camera);

    CHECK(isNear(glm::vec3(view * glm::dvec4(transform.position, 1.0)), glm::vec3(0.0f)));
    CHECK(isNear(glm::vec3(view * glm::dvec4(transform.position + glm::dvec3(transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f)), 1.0)),
                 glm::vec3(0.0f, 0.0f, -1.0f)));
}

// In doubles: 6400 km from the origin a float view matrix would be off by up to half a metre.
TEST_CASE("the view matrix is exact far from the origin")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::dvec3(4197000.25, 168000.5, 4786000.75);
    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.7f);
    auto camera = engine.createCamera(perspective(), transform);

    const glm::dmat4 view = engine.getViewMatrix(camera);
    const glm::dvec3 ahead = transform.position + glm::dvec3(transform.rotation * glm::vec3(0.0f, 0.0f, -0.001f));
    CHECK(glm::length(glm::dvec3(view * glm::dvec4(transform.position, 1.0))) < 1e-6);
    CHECK(glm::length(glm::dvec3(view * glm::dvec4(ahead, 1.0)) - glm::dvec3(0.0, 0.0, -0.001)) < 1e-6);
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

TEST_CASE("the trackball wheel scales the radius by a step each and never reaches the center")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(0.0f, 0.0f, 5.0f);
    auto camera = engine.createCamera(perspective(), transform);
    engine.setCameraController(camera, trackBall(5.0f, glm::vec3(0.0f)));

    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = 2.0f});
    CHECK(isNear(engine.getCameraTransform(camera).position, glm::vec3(0.0f, 0.0f, 5.0f * 0.81f)));
    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = -2.0f});
    CHECK(isNear(engine.getCameraTransform(camera).position, glm::vec3(0.0f, 0.0f, 5.0f)));

    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = 1000.0f});
    CHECK(engine.getCameraController(camera).trackBall.radius == doctest::Approx(rasm::MIN_TRACK_BALL_RADIUS));
    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = -1.0f});
    CHECK(engine.getCameraController(camera).trackBall.radius == doctest::Approx(rasm::MIN_TRACK_BALL_RADIUS / rasm::CAMERA_WHEEL_ZOOM));
}

TEST_CASE("an orthographic trackball zooms its box at the pointer, which stays on the same world point")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(0.0f, 0.0f, 10.0f);
    const rasm::CameraProjection box{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic = {.left = -8.0f, .right = 8.0f, .bottom = -4.0f, .top = 4.0f, .nearZ = 0.1f, .farZ = 50.0f}};
    auto camera = engine.createCamera(box, transform);
    engine.setCameraController(camera, trackBall(10.0f, glm::vec3(0.0f)));

    // The pointer at (0.5, 0.25) in the input's NDC, which divides by the smaller side: 4 units a step, at (2, 1).
    const glm::vec2 pointer(0.5f, 0.25f);
    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = 3.0f, .pointer = pointer});
    const rasm::CameraProjection zoomed = engine.getCameraProjection(camera);
    const float zoom = 0.9f * 0.9f * 0.9f;
    CHECK(zoomed.orthographic.right == doctest::Approx(8.0f * zoom));
    CHECK(zoomed.orthographic.top == doctest::Approx(4.0f * zoom));
    CHECK(engine.getCameraController(camera).trackBall.radius == doctest::Approx(10.0f));
    // Under the pointer: the camera's position across the view plus the pointer's offset in the box.
    const glm::dvec3 position = engine.getCameraTransform(camera).position;
    const float perNdc = 4.0f * zoom;
    CHECK(position.x + pointer.x * perNdc == doctest::Approx(2.0));
    CHECK(position.y + pointer.y * perNdc == doctest::Approx(1.0));
    CHECK(position.z == doctest::Approx(10.0));

    // A thousand steps either way stop at the limits; the box never collapses to 0 or grows to infinity.
    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = 1000.0f});
    CHECK(engine.getCameraProjection(camera).orthographic.top == doctest::Approx(rasm::MIN_TRACK_BALL_RADIUS));
    engine.updateCamera(camera, rasm::CameraInput{.wheelDelta = -1000.0f});
    CHECK(engine.getCameraProjection(camera).orthographic.top == doctest::Approx(1e9f));
    CHECK(std::isfinite(engine.getCameraTransform(camera).position.x));
}

TEST_CASE("a pan moves the eased pose with the center, so the distance does not jump while easing")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(0.0f, 0.0f, 10.0f);
    auto camera = engine.createCamera(perspective(), transform);
    rasm::CameraController controller = trackBall(10.0f, glm::vec3(0.0f));
    controller.easingSpeed = 12.0f;
    engine.setCameraController(camera, controller);
    engine.updateCamera(camera, rasm::CameraInput{.deltaTime = 0.016f, .panStart = glm::vec2(0.0f), .panEnd = glm::vec2(0.3f, 0.0f)});
    const glm::dvec3 center = engine.getCameraController(camera).trackBall.center;
    CHECK(glm::length(engine.getCameraTransform(camera).position - center) == doctest::Approx(10.0));
}

TEST_CASE("a pan drag moves the trackball's center with the pointer, in perspective and orthographic views")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform transform;
    transform.position = glm::vec3(0.0f, 0.0f, 10.0f);
    auto camera = engine.createCamera(perspective(), transform);
    engine.setCameraController(camera, trackBall(10.0f, glm::vec3(0.0f)));
    // The scene follows the pointer: a drag to the right moves the center to the left, by the view's half height at
    // the center per NDC unit (perspective() is 60 degrees, aspect 1.5).
    engine.updateCamera(camera, rasm::CameraInput{.panStart = glm::vec2(0.0f), .panEnd = glm::vec2(0.5f, 0.0f)});
    const double half = 10.0 * std::tan(glm::radians(30.0));
    CHECK(engine.getCameraController(camera).trackBall.center.x == doctest::Approx(-0.5 * half).epsilon(1e-5));
    CHECK(engine.getCameraTransform(camera).position.x == doctest::Approx(-0.5 * half).epsilon(1e-5));
    CHECK(engine.getCameraTransform(camera).rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

    engine.setCameraProjection(camera, rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic = {.left = -8.0f, .right = 8.0f, .bottom = -4.0f, .top = 4.0f, .nearZ = 0.1f, .farZ = 50.0f}});
    engine.setCameraController(camera, trackBall(10.0f, glm::vec3(0.0f)));
    engine.updateCamera(camera, rasm::CameraInput{.panStart = glm::vec2(0.0f), .panEnd = glm::vec2(0.0f, -0.25f)});
    CHECK(engine.getCameraController(camera).trackBall.center.y == doctest::Approx(1.0));
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
    CHECK(glm::length(result.position - glm::dvec3(center)) == doctest::Approx(4.0));
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

TEST_CASE("a rig the application keeps moves like an engine camera with the same controller")
{
    rasm::Engine engine(headlessConfig());
    rasm::Transform start;
    start.position = glm::vec3(1.0f, 0.5f, 3.0f);
    start.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.4f);
    auto camera = engine.createCamera(perspective(), start);
    auto controller = freeFly(3.0f);
    controller.easingSpeed = 6.0f;
    engine.setCameraController(camera, controller);
    rasm::CameraRig rig = rasm::createCameraRig(controller, start);

    const rasm::CameraInput inputs[] = {
        {.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.2f, -0.1f), .deltaTime = 1.0f / 60.0f},
        {.deltaTime = 1.0f / 60.0f, .forward = true, .left = true},
        {.deltaTime = 1.0f / 60.0f},
    };
    for (const rasm::CameraInput &input : inputs)
    {
        engine.updateCamera(camera, input);
        rasm::updateCameraRig(rig, input);
    }

    const rasm::Transform engineTransform = engine.getCameraTransform(camera);
    CHECK(engineTransform.position == rig.transform.position);
    CHECK(engineTransform.rotation == rig.transform.rotation);
    CHECK(engine.getCameraController(camera).freeFly.yaw == rig.controller.freeFly.yaw);
}

TEST_CASE("createCameraRig starts at rest, with a free-fly's angles taken from the pose")
{
    rasm::Transform pose;
    pose.position = glm::vec3(0.0f, 2.0f, 0.0f);
    pose.rotation = glm::angleAxis(0.6f, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(-0.3f, glm::vec3(1.0f, 0.0f, 0.0f));

    const rasm::CameraRig rig = rasm::createCameraRig(freeFly(1.0f), pose);
    CHECK(rig.controller.freeFly.yaw == doctest::Approx(0.6f));
    CHECK(rig.controller.freeFly.pitch == doctest::Approx(-0.3f));
    CHECK(isNear(rig.transform.position, pose.position));
    CHECK(isNear(rig.targetTransform.position, pose.position));

    // No input: the rebuilt rotation is the pose's.
    rasm::CameraRig moved = rig;
    rasm::updateCameraRig(moved, rasm::CameraInput{.deltaTime = 0.016f});
    CHECK(isNear(moved.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f), pose.rotation * glm::vec3(0.0f, 0.0f, -1.0f)));
}

TEST_CASE("a NONE rig ignores input and only eases toward the target the application sets")
{
    rasm::CameraRig rig = rasm::createCameraRig(rasm::CameraController{.type = rasm::CameraController::Type::NONE, .easingSpeed = 10.0f}, rasm::Transform{});
    rasm::updateCameraRig(rig, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.5f), .wheelDelta = 3.0f, .deltaTime = 0.1f, .forward = true});
    CHECK(isNear(rig.transform.position, glm::vec3(0.0f)));
    CHECK(isNear(rig.targetTransform.position, glm::vec3(0.0f)));

    rig.targetTransform.position = glm::vec3(1.0f, 0.0f, 0.0f);
    rasm::updateCameraRig(rig, rasm::CameraInput{.deltaTime = 0.1f});
    CHECK(rig.transform.position.x == doctest::Approx(1.0f - std::exp(-1.0f)));
}

TEST_CASE("an engine camera with a NONE controller keeps the pose the application sets")
{
    rasm::Engine engine(headlessConfig());
    auto camera = engine.createCamera(perspective(), rasm::Transform{});
    engine.setCameraController(camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
    rasm::Transform pose;
    pose.position = glm::vec3(4.0f, 5.0f, 6.0f);
    engine.setCameraTransform(camera, pose);

    engine.updateCamera(camera, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.5f), .deltaTime = 0.1f, .forward = true});
    CHECK(isNear(engine.getCameraTransform(camera).position, pose.position));
}

TEST_CASE("an invalid camera handle is rejected")
{
    rasm::Engine engine(headlessConfig());

    CHECK(engine.getViewMatrix(rasm::CameraHandle{}) == glm::dmat4(1.0));
    CHECK(engine.getProjectionMatrix(rasm::CameraHandle{}) == glm::mat4(1.0f));
}

// After a drag stops, the eased pose reaches the target and stays there bit for bit, near the origin and far from it,
// though a trackball's positions carry the float rounding of its radius (B24).
TEST_CASE("a trackball at rest settles exactly, also far from the origin")
{
    for (const glm::dvec3 center : {glm::dvec3(0.0), glm::dvec3(4197000.0, 168000.0, 4786000.0)})
    {
        rasm::CameraController controller{.type = rasm::CameraController::Type::TRACK_BALL,
                                          .trackBall = {.radius = 15.0f, .center = center, .orbitSensitivity = 1.0f}};
        rasm::CameraRig rig = rasm::createCameraRig(controller, rasm::Transform{center + glm::dvec3(0.0, 0.0, 15.0)});
        rasm::updateCameraRig(rig, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.3f, 0.2f), .deltaTime = 1.0f / 60.0f});
        for (int frame = 0; frame < 600; ++frame)
            rasm::updateCameraRig(rig, rasm::CameraInput{.deltaTime = 1.0f / 60.0f});
        const rasm::Transform settled = rig.transform;
        rasm::updateCameraRig(rig, rasm::CameraInput{.deltaTime = 1.0f / 60.0f});
        CHECK(rig.transform.position == settled.position);
        CHECK(rig.transform.position == rig.targetTransform.position);
        CHECK(rig.transform.rotation == rig.targetTransform.rotation);
    }
}

// A free-fly controller without an up, or with +Y, turns exactly as before up existed; another up keeps the camera
// level with its own horizon.
TEST_CASE("a free-fly rig yaws around its up axis")
{
    const rasm::CameraInput drag{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.3f, 0.1f), .deltaTime = 1.0f / 60.0f, .forward = true};
    rasm::CameraController controller{.type = rasm::CameraController::Type::FREE_FLY,
                                      .freeFly = {.yaw = 0.0f, .pitch = 0.0f, .speed = 2.0f, .lookSensitivity = 1.0f, .up = glm::vec3(0.0f)},
                                      .easingSpeed = 0.0f};
    rasm::CameraRig none = rasm::createCameraRig(controller, rasm::Transform{});
    controller.freeFly.up = glm::vec3(0.0f, 1.0f, 0.0f);
    rasm::CameraRig plusY = rasm::createCameraRig(controller, rasm::Transform{});
    rasm::updateCameraRig(none, drag);
    rasm::updateCameraRig(plusY, drag);
    CHECK(none.transform.rotation == plusY.transform.rotation);
    CHECK(none.transform.position == plusY.transform.position);

    const glm::vec3 up = glm::normalize(glm::vec3(0.66f, 0.03f, 0.75f));
    controller.freeFly.up = up;
    // A camera level with that horizon: turned from +Y to up.
    const glm::quat level = glm::normalize(glm::quat(1.0f + up.y, glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up)));
    rasm::CameraRig tilted = rasm::createCameraRig(controller, rasm::Transform{.position = glm::dvec3(4197000.0, 168000.0, 4786000.0), .rotation = level});
    rasm::updateCameraRig(tilted, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.4f, 0.0f), .deltaTime = 1.0f / 60.0f});
    const glm::vec3 cameraUp = tilted.transform.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 forward = tilted.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(glm::dot(cameraUp, up) == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(glm::dot(forward, up) == doctest::Approx(0.0f).epsilon(1e-5));
}

// A globe's normal may sweep over the whole sphere, through -Y too: the camera follows it without a jump.
TEST_CASE("a free-fly rig follows an up that sweeps past -Y without a jump")
{
    rasm::CameraController controller{.type = rasm::CameraController::Type::FREE_FLY,
                                      .freeFly = {.yaw = 0.0f, .pitch = 0.0f, .speed = 2.0f, .lookSensitivity = 1.0f, .up = glm::vec3(std::sin(0.01f), std::cos(0.01f), 0.0f)},
                                      .easingSpeed = 0.0f};
    rasm::CameraRig rig = rasm::createCameraRig(controller, rasm::Transform{.rotation = glm::angleAxis(0.01f, glm::vec3(0.0f, 0.0f, -1.0f))});
    glm::vec3 lastForward = rig.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    float largestTurn = 0.0f;
    for (int step = 2; step < 600; ++step)
    {
        const float angle = 0.01f * static_cast<float>(step);
        rig.controller.freeFly.up = glm::vec3(std::sin(angle), std::cos(angle), 0.3f);
        rasm::updateCameraRig(rig, rasm::CameraInput{.deltaTime = 1.0f / 60.0f});
        const glm::vec3 forward = rig.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        largestTurn = std::max(largestTurn, std::acos(glm::clamp(glm::dot(forward, lastForward), -1.0f, 1.0f)));
        lastForward = forward;
        const glm::vec3 up = glm::normalize(rig.controller.freeFly.up);
        CHECK(glm::dot(rig.transform.rotation * glm::vec3(0.0f, 1.0f, 0.0f), up) > 0.0f);
    }
    CHECK(largestTurn < 0.05f);
}

namespace
{
    rasm::CameraRig orbitRig(rasm::CameraController::OrbitStyle style)
    {
        const rasm::CameraController controller = {.type = rasm::CameraController::Type::TRACK_BALL,
                                                   .trackBall = {.radius = 10.0f, .center = glm::dvec3(0.0), .orbitSensitivity = 1.0f, .style = style},
                                                   .easingSpeed = 0.0f};
        rasm::Transform start;
        start.position = glm::dvec3(0.0, 0.0, 10.0);
        return rasm::createCameraRig(controller, start);
    }

    float getAngle(const glm::quat &a, const glm::quat &b)
    {
        return 2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a, b))));
    }
}

TEST_CASE("a turntable drag turns the same at the center and at the edge of the view")
{
    rasm::CameraRig centre = orbitRig(rasm::CameraController::OrbitStyle::TURNTABLE);
    rasm::CameraRig edge = orbitRig(rasm::CameraController::OrbitStyle::TURNTABLE);
    rasm::updateCameraRig(centre, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.2f, 0.1f)});
    rasm::updateCameraRig(edge, rasm::CameraInput{.mouseStart = glm::vec2(1.7f, 0.95f), .mouseEnd = glm::vec2(1.9f, 1.05f)});
    CHECK(getAngle(centre.transform.rotation, edge.transform.rotation) < 1e-5f);
    // A drag to the right turns the camera to the left around the center: the scene follows the pointer.
    CHECK(centre.transform.position.x < 0.0);
    CHECK(glm::length(centre.transform.position) == doctest::Approx(10.0));
}

TEST_CASE("a turntable never rolls and never passes the pole")
{
    rasm::CameraRig rig = orbitRig(rasm::CameraController::OrbitStyle::TURNTABLE);
    for (int i = 0; i < 40; ++i)
    {
        rasm::updateCameraRig(rig, rasm::CameraInput{.mouseStart = glm::vec2(0.0f), .mouseEnd = glm::vec2(0.13f, -0.2f)});
        const glm::vec3 forward = rig.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right = rig.transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        CHECK(std::abs(right.y) < 1e-4f);
        CHECK(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)) >= glm::radians(-89.0f) - 1e-4f);
        CHECK(rig.transform.position.y > 0.0);
    }
    // At the limit, more of the same drag only turns around up.
    const glm::vec3 forward = rig.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(std::asin(forward.y) == doctest::Approx(glm::radians(-89.0f)).epsilon(1e-3));
}

TEST_CASE("an arcball drag past the edge of its sphere goes on turning the same way")
{
    rasm::CameraRig rig = orbitRig(rasm::CameraController::OrbitStyle::ARCBALL);
    float last = 0.0f;
    glm::quat before = rig.transform.rotation;
    for (int i = 0; i < 12; ++i)
    {
        const float x = 0.5f + 0.15f * static_cast<float>(i);
        rasm::updateCameraRig(rig, rasm::CameraInput{.mouseStart = glm::vec2(x, 0.0f), .mouseEnd = glm::vec2(x + 0.15f, 0.0f)});
        const float angle = getAngle(before, rig.transform.rotation);
        // Each step turns around the camera's up, to the same side, and never jumps.
        CHECK(angle > 1e-4f);
        CHECK(angle < 0.3f);
        CHECK(rig.transform.position.x < 0.0);
        before = rig.transform.rotation;
        last = angle;
    }
    CHECK(last > 0.0f);
}
