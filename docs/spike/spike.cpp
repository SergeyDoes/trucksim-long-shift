// Fatigue Drift, stage 1 spike.
//
// Feeds a visible sine into steering through two SDK input devices and logs what
// the game reports back, to check in-game that the offset is added on top of the
// player's wheel / gamepad / keyboard input:
//   - semantical device "fatigue_drift_sem", input "steering": the game maps it onto
//     `semantical.steering` in the `steering` mix of controls.sii, no binding needed;
//   - generic device "fatigue_drift", axis "steer_offset": fallback, bound by hand.
// Hotkeys (only while the game window is focused):
//   Ctrl+Shift+F9  cycles off -> semantical -> generic -> off (starts in semantical, or in the mode
//                  named by the FATIGUE_DRIFT_SPIKE_MODE environment variable);
//   Ctrl+Shift+F10 cycles the sine amplitude 0.3 -> 0.2 -> 0.1 -> 0.03.
// Frames where the game stops polling the input device for a while are logged as stalls.
//
// The SDK calls everything on the game's main thread, so no locking is needed.

#include <windows.h>

#include <share.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <new>
#include <optional>

#include "scssdk_input.h"
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kAmplitudes[] = {0.3f, 0.2f, 0.1f, 0.03f};
constexpr int kAmplitudeCount = static_cast<int>(sizeof(kAmplitudes) / sizeof(kAmplitudes[0]));
constexpr double kPeriodSeconds = 4.0;
constexpr double kStallSeconds = 0.3;
constexpr double kCsvIntervalSeconds = 0.1;
constexpr double kTwoPi = 6.283185307179586;
constexpr wchar_t kLogFileName[] = L"fatigue_drift.log";
constexpr char kConsolePrefix[] = "[fatigue_drift] ";

enum class Mode { semantical, generic, off };

const char* mode_name(Mode mode)
{
    switch (mode) {
    case Mode::semantical: return "semantical";
    case Mode::generic: return "generic";
    case Mode::off: return "off";
    }
    return "?";
}

Mode next_mode(Mode mode)
{
    switch (mode) {
    case Mode::semantical: return Mode::generic;
    case Mode::generic: return Mode::off;
    case Mode::off: return Mode::semantical;
    }
    return Mode::off;
}

Mode initial_mode()
{
    char name[32];
    const DWORD length = GetEnvironmentVariableA("FATIGUE_DRIFT_SPIKE_MODE", name, sizeof(name));
    if (length > 0 && length < sizeof(name)) {
        for (Mode mode : {Mode::semantical, Mode::generic, Mode::off}) {
            if (std::strcmp(name, mode_name(mode)) == 0) {
                return mode;
            }
        }
    }
    return Mode::semantical;
}

struct Device {
    Mode drives_in;  // the mode in which this device outputs the sine
    bool active = false;
    bool reported_this_frame = false;
    float last_value = 0.0f;

    // Poll statistics: how often the game starts a frame for this device.
    double last_poll_time = -1.0;
    int polls_since_row = 0;
    double max_gap_since_row = 0.0;
};

struct Spike {
    Clock::time_point start = Clock::now();
    int initialized_apis = 0;  // telemetry and input are initialized independently, in any order
    FILE* log_file = nullptr;
    scs_log_t telemetry_log = nullptr;
    scs_log_t input_log = nullptr;

    Mode mode = initial_mode();
    int amplitude_index = 0;
    bool mode_hotkey_down = false;
    bool amplitude_hotkey_down = false;
    bool paused = true;  // telemetry starts in the paused state
    double next_csv_time = 0.0;

    std::optional<float> input_steering;
    std::optional<float> effective_steering;
    std::optional<float> speed;          // m/s, negative in reverse
    std::optional<scs_s32_t> rest_stop;  // in-game minutes

