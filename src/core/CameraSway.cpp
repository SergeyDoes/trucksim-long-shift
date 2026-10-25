#include "CameraSway.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace long_shift {
namespace {

constexpr double kMaxStep = 0.1;          // longer frames (loading, hitches) are clamped to avoid jumps
constexpr double kStrengthTimeS = 2.0;    // how fast the strength follows its target
constexpr float kNoiseMinS = 2.0f;        // a smooth noise heads for a new random value every 2-6 s
constexpr float kNoiseMaxS = 6.0f;
constexpr double kTwoPi = 6.283185307179586;
// At randomness 1: the speed along the loop varies by +-50 %, each reach drops to as little as 60 %,
// and the vertical shifts against the horizontal by up to a quarter turn, bending the eight.
constexpr float kSpeedSpread = 0.5f;
constexpr float kReachSpread = 0.4f;
constexpr float kShapeSpread = 1.5707963f;

float smoothstep(double x)
{
    const float t = static_cast<float>(std::clamp(x, 0.0, 1.0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

float sway_strength(const SwayParams& params, std::optional<int> rest_stop_min, float speed_mps)
{
    const float standing = std::clamp(params.standing_strength, 0.0f, 1.0f);
    const float speed = standing + (1.0f - standing) * speed_factor(params.scaling, speed_mps);
    return fatigue_factor(params.scaling, rest_stop_min) * speed;
}

float SmoothNoise::value() const
{
    if (duration_ <= 0.0) {
        return to_;
    }
    return from_ + (to_ - from_) * smoothstep(time_ / duration_);
}

void SmoothNoise::advance(double dt, std::mt19937& random)
{
    time_ += dt;
    if (time_ < duration_) {
        return;
    }
    from_ = to_;
    to_ = std::uniform_real_distribution<float>(-1.0f, 1.0f)(random);
    time_ = 0.0;
    duration_ = std::uniform_real_distribution<float>(kNoiseMinS, kNoiseMaxS)(random);
}

CameraSway::Step CameraSway::step(double dt_s, bool paused)
{
    if (paused) {
        return {};
    }
    const double dt = std::clamp(dt_s, 0.0, kMaxStep);
    strength_ += (target_strength_ - strength_) * static_cast<float>(1.0 - std::exp(-dt / kStrengthTimeS));

    for (SmoothNoise* noise : {&speed_noise_, &left_right_noise_, &up_down_noise_, &shape_noise_}) {
        noise->advance(dt, random_);
    }
    const float randomness = std::clamp(params_.randomness, 0.0f, 1.0f);
    const double loop_time = std::max(0.5f, params_.loop_time_s);
    phase_ += kTwoPi / loop_time * (1.0 + kSpeedSpread * randomness * speed_noise_.value()) * dt;

    const auto reach = [&](float full, const SmoothNoise& noise) {
        return full * (1.0f - kReachSpread * randomness * (0.5f + 0.5f * noise.value()));
    };
    const float shape = kShapeSpread * randomness * shape_noise_.value();
    Step position;
    position.left_right = strength_ * reach(params_.left_right, left_right_noise_) * static_cast<float>(std::sin(phase_));
    position.up_down =
        strength_ * reach(params_.up_down, up_down_noise_) * static_cast<float>(std::sin(2.0 * phase_ + shape));

    const Step step{position.left_right - sent_.left_right, position.up_down - sent_.up_down};
    sent_ = position;
    return step;
}

}  // namespace long_shift
