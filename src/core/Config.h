#pragma once

// long_shift.ini: parsing and the default file contents. Pure logic, testable outside the game.

#include <string>
#include <string_view>
#include <vector>

#include "CameraSway.h"
#include "FatigueModel.h"

namespace long_shift {

// Everything long_shift.ini controls.
struct Settings {
    bool enabled = true;     // [general] enabled
    bool status_log = false;  // [general] status_log: a status line in long_shift.log every second
    FatigueParams model;
    SwayParams camera;  // [camera]
};

struct ParsedSettings {
    Settings settings;                  // defaults wherever the text had nothing usable
    std::vector<std::string> warnings;  // what was ignored and why, with line numbers
};

// Never fails: unknown keys, bad numbers and out-of-range values are reported and keep their defaults.
ParsedSettings parse_settings(std::string_view text);

// The commented long_shift.ini the plugin writes when there is none; parses to Settings{}.
std::string_view default_settings_text();

}  // namespace long_shift
