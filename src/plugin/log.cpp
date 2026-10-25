#include "log.h"

#include <windows.h>

#include <share.h>

#include "paths.h"

namespace long_shift {
namespace {

constexpr wchar_t kFileName[] = L"long_shift.log";
constexpr long kMaxFileBytes = 1024 * 1024;
constexpr char kConsolePrefix[] = "[long_shift] ";

}  // namespace

Log::~Log()
{
    if (file_) {
        std::fclose(file_);
    }
}

bool Log::open()
{
    if (file_) {
        return true;
    }
    const std::wstring path = plugin_file_path(kFileName);
    if (path.empty()) {
        return false;
    }
    // Others may read the log while the game runs.
    file_ = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
    if (file_ && std::fseek(file_, 0, SEEK_END) == 0 && std::ftell(file_) > kMaxFileBytes) {
        std::fclose(file_);
        file_ = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    }
    if (file_) {
        std::fprintf(file_, "\n");
    }
    return file_ != nullptr;
}

void Log::info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    write(SCS_LOG_TYPE_message, false, format, args);
    va_end(args);
}

void Log::message(scs_log_type_t type, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    write(type, true, format, args);
    va_end(args);
}

void Log::write(scs_log_type_t type, bool to_console, const char* format, va_list args)
{
    char message[512];
    std::vsnprintf(message, sizeof(message), format, args);

    if (file_) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        const char* const level = type == SCS_LOG_TYPE_error ? "ERROR " : type == SCS_LOG_TYPE_warning ? "WARNING " : "";
        std::fprintf(file_, "%02u:%02u:%02u.%03u %s%s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, level,
                     message);
        std::fflush(file_);
    }
    if (to_console && console_) {
        char line[sizeof(kConsolePrefix) + sizeof(message)];
        std::snprintf(line, sizeof(line), "%s%s", kConsolePrefix, message);
        console_(type, line);
    }
}

}  // namespace long_shift
