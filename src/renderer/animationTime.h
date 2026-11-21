// clang-format off
#pragma once

#include "rasm/animation.h"

namespace rasm
{

    // The GPU adds the clock in floats, so it gets the seconds since a base that moves forward this often: a float
    // of at most 64 has steps of 8 µs.
    constexpr double    ANIMATION_REBASE_SECONDS    = 64.0;

    double              getAnimationBase(double base, double clock);
    float               getAnimationPhase(const AnimationPlay& play, float duration, double base);

}
// clang-format on
