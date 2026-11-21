#include "importMeshText.h"

#include "doctest/doctest.h"
#include "renderer/animation.h"
#include "renderer/animationTime.h"
#include "renderer/uberMaterialTypes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // One triangle mesh, a key time accessor (accessor 1: 0 and 2 s) and a rotation accessor (accessor 2: two keys).
    struct ClipBuffer
    {
        std::vector<unsigned char> buffer;
        std::string views;
        std::string accessors;
    };

    ClipBuffer makeClipBuffer()
    {
        const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float times[] = {0.0f, 2.0f};
        const float rotations[] = {0, 0, 0, 1, 0, 0.7071068f, 0, 0.7071068f};
        ClipBuffer clip;
        clip.views = addView(clip.buffer, positions, sizeof(positions)) + "," + addView(clip.buffer, times, sizeof(times)) + "," +
                     addView(clip.buffer, rotations, sizeof(rotations));
        clip.accessors = R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                         {"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR"},
                                         {"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"}])";
        return clip;
    }

    // The GPU's clip time (shaders/passes/animate.slang, getClipTime), in floats.
    float getGpuClipTime(float phase, float clock, float speed, float duration, bool loop)
    {
        const float time = phase + clock * speed;
        if (loop)
            return time - std::floor(time / duration) * duration;
        return std::fmin(std::fmax(time, 0.0f), duration);
    }

    double getExactClipTime(const rasm::AnimationPlay &play, double clock, double duration)
    {
        const double time = play.clipTime + (clock - play.startTime) * play.speed;
        if (play.loop)
            return time - std::floor(time / duration) * duration;
        return std::fmin(std::fmax(time, 0.0), duration);
    }
}

TEST_CASE("a clip keeps the nodes it moves, parents first, under a node that holds the static parents")
{
    const ClipBuffer clip = makeClipBuffer();
    // 0 static root (scale 2) -> 1 animated (no mesh) -> 2 mesh; 3 static mesh beside it.
    const rasm::GLTFRaw raw = importGltf(clip.buffer, clip.views, clip.accessors + R"(,
        "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "nodes":[{"scale":[2,2,2],"children":[1,3]},{"translation":[1,0,0],"children":[2]},{"mesh":0},{"mesh":0}],
        "scenes":[{"nodes":[0]}],
        "animations":[{"name":"Turn","samplers":[{"input":1,"output":2}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]}])");

    REQUIRE(raw.subGltfRaws.size() == 2);
    REQUIRE(raw.animation.nodes.size() == 3);
    CHECK(raw.animation.nodes[0].parent == -1);
    CHECK(raw.animation.nodes[0].rest == glm::mat4(glm::vec4(2, 0, 0, 0), glm::vec4(0, 2, 0, 0), glm::vec4(0, 0, 2, 0), glm::vec4(0, 0, 0, 1)));
    CHECK(raw.animation.nodes[1].parent == 0);
    CHECK(raw.animation.nodes[1].translation == glm::vec3(1, 0, 0));
    CHECK(raw.animation.nodes[2].parent == 1);
    CHECK(raw.subGltfRaws[0].animatedNode == 2);
    CHECK(raw.subGltfRaws[1].animatedNode == -1);

    REQUIRE(raw.animation.clips.size() == 1);
    const rasm::AnimationClipRaw &turn = raw.animation.clips[0];
    CHECK(turn.info.name == "Turn");
    CHECK(turn.info.duration == 2.0f);
    REQUIRE(turn.channels.size() == 1);
    CHECK(turn.channels[0].node == 1);
    CHECK(turn.channels[0].path == rasm::AnimationPath::ROTATION);
    REQUIRE(turn.samplers.size() == 1);
    CHECK(turn.samplers[0].interpolation == rasm::AnimationInterpolation::LINEAR);
    CHECK(turn.samplers[0].values[1].y == doctest::Approx(0.7071068f));
}

TEST_CASE("a model whose clips move no mesh has no animation")
{
    const ClipBuffer clip = makeClipBuffer();
    const rasm::GLTFRaw raw = importGltf(clip.buffer, clip.views, clip.accessors + R"(,
        "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "nodes":[{"mesh":0},{}],
        "scenes":[{"nodes":[0,1]}],
        "animations":[{"samplers":[{"input":1,"output":2}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]}])");

    CHECK(raw.animation.nodes.empty());
    CHECK(raw.subGltfRaws[0].animatedNode == -1);
}

