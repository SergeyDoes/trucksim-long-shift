// Long Shift: as the driver's rest approaches, the steering pulls hard to one side in random "yawns"
// and the view sways like a drowsy head. Telemetry feeds FatigueModel and CameraSway, and their output
// goes into the game's `steering` and mouse-look mixes through a semantical input device, so nothing
// has to be bound. Settings come from long_shift.ini next to the DLL and are re-read while the game runs.
//
// The SDK calls everything on the game's main thread, so no locking is needed.

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <optional>
#include <random>
#include <string>

#include "CameraSway.h"
#include "Config.h"
#include "FatigueModel.h"
#include "log.h"
#include "paths.h"
#include "settings_file.h"
#include "scssdk_input.h"
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

namespace long_shift {
namespace {

constexpr char kDeviceName[] = "long_shift";
constexpr char kDeviceDisplayName[] = "Long Shift";
// Matches `semantical.steering` in the steering mix of controls.sii: the game adds it to the wheel,
// gamepad and keyboard input without any binding. Positive steers right.
constexpr char kSteeringInput[] = "steering";
// The camera inputs are added to the mouse look like mouse movement, a change per frame rather than a
// position: `semantical.camud` up-down and `semantical.camlr` left-right in controls.sii.
constexpr char kCameraUpDownInput[] = "camud";
constexpr char kCameraLeftRightInput[] = "camlr";
enum : scs_u32_t { kSteeringIndex, kCameraUpDownIndex, kCameraLeftRightIndex, kInputCount };

using Clock = std::chrono::steady_clock;
// While the reported values change the game polls the device again within the same frame, microseconds
// apart; only a poll after a longer gap is a new frame that may move the camera.
constexpr auto kSameFrameGap = std::chrono::milliseconds(1);
constexpr wchar_t kSettingsFileName[] = L"long_shift.ini";
constexpr ULONGLONG kSettingsCheckMs = 1000;

struct Plugin {
    explicit Plugin(std::uint32_t seed)
        : settings_file(plugin_file_path(kSettingsFileName)), model(FatigueParams{}, seed), sway(seed ^ 0x5bd1e995u)
    {
        frame.paused = true;
    }

    int initialized_apis = 0;  // telemetry and input are initialized separately, in any order
    Log log;
    SettingsFile settings_file;
    Settings settings;
    ULONGLONG next_settings_check_ms = 0;
    scs_log_t telemetry_console = nullptr;
    scs_log_t input_console = nullptr;

    // Telemetry of the current frame, filled in by the callbacks.
    FatigueInput frame;
    std::optional<scs_timestamp_t> last_frame_time;

    FatigueModel model;
    CameraSway sway;
    // What the input device reports. It changes at most once per telemetry frame: the game re-polls a
    // device at once while its value keeps changing, and a value recomputed on every call hangs the
    // main thread (seen in an early in-game test as ~6M polls per second).
    float offset = 0.0f;
    scs_u32_t next_input = 0;  // input to report next in this frame; kInputCount when all are reported
    scs_u32_t input_count = kInputCount;

    // The camera sway is stepped once per rendered frame; the step is reported for that whole frame.
    std::optional<Clock::time_point> last_input_poll;
    std::optional<Clock::time_point> last_camera_frame;
    CameraSway::Step camera_step;

    // For the log only.
    bool was_armed = false;
    float yawn_peak = 0.0f;
    double status_timer = 0.0;
    // Cost of the plugin for the status line: time spent in its callbacks and input polls since then.
    Clock::duration busy{};
    int input_polls = 0;
    std::optional<Clock::time_point> last_status;

