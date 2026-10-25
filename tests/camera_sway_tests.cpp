#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

#include "CameraSway.h"

using long_shift::CameraSway;
using long_shift::SwayParams;

namespace {

constexpr double kFrame = 1.0 / 143.0;

// Runs the sway for the given time and returns the largest step of a frame.
float run(CameraSway& sway, double seconds, bool paused = false)
{
    float largest = 0.0f;
    for (double t = 0.0; t < seconds; t += kFrame) {
        const CameraSway::Step step = sway.step(kFrame, paused);
        largest = std::max({largest, std::fabs(step.left_right), std::fabs(step.up_down)});
    }
    return largest;
}

struct LoopStats {
    std::vector<double> loop_times;     // between upward zero crossings of left-right
    std::vector<float> up_down_at_start;  // where up-down is at those crossings
};

LoopStats loops(float randomness, unsigned seed)
{
    SwayParams params;
    params.randomness = randomness;
    CameraSway sway(seed);
    sway.set_params(params);
    sway.set_target_strength(1.0f);
    run(sway, 30.0);  // let the strength settle

    LoopStats stats;
    float previous = sway.left_right();
    double last_crossing = -1.0;
    for (double t = 0.0; t < 40.0 * params.loop_time_s; t += kFrame) {
        sway.step(kFrame, false);
        if (previous < 0.0f && sway.left_right() >= 0.0f) {
            if (last_crossing >= 0.0) {
                stats.loop_times.push_back(t - last_crossing);
            }
            last_crossing = t;
            stats.up_down_at_start.push_back(sway.up_down());
        }
        previous = sway.left_right();
    }
    return stats;
}

}  // namespace

TEST_CASE("the sway is there when standing, stronger with speed, and only when tired")
{
    const SwayParams params;  // standing 0.6, full at 15 m/s, from 120 to 0 minutes before the rest
    CHECK(long_shift::sway_strength(params, std::nullopt, 15.0f) == 0.0f);
    CHECK(long_shift::sway_strength(params, 600, 15.0f) == 0.0f);
    CHECK(long_shift::sway_strength(params, 0, 0.0f) == doctest::Approx(0.6f));
    CHECK(long_shift::sway_strength(params, 0, 7.5f) == doctest::Approx(0.8f));
    CHECK(long_shift::sway_strength(params, 0, -15.0f) == doctest::Approx(1.0f));
    CHECK(long_shift::sway_strength(params, 60, 0.0f) == doctest::Approx(0.3f));

    SwayParams none_standing;
    none_standing.standing_strength = 0.0f;
    CHECK(long_shift::sway_strength(none_standing, 0, 0.0f) == 0.0f);
}

TEST_CASE("no strength, no sway")
{
    CameraSway sway(1);
    sway.set_params(SwayParams{});
    CHECK(run(sway, 30.0) == 0.0f);
    CHECK(sway.left_right() == 0.0f);
    CHECK(sway.up_down() == 0.0f);
}

TEST_CASE("a clean sway is a figure eight: up-down twice for every left-right")
{
    SwayParams params;
    params.randomness = 0.0f;
    CameraSway sway(2);
    sway.set_params(params);
    sway.set_target_strength(1.0f);
    run(sway, 30.0);

    int left_right_crossings = 0;
    int up_down_crossings = 0;
    float reach_left_right = 0.0f;
    float reach_up_down = 0.0f;
    float previous_left_right = sway.left_right();
    float previous_up_down = sway.up_down();
    for (double t = 0.0; t < 10.0 * params.loop_time_s; t += kFrame) {
        sway.step(kFrame, false);
        left_right_crossings += (sway.left_right() > 0.0f) != (previous_left_right > 0.0f) ? 1 : 0;
        up_down_crossings += (sway.up_down() > 0.0f) != (previous_up_down > 0.0f) ? 1 : 0;
        reach_left_right = std::max(reach_left_right, std::fabs(sway.left_right()));
        reach_up_down = std::max(reach_up_down, std::fabs(sway.up_down()));
        previous_left_right = sway.left_right();
        previous_up_down = sway.up_down();
    }
    CHECK(left_right_crossings >= 19);
    CHECK(left_right_crossings <= 21);
    CHECK(up_down_crossings >= 39);
    CHECK(up_down_crossings <= 41);
    CHECK(reach_left_right == doctest::Approx(params.left_right).epsilon(0.01));
    CHECK(reach_up_down == doctest::Approx(params.up_down).epsilon(0.01));
}

