#include "FatigueModel.h"

#include <algorithm>
#include <cmath>

namespace long_shift {
namespace {

constexpr double kMaxStep = 0.1;            // longer frames (loading, hitches) are clamped to avoid jumps
// Yawn timing, seconds: ease in, hold, ease out.
constexpr float kRampMin = 2.0f, kRampMax = 4.0f;
constexpr float kHoldMin = 2.0f, kHoldMax = 5.0f;
constexpr float kReleaseMin = 1.0f, kReleaseMax = 2.0f;
constexpr double kMeanYawnS = (kRampMin + kRampMax + kHoldMin + kHoldMax + kReleaseMin + kReleaseMax) / 2.0;
constexpr double kMinIdleWaitS = 1.0;
constexpr double kWakeTimeConstant = 0.15;  // s: how fast the offset collapses once the player reacts
constexpr float kEndLevel = 0.002f;         // a waking envelope below this counts as finished
constexpr float kWakeMinOffset = 0.01f;     // smaller offsets may not have reached input.steering yet
constexpr float kWakeSteering = 0.1f;

float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

// 0 -> 1 with zero slope at both ends, so the offset never jerks.
float smoothstep(double x)
{
    const float t = clamp01(static_cast<float>(x));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

float fatigue_factor(const Scaling& scaling, std::optional<int> rest_stop_min)
{
    if (!rest_stop_min) {
        return 0.0f;
    }
    const float minutes_before_rest = static_cast<float>(*rest_stop_min);
    const float start = scaling.start_minutes_before_rest;
    const float full = scaling.full_minutes_before_rest;
    if (start <= full) {
        return minutes_before_rest <= full ? 1.0f : 0.0f;
    }
    return clamp01((start - minutes_before_rest) / (start - full));
}

float speed_factor(const Scaling& scaling, float speed_mps)
{
    const float speed = std::fabs(speed_mps);
    if (scaling.full_speed_mps <= 0.0f) {
        return speed > 0.0f ? 1.0f : 0.0f;
    }
    return clamp01(speed / scaling.full_speed_mps);
}

FatigueModel::FatigueModel(const FatigueParams& params, std::uint32_t seed)
    : params_(params), random_(seed), pause_left_(params.yawn_min_pause_s)
{
}

float FatigueModel::update(const FatigueInput& input)
{
    if (input.paused) {
        // Drop the yawn rather than jump back into it after the pause.
        phase_ = Phase::idle;
        offset_ = 0.0f;
        yawn_scale_ = 0.0f;
        return offset_;
    }

    const double dt = std::clamp(input.dt_s, 0.0, kMaxStep);
    yawn_scale_ = fatigue_factor(params_.yawn_scaling, input.rest_stop_min) *
                  speed_factor(params_.yawn_scaling, input.speed_mps);

    if ((phase_ == Phase::drifting || phase_ == Phase::releasing) && player_counter_steers(input.input_steering)) {
        enter(Phase::waking);
    }
    advance(dt);
    if (phase_ == Phase::idle && yawn_starts(dt)) {
        start_yawn();
    }
    offset_ = std::clamp(direction_ * strength_ * envelope() * yawn_scale_, -1.0f, 1.0f);
    return offset_;
}

// input.steering reflects the offset the game saw last frame. Its positive side is left, ours is
// right, so while the player lets the truck drift the two have opposite signs; the same sign means
// the player steers against the drift hard enough to outweigh it.
bool FatigueModel::player_counter_steers(float input_steering) const
{
    if (std::fabs(offset_) < kWakeMinOffset) {
        return false;
    }
    return offset_ > 0.0f ? input_steering > kWakeSteering : input_steering < -kWakeSteering;
}

bool FatigueModel::yawn_starts(double dt)
{
    pause_left_ = std::max(0.0, pause_left_ - dt);
    if (pause_left_ > 0.0 || yawn_scale_ <= 0.0f || params_.yawn_strength <= 0.0f ||
        params_.yawn_average_interval_s <= 0.0f) {
        return false;
    }
    // Start to start = yawn + pause + a random wait; pick the wait so the mean matches the interval.
    const double mean_wait_s =
        std::max(kMinIdleWaitS, params_.yawn_average_interval_s - kMeanYawnS - params_.yawn_min_pause_s);
    const double rate_per_s = 1.0 / mean_wait_s;
    return uniform(0.0f, 1.0f) < 1.0 - std::exp(-rate_per_s * dt);
}

void FatigueModel::start_yawn()
{
    direction_ = uniform(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
    strength_ = params_.yawn_strength * uniform(0.6f, 1.0f);
    ramp_s_ = uniform(kRampMin, kRampMax);
    hold_s_ = uniform(kHoldMin, kHoldMax);
    release_s_ = uniform(kReleaseMin, kReleaseMax);
    phase_ = Phase::drifting;
    phase_time_ = 0.0;
}

void FatigueModel::enter(Phase phase)
{
    level_ = envelope();
    phase_ = phase;
    phase_time_ = 0.0;
}

void FatigueModel::finish_yawn()
{
    phase_ = Phase::idle;
    phase_time_ = 0.0;
    pause_left_ = params_.yawn_min_pause_s;
}

void FatigueModel::advance(double dt)
{
    phase_time_ += dt;
    switch (phase_) {
    case Phase::idle:
        break;
    case Phase::drifting:
        if (phase_time_ >= ramp_s_ + hold_s_) {
            const double overshoot = phase_time_ - (ramp_s_ + hold_s_);
            phase_time_ = ramp_s_ + hold_s_;
            enter(Phase::releasing);
            phase_time_ = overshoot;
        }
        break;
    case Phase::releasing:
        if (phase_time_ >= release_s_) {
            finish_yawn();
        }
        break;
    case Phase::waking:
        if (envelope() < kEndLevel) {
            finish_yawn();
        }
        break;
    }
}

float FatigueModel::envelope() const
{
    switch (phase_) {
    case Phase::idle:
        return 0.0f;
    case Phase::drifting:
        return phase_time_ < ramp_s_ ? smoothstep(phase_time_ / ramp_s_) : 1.0f;
    case Phase::releasing:
        return level_ * (1.0f - smoothstep(phase_time_ / release_s_));
    case Phase::waking:
        return level_ * static_cast<float>(std::exp(-phase_time_ / kWakeTimeConstant));
    }
    return 0.0f;
}

float FatigueModel::uniform(float from, float to)
{
    return std::uniform_real_distribution<float>(from, to)(random_);
}

}  // namespace long_shift
