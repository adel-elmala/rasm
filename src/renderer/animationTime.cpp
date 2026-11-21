#include "renderer/animationTime.h"

#include <algorithm>
#include <cmath>

namespace rasm
{
    // A clock that went back, or ran past the window, starts a new base.
    double getAnimationBase(double base, double clock)
    {
        const double sinceBase = clock - base;
        return sinceBase >= 0.0 && sinceBase < ANIMATION_REBASE_SECONDS ? base : clock;
    }

    // The clip's time at the base, in doubles, so the GPU's float sum `phase + (clock - base) * speed` stays small.
    // A looping clip wraps it into one loop. A clip that holds its last pose (or its first, played backwards) is
    // clamped on the side it moves away from, so it holds there; on the other side it keeps its distance, so a clip
    // that starts later still waits.
    float getAnimationPhase(const AnimationPlay &play, float duration, double base)
    {
        double phase = static_cast<double>(play.clipTime) + (base - play.startTime) * static_cast<double>(play.speed);
        if (duration <= 0.0f)
            return 0.0f;
        if (play.loop)
            phase -= std::floor(phase / duration) * duration;
        else if (play.speed >= 0.0f)
            phase = std::min(phase, static_cast<double>(duration));
        else
            phase = std::max(phase, 0.0);
        return static_cast<float>(phase);
    }
}
