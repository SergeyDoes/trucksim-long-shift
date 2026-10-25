#pragma once

// Drowsy head sway: the interior camera drifts along a smeared figure eight. Pure logic, no SDK.

#include <cstdint>
#include <optional>
#include <random>

#include "FatigueModel.h"

namespace long_shift {

// Tunables, read from long_shift.ini (see Config.h); these are the defaults.
struct SwayParams {
    // Reach of the sway at full strength, in the game's mouse-look units (about 10-20 is a clear head
    // movement): left-right and up-down.
    float left_right = 10.0f;
    float up_down = 6.0f;
    // Average time for one figure eight, seconds.
    float loop_time_s = 12.0f;
    // 0 = a clean figure eight; up to 1 the speed, reach and shape wander more and more at random.
    float randomness = 0.5f;
    // Share of the sway left when standing still, 0..1; it grows with speed to the full sway at
    // scaling.full_speed_mps.
    float standing_strength = 0.6f;
    // Full at 15 m/s; from 120 minutes before the rest, full when it is due.
    Scaling scaling{15.0f, 120.0f, 0.0f};
};

// How strong the sway should be, 0..1: the fatigue factor times the speed factor, which starts from
// standing_strength instead of 0.
float sway_strength(const SwayParams& params, std::optional<int> rest_stop_min, float speed_mps);

// Glides between random values in [-1, 1], a new one every few seconds.
class SmoothNoise {
public:
    float value() const;
    void advance(double dt, std::mt19937& random);

private:
    float from_ = 0.0f;
    float to_ = 0.0f;
    double time_ = 0.0;
    double duration_ = 0.0;
};

// The camera inputs take a change per frame, like mouse movement, so step() returns how far to move the
// view in this frame. The steps add up to the current sway position and, as the sway fades out, back
// to where the player was looking.
class CameraSway {
public:
    struct Step {
        float left_right = 0.0f;
        float up_down = 0.0f;
    };

    explicit CameraSway(std::uint32_t seed) : random_(seed) {}

    void set_params(const SwayParams& params) { params_ = params; }
    // How strong the sway should be now, 0..1; the actual strength follows smoothly.
    void set_target_strength(float strength) { target_strength_ = strength; }

    // Once per rendered frame. On pause the view stays put and the sway continues from there afterwards.
    Step step(double dt_s, bool paused);

    float strength() const { return strength_; }
    float left_right() const { return sent_.left_right; }  // position sent so far
    float up_down() const { return sent_.up_down; }

private:
    SwayParams params_;
    std::mt19937 random_;
    double phase_ = 0.0;  // radians along the figure eight
    float strength_ = 0.0f;
    float target_strength_ = 0.0f;
    SmoothNoise speed_noise_;
    SmoothNoise left_right_noise_;
    SmoothNoise up_down_noise_;
    SmoothNoise shape_noise_;
    Step sent_;
};

}  // namespace long_shift
