#pragma once

#include <string>

namespace long_shift {

// Full path of a file in the directory this DLL was loaded from (bin/win_x64/plugins in the game);
// empty if it cannot be determined.
std::wstring plugin_file_path(const wchar_t* file_name);

}  // namespace long_shift
