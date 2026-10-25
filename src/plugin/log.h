#pragma once

#include <cstdarg>
#include <cstdio>

#include "scssdk.h"

namespace long_shift {

// long_shift.log next to the DLL, optionally mirrored to the game console (and game.log.txt).
class Log {
public:
    Log() = default;
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
    ~Log();

    // Opens the file for appending and starts it over once it has grown past 1 MB. Returns false
    // when the file cannot be opened: the plugin keeps working, only the file log is missing.
    bool open();
    bool is_open() const { return file_ != nullptr; }

    // The game's log function. The game may only be called while it is calling into the plugin.
    void set_console(scs_log_t console) { console_ = console; }

    // To the file only.
    void info(const char* format, ...);
    // To the file and the game console.
    void message(scs_log_type_t type, const char* format, ...);

private:
    void write(scs_log_type_t type, bool to_console, const char* format, va_list args);

    std::FILE* file_ = nullptr;
    scs_log_t console_ = nullptr;
};

}  // namespace long_shift
