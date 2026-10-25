#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "Config.h"

namespace long_shift {

// long_shift.ini next to the DLL: written with the defaults when missing, re-read when it changes.
class SettingsFile {
public:
    explicit SettingsFile(std::wstring path) : path_(std::move(path)) {}

    // Reads the file, creating it with the defaults first when there is none. Problems (no plugin
    // folder, unreadable file) come back as warnings next to the default settings.
    ParsedSettings load();

    // Loads the file again if it was edited, created or deleted since the last load.
    std::optional<ParsedSettings> reload_if_changed();

private:
    std::optional<std::uint64_t> last_write_time() const;  // empty when the file does not exist

    std::wstring path_;
    std::optional<std::uint64_t> loaded_write_time_;
};

}  // namespace long_shift
