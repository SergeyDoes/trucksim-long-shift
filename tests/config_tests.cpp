#include <doctest/doctest.h>

#include <string>

#include "Config.h"

using long_shift::FatigueParams;
using long_shift::parse_settings;
using long_shift::ParsedSettings;
using long_shift::Scaling;
using long_shift::Settings;
using long_shift::SwayParams;

namespace {

bool same(const Scaling& a, const Scaling& b)
{
    return a.full_speed_mps == b.full_speed_mps && a.start_minutes_before_rest == b.start_minutes_before_rest &&
           a.full_minutes_before_rest == b.full_minutes_before_rest;
}

bool same(const Settings& a, const Settings& b)
{
    const FatigueParams& x = a.model;
    const FatigueParams& y = b.model;
    const SwayParams& c = a.camera;
    const SwayParams& d = b.camera;
    return a.enabled == b.enabled && a.status_log == b.status_log && x.yawn_strength == y.yawn_strength &&
           x.yawn_average_interval_s == y.yawn_average_interval_s && x.yawn_min_pause_s == y.yawn_min_pause_s &&
           same(x.yawn_scaling, y.yawn_scaling) && c.left_right == d.left_right && c.up_down == d.up_down &&
           c.loop_time_s == d.loop_time_s && c.randomness == d.randomness &&
           c.standing_strength == d.standing_strength && same(c.scaling, d.scaling);
}

bool mentions(const ParsedSettings& parsed, const std::string& text)
{
    for (const std::string& warning : parsed.warnings) {
        if (warning.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("the default file holds exactly the defaults")
{
    const ParsedSettings parsed = parse_settings(long_shift::default_settings_text());
    CHECK(parsed.warnings.empty());
    CHECK(same(parsed.settings, Settings{}));
}

TEST_CASE("the status log is off by default")
{
    CHECK_FALSE(Settings{}.status_log);
}

TEST_CASE("an empty file means defaults")
{
    const ParsedSettings parsed = parse_settings("");
    CHECK(parsed.warnings.empty());
    CHECK(same(parsed.settings, Settings{}));
}

TEST_CASE("all settings are read")
{
    const ParsedSettings parsed = parse_settings(R"(
[general]
enabled = 0
status_log = 0
[yawn]
strength = 0.3
average_interval_s = 45
min_pause_s = 12
full_speed_mps = 20
start_minutes_before_rest = 45
full_minutes_before_rest = -60
[camera]
left_right = 12
up_down = 7
loop_time_s = 10
randomness = 0.8
standing_strength = 0.1
full_speed_mps = 18
start_minutes_before_rest = 60
full_minutes_before_rest = -10
)");
    CHECK(parsed.warnings.empty());
    const Settings& s = parsed.settings;
    CHECK_FALSE(s.enabled);
    CHECK_FALSE(s.status_log);
    CHECK(s.model.yawn_strength == doctest::Approx(0.3f));
    CHECK(s.model.yawn_average_interval_s == 45.0f);
    CHECK(s.model.yawn_min_pause_s == 12.0f);
    CHECK(s.model.yawn_scaling.full_speed_mps == 20.0f);
    CHECK(s.model.yawn_scaling.start_minutes_before_rest == 45.0f);
    CHECK(s.model.yawn_scaling.full_minutes_before_rest == -60.0f);
    CHECK(s.camera.left_right == 12.0f);
    CHECK(s.camera.up_down == 7.0f);
    CHECK(s.camera.loop_time_s == 10.0f);
    CHECK(s.camera.randomness == doctest::Approx(0.8f));
    CHECK(s.camera.standing_strength == doctest::Approx(0.1f));
    CHECK(s.camera.scaling.full_speed_mps == 18.0f);
    CHECK(s.camera.scaling.start_minutes_before_rest == 60.0f);
    CHECK(s.camera.scaling.full_minutes_before_rest == -10.0f);
}

TEST_CASE("Notepad habits are tolerated: BOM, CRLF, comments, spaces, case, decimal comma")
{
    const ParsedSettings parsed =
        parse_settings("\xEF\xBB\xBF; comment\r\n  [ Yawn ]  \r\n\tSTRENGTH =  0,15 ; weaker\r\n# another comment\r\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.settings.model.yawn_strength == doctest::Approx(0.15f));
}

TEST_CASE("mistakes are reported with line numbers and keep the defaults")
{
    const Settings defaults;
    const ParsedSettings parsed = parse_settings(R"([yawn]
strength = lots
strenght = 0.2
average_interval_s = 5000
[camera]
left_right = 12
[general]
enabled = 2
just some text
[broken
amplitude = 5
)");
    CHECK(parsed.warnings.size() == 7);
    CHECK(mentions(parsed, "line 2: strength: \"lots\" is not a number"));
    CHECK(mentions(parsed, "line 3: unknown setting [yawn] strenght"));
    CHECK(mentions(parsed, "line 4: average_interval_s must be between 5 and 3600"));
    CHECK(mentions(parsed, "line 8: enabled must be 0 or 1"));
    CHECK(mentions(parsed, "line 9: expected \"name = value\""));
    CHECK(mentions(parsed, "line 10: broken section header"));
    CHECK(mentions(parsed, "line 11: unknown setting [] amplitude"));  // the broken header closed the section
    CHECK(parsed.settings.model.yawn_strength == defaults.model.yawn_strength);
    CHECK(parsed.settings.model.yawn_average_interval_s == defaults.model.yawn_average_interval_s);
    CHECK(parsed.settings.enabled);
    CHECK(parsed.settings.camera.left_right == 12.0f);  // good lines still apply
}

TEST_CASE("the same key in another section is a different setting")
{
    const ParsedSettings parsed = parse_settings("[camera]\nfull_speed_mps = 12\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.settings.camera.scaling.full_speed_mps == 12.0f);
    CHECK(parsed.settings.model.yawn_scaling.full_speed_mps == FatigueParams{}.yawn_scaling.full_speed_mps);
}

TEST_CASE("settings outside a section are unknown")
{
    const ParsedSettings parsed = parse_settings("strength = 0.2\n");
    CHECK(mentions(parsed, "line 1: unknown setting [] strength"));
    CHECK(parsed.settings.model.yawn_strength == Settings{}.model.yawn_strength);
}

TEST_CASE("equal fatigue ends switch an effect on at once")
{
    const ParsedSettings parsed = parse_settings("[yawn]\nstart_minutes_before_rest = 0\nfull_minutes_before_rest = 0\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.settings.model.yawn_scaling.start_minutes_before_rest == 0.0f);
    CHECK(parsed.settings.model.yawn_scaling.full_minutes_before_rest == 0.0f);
}

TEST_CASE("an upside-down fatigue range falls back to that section's defaults")
{
    ParsedSettings parsed = parse_settings("[yawn]\nstart_minutes_before_rest = 0\nfull_minutes_before_rest = 60\n");
    CHECK(mentions(parsed, "[yawn] start_minutes_before_rest must not be below full_minutes_before_rest"));
    CHECK(same(parsed.settings.model.yawn_scaling, FatigueParams{}.yawn_scaling));

    parsed = parse_settings("[camera]\nstart_minutes_before_rest = 0\nfull_minutes_before_rest = 60\n");
    CHECK(mentions(parsed, "[camera] start_minutes_before_rest must not be below full_minutes_before_rest"));
    CHECK(same(parsed.settings.camera.scaling, SwayParams{}.scaling));
}
