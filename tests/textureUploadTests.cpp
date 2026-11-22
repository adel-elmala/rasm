#include "assets/meshImport.h"
#include "importMeshText.h"
#include "rhi/mipChain.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "rasm/readback.h"

#include "doctest/doctest.h"
#include "tiny_gltf.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace rasm;

namespace
{
    VkPhysicalDeviceMemoryProperties makeMemory(const VkMemoryPropertyFlags *flags, const uint32_t *heaps, uint32_t count)
    {
        VkPhysicalDeviceMemoryProperties memory = {};
        memory.memoryTypeCount = count;
        for (uint32_t i = 0; i < count; ++i)
            memory.memoryTypes[i] = {.propertyFlags = flags[i], .heapIndex = heaps[i]};
        return memory;
    }

    float getLength(const unsigned char *texel)
    {
        const float x = texel[0] / 127.5f - 1.0f;
        const float y = texel[1] / 127.5f - 1.0f;
        const float z = texel[2] / 127.5f - 1.0f;
        return std::sqrt(x * x + y * y + z * z);
    }

    // Two texels pointing along +x and two along +z: their mean points halfway between, at length 0.71.
    std::vector<unsigned char> makeNormalChecker()
    {
        return {255, 128, 128, 255, 128, 128, 255, 255, 128, 128, 255, 255, 255, 128, 128, 255};
    }
}

