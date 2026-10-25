#pragma once

// Yawn model: pure logic, no SDK dependencies, so it can be unit-tested outside the game.

#include <cstdint>
#include <optional>
#include <random>

namespace long_shift {

// How strongly an effect applies depending on speed and fatigue: two factors from 0 to 1, multiplied.
struct Scaling {
    // Speed factor: 0 when standing, growing linearly to 1 at full_speed_mps and above.
    // m/s, as truck.speed reports it (25 m/s = 90 km/h).
    float full_speed_mps = 25.0f;
    // Fatigue factor from rest.stop, the in-game minutes until the mandatory rest: 0 until
    // start_minutes_before_rest before the rest, growing linearly to 1 at full_minutes_before_rest
    // (0 = when the rest is due; overdue stays at 1). Unknown rest.stop (fatigue simulation off) means 0.
    float start_minutes_before_rest = 120.0f;
    float full_minutes_before_rest = 0.0f;
};

// Tunables, read from long_shift.ini (see Config.h); these are the defaults.
struct FatigueParams {
    // Yawns: a strong pull to one side for a few seconds. The strength is a fraction of full steering
    // lock with both factors at 1; 0 turns yawns off.
    float yawn_strength = 0.25f;
    // Average time from the start of one yawn to the start of the next if the player lets them run
    // their course. Cannot be shorter than a yawn plus yawn_min_pause_s.
    float yawn_average_interval_s = 30.0f;
    // Minimum pause after a yawn ends, seconds.
    float yawn_min_pause_s = 10.0f;
    // Full at 25 m/s; from 30 minutes before the rest, full when it is due.
    Scaling yawn_scaling{25.0f, 30.0f, 0.0f};
};

// What the model needs from one telemetry frame.
struct FatigueInput {
    double dt_s = 0.0;                 // real time since the previous update
    std::optional<int> rest_stop_min;  // rest.stop in in-game minutes; empty when the game does not report it
    float speed_mps = 0.0f;            // truck.speed: m/s, negative in reverse
    float input_steering = 0.0f;       // truck.input.steering: positive = left; while our input is active only its sign is meaningful
    bool paused = false;
};

enum class Phase {
    idle,       // no yawn
    drifting,   // the yawn eases in towards its strength and holds there
    releasing,  // the yawn ended on its own and eases back to 0
    waking,     // the player steered against the yawn, it collapses quickly
};

// 0..1 factor from rest.stop.
float fatigue_factor(const Scaling& scaling, std::optional<int> rest_stop_min);

// 0..1 factor from speed; the direction of travel does not matter.
float speed_factor(const Scaling& scaling, float speed_mps);

// As the driver gets tired the steering pulls hard to one side in random yawns, each until it ends on
// its own or the player steers against it. Scaled by the yawn speed and fatigue factors.
class FatigueModel {
public:
    FatigueModel(const FatigueParams& params, std::uint32_t seed);

    // Takes new settings while driving; the current yawn finishes smoothly.
    void set_params(const FatigueParams& params) { params_ = params; }
    const FatigueParams& params() const { return params_; }

    // Advances the model by one frame and returns the steering offset: positive steers right (the
    // convention of the semantical `steering` input), never more than full lock.
    float update(const FatigueInput& input);

    float offset() const { return offset_; }
    // Speed factor times fatigue factor of the last frame.
    float yawn_scale() const { return yawn_scale_; }
    Phase phase() const { return phase_; }

private:
    bool player_counter_steers(float input_steering) const;
    bool yawn_starts(double dt);
    void start_yawn();
    void enter(Phase phase);
    void finish_yawn();
    void advance(double dt);
    float envelope() const;  // 0..1 shape of the current yawn
    float uniform(float from, float to);

    FatigueParams params_;
    std::mt19937 random_;
    float yawn_scale_ = 0.0f;
    float offset_ = 0.0f;
    Phase phase_ = Phase::idle;
    double phase_time_ = 0.0;  // seconds since the current phase began
    double pause_left_ = 0.0;  // seconds until another yawn may start
    float level_ = 0.0f;       // envelope when releasing or waking began

    // Current yawn, drawn when it starts.
    float direction_ = 1.0f;  // +1 right, -1 left
    float strength_ = 0.0f;   // before scaling
    double ramp_s_ = 0.0;
    double hold_s_ = 0.0;
    double release_s_ = 0.0;
};

}  // namespace long_shift
