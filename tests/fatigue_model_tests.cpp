#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <random>
#include <vector>

#include "FatigueModel.h"

using long_shift::FatigueInput;
using long_shift::FatigueModel;
using long_shift::FatigueParams;
using long_shift::Phase;
using long_shift::Scaling;

namespace {

constexpr double kFrame = 1.0 / 60.0;
constexpr float kHighwaySpeed = 25.0f;  // m/s = 90 km/h: the speed factor is 1
constexpr float kMaxFrameStep = 0.01f;  // largest offset change per 1/60 s frame without waking

FatigueInput driving(std::optional<int> rest_stop_min, float speed_mps = kHighwaySpeed)
{
    FatigueInput input;
    input.dt_s = kFrame;
    input.rest_stop_min = rest_stop_min;
    input.speed_mps = speed_mps;
    return input;
}

// Yawns of 0.25 fading in over the last 120 minutes before the rest, full at 25 m/s: the setup most
// tests are written against, independent of the defaults.
FatigueParams yawning()
{
    FatigueParams params;
    params.yawn_strength = 0.25f;
    params.yawn_average_interval_s = 30.0f;
    params.yawn_min_pause_s = 8.0f;
    params.yawn_scaling = Scaling{25.0f, 120.0f, 0.0f};
    return params;
}

// What input.steering shows while the player does not touch the controls: the sign of the total
// steering, which is our offset with the game's sign convention (positive = left).
float idle_player_steering(float offset)
{
    return offset > 0.0f ? -1.0f : offset < 0.0f ? 1.0f : 0.0f;
}

struct DriveStats {
    int yawns = 0;
    int wakes = 0;
    int left = 0;
    int right = 0;
    std::vector<float> peaks;
    float max_abs_offset = 0.0f;
    float max_step = 0.0f;
};

// Drives the model with an idle player for the given time and collects what the drift looked like.
DriveStats simulate(FatigueModel& model, FatigueInput input, double seconds)
{
    DriveStats stats;
    float previous = model.offset();
    float peak = 0.0f;
    for (double t = 0.0; t < seconds; t += input.dt_s) {
        const Phase before = model.phase();
        input.input_steering = idle_player_steering(model.offset());
        const float offset = model.update(input);
        if (before == Phase::idle && model.phase() == Phase::drifting) {
            ++stats.yawns;
            peak = 0.0f;
        }
        if (before != Phase::waking && model.phase() == Phase::waking) {
            ++stats.wakes;
        }
        if (std::fabs(offset) > std::fabs(peak)) {
            peak = offset;
        }
        if (before != Phase::idle && model.phase() == Phase::idle) {
            stats.peaks.push_back(std::fabs(peak));
            (peak > 0.0f ? stats.right : stats.left)++;
        }
        stats.max_abs_offset = std::max(stats.max_abs_offset, std::fabs(offset));
        stats.max_step = std::max(stats.max_step, std::fabs(offset - previous));
        previous = offset;
    }
    return stats;
}

// Runs frames until a yawn pulls with a clearly visible offset.
bool drive_until_yawning(FatigueModel& model, FatigueInput input, float min_offset = 0.05f)
{
    for (int frame = 0; frame < 60 * 60 * 30; ++frame) {
        input.input_steering = idle_player_steering(model.offset());
        model.update(input);
        if (model.phase() == Phase::drifting && std::fabs(model.offset()) > min_offset) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("fatigue factor grows over the chosen minutes before the rest")
{
    const Scaling scaling{25.0f, 120.0f, 0.0f};
    CHECK(long_shift::fatigue_factor(scaling, std::nullopt) == 0.0f);
    CHECK(long_shift::fatigue_factor(scaling, 600) == 0.0f);
    CHECK(long_shift::fatigue_factor(scaling, 120) == 0.0f);
    CHECK(long_shift::fatigue_factor(scaling, 90) == doctest::Approx(0.25f));
    CHECK(long_shift::fatigue_factor(scaling, 60) == doctest::Approx(0.5f));
    CHECK(long_shift::fatigue_factor(scaling, 0) == 1.0f);
    CHECK(long_shift::fatigue_factor(scaling, -45) == 1.0f);
}

TEST_CASE("equal fatigue ends make a switch: off until the rest is due, then full")
{
    const Scaling scaling{25.0f, 0.0f, 0.0f};
    CHECK(long_shift::fatigue_factor(scaling, 1) == 0.0f);
    CHECK(long_shift::fatigue_factor(scaling, 0) == 1.0f);
    CHECK(long_shift::fatigue_factor(scaling, -10) == 1.0f);
}

TEST_CASE("speed factor ramps from 0 to full speed in both directions")
{
    const Scaling scaling{25.0f, 120.0f, 0.0f};
    CHECK(long_shift::speed_factor(scaling, 0.0f) == 0.0f);
    CHECK(long_shift::speed_factor(scaling, 5.0f) == doctest::Approx(0.2f));
    CHECK(long_shift::speed_factor(scaling, 12.5f) == doctest::Approx(0.5f));
    CHECK(long_shift::speed_factor(scaling, 25.0f) == 1.0f);
    CHECK(long_shift::speed_factor(scaling, 40.0f) == 1.0f);
    CHECK(long_shift::speed_factor(scaling, -12.5f) == doctest::Approx(0.5f));
}

TEST_CASE("yawns start 30 minutes before the rest by default")
{
    const FatigueParams params;
    CHECK(long_shift::fatigue_factor(params.yawn_scaling, 31) == 0.0f);
    CHECK(long_shift::fatigue_factor(params.yawn_scaling, 15) == doctest::Approx(0.5f));
    CHECK(long_shift::fatigue_factor(params.yawn_scaling, 0) == 1.0f);
}

TEST_CASE("driver who may still drive never drifts")
{
    for (const int rest_stop_min : {600, 121, 120}) {
        FatigueModel model(yawning(), 1);
        const DriveStats stats = simulate(model, driving(rest_stop_min), 60.0 * 60.0);
        CHECK(stats.yawns == 0);
        CHECK(stats.max_abs_offset == 0.0f);
        CHECK(model.yawn_scale() == 0.0f);
    }
}

TEST_CASE("unknown rest.stop means no drift")
{
    FatigueModel model(yawning(), 2);
    const DriveStats stats = simulate(model, driving(std::nullopt), 30.0 * 60.0);
    CHECK(stats.max_abs_offset == 0.0f);
}

TEST_CASE("standing truck never drifts, a crawling one barely")
{
    FatigueModel standing(yawning(), 3);
    CHECK(simulate(standing, driving(0, 0.0f), 30.0 * 60.0).max_abs_offset == 0.0f);

    FatigueModel crawling(yawning(), 3);
    const DriveStats stats = simulate(crawling, driving(0, -1.0f), 30.0 * 60.0);  // 1 m/s in reverse
    CHECK(stats.max_abs_offset > 0.0f);
    CHECK(stats.max_abs_offset <= yawning().yawn_strength / 25.0f);
}

TEST_CASE("no drift while paused")
{
    FatigueModel model(yawning(), 4);
    REQUIRE(drive_until_yawning(model, driving(0)));
    FatigueInput paused = driving(0);
    paused.paused = true;
    for (int frame = 0; frame < 600; ++frame) {
        CHECK(model.update(paused) == 0.0f);
    }
    CHECK(model.phase() == Phase::idle);
}

TEST_CASE("yawn strength 0 turns yawns off")
{
    FatigueParams params = yawning();
    params.yawn_strength = 0.0f;
    FatigueModel model(params, 20);
    const DriveStats stats = simulate(model, driving(0), 30.0 * 60.0);
    CHECK(stats.yawns == 0);
    CHECK(stats.max_abs_offset == 0.0f);
}

TEST_CASE("tired driver yawns both ways, smoothly, with varying strength")
{
    const FatigueParams params = yawning();
    FatigueModel model(params, 5);
    const DriveStats stats = simulate(model, driving(0), 30.0 * 60.0);

    CHECK(stats.yawns >= 10);
    CHECK(stats.wakes == 0);  // the player never touched the controls
    CHECK(stats.left > 0);
    CHECK(stats.right > 0);
    CHECK(stats.max_abs_offset <= params.yawn_strength);
    CHECK(stats.max_abs_offset > 0.5f * params.yawn_strength);
    CHECK(stats.max_step <= kMaxFrameStep);
    REQUIRE(stats.peaks.size() >= 10);
    const auto [weakest, strongest] = std::minmax_element(stats.peaks.begin(), stats.peaks.end());
    CHECK(*strongest - *weakest > 0.05f);
}

TEST_CASE("yawns start every 30 seconds on average")
{
    FatigueModel model(yawning(), 14);
    const DriveStats stats = simulate(model, driving(0), 60.0 * 60.0);
    CHECK(stats.yawns >= 100);
    CHECK(stats.yawns <= 140);
}

TEST_CASE("drift fades in as the rest approaches")
{
    FatigueModel far(yawning(), 6);
    CHECK(simulate(far, driving(121), 30.0 * 60.0).max_abs_offset == 0.0f);

    FatigueModel half(yawning(), 6);
    FatigueModel full(yawning(), 6);  // same seed: the same yawns, only the fatigue differs
    const DriveStats half_stats = simulate(half, driving(60), 10.0 * 60.0);
    const DriveStats full_stats = simulate(full, driving(-30), 10.0 * 60.0);
    CHECK(half.yawn_scale() == doctest::Approx(0.5f));
    CHECK(full.yawn_scale() == 1.0f);
    REQUIRE(full_stats.max_abs_offset > 0.0f);
    CHECK(half_stats.max_abs_offset == doctest::Approx(full_stats.max_abs_offset * 0.5f).epsilon(0.01));
    CHECK(half_stats.yawns == full_stats.yawns);
}

TEST_CASE("drift grows with speed")
{
    FatigueModel slow(yawning(), 7);
    FatigueModel fast(yawning(), 7);  // same seed: the same yawns, only the speed differs
    const DriveStats slow_stats = simulate(slow, driving(0, 5.0f), 10.0 * 60.0);
    const DriveStats fast_stats = simulate(fast, driving(0, 20.0f), 10.0 * 60.0);
    REQUIRE(fast_stats.max_abs_offset > 0.0f);
    CHECK(slow_stats.max_abs_offset == doctest::Approx(fast_stats.max_abs_offset * 0.25f).epsilon(0.01));
}

TEST_CASE("steering against a yawn wakes the driver up")
{
    const FatigueParams params = yawning();
    FatigueModel model(params, 8);
    FatigueInput input = driving(0);
    REQUIRE(drive_until_yawning(model, input));

    // The player steers against the drift: input.steering takes the sign of our offset.
    input.input_steering = model.offset() > 0.0f ? 1.0f : -1.0f;
    model.update(input);
    CHECK(model.phase() == Phase::waking);
    for (double t = 0.0; t < 1.0; t += kFrame) {
        model.update(input);
    }
    CHECK(model.phase() == Phase::idle);
    CHECK(std::fabs(model.offset()) < 0.005f);

    // No new yawn right after waking up.
    input.input_steering = 0.0f;
    for (double t = 0.0; t < params.yawn_min_pause_s - 0.5; t += kFrame) {
        CHECK(model.update(input) == 0.0f);
    }
}

TEST_CASE("letting the truck drift does not count as waking up")
{
    FatigueModel model(yawning(), 9);
    FatigueInput input = driving(0);
    REQUIRE(drive_until_yawning(model, input));
    const float drift = model.offset();
    for (double t = 0.0; t < 0.5; t += kFrame) {
        input.input_steering = idle_player_steering(model.offset());
        model.update(input);
        CHECK(model.phase() != Phase::waking);
    }
    CHECK(std::fabs(model.offset()) >= std::fabs(drift) * 0.4f);
}

TEST_CASE("settings can change while driving")
{
    FatigueParams params = yawning();
    params.yawn_strength = 0.0f;
    FatigueModel model(params, 19);
    CHECK(simulate(model, driving(0), 60.0).max_abs_offset == 0.0f);

    params.yawn_strength = 0.25f;
    model.set_params(params);
    const DriveStats after = simulate(model, driving(0), 5.0 * 60.0);
    CHECK(after.yawns > 0);
    CHECK(after.max_step <= kMaxFrameStep);  // no jump on the change
}

TEST_CASE("same seed gives the same drift")
{
    FatigueModel a(yawning(), 10);
    FatigueModel b(yawning(), 10);
    FatigueModel c(yawning(), 11);
    bool differs = false;
    FatigueInput input = driving(0);
    for (int frame = 0; frame < 60 * 60 * 10; ++frame) {
        input.input_steering = idle_player_steering(a.offset());
        const float offset = a.update(input);
        CHECK(b.update(input) == offset);
        differs = differs || c.update(input) != offset;
    }
    CHECK(differs);
}

TEST_CASE("a long frame does not make the offset jump")
{
    FatigueModel model(yawning(), 12);
    FatigueInput input = driving(0);
    REQUIRE(drive_until_yawning(model, input, 0.01f));
    const float before = model.offset();
    input.dt_s = 5.0;
    input.input_steering = idle_player_steering(before);
    const float after = model.update(input);
    CHECK(std::fabs(after - before) <= 0.1f);
}

TEST_CASE("offset stays within bounds whatever the input")
{
    FatigueParams extreme = yawning();
    extreme.yawn_strength = 1.0f;
    for (const FatigueParams& params : {yawning(), extreme}) {
        FatigueModel model(params, 13);
        std::mt19937 random(99);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (int frame = 0; frame < 200000; ++frame) {
            FatigueInput input;
            input.dt_s = unit(random) * 0.5;
            input.speed_mps = (unit(random) - 0.5f) * 80.0f;
            input.input_steering = std::round((unit(random) - 0.5f) * 2.0f);
            if (unit(random) > 0.05f) {
                input.rest_stop_min = static_cast<int>(unit(random) * 250.0f) - 50;
            }
            input.paused = unit(random) < 0.02f;
            const float offset = model.update(input);
            REQUIRE(std::isfinite(offset));
            REQUIRE(std::fabs(offset) <= params.yawn_strength);
        }
    }
}
