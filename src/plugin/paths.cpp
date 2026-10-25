#include "paths.h"

#include <windows.h>

namespace long_shift {

std::wstring plugin_file_path(const wchar_t* file_name)
{
    static const int anchor = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &module)) {
        return {};
    }
    wchar_t path[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH * 2);
    if (length == 0 || length >= MAX_PATH * 2) {
        return {};
    }
    std::wstring directory(path, length);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    directory.resize(slash + 1);
    return directory + file_name;
}

}  // namespace long_shift
