#include "renderer/lightClusters.h"

#include "doctest/doctest.h"

#include <cmath>
#include <vector>

using rasm::Light;
using rasm::LightHandle;
using rasm::LightType;

namespace
{
    Light makeLight(uint64_t index, LightType type, float range)
    {
        Light light{};
        light.handle = LightHandle{index, 1};
        light.type = type;
        light.range = range;
        return light;
    }

    // Every slice's range holds the depths that map to it, and the slices tile sliceNear..sliceFar without a gap.
    void checkSlices(float sliceNear, float sliceFar, bool even)
    {
        CHECK(rasm::getClusterSliceDepth(0, sliceNear, sliceFar, even) == doctest::Approx(sliceNear));
        CHECK(rasm::getClusterSliceDepth(rasm::CLUSTER_SLICES, sliceNear, sliceFar, even) == doctest::Approx(sliceFar));
        for (uint32_t slice = 0; slice < rasm::CLUSTER_SLICES; ++slice)
        {
            const float start = rasm::getClusterSliceDepth(slice, sliceNear, sliceFar, even);
            const float end = rasm::getClusterSliceDepth(slice + 1, sliceNear, sliceFar, even);
            CHECK(end > start);
            for (float t : {0.01f, 0.5f, 0.99f})
            {
                const float depth = start + (end - start) * t;
                CHECK(rasm::getClusterSlice(depth, sliceNear, sliceFar, even) == static_cast<int32_t>(slice));
            }
        }
        CHECK(rasm::getClusterSlice(sliceNear, sliceNear, sliceFar, even) == 0);
        CHECK(rasm::getClusterSlice(sliceFar, sliceNear, sliceFar, even) == static_cast<int32_t>(rasm::CLUSTER_SLICES) - 1);
    }
}

TEST_CASE("light clusters: lights without a range are global")
{
    CHECK(rasm::isGlobalLight(makeLight(1, LightType::DIRECTIONAL, 10.0f)));
    CHECK(rasm::isGlobalLight(makeLight(1, LightType::POINT, 0.0f)));
    CHECK(rasm::isGlobalLight(makeLight(1, LightType::SPOT, 0.0f)));
    CHECK(rasm::isGlobalLight(makeLight(1, LightType::POINT, std::nanf(""))));
    CHECK_FALSE(rasm::isGlobalLight(makeLight(1, LightType::POINT, 4.0f)));
    CHECK_FALSE(rasm::isGlobalLight(makeLight(1, LightType::SPOT, 4.0f)));
}

TEST_CASE("light clusters: a scene's list has the global lights first, each part in the scene's order")
{
    // The registry by handle index; slot 0 is never used.
    std::vector<Light> lights = {Light{}, makeLight(1, LightType::POINT, 5.0f), makeLight(2, LightType::DIRECTIONAL, 0.0f), makeLight(3, LightType::SPOT, 3.0f),
                                 makeLight(4, LightType::POINT, 0.0f), makeLight(5, LightType::POINT, 2.0f)};
    const LightHandle scene[] = {lights[5].handle, lights[2].handle, lights[1].handle, lights[4].handle, lights[3].handle};
    std::vector<uint32_t> rows;
    const uint32_t globalCount = rasm::buildSceneLightList(lights.data(), scene, 5, rows);
    CHECK(globalCount == 2);
    CHECK(rows == std::vector<uint32_t>{2, 4, 5, 1, 3});

    rows = {7, 7};
    CHECK(rasm::buildSceneLightList(lights.data(), scene, 0, rows) == 0);
    CHECK(rows.empty());
}

TEST_CASE("light clusters: perspective slices grow by the same factor")
{
    checkSlices(0.1f, 100.0f, false);
    checkSlices(2.0f, 3.0f, false);
    // Each slice is the same factor longer than the one before.
    const float first = rasm::getClusterSliceDepth(1, 0.1f, 100.0f, false) / rasm::getClusterSliceDepth(0, 0.1f, 100.0f, false);
    const float last = rasm::getClusterSliceDepth(rasm::CLUSTER_SLICES, 0.1f, 100.0f, false) / rasm::getClusterSliceDepth(rasm::CLUSTER_SLICES - 1, 0.1f, 100.0f, false);
    CHECK(first == doctest::Approx(last));
    CHECK(first == doctest::Approx(std::pow(1000.0f, 1.0f / static_cast<float>(rasm::CLUSTER_SLICES))));
}

TEST_CASE("light clusters: orthographic slices are even, and may start behind the camera")
{
    checkSlices(-20.0f, 40.0f, true);
    checkSlices(0.0f, 1.0f, true);
    CHECK(rasm::getClusterSliceDepth(12, -20.0f, 40.0f, true) == doctest::Approx(10.0f));
}

TEST_CASE("light clusters: no slice outside the lights' depths")
{
    CHECK(rasm::getClusterSlice(0.05f, 0.1f, 100.0f, false) == -1);
    CHECK(rasm::getClusterSlice(100.5f, 0.1f, 100.0f, false) == -1);
    CHECK(rasm::getClusterSlice(-30.0f, -20.0f, 40.0f, true) == -1);
    // No visible light: the far depth is before the near one.
    CHECK(rasm::getClusterSlice(0.5f, 1.0f, 0.0f, false) == -1);
    CHECK(rasm::getClusterSlice(0.5f, 1.0f, 0.0f, true) == -1);
    // Log slices need a near depth above 0.
    CHECK(rasm::getClusterSlice(0.5f, 0.0f, 1.0f, false) == -1);
    CHECK(rasm::getClusterSlice(std::nanf(""), 0.1f, 100.0f, false) == -1);
}

TEST_CASE("light clusters: the buffer holds the fixed grid and a sphere and a bit per light")
{
    const uint64_t fixed = rasm::getClusterBufferSize(0);
    CHECK(fixed == (4 + rasm::CLUSTER_COUNT + uint64_t{rasm::CLUSTER_COUNT} * rasm::MAX_CLUSTER_LIGHTS + 144 + 144 * uint64_t{rasm::CLUSTER_SLICES} * rasm::MAX_CLUSTER_LIGHTS) * 4);
    CHECK(rasm::getClusterBufferSize(32) == fixed + uint64_t{32} * 16 + 4);
    CHECK(rasm::getClusterBufferSize(33) == fixed + uint64_t{33} * 16 + 8);
    // The spheres are float4s, so they start 16-byte aligned.
    CHECK((fixed % 16) == 0);
}
