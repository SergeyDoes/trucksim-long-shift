#include "settings_file.h"

#include <windows.h>

#include <fstream>

namespace long_shift {
namespace {

constexpr std::streamsize kMaxFileBytes = 64 * 1024;

}  // namespace

ParsedSettings SettingsFile::load()
{
    if (path_.empty()) {
        ParsedSettings parsed;
        parsed.warnings.emplace_back("cannot find the plugin folder, default settings used");
        return parsed;
    }
    if (!last_write_time()) {
        std::ofstream(path_, std::ios::binary) << default_settings_text();
    }
    loaded_write_time_ = last_write_time();

    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        ParsedSettings parsed;
        parsed.warnings.emplace_back("cannot read long_shift.ini, default settings used");
        return parsed;
    }
    std::string text(kMaxFileBytes, '\0');
    file.read(text.data(), kMaxFileBytes);
    text.resize(static_cast<size_t>(file.gcount()));
    return parse_settings(text);
}

std::optional<ParsedSettings> SettingsFile::reload_if_changed()
{
    if (path_.empty() || last_write_time() == loaded_write_time_) {
        return std::nullopt;
    }
    return load();
}

std::optional<std::uint64_t> SettingsFile::last_write_time() const
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

}  // namespace long_shift