TEST_CASE("the steps add up to the position and lead back when the sway fades out")
{
    CameraSway sway(3);
    sway.set_params(SwayParams{});
    sway.set_target_strength(1.0f);
    float left_right = 0.0f;
    float up_down = 0.0f;
    for (double t = 0.0; t < 60.0; t += kFrame) {
        const CameraSway::Step step = sway.step(kFrame, false);
        left_right += step.left_right;
        up_down += step.up_down;
    }
    CHECK(std::fabs(left_right - sway.left_right()) < 1e-3f);
    CHECK(std::fabs(up_down - sway.up_down()) < 1e-3f);
    CHECK(sway.strength() == doctest::Approx(1.0f).epsilon(0.001));

    sway.set_target_strength(0.0f);  // rested, or switched off in the ini
    float largest_step = 0.0f;
    for (double t = 0.0; t < 30.0; t += kFrame) {
        const CameraSway::Step step = sway.step(kFrame, false);
        left_right += step.left_right;
        up_down += step.up_down;
        largest_step = std::max({largest_step, std::fabs(step.left_right), std::fabs(step.up_down)});
    }
    CHECK(largest_step < 0.2f);  // the head glides back, it does not snap
    CHECK(std::fabs(left_right) < 0.01f);  // back where the player was looking
    CHECK(std::fabs(up_down) < 0.01f);
}

TEST_CASE("randomness smears the figure eight")
{
    const auto spread = [](const std::vector<double>& values) {
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        return *high - *low;
    };
    const auto widest = [](const std::vector<float>& values) {
        float widest_value = 0.0f;
        for (const float value : values) {
            widest_value = std::max(widest_value, std::fabs(value));
        }
        return widest_value;
    };
    const SwayParams defaults;

    const LoopStats clean = loops(0.0f, 4);
    REQUIRE(clean.loop_times.size() >= 30);
    CHECK(spread(clean.loop_times) < 0.02 * defaults.loop_time_s);  // every loop the same
    CHECK(widest(clean.up_down_at_start) < 0.05f * defaults.up_down);  // always the same shape

    const LoopStats smeared = loops(1.0f, 4);
    REQUIRE(smeared.loop_times.size() >= 20);
    CHECK(spread(smeared.loop_times) > 0.3 * defaults.loop_time_s);
    CHECK(widest(smeared.up_down_at_start) > 0.3f * defaults.up_down);  // the eight bends
}

TEST_CASE("pause holds the view still and the sway resumes without a jump")
{
    CameraSway sway(5);
    sway.set_params(SwayParams{});
    sway.set_target_strength(1.0f);
    run(sway, 20.0);
    const float left_right = sway.left_right();
    const float up_down = sway.up_down();
    CHECK(run(sway, 5.0, true) == 0.0f);
    CHECK(sway.left_right() == left_right);
    CHECK(sway.up_down() == up_down);
    const CameraSway::Step step = sway.step(kFrame, false);
    CHECK(std::fabs(step.left_right) < 0.2f);
    CHECK(std::fabs(step.up_down) < 0.2f);
}

TEST_CASE("no jumps between frames, even after a long one")
{
    SwayParams params;
    params.randomness = 1.0f;
    CameraSway sway(6);
    sway.set_params(params);
    sway.set_target_strength(1.0f);
    CHECK(run(sway, 5.0 * 60.0) < 0.2f);

    const CameraSway::Step step = sway.step(5.0, false);  // a hitch: clamped to 0.1 s
    CHECK(std::fabs(step.left_right) < 2.5f);
    CHECK(std::fabs(step.up_down) < 2.5f);
}

TEST_CASE("same seed, same sway")
{
    CameraSway a(7);
    CameraSway b(7);
    CameraSway c(8);
    for (CameraSway* sway : {&a, &b, &c}) {
        sway->set_params(SwayParams{});
        sway->set_target_strength(1.0f);
    }
    bool differs = false;
    for (double t = 0.0; t < 60.0; t += kFrame) {
        const CameraSway::Step step = a.step(kFrame, false);
        CHECK(b.step(kFrame, false).left_right == step.left_right);
        differs = differs || c.step(kFrame, false).left_right != step.left_right;
    }
    CHECK(differs);
}