    void use_console()
    {
        log.set_console(telemetry_console ? telemetry_console : input_console);
    }
};

Plugin* g_plugin = nullptr;

// Adds the time the game spends in a plugin callback to Plugin::busy.
class BusyTimer {
public:
    explicit BusyTimer(Plugin& plugin) : plugin_(plugin), start_(Clock::now()) {}
    BusyTimer(const BusyTimer&) = delete;
    BusyTimer& operator=(const BusyTimer&) = delete;
    ~BusyTimer() { plugin_.busy += Clock::now() - start_; }

private:
    Plugin& plugin_;
    Clock::time_point start_;
};

void format_scaling(const Scaling& scaling, char (&text)[96])
{
    std::snprintf(text, sizeof(text), "full at %g m/s, from %g to %g min before rest", scaling.full_speed_mps,
                  scaling.start_minutes_before_rest, scaling.full_minutes_before_rest);
}

void apply_settings(Plugin& plugin, const ParsedSettings& parsed, const char* what)
{
    plugin.settings = parsed.settings;
    plugin.model.set_params(parsed.settings.model);

    const FatigueParams& model = parsed.settings.model;
    char yawn_scaling[96];
    format_scaling(model.yawn_scaling, yawn_scaling);
    char yawns[192] = "off";
    if (model.yawn_strength > 0.0f) {
        std::snprintf(yawns, sizeof(yawns), "%.2f every %g s, pause %g s, %s", model.yawn_strength,
                      model.yawn_average_interval_s, model.yawn_min_pause_s, yawn_scaling);
    }
    plugin.log.message(SCS_LOG_TYPE_message, "%s: %s; yawns %s", what, parsed.settings.enabled ? "on" : "off", yawns);
    plugin.sway.set_params(parsed.settings.camera);
    const SwayParams& camera = parsed.settings.camera;
    char camera_scaling[96];
    format_scaling(camera.scaling, camera_scaling);
    char sway[192] = "off";
    if (camera.left_right > 0.0f || camera.up_down > 0.0f) {
        std::snprintf(sway, sizeof(sway), "%g x %g, loop %g s, randomness %g, standing %g, %s", camera.left_right,
                      camera.up_down, camera.loop_time_s, camera.randomness, camera.standing_strength, camera_scaling);
    }
    plugin.log.message(SCS_LOG_TYPE_message, "%s: camera sway %s", what, sway);
    for (const std::string& warning : parsed.warnings) {
        plugin.log.message(SCS_LOG_TYPE_warning, "long_shift.ini: %s", warning.c_str());
    }
}

// Picks up edits of long_shift.ini while the game runs; the file is checked once a second.
void reload_settings_if_changed(Plugin& plugin)
{
    const ULONGLONG now = GetTickCount64();
    if (now < plugin.next_settings_check_ms) {
        return;
    }
    plugin.next_settings_check_ms = now + kSettingsCheckMs;
    if (const std::optional<ParsedSettings> parsed = plugin.settings_file.reload_if_changed()) {
        apply_settings(plugin, *parsed, "settings reloaded");
    }
}

Plugin* acquire_plugin()
{
    if (!g_plugin) {
        g_plugin = new (std::nothrow) Plugin(std::random_device{}());
        if (!g_plugin) {
            return nullptr;
        }
        g_plugin->log.open();
        apply_settings(*g_plugin, g_plugin->settings_file.load(), "settings");
    }
    ++g_plugin->initialized_apis;
    return g_plugin;
}

void release_plugin()
{
    if (!g_plugin || --g_plugin->initialized_apis > 0) {
        return;
    }
    delete g_plugin;
    g_plugin = nullptr;
}

const char* phase_name(Phase phase)
{
    switch (phase) {
    case Phase::idle: return "idle";
    case Phase::drifting: return "drifting";
    case Phase::releasing: return "releasing";
    case Phase::waking: return "waking";
    }
    return "?";
}

float speed_mps(const Plugin& plugin)
{
    return std::abs(plugin.frame.speed_mps);
}

void format_rest_stop(const Plugin& plugin, char (&text)[16])
{
    if (plugin.frame.rest_stop_min) {
        std::snprintf(text, sizeof(text), "%d", *plugin.frame.rest_stop_min);
    } else {
        std::snprintf(text, sizeof(text), "none");
    }
}

// Once per second of driving: everything the model decides on, to check its behaviour in the game.
void log_status(Plugin& plugin, double dt)
{
    plugin.status_timer += dt;
    if (!plugin.settings.status_log || plugin.frame.paused || plugin.status_timer < 1.0) {
        return;
    }
    plugin.status_timer = 0.0;
    char rest_stop[16];
    format_rest_stop(plugin, rest_stop);
    const Clock::time_point now = Clock::now();
    const double wall_s = plugin.last_status ? std::chrono::duration<double>(now - *plugin.last_status).count() : 0.0;
    const double busy_us = std::chrono::duration<double, std::micro>(plugin.busy).count();
    plugin.log.info("status: speed %.1f m/s, rest.stop %s, yawn x%.2f, camera x%.2f, input.steering %+.2f, %s, "
                    "offset %+.3f; plugin %.0f us/s, %.0f polls/s",
                    speed_mps(plugin), rest_stop, plugin.model.yawn_scale(), plugin.sway.strength(),
                    plugin.frame.input_steering, phase_name(plugin.model.phase()), plugin.offset,
                    wall_s > 0.0 ? busy_us / wall_s : 0.0, wall_s > 0.0 ? plugin.input_polls / wall_s : 0.0);
    plugin.last_status = now;
    plugin.busy = Clock::duration{};
    plugin.input_polls = 0;
}

void log_model_changes(Plugin& plugin, Phase before)
{
    const FatigueModel& model = plugin.model;
    // Tired enough for the yawns or the camera sway, whatever the speed. Not judged on pause, when the
    // telemetry is stale.
    const bool armed = plugin.frame.paused
                           ? plugin.was_armed
                           : fatigue_factor(model.params().yawn_scaling, plugin.frame.rest_stop_min) > 0.0f ||
                                 fatigue_factor(plugin.settings.camera.scaling, plugin.frame.rest_stop_min) > 0.0f;
    if (armed != plugin.was_armed) {
        plugin.was_armed = armed;
        char rest_stop[16];
        format_rest_stop(plugin, rest_stop);
        plugin.log.message(SCS_LOG_TYPE_message, armed ? "driver getting tired (rest.stop %s min)"
                                                       : "driver rested (rest.stop %s min)",
                           rest_stop);
    }
    if (before == Phase::idle && model.phase() == Phase::drifting) {
        plugin.yawn_peak = 0.0f;
        plugin.log.info("yawn started at %.1f m/s, yawn x%.2f", speed_mps(plugin), model.yawn_scale());
    }
    if (std::abs(model.offset()) > std::abs(plugin.yawn_peak)) {
        plugin.yawn_peak = model.offset();
    }
    if (before != Phase::waking && model.phase() == Phase::waking) {
        plugin.log.info("player steered against the yawn: offset %+.3f, input.steering %+.2f, %.1f m/s", model.offset(),
                        plugin.frame.input_steering, speed_mps(plugin));
    }
    if (before != Phase::idle && model.phase() == Phase::idle) {
        plugin.log.info("yawn ended, peak %.3f %s", std::abs(plugin.yawn_peak), plugin.yawn_peak > 0.0f ? "right" : "left");
    }
}

void update_model(Plugin& plugin)
{
    const Phase before = plugin.model.phase();
    FatigueInput frame = plugin.frame;
    frame.paused = frame.paused || !plugin.settings.enabled;  // switched off in the ini: no drift, as on pause
    plugin.offset = plugin.model.update(frame);
    if (!plugin.frame.paused) {
        // The input frames step the sway; this only sets how strong it should be.
        plugin.sway.set_target_strength(
            plugin.settings.enabled
                ? sway_strength(plugin.settings.camera, plugin.frame.rest_stop_min, plugin.frame.speed_mps)
                : 0.0f);
    }
    log_model_changes(plugin, before);
    log_status(plugin, plugin.frame.dt_s);
    plugin.frame.dt_s = 0.0;
}

// Telemetry.

SCSAPI_VOID on_telemetry_event(const scs_event_t event, const void* const event_info, const scs_context_t context)
{
    Plugin& plugin = *static_cast<Plugin*>(context);
    const BusyTimer timer(plugin);
    switch (event) {
    case SCS_TELEMETRY_EVENT_frame_start: {
        // Physics time that stops on pause: the model's timers must not run while the game is paused.
        const auto& info = *static_cast<const scs_telemetry_frame_start_t*>(event_info);
        const bool restarted = (info.flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart) != 0;
        const scs_timestamp_t now = info.paused_simulation_time;
        plugin.frame.dt_s = plugin.last_frame_time && !restarted && now >= *plugin.last_frame_time
                                ? static_cast<double>(now - *plugin.last_frame_time) / 1e6
                                : 0.0;
        plugin.last_frame_time = now;
        break;
    }
    case SCS_TELEMETRY_EVENT_frame_end:
        reload_settings_if_changed(plugin);
        update_model(plugin);
        break;
    case SCS_TELEMETRY_EVENT_paused:
        plugin.frame.paused = true;
        update_model(plugin);  // drop the offset right away
        plugin.log.info("paused");
        break;
    case SCS_TELEMETRY_EVENT_started:
        plugin.frame.paused = false;
        plugin.log.info("started");
        break;
    default:
        break;
    }
}

SCSAPI_VOID on_float_channel(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t context)
{
    *static_cast<float*>(context) = value && value->type == SCS_VALUE_TYPE_float ? value->value_float.value : 0.0f;
}

SCSAPI_VOID on_rest_stop(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t context)
{
    auto& rest_stop = *static_cast<std::optional<int>*>(context);
    if (value && value->type == SCS_VALUE_TYPE_s32) {
        rest_stop = value->value_s32.value;
    } else {
        rest_stop.reset();
    }
}

bool register_channel(Plugin& plugin, const scs_telemetry_init_params_v100_t& init, const char* name,
                      scs_value_type_t type, scs_telemetry_channel_callback_t callback, void* target)
{
    // no_value: learn when a channel disappears (rest.stop with the fatigue simulation off).
    const scs_result_t result =
        init.register_for_channel(name, SCS_U32_NIL, type, SCS_TELEMETRY_CHANNEL_FLAG_no_value, callback, target);
    if (result != SCS_RESULT_ok) {
        plugin.log.message(SCS_LOG_TYPE_warning, "telemetry channel %s unavailable (%d)", name, result);
        return false;
    }
    return true;
}

scs_result_t init_telemetry(const scs_telemetry_init_params_v100_t& init)
{
    Plugin* const plugin = acquire_plugin();
    if (!plugin) {
        init.common.log(SCS_LOG_TYPE_error, "[long_shift] out of memory");
        return SCS_RESULT_generic_error;
    }
    plugin->telemetry_console = init.common.log;
    plugin->use_console();
    plugin->log.message(SCS_LOG_TYPE_message, "Long Shift " LONG_SHIFT_VERSION ", telemetry init: %s (%s), telemetry version %u.%u",
                        init.common.game_name,
                        init.common.game_id, SCS_GET_MAJOR_VERSION(init.common.game_version),
                        SCS_GET_MINOR_VERSION(init.common.game_version));

    const bool events_registered =
        init.register_for_event(SCS_TELEMETRY_EVENT_frame_start, on_telemetry_event, plugin) == SCS_RESULT_ok &&
        init.register_for_event(SCS_TELEMETRY_EVENT_frame_end, on_telemetry_event, plugin) == SCS_RESULT_ok &&
        init.register_for_event(SCS_TELEMETRY_EVENT_paused, on_telemetry_event, plugin) == SCS_RESULT_ok &&
        init.register_for_event(SCS_TELEMETRY_EVENT_started, on_telemetry_event, plugin) == SCS_RESULT_ok;
    if (!events_registered) {
        plugin->log.message(SCS_LOG_TYPE_error, "cannot register telemetry events, drift disabled");
        plugin->telemetry_console = nullptr;
        plugin->use_console();
        release_plugin();
        return SCS_RESULT_generic_error;
    }

    if (!register_channel(*plugin, init, SCS_TELEMETRY_CHANNEL_next_rest_stop, SCS_VALUE_TYPE_s32, on_rest_stop,
                          &plugin->frame.rest_stop_min)) {
        plugin->log.message(SCS_LOG_TYPE_warning, "without rest.stop the drift stays off");
    }
    register_channel(*plugin, init, SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, on_float_channel,
                     &plugin->frame.speed_mps);
    register_channel(*plugin, init, SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, SCS_VALUE_TYPE_float,
                     on_float_channel, &plugin->frame.input_steering);
    return SCS_RESULT_ok;
}

void shutdown_telemetry()
{
    if (!g_plugin) {
        return;
    }
    Plugin& plugin = *g_plugin;
    plugin.log.info("telemetry shutdown");
    // Without telemetry the model gets no frames; keep the device quiet.
    plugin.frame = FatigueInput{};
    plugin.frame.paused = true;
    plugin.last_frame_time.reset();
    update_model(plugin);
    plugin.telemetry_console = nullptr;
    plugin.use_console();
    release_plugin();
}

// Input.

// Once per rendered frame: the next step of the camera sway. On pause the view stays put.
void advance_camera(Plugin& plugin, Clock::time_point now)
{
    const double dt = plugin.last_camera_frame ? std::chrono::duration<double>(now - *plugin.last_camera_frame).count() : 0.0;
    plugin.last_camera_frame = now;
    plugin.camera_step = plugin.sway.step(dt, plugin.frame.paused);
}

SCSAPI_VOID on_device_active(const scs_u8_t active, const scs_context_t context)
{
    Plugin& plugin = *static_cast<Plugin*>(context);
    plugin.next_input = 0;
    plugin.log.info("input device %s", active ? "active" : "inactive");
}

// Called repeatedly every input frame until it returns not_found: one event per input and frame.
SCSAPI_RESULT on_device_event(scs_input_event_t* const event_info, const scs_u32_t flags, const scs_context_t context)
{
    Plugin& plugin = *static_cast<Plugin*>(context);
    const BusyTimer timer(plugin);
    if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) {
        ++plugin.input_polls;
        plugin.next_input = 0;
        const Clock::time_point now = Clock::now();
        if (!plugin.last_input_poll || now - *plugin.last_input_poll > kSameFrameGap) {
            advance_camera(plugin, now);
        }
        plugin.last_input_poll = now;
    }
    if (plugin.next_input >= plugin.input_count) {
        return SCS_RESULT_not_found;
    }
    event_info->input_index = plugin.next_input;
    switch (plugin.next_input) {
    case kSteeringIndex:
        event_info->value_float.value = plugin.offset;
        break;
    case kCameraUpDownIndex:
        event_info->value_float.value = plugin.camera_step.up_down;
        break;
    case kCameraLeftRightIndex:
        event_info->value_float.value = plugin.camera_step.left_right;
        break;
    default:
        break;
    }
    ++plugin.next_input;
    return SCS_RESULT_ok;
}

scs_result_t init_input(const scs_input_init_params_v100_t& init)
{
    Plugin* const plugin = acquire_plugin();
    if (!plugin) {
        init.common.log(SCS_LOG_TYPE_error, "[long_shift] out of memory");
        return SCS_RESULT_generic_error;
    }
    plugin->input_console = init.common.log;
    plugin->use_console();
    plugin->log.info("input init: %s (%s), input version %u.%u", init.common.game_name, init.common.game_id,
                     SCS_GET_MAJOR_VERSION(init.common.game_version), SCS_GET_MINOR_VERSION(init.common.game_version));

    const scs_input_device_input_t inputs[kInputCount] = {
        {kSteeringInput, "Steering", SCS_VALUE_TYPE_float},
        {kCameraUpDownInput, "Camera up down", SCS_VALUE_TYPE_float},
        {kCameraLeftRightInput, "Camera left right", SCS_VALUE_TYPE_float},
    };
    scs_input_device_t device{};
    device.name = kDeviceName;
    device.display_name = kDeviceDisplayName;
    device.type = SCS_INPUT_DEVICE_TYPE_semantical;
    device.input_count = kInputCount;
    device.inputs = inputs;
    device.callback_context = plugin;
    device.input_active_callback = on_device_active;
    device.input_event_callback = on_device_event;

    scs_result_t result = init.register_device(&device);
    if (result != SCS_RESULT_ok) {
        // Without the camera inputs only the sway is lost; steering is what matters.
        plugin->log.message(SCS_LOG_TYPE_warning, "camera inputs refused (%d), registering steering only", result);
        device.input_count = 1;
        result = init.register_device(&device);
    }
    plugin->input_count = device.input_count;
    if (result != SCS_RESULT_ok) {
        plugin->log.message(SCS_LOG_TYPE_error, "cannot register the input device (%d), drift disabled", result);
        plugin->input_console = nullptr;
        plugin->use_console();
        release_plugin();
        return SCS_RESULT_generic_error;
    }
    plugin->log.message(SCS_LOG_TYPE_message, "ready: semantical steering input registered");
    return SCS_RESULT_ok;
}

void shutdown_input()
{
    if (!g_plugin) {
        return;
    }
    g_plugin->log.info("input shutdown");
    g_plugin->input_console = nullptr;
    g_plugin->use_console();
    release_plugin();
}

// Nothing may propagate out of the plugin into the game.
template <typename Function>
scs_result_t guarded(scs_log_t game_log, Function&& function) noexcept
{
    try {
        return function();
    } catch (const std::exception& error) {
        game_log(SCS_LOG_TYPE_error, "[long_shift] initialization failed");
        if (g_plugin) {
            g_plugin->log.info("initialization failed: %s", error.what());
        }
    } catch (...) {
        game_log(SCS_LOG_TYPE_error, "[long_shift] initialization failed");
    }
    return SCS_RESULT_generic_error;
}

}  // namespace
}  // namespace long_shift

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    // Check the version before reading params: their layout depends on it.
    if (version != SCS_TELEMETRY_VERSION_1_01 && version != SCS_TELEMETRY_VERSION_1_00) {
        return SCS_RESULT_unsupported;
    }
    const auto& init = *static_cast<const scs_telemetry_init_params_v100_t*>(params);
    return long_shift::guarded(init.common.log, [&] { return long_shift::init_telemetry(init); });
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    long_shift::shutdown_telemetry();
}

SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t* const params)
{
    if (version != SCS_INPUT_VERSION_1_00) {
        return SCS_RESULT_unsupported;
    }
    const auto& init = *static_cast<const scs_input_init_params_v100_t*>(params);
    return long_shift::guarded(init.common.log, [&] { return long_shift::init_input(init); });
}

SCSAPI_VOID scs_input_shutdown(void)
{
    long_shift::shutdown_input();
}
