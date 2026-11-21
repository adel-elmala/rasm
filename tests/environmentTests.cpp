#include "renderer/environmentBake.h"

#include "doctest/doctest.h"

using namespace rasm;

TEST_CASE("the environment cube covers the image's width with its four side faces")
{
    // 90 of the image's 360 degrees per face, a power of two.
    CHECK(getEnvironmentCubeSize(2048) == 512);
    CHECK(getEnvironmentCubeSize(4096) == 1024);
    CHECK(getEnvironmentCubeSize(3000) == 512);

    SUBCASE("between the limits")
    {
        CHECK(getEnvironmentCubeSize(16) == ENVIRONMENT_MIN_CUBE_SIZE);
        CHECK(getEnvironmentCubeSize(1u << 15) == ENVIRONMENT_MAX_CUBE_SIZE);
    }
}

TEST_CASE("a mip chain ends at one texel")
{
    CHECK(getMipCount(1) == 1);
    CHECK(getMipCount(2) == 2);
    CHECK(getMipCount(512) == 10);
    CHECK(getMipCount(SPECULAR_CUBE_SIZE) >= SPECULAR_MIPS);
}

TEST_CASE("one bake fits its storage slots")
{
    // The LUT, every mip of the largest cube, the specular cube's mips and the irradiance cube.
    CHECK(ENVIRONMENT_STORAGE_SLOT > BRDF_LUT_STORAGE_SLOT);
    CHECK(ENVIRONMENT_STORAGE_SLOT + getMipCount(ENVIRONMENT_MAX_CUBE_SIZE) + SPECULAR_MIPS + 1 <= MAX_BINDLESS_STORAGE_IMAGES);
}
