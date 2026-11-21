// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{

    enum class AnimationPath : uint32_t
    {
        TRANSLATION,
        ROTATION,
        SCALE,
    };

    enum class AnimationInterpolation : uint32_t
    {
        STEP,
        LINEAR,
        CUBICSPLINE,
    };

    // AnimationPlay::clip: every clip of the mesh at once. A node moves by the last clip that animates it. Not
    // UINT32_MAX, so findAnimation's -1 cast to a clip is refused instead of playing every clip.
    constexpr uint32_t ALL_ANIMATION_CLIPS = UINT32_MAX - 1;

    struct AnimationClip
    {
        std::string                     name;
        float                           duration        = 0.0f;                         // seconds: the clip's last key time
    };

    struct AnimationPlay
    {
        uint32_t                        clip            = 0;                            // an index of getAnimations, or ALL_ANIMATION_CLIPS
        double                          startTime       = 0.0;                          // on the animation clock (setAnimationTime)
        float                           clipTime        = 0.0f;                         // the clip's time at startTime
        float                           speed           = 1.0f;                         // 0 holds clipTime
        bool                            loop            = true;                         // false: holds the first or last pose past the ends
    };

    struct AnimationNode
    {
        int32_t                         parent          = -1;                           // an earlier node, or -1
        glm::mat4                       rest            = glm::mat4(1.0f);              // the local matrix at rest
        glm::vec3                       translation     = glm::vec3(0.0f);              // the rest pose of the paths a clip animates
        glm::vec4                       rotation        = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);   // quaternion x, y, z, w
        glm::vec3                       scale           = glm::vec3(1.0f);
    };

    struct AnimationSampler
    {
        AnimationInterpolation          interpolation   = AnimationInterpolation::LINEAR;
        std::vector<float>              times;
        std::vector<glm::vec4>          values;                                         // one per key; CUBICSPLINE: in-tangent, value, out-tangent
    };

    struct AnimationChannel
    {
        uint32_t                        node            = 0;
        AnimationPath                   path            = AnimationPath::TRANSLATION;
        uint32_t                        sampler         = 0;                            // in the clip's samplers
    };

    struct AnimationClipRaw
    {
        AnimationClip                   info;
        std::vector<AnimationSampler>   samplers;
        std::vector<AnimationChannel>   channels;
    };

    // A joint of a skin: the node that moves it, and the inverse of the node's world matrix in the bind pose.
    struct AnimationJoint
    {
        uint32_t                        node            = 0;                            // in AnimationRaw::nodes
        glm::mat4                       inverseBind     = glm::mat4(1.0f);
    };

    struct AnimationRaw
    {
        std::vector<AnimationNode>      nodes;                                          // parents before children
        std::vector<AnimationClipRaw>   clips;
        std::vector<AnimationJoint>     joints;                                         // every skin's joints, one skin after another
    };

}
// clang-format on