TEST_CASE("a chain deeper than one Animate group imports every node after its parent, with one shared sampler")
{
    constexpr int LINKS = 100;
    const ClipBuffer clip = makeClipBuffer();
    std::string nodes;
    std::string channels;
    for (int i = 0; i < LINKS; ++i)
    {
        nodes += std::string(i > 0 ? "," : "") + "{\"mesh\":0,\"translation\":[0,1,0]" + (i + 1 < LINKS ? ",\"children\":[" + std::to_string(i + 1) + "]" : "") + "}";
        channels += std::string(i > 0 ? "," : "") + "{\"sampler\":0,\"target\":{\"node\":" + std::to_string(i) + ",\"path\":\"rotation\"}}";
    }
    const rasm::GLTFRaw raw = importGltf(clip.buffer, clip.views, clip.accessors + R"(,
        "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "nodes":[)" + nodes + R"(],
        "scenes":[{"nodes":[0]}],
        "animations":[{"samplers":[{"input":1,"output":2}],"channels":[)" +
                                                                      channels + "]}]");

    REQUIRE(raw.animation.nodes.size() == LINKS);
    for (int i = 0; i < LINKS; ++i)
    {
        CHECK(raw.animation.nodes[i].parent == i - 1);
        CHECK(raw.subGltfRaws[i].animatedNode == i);
    }
    REQUIRE(raw.animation.clips.size() == 1);
    CHECK(raw.animation.clips[0].samplers.size() == 1);
    CHECK(raw.animation.clips[0].channels.size() == LINKS);
}

TEST_CASE("CUBICSPLINE keeps three values per key, and a sampler with the wrong count is skipped")
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float times[] = {0.0f, 1.0f};
    // In-tangent, value, out-tangent for each key.
    const float translations[] = {0, 0, 0, 1, 2, 3, 0, 1, 0, 0, 0, 0, 4, 5, 6, 0, 0, 0};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, positions, sizeof(positions)) + "," + addView(buffer, times, sizeof(times)) + "," + addView(buffer, translations, sizeof(translations));
    const rasm::GLTFRaw raw = importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                                                       {"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR"},
                                                                       {"bufferView":2,"componentType":5126,"count":6,"type":"VEC3"},
                                                                       {"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "nodes":[{"mesh":0},{"mesh":0}],
        "scenes":[{"nodes":[0,1]}],
        "animations":[{"name":"Spline","samplers":[{"input":1,"output":2,"interpolation":"CUBICSPLINE"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]},
                      {"name":"Broken","samplers":[{"input":1,"output":3,"interpolation":"CUBICSPLINE"}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}])");

    REQUIRE(raw.animation.clips.size() == 2);
    const rasm::AnimationClipRaw &spline = raw.animation.clips[0];
    REQUIRE(spline.samplers.size() == 1);
    CHECK(spline.samplers[0].interpolation == rasm::AnimationInterpolation::CUBICSPLINE);
    REQUIRE(spline.samplers[0].values.size() == 6);
    CHECK(spline.samplers[0].values[1] == glm::vec4(1, 2, 3, 0));
    CHECK(spline.samplers[0].values[4] == glm::vec4(4, 5, 6, 0));
    // The clip stays, so clip indices match the file, but it moves nothing.
    CHECK(raw.animation.clips[1].info.name == "Broken");
    CHECK(raw.animation.clips[1].channels.empty());
}

TEST_CASE("the pool data has a channel row per clip, then one with every clip's channels")
{
    rasm::AnimationRaw animation;
    animation.nodes.resize(2);
    animation.nodes[1].parent = 0;
    for (uint32_t c = 0; c < 2; ++c)
    {
        rasm::AnimationClipRaw clip;
        clip.samplers.push_back({.interpolation = rasm::AnimationInterpolation::STEP, .times = {0.0f, 1.0f}, .values = {glm::vec4(1.0f), glm::vec4(2.0f)}});
        clip.channels.push_back({.node = c, .path = c == 0 ? rasm::AnimationPath::TRANSLATION : rasm::AnimationPath::SCALE, .sampler = 0});
        animation.clips.push_back(clip);
    }
    const int32_t subMeshNodes[] = {1};
    const uint64_t address = 0x10000;
    const std::vector<uint8_t> bytes = rasm::buildAnimationPoolData(animation, subMeshNodes, 1, address);
    REQUIRE(bytes.size() == rasm::getAnimationPoolBytes(animation, 1));

    rasm::gfx::AnimationMeshData header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.nodeCount == 2);
    CHECK(header.subMeshCount == 1);
    CHECK(header.nodes % 16 == 0);

    std::vector<int32_t> channels(3 * 2 * 3);
    std::memcpy(channels.data(), bytes.data() + (header.channels - address), channels.size() * sizeof(int32_t));
    // Clip 0: node 0 translation, sampler 0. Clip 1: node 1 scale, sampler 1. All: both.
    CHECK(channels[0] == 0);
    CHECK(channels[1 * 6 + 3 + 2] == 1);
    CHECK(channels[2 * 6 + 0] == 0);
    CHECK(channels[2 * 6 + 3 + 2] == 1);
    CHECK(channels[2 * 6 + 1] == -1);

    rasm::gfx::AnimationSamplerData second;
    std::memcpy(&second, bytes.data() + (header.samplers - address) + sizeof(second), sizeof(second));
    CHECK(second.keyCount == 2);
    CHECK(second.interpolation == static_cast<uint32_t>(rasm::AnimationInterpolation::STEP));
    float firstValue = 0.0f;
    std::memcpy(&firstValue, bytes.data() + (header.keys - address) + second.firstValue * sizeof(float), sizeof(float));
    CHECK(firstValue == 1.0f);
}

TEST_CASE("the clip time stays within 0.1 ms after the clock has run 24 hours")
{
    struct Case
    {
        rasm::AnimationPlay play;
        float duration;
    };
    const Case cases[] = {
        {{.startTime = 0.0, .speed = 1.0f, .loop = true}, 1.25f},
        {{.startTime = 3.3, .clipTime = 0.4f, .speed = 1.5f, .loop = true}, 10.0f},
        {{.startTime = 86000.0, .speed = 1.0f, .loop = false}, 600.0f},
        {{.startTime = 12.0, .speed = -0.5f, .loop = true}, 2.0f},
    };
    for (const Case &c : cases)
    {
        double base = 0.0;
        float phase = rasm::getAnimationPhase(c.play, c.duration, base);
        double worst = 0.0;
        // Every frame of the last two minutes of the day, at 60 Hz, after rebasing all the way there.
        for (double clock = 0.0; clock < 86400.0; clock += 30.0)
        {
            const double newBase = rasm::getAnimationBase(base, clock);
            if (newBase != base)
            {
                base = newBase;
                phase = rasm::getAnimationPhase(c.play, c.duration, base);
            }
        }
        for (int frame = 0; frame < 7200; ++frame)
        {
            const double clock = 86280.0 + frame / 60.0;
            const double newBase = rasm::getAnimationBase(base, clock);
            if (newBase != base)
            {
                base = newBase;
                phase = rasm::getAnimationPhase(c.play, c.duration, base);
            }
            const float gpu = getGpuClipTime(phase, static_cast<float>(clock - base), c.play.speed, c.duration, c.play.loop);
            double error = std::fabs(gpu - getExactClipTime(c.play, clock, c.duration));
            // A looping time just below the end equals one just past 0.
            if (c.play.loop)
                error = std::fmin(error, c.duration - error);
            worst = std::fmax(worst, error);
        }
        CHECK(worst < 1e-4);
    }
}

TEST_CASE("speed 0 holds the clip time, and a clip that does not loop holds its last pose")
{
    const rasm::AnimationPlay hold{.startTime = 5.0, .clipTime = 0.75f, .speed = 0.0f, .loop = true};
    CHECK(getGpuClipTime(rasm::getAnimationPhase(hold, 2.0f, 1000.0), 33.0f, hold.speed, 2.0f, hold.loop) == doctest::Approx(0.75f));

    const rasm::AnimationPlay once{.startTime = 0.0, .speed = 1.0f, .loop = false};
    CHECK(getGpuClipTime(rasm::getAnimationPhase(once, 2.0f, 1000.0), 10.0f, once.speed, 2.0f, once.loop) == 2.0f);
    // A clip that starts later still waits at its first pose.
    const rasm::AnimationPlay later{.startTime = 1100.0, .speed = 1.0f, .loop = false};
    CHECK(getGpuClipTime(rasm::getAnimationPhase(later, 2.0f, 1000.0), 50.0f, later.speed, 2.0f, later.loop) == 0.0f);
    CHECK(getGpuClipTime(rasm::getAnimationPhase(later, 2.0f, 1064.0), 37.0f, later.speed, 2.0f, later.loop) == doctest::Approx(1.0f));
}