    // Sampled once per telemetry frame. The input callback must not recompute it: the game re-polls
    // a device at once while its value keeps changing, and a value that changes on every call keeps
    // the main thread spinning (seen in ATS 1.61 as ~6M polls per second and multi-second freezes).
    float sine = 0.0f;

    Device semantical{Mode::semantical};
    Device generic{Mode::generic};

    double seconds() const { return std::chrono::duration<double>(Clock::now() - start).count(); }
    float amplitude() const { return kAmplitudes[amplitude_index]; }
};

Spike* g_spike = nullptr;

// Writes to the log file and to the game console. Only valid while the game is calling into the plugin.
void log_message(scs_log_type_t type, const char* format, ...)
{
    if (!g_spike) {
        return;
    }
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (g_spike->log_file) {
        std::fprintf(g_spike->log_file, "# %.3f %s\n", g_spike->seconds(), message);
        std::fflush(g_spike->log_file);
    }
    const scs_log_t game_log = g_spike->telemetry_log ? g_spike->telemetry_log : g_spike->input_log;
    if (game_log) {
        char line[sizeof(kConsolePrefix) + sizeof(message)];
        std::snprintf(line, sizeof(line), "%s%s", kConsolePrefix, message);
        game_log(type, line);
    }
}

// Opens fatigue_drift.log next to the DLL in append mode, so several game sessions end up in one file.
void open_log_file(Spike& spike)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_spike), &module)) {
        return;
    }
    wchar_t path[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH * 2);
    if (length == 0 || length >= MAX_PATH * 2) {
        return;
    }
    wchar_t* const slash = std::wcsrchr(path, L'\\');
    if (!slash || wcscpy_s(slash + 1, static_cast<size_t>(path + MAX_PATH * 2 - (slash + 1)), kLogFileName) != 0) {
        return;
    }
    spike.log_file = _wfsopen(path, L"a", _SH_DENYWR);  // others may read the log while the game runs
    if (!spike.log_file) {
        return;
    }

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    std::fprintf(spike.log_file, "# ==== session %s, mode %s, sine amplitude %.2f, period %.1f s\n", stamp,
                 mode_name(spike.mode), spike.amplitude(), kPeriodSeconds);
    std::fprintf(spike.log_file, "t,mode,amp,offset,input_steering,effective_steering,speed_kmh,rest_stop_min,paused,"
                                 "sem_active,gen_active,sem_polls,sem_max_gap_ms\n");
    std::fflush(spike.log_file);
}

Spike* acquire_spike()
{
    if (!g_spike) {
        g_spike = new (std::nothrow) Spike();
        if (!g_spike) {
            return nullptr;
        }
        open_log_file(*g_spike);
    }
    ++g_spike->initialized_apis;
    return g_spike;
}

void release_spike()
{
    if (!g_spike || --g_spike->initialized_apis > 0) {
        return;
    }
    if (g_spike->log_file) {
        std::fprintf(g_spike->log_file, "# %.3f closed\n", g_spike->seconds());
        std::fclose(g_spike->log_file);
    }
    delete g_spike;
    g_spike = nullptr;
}

float device_value(const Spike& spike, const Device& device)
{
    if (spike.mode != device.drives_in) {
        return 0.0f;
    }
    // The semantical input goes straight into the steering mix, so keep it quiet in menus.
    // The generic axis keeps moving so that the Controls screen can detect it for binding.
    if (device.drives_in == Mode::semantical && spike.paused) {
        return 0.0f;
    }
    return spike.sine;
}

bool key_down(int virtual_key)
{
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool game_window_focused()
{
    DWORD process_id = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process_id);
    return process_id == GetCurrentProcessId();
}

