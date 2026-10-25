#include "Config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <optional>

namespace long_shift {
namespace {

// The file the plugin writes when there is none. Must parse to Settings{}.
constexpr std::string_view kDefaultText = R"(; Long Shift settings.
; Saved changes are applied while the game runs, within about a second.
; Delete this file to get the defaults back. Problems in this file are reported in long_shift.log.
;
; The yawns and the camera sway are each scaled by two factors from 0 to 1:
;   speed   - 0 when standing, growing linearly to 1 at full_speed_mps and faster
;             (m/s; 25 m/s = 90 km/h);
;   fatigue - 0 until start_minutes_before_rest in-game minutes before the mandatory rest, growing
;             linearly to 1 at full_minutes_before_rest (0 = when the rest is due; overdue stays at 1).
;             Equal values switch the effect on at once, e.g. both 0 = only once the rest is due.

[general]
; 1 = on, 0 = off.
enabled = 1
; 1 = write a status line to long_shift.log every second (for troubleshooting).
status_log = 0

[yawn]
; A strong pull of the steering to one side for a few seconds. Strength is a fraction of full
; steering lock (0.1 = 10 %); 0 = no yawns.
strength = 0.25
; Average seconds from the start of one yawn to the start of the next.
average_interval_s = 30
; Minimum pause after a yawn, seconds.
min_pause_s = 10
full_speed_mps = 25
start_minutes_before_rest = 30
full_minutes_before_rest = 0

[camera]
; The view sways like a drowsy head along a figure eight, smeared at random.
; Reach in the game's mouse-look units (about 10-20 is a clear head movement); 0 = off.
left_right = 10
up_down = 6
; Average seconds for one figure eight.
loop_time_s = 12
; 0 = a clean figure eight, 1 = speed, reach and shape wander a lot.
randomness = 0.5
; Share of the sway left when standing still (0 = none, 1 = as at full speed).
standing_strength = 0.6
full_speed_mps = 15
start_minutes_before_rest = 120
full_minutes_before_rest = 0
)";

using Setter = void (*)(Settings&, float);

struct Field {
    std::string_view section;
    std::string_view key;
    float min;
    float max;
    bool flag;  // 0 or 1 only
    Setter set;
};

const Field kFields[] = {
    {"general", "enabled", 0.0f, 1.0f, true, [](Settings& s, float v) { s.enabled = v != 0.0f; }},
    {"general", "status_log", 0.0f, 1.0f, true, [](Settings& s, float v) { s.status_log = v != 0.0f; }},

    {"yawn", "strength", 0.0f, 1.0f, false, [](Settings& s, float v) { s.model.yawn_strength = v; }},
    {"yawn", "average_interval_s", 5.0f, 3600.0f, false, [](Settings& s, float v) { s.model.yawn_average_interval_s = v; }},
    {"yawn", "min_pause_s", 0.0f, 600.0f, false, [](Settings& s, float v) { s.model.yawn_min_pause_s = v; }},
    {"yawn", "full_speed_mps", 0.1f, 100.0f, false, [](Settings& s, float v) { s.model.yawn_scaling.full_speed_mps = v; }},
    {"yawn", "start_minutes_before_rest", -1440.0f, 1440.0f, false,
     [](Settings& s, float v) { s.model.yawn_scaling.start_minutes_before_rest = v; }},
    {"yawn", "full_minutes_before_rest", -1440.0f, 1440.0f, false,
     [](Settings& s, float v) { s.model.yawn_scaling.full_minutes_before_rest = v; }},

    {"camera", "left_right", 0.0f, 100.0f, false, [](Settings& s, float v) { s.camera.left_right = v; }},
    {"camera", "up_down", 0.0f, 100.0f, false, [](Settings& s, float v) { s.camera.up_down = v; }},
    {"camera", "loop_time_s", 1.0f, 120.0f, false, [](Settings& s, float v) { s.camera.loop_time_s = v; }},
    {"camera", "randomness", 0.0f, 1.0f, false, [](Settings& s, float v) { s.camera.randomness = v; }},
    {"camera", "standing_strength", 0.0f, 1.0f, false, [](Settings& s, float v) { s.camera.standing_strength = v; }},
    {"camera", "full_speed_mps", 0.1f, 100.0f, false, [](Settings& s, float v) { s.camera.scaling.full_speed_mps = v; }},
    {"camera", "start_minutes_before_rest", -1440.0f, 1440.0f, false,
     [](Settings& s, float v) { s.camera.scaling.start_minutes_before_rest = v; }},
    {"camera", "full_minutes_before_rest", -1440.0f, 1440.0f, false,
     [](Settings& s, float v) { s.camera.scaling.full_minutes_before_rest = v; }},
};

std::string_view trim(std::string_view text)
{
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool same_name(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

const Field* find_field(std::string_view section, std::string_view key)
{
    for (const Field& field : kFields) {
        if (same_name(field.section, section) && same_name(field.key, key)) {
            return &field;
        }
    }
    return nullptr;
}

std::optional<float> parse_number(std::string_view text)
{
    std::string number(text);
    std::replace(number.begin(), number.end(), ',', '.');  // "0,15" as typed with a Russian layout
    float value = 0.0f;
    const char* const end = number.data() + number.size();
    const auto [stop, error] = std::from_chars(number.data(), end, value);
    if (number.empty() || error != std::errc() || stop != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

void warn(std::vector<std::string>& warnings, const char* format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    warnings.emplace_back(message);
}

// The fatigue range must not be upside down; otherwise both ends fall back to the section's defaults.
// Equal ends are fine: the effect then switches on at once.
void check_scaling(Scaling& scaling, const Scaling& defaults, const char* section, std::vector<std::string>& warnings)
{
    if (scaling.start_minutes_before_rest < scaling.full_minutes_before_rest) {
        warn(warnings, "[%s] start_minutes_before_rest must not be below full_minutes_before_rest, defaults used for both",
             section);
        scaling.start_minutes_before_rest = defaults.start_minutes_before_rest;
        scaling.full_minutes_before_rest = defaults.full_minutes_before_rest;
    }
}

}  // namespace

std::string_view default_settings_text()
{
    return kDefaultText;
}

ParsedSettings parse_settings(std::string_view text)
{
    ParsedSettings result;
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";  // Notepad may add it
    if (text.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        text.remove_prefix(kUtf8Bom.size());
    }

    std::string section;
    int line_number = 0;
    while (!text.empty()) {
        const size_t line_end = text.find('\n');
        std::string_view line = text.substr(0, line_end);
        text = line_end == std::string_view::npos ? std::string_view() : text.substr(line_end + 1);
        ++line_number;

        line = trim(line.substr(0, line.find_first_of(";#")));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                warn(result.warnings, "line %d: broken section header", line_number);
                section.clear();
            } else {
                section = std::string(trim(line.substr(1, line.size() - 2)));
            }
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            warn(result.warnings, "line %d: expected \"name = value\"", line_number);
            continue;
        }
        const std::string key(trim(line.substr(0, equals)));
        const std::string value_text(trim(line.substr(equals + 1)));
        const Field* const field = find_field(section, key);
        if (!field) {
            warn(result.warnings, "line %d: unknown setting [%s] %s", line_number, section.c_str(), key.c_str());
            continue;
        }
        const std::optional<float> value = parse_number(value_text);
        if (!value) {
            warn(result.warnings, "line %d: %s: \"%s\" is not a number, default kept", line_number, key.c_str(),
                 value_text.c_str());
            continue;
        }
        if (field->flag ? (*value != 0.0f && *value != 1.0f) : (*value < field->min || *value > field->max)) {
            if (field->flag) {
                warn(result.warnings, "line %d: %s must be 0 or 1, default kept", line_number, key.c_str());
            } else {
                warn(result.warnings, "line %d: %s must be between %g and %g, default kept", line_number, key.c_str(),
                     field->min, field->max);
            }
            continue;
        }
        field->set(result.settings, *value);
    }

    check_scaling(result.settings.model.yawn_scaling, FatigueParams{}.yawn_scaling, "yawn", result.warnings);
    check_scaling(result.settings.camera.scaling, SwayParams{}.scaling, "camera", result.warnings);
    return result;
}

}  // namespace long_shift