TEST_CASE("host image copy uses only device-local memory types that copied images use too")
{
    constexpr VkMemoryPropertyFlags DEVICE = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // The RTX 3060's types: host-copy images may use only the host-visible ones (3, 4, and 5 through resizable BAR),
    // copied images only 1 and 2. They share no type, so textures come from the staging ring.
    const VkMemoryPropertyFlags nvidiaFlags[] = {0, DEVICE, DEVICE, HOST, HOST | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, DEVICE | HOST};
    const uint32_t nvidiaHeaps[] = {1, 0, 0, 1, 1, 0};
    const auto nvidia = makeMemory(nvidiaFlags, nvidiaHeaps, 6);
    CHECK(rhi::getHostCopyMemoryTypes(nvidia, 0x38, 0x03) == 0);

    // lavapipe and unified memory: one device-local, host-visible type for everything.
    const VkMemoryPropertyFlags unifiedFlags[] = {DEVICE | HOST | VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    const uint32_t unifiedHeaps[] = {0};
    CHECK(rhi::getHostCopyMemoryTypes(makeMemory(unifiedFlags, unifiedHeaps, 1), 0x1, 0x1) == 0x1);

    // A driver with the same types for both: the device-local ones, and never system RAM.
    const VkMemoryPropertyFlags discreteFlags[] = {DEVICE, HOST, DEVICE | HOST};
    const uint32_t discreteHeaps[] = {0, 1, 2};
    CHECK(rhi::getHostCopyMemoryTypes(makeMemory(discreteFlags, discreteHeaps, 3), 0x7, 0x7) == 0x5);
    CHECK(rhi::getHostCopyMemoryTypes(makeMemory(discreteFlags, discreteHeaps, 3), 0x2, 0x7) == 0);
}

TEST_CASE("buildTextureLevels packs level 0 and every mip in one block")
{
    std::vector<unsigned char> pixels(8 * 4 * 4);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<unsigned char>(i);
    TextureRaw raw;
    REQUIRE(rhi::buildTextureLevels(pixels.data(), 8, 4, Format::R8G8B8A8_UNORM, false, rhi::getFullMipCount(8, 4), raw));

    CHECK(raw.width == 8);
    CHECK(raw.height == 4);
    CHECK(raw.format == Format::R8G8B8A8_UNORM);
    REQUIRE(raw.levelSizes.size() == 4);
    CHECK(raw.levelSizes[0] == 8 * 4 * 4);
    CHECK(raw.levelSizes[1] == 4 * 2 * 4);
    CHECK(raw.levelSizes[2] == 2 * 1 * 4);
    CHECK(raw.levelSizes[3] == 1 * 1 * 4);
    CHECK(raw.levels.size() == (32 + 8 + 2 + 1) * 4);
    CHECK(std::equal(pixels.begin(), pixels.end(), raw.levels.begin()));
}

TEST_CASE("sRGB mips average in linear light, data mips as stored")
{
    // Black and white side by side: the mean light is 0.5, which sRGB stores as 188; the stored numbers average to 128.
    const std::vector<unsigned char> pixels = {0, 0, 0, 255, 255, 255, 255, 255};
    TextureRaw srgb;
    TextureRaw data;
    REQUIRE(rhi::buildTextureLevels(pixels.data(), 2, 1, Format::R8G8B8A8_SRGB, false, 2, srgb));
    REQUIRE(rhi::buildTextureLevels(pixels.data(), 2, 1, Format::R8G8B8A8_UNORM, false, 2, data));
    CHECK(std::abs(static_cast<int>(srgb.levels[8]) - 188) <= 1);
    CHECK(std::abs(static_cast<int>(data.levels[8]) - 128) <= 1);
}

TEST_CASE("normal map mips are made unit length again")
{
    const std::vector<unsigned char> pixels = makeNormalChecker();
    TextureRaw normals;
    TextureRaw plain;
    REQUIRE(rhi::buildTextureLevels(pixels.data(), 2, 2, Format::R8G8B8A8_UNORM, true, 2, normals));
    REQUIRE(rhi::buildTextureLevels(pixels.data(), 2, 2, Format::R8G8B8A8_UNORM, false, 2, plain));

    const unsigned char *mip = normals.levels.data() + normals.levelSizes[0];
    CHECK(getLength(mip) == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(getLength(plain.levels.data() + plain.levelSizes[0]) == doctest::Approx(0.71f).epsilon(0.03));
    // Halfway between +x and +z, alpha kept.
    CHECK(std::abs(static_cast<int>(mip[0]) - static_cast<int>(mip[2])) <= 1);
    CHECK(mip[3] == 255);
    // Level 0 is the file's.
    CHECK(std::equal(pixels.begin(), pixels.end(), normals.levels.begin()));
}

TEST_CASE("a glTF's PNG images get their mips on import, renormalized in a normal slot")
{
    const std::string name = "rasm_tests_" + std::to_string(std::random_device{}()) + ".png";
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    ReadbackImage image = {.width = 2, .height = 2, .pixels = makeNormalChecker()};
    REQUIRE(writePng(path.string(), image));

    // Image 0 is material 0's normal map and material 1's occlusion: two sets of levels.
    const GLTFRaw raw = importMeshText(R"({"asset":{"version":"2.0"},"images":[{"uri":")" + name + R"("}],"textures":[{"source":0}],
        "materials":[{"normalTexture":{"index":0}},{"occlusionTexture":{"index":0}},{"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":0}}}]})",
                                       ".gltf");
    std::filesystem::remove(path);

    REQUIRE(raw.loaded);
    CHECK(raw.imageLevels.size() == 2);
    REQUIRE(raw.imageLevels.contains(getImageLevelsKey(0, KtxSlot::NORMAL)));
    REQUIRE(raw.imageLevels.contains(getImageLevelsKey(0, KtxSlot::OCCLUSION)));
    CHECK(getImageLevelsKey(0, KtxSlot::OCCLUSION) == getImageLevelsKey(0, KtxSlot::DATA));

    const TextureRaw &normals = *raw.imageLevels.at(getImageLevelsKey(0, KtxSlot::NORMAL));
    const TextureRaw &data = *raw.imageLevels.at(getImageLevelsKey(0, KtxSlot::DATA));
    CHECK(normals.format == Format::R8G8B8A8_UNORM);
    REQUIRE(normals.levelSizes.size() == 2);
    CHECK(getLength(normals.levels.data() + normals.levelSizes[0]) == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(getLength(data.levels.data() + data.levelSizes[0]) == doctest::Approx(0.71f).epsilon(0.03));
    // The decoded pixels go once the levels are made.
    CHECK(raw.model->images[0].image.empty());
}