// Returns true once per press of Ctrl+Shift+<key>.
bool hotkey_pressed(int virtual_key, bool& was_down)
{
    const bool down = key_down(VK_CONTROL) && key_down(VK_SHIFT) && key_down(virtual_key) && game_window_focused();
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

void poll_hotkeys(Spike& spike)
{
    if (hotkey_pressed(VK_F9, spike.mode_hotkey_down)) {
        spike.mode = next_mode(spike.mode);
        log_message(SCS_LOG_TYPE_message, "mode -> %s", mode_name(spike.mode));
    }
    if (hotkey_pressed(VK_F10, spike.amplitude_hotkey_down)) {
        spike.amplitude_index = (spike.amplitude_index + 1) % kAmplitudeCount;
        log_message(SCS_LOG_TYPE_message, "amplitude -> %.2f", spike.amplitude());
    }
}

void format_optional(char (&buffer)[24], const std::optional<float>& value, float scale, int precision)
{
    if (value) {
        std::snprintf(buffer, sizeof(buffer), "%.*f", precision, *value * scale);
    } else {
        buffer[0] = '\0';
    }
}

void write_csv_row(Spike& spike)
{
    const double now = spike.seconds();
    if (!spike.log_file || now < spike.next_csv_time) {
        return;
    }
    spike.next_csv_time = now + kCsvIntervalSeconds;

    const Device* const driving = spike.mode == Mode::semantical ? &spike.semantical
                                  : spike.mode == Mode::generic  ? &spike.generic
                                                                 : nullptr;
    char input_steering[24], effective_steering[24], speed_kmh[24], rest_stop[24];
    format_optional(input_steering, spike.input_steering, 1.0f, 4);
    format_optional(effective_steering, spike.effective_steering, 1.0f, 4);
    format_optional(speed_kmh, spike.speed, 3.6f, 1);
    if (spike.rest_stop) {
        std::snprintf(rest_stop, sizeof(rest_stop), "%d", *spike.rest_stop);
    } else {
        rest_stop[0] = '\0';
    }
    Device& semantical = spike.semantical;
    std::fprintf(spike.log_file, "%.3f,%s,%.2f,%.4f,%s,%s,%s,%s,%d,%d,%d,%d,%.1f\n", now, mode_name(spike.mode),
                 spike.amplitude(), driving ? driving->last_value : 0.0f, input_steering, effective_steering, speed_kmh,
                 rest_stop, spike.paused ? 1 : 0, semantical.active ? 1 : 0, spike.generic.active ? 1 : 0,
                 semantical.polls_since_row, semantical.max_gap_since_row * 1000.0);
    std::fflush(spike.log_file);
    semantical.polls_since_row = 0;
    semantical.max_gap_since_row = 0.0;
}

void tick(Spike& spike)
{
    poll_hotkeys(spike);
    write_csv_row(spike);
}

// Called at the start of every input frame of a device; reports frames the game took too long to start.
void record_poll(Spike& spike, Device& device)
{
    const double now = spike.seconds();
    if (device.last_poll_time >= 0.0) {
        const double gap = now - device.last_poll_time;
        if (gap > device.max_gap_since_row) {
            device.max_gap_since_row = gap;
        }
        if (gap > kStallSeconds) {
            log_message(SCS_LOG_TYPE_warning, "stall: %s device not polled for %.3f s, last value sent %.4f",
                        mode_name(device.drives_in), gap, device.last_value);
        }
    }
    device.last_poll_time = now;
    ++device.polls_since_row;
}

// Telemetry.

SCSAPI_VOID on_telemetry_event(const scs_event_t event, const void* const, const scs_context_t context)
{
    Spike& spike = *static_cast<Spike*>(context);
    switch (event) {
    case SCS_TELEMETRY_EVENT_frame_start:
        spike.sine = spike.amplitude() * static_cast<float>(std::sin(kTwoPi * spike.seconds() / kPeriodSeconds));
        tick(spike);
        break;
    case SCS_TELEMETRY_EVENT_paused:
        spike.paused = true;
        log_message(SCS_LOG_TYPE_message, "paused");
        break;
    case SCS_TELEMETRY_EVENT_started:
        spike.paused = false;
        log_message(SCS_LOG_TYPE_message, "started, mode %s", mode_name(spike.mode));
        break;
    default:
        break;
    }
}

SCSAPI_VOID on_float_channel(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t context)
{
    auto& target = *static_cast<std::optional<float>*>(context);
    if (value && value->type == SCS_VALUE_TYPE_float) {
        target = value->value_float.value;
    } else {
        target.reset();
    }
}

SCSAPI_VOID on_s32_channel(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t context)
{
    auto& target = *static_cast<std::optional<scs_s32_t>*>(context);
    if (value && value->type == SCS_VALUE_TYPE_s32) {
        target = value->value_s32.value;
    } else {
        target.reset();
    }
}

void register_channel(const scs_telemetry_init_params_v100_t& init, const char* name, scs_value_type_t type,
                      scs_telemetry_channel_callback_t callback, void* target)
{
    // no_value: get a NULL value when the channel disappears (e.g. rest.stop with fatigue simulation off).
    const scs_result_t result =
        init.register_for_channel(name, SCS_U32_NIL, type, SCS_TELEMETRY_CHANNEL_FLAG_no_value, callback, target);
    if (result != SCS_RESULT_ok) {
        log_message(SCS_LOG_TYPE_warning, "channel %s unavailable (%d)", name, result);
    }
}

// Input.

SCSAPI_VOID on_device_active(const scs_u8_t active, const scs_context_t context)
{
    Device& device = *static_cast<Device*>(context);
    device.active = active != 0;
    device.reported_this_frame = false;
    device.last_value = 0.0f;
    device.last_poll_time = -1.0;
    log_message(SCS_LOG_TYPE_message, "%s device %s", mode_name(device.drives_in), device.active ? "activated" : "deactivated");
}

// The game calls this repeatedly every frame until it returns not_found; we report one value per frame.
SCSAPI_RESULT on_device_event(scs_input_event_t* const event_info, const scs_u32_t flags, const scs_context_t context)
{
    Device& device = *static_cast<Device*>(context);
    if (!g_spike) {
        return SCS_RESULT_not_found;
    }
    if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) {
        device.reported_this_frame = false;
        record_poll(*g_spike, device);
        tick(*g_spike);
    }
    if (device.reported_this_frame) {
        return SCS_RESULT_not_found;
    }
    device.reported_this_frame = true;
    device.last_value = device_value(*g_spike, device);
    event_info->input_index = 0;
    event_info->value_float.value = device.last_value;
    return SCS_RESULT_ok;
}

bool register_device(const scs_input_init_params_v100_t& init, const char* name, const char* display_name,
                     scs_input_device_type_t type, const scs_input_device_input_t& input, Device& device)
{
    device.active = false;
    device.reported_this_frame = false;
    device.last_value = 0.0f;
    device.last_poll_time = -1.0;

    scs_input_device_t info{};
    info.name = name;
    info.display_name = display_name;
    info.type = type;
    info.input_count = 1;
    info.inputs = &input;
    info.callback_context = &device;
    info.input_active_callback = on_device_active;
    info.input_event_callback = on_device_event;

    const scs_result_t result = init.register_device(&info);
    if (result != SCS_RESULT_ok) {
        log_message(SCS_LOG_TYPE_warning, "cannot register device %s (%d)", name, result);
        return false;
    }
    log_message(SCS_LOG_TYPE_message, "registered device %s, input %s", name, input.name);
    return true;
}

}  // namespace

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_01 && version != SCS_TELEMETRY_VERSION_1_00) {
        return SCS_RESULT_unsupported;
    }
    const auto& init = *static_cast<const scs_telemetry_init_params_v100_t*>(params);
    Spike* const spike = acquire_spike();
    if (!spike) {
        init.common.log(SCS_LOG_TYPE_error, "[fatigue_drift] out of memory");
        return SCS_RESULT_generic_error;
    }
    spike->telemetry_log = init.common.log;
    spike->paused = true;
    log_message(SCS_LOG_TYPE_message, "telemetry init: %s (%s), telemetry game version %u.%u, log file %s",
                init.common.game_name, init.common.game_id, SCS_GET_MAJOR_VERSION(init.common.game_version),
                SCS_GET_MINOR_VERSION(init.common.game_version), spike->log_file ? "open" : "NOT available");

    const bool events_registered =
        init.register_for_event(SCS_TELEMETRY_EVENT_frame_start, on_telemetry_event, spike) == SCS_RESULT_ok &&
        init.register_for_event(SCS_TELEMETRY_EVENT_paused, on_telemetry_event, spike) == SCS_RESULT_ok &&
        init.register_for_event(SCS_TELEMETRY_EVENT_started, on_telemetry_event, spike) == SCS_RESULT_ok;
    if (!events_registered) {
        log_message(SCS_LOG_TYPE_error, "cannot register telemetry events");
        spike->telemetry_log = nullptr;
        release_spike();
        return SCS_RESULT_generic_error;
    }

    register_channel(init, SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, SCS_VALUE_TYPE_float, on_float_channel, &spike->input_steering);
    register_channel(init, SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, SCS_VALUE_TYPE_float, on_float_channel, &spike->effective_steering);
    register_channel(init, SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, on_float_channel, &spike->speed);
    register_channel(init, SCS_TELEMETRY_CHANNEL_next_rest_stop, SCS_VALUE_TYPE_s32, on_s32_channel, &spike->rest_stop);
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    if (!g_spike) {
        return;
    }
    log_message(SCS_LOG_TYPE_message, "telemetry shutdown");
    g_spike->telemetry_log = nullptr;
    g_spike->paused = true;
    g_spike->input_steering.reset();
    g_spike->effective_steering.reset();
    g_spike->speed.reset();
    g_spike->rest_stop.reset();
    g_spike->sine = 0.0f;
    release_spike();
}

SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t* const params)
{
    if (version != SCS_INPUT_VERSION_1_00) {
        return SCS_RESULT_unsupported;
    }
    const auto& init = *static_cast<const scs_input_init_params_v100_t*>(params);
    Spike* const spike = acquire_spike();
    if (!spike) {
        init.common.log(SCS_LOG_TYPE_error, "[fatigue_drift] out of memory");
        return SCS_RESULT_generic_error;
    }
    spike->input_log = init.common.log;
    log_message(SCS_LOG_TYPE_message, "input init: %s (%s), input game version %u.%u, mode %s", init.common.game_name,
                init.common.game_id, SCS_GET_MAJOR_VERSION(init.common.game_version),
                SCS_GET_MINOR_VERSION(init.common.game_version), mode_name(spike->mode));

    const scs_input_device_input_t semantical_input = {"steering", "Steering", SCS_VALUE_TYPE_float};
    const bool semantical_registered = register_device(init, "fatigue_drift_sem", "Fatigue Drift semantical",
                                                       SCS_INPUT_DEVICE_TYPE_semantical, semantical_input, spike->semantical);
    const scs_input_device_input_t generic_input = {"steer_offset", "Steer offset", SCS_VALUE_TYPE_float};
    const bool generic_registered = register_device(init, "fatigue_drift", "Fatigue Drift", SCS_INPUT_DEVICE_TYPE_generic,
                                                    generic_input, spike->generic);
    if (!semantical_registered && !generic_registered) {
        log_message(SCS_LOG_TYPE_error, "no input device registered");
        spike->input_log = nullptr;
        release_spike();
        return SCS_RESULT_generic_error;
    }
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_input_shutdown(void)
{
    if (!g_spike) {
        return;
    }
    log_message(SCS_LOG_TYPE_message, "input shutdown");
    g_spike->input_log = nullptr;
    g_spike->semantical.active = false;
    g_spike->generic.active = false;
    release_spike();
}
