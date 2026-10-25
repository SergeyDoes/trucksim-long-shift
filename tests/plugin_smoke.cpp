// Smoke test for long_shift.dll: loads the plugin outside the game and drives it through a fake
// of the SDK telemetry and input APIs, including the game's habit of re-polling an input device at
// once while its value keeps changing.
// Usage: long_shift_plugin_smoke <path to long_shift.dll>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"
#include "scssdk_input.h"
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);           \
            ++g_failures;                                                              \
        }                                                                              \
    } while (false)

// Fake game.

struct EventRegistration {
    scs_telemetry_event_callback_t callback;
    scs_context_t context;
};

struct ChannelRegistration {
    std::string name;
    scs_value_type_t type;
    scs_telemetry_channel_callback_t callback;
    scs_context_t context;
};

struct DeviceRegistration {
    std::string name;
    scs_input_device_type_t type;
    std::vector<std::string> input_names;
    std::vector<scs_value_type_t> input_types;
    scs_context_t context;
    scs_input_active_callback_t active_callback;
    scs_input_event_callback_t event_callback;
};

struct FakeGame {
    std::map<scs_event_t, EventRegistration> events;
    std::vector<ChannelRegistration> channels;
    std::vector<DeviceRegistration> devices;
    bool inside_input_init = false;
    bool refuse_events = false;  // simulates a game that rejects the plugin's telemetry registrations
    scs_timestamp_t time_us = 0;
};

FakeGame g_game;

bool valid_config_name(const std::string& text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool valid_display_name(const std::string& text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ' ' ||
               c == '.';
    });
}

SCSAPI_VOID fake_log(const scs_log_type_t type, const scs_string_t message)
{
    std::printf("  game log [%d] %s\n", type, message);
}

SCSAPI_RESULT fake_register_for_event(const scs_event_t event, const scs_telemetry_event_callback_t callback,
                                      const scs_context_t context)
{
    if (g_game.refuse_events) {
        return SCS_RESULT_generic_error;
    }
    if (!callback) {
        return SCS_RESULT_invalid_parameter;
    }
    if (g_game.events.count(event)) {
        return SCS_RESULT_already_registered;
    }
    g_game.events[event] = {callback, context};
    return SCS_RESULT_ok;
}

SCSAPI_RESULT fake_unregister_from_event(const scs_event_t event)
{
    return g_game.events.erase(event) ? SCS_RESULT_ok : SCS_RESULT_not_found;
}

SCSAPI_RESULT fake_register_for_channel(const scs_string_t name, const scs_u32_t index, const scs_value_type_t type,
                                        const scs_u32_t, const scs_telemetry_channel_callback_t callback,
                                        const scs_context_t context)
{
    static const std::map<std::string, scs_value_type_t> known_channels = {
        {SCS_TELEMETRY_CHANNEL_next_rest_stop, SCS_VALUE_TYPE_s32},
        {SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float},
        {SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, SCS_VALUE_TYPE_float},
    };
    CHECK(index == SCS_U32_NIL);
    const auto known = known_channels.find(name);
    if (known == known_channels.end()) {
        return SCS_RESULT_not_found;
    }
    if (known->second != type) {
        return SCS_RESULT_unsupported_type;
    }
    g_game.channels.push_back({name, type, callback, context});
    return SCS_RESULT_ok;
}

SCSAPI_RESULT fake_unregister_from_channel(const scs_string_t name, const scs_u32_t, const scs_value_type_t type)
{
    const auto before = g_game.channels.size();
    g_game.channels.erase(std::remove_if(g_game.channels.begin(), g_game.channels.end(),
                                         [&](const ChannelRegistration& c) { return c.name == name && c.type == type; }),
                          g_game.channels.end());
    return g_game.channels.size() != before ? SCS_RESULT_ok : SCS_RESULT_not_found;
}

// Validates the device as scssdk_input_device.h documents it and copies it: the plugin's structure
// only has to live for the duration of the call.
SCSAPI_RESULT fake_register_device(const scs_input_device_t* const info)
{
    if (!g_game.inside_input_init) {
        return SCS_RESULT_not_now;
    }
    if (!info || !info->name || !info->display_name || !valid_config_name(info->name) ||
        !valid_display_name(info->display_name) || !info->input_event_callback || info->input_count == 0 ||
        info->input_count > SCS_INPUT_MAX_INPUT_COUNT || !info->inputs ||
        (info->type != SCS_INPUT_DEVICE_TYPE_generic && info->type != SCS_INPUT_DEVICE_TYPE_semantical)) {
        return SCS_RESULT_invalid_parameter;
    }
    DeviceRegistration device{info->name, info->type, {}, {}, info->callback_context, info->input_active_callback,
                              info->input_event_callback};
    for (scs_u32_t i = 0; i < info->input_count; ++i) {
        const scs_input_device_input_t& input = info->inputs[i];
        if (!input.name || !input.display_name || !valid_config_name(input.name) ||
            !valid_display_name(input.display_name) ||
            (input.value_type != SCS_VALUE_TYPE_bool && input.value_type != SCS_VALUE_TYPE_float)) {
            return SCS_RESULT_invalid_parameter;
        }
        device.input_names.push_back(input.name);
        device.input_types.push_back(input.value_type);
    }
    g_game.devices.push_back(device);
    return SCS_RESULT_ok;
}

void send_event(scs_event_t event, const void* info = nullptr)
{
    const auto registration = g_game.events.find(event);
    if (registration != g_game.events.end()) {
        registration->second.callback(event, info, registration->second.context);
    }
}

void send_channel(const char* name, const scs_value_t* value, scs_value_type_t type)
{
    for (const auto& channel : g_game.channels) {
        if (channel.name == name && channel.type == type) {
            channel.callback(name, SCS_U32_NIL, value, channel.context);
        }
    }
}

// What the truck and the driver are doing.
struct World {
    std::optional<int> rest_stop_min = 600;
    float speed_mps = 0.0f;
    float input_steering = 0.0f;
    bool paused = true;
};

// One physics frame of telemetry, 1/60 s of game time. On pause the game keeps sending the frame
// events but no channel data.
void telemetry_frame(const World& world)
{
    scs_telemetry_frame_start_t frame_start{};
    frame_start.paused_simulation_time = g_game.time_us;
    send_event(SCS_TELEMETRY_EVENT_frame_start, &frame_start);
    if (!world.paused) {
        g_game.time_us += 16667;
        scs_value_t value{};
        if (world.rest_stop_min) {
            value.type = SCS_VALUE_TYPE_s32;
            value.value_s32.value = *world.rest_stop_min;
            send_channel(SCS_TELEMETRY_CHANNEL_next_rest_stop, &value, SCS_VALUE_TYPE_s32);
        } else {
            send_channel(SCS_TELEMETRY_CHANNEL_next_rest_stop, nullptr, SCS_VALUE_TYPE_s32);
        }
        value.type = SCS_VALUE_TYPE_float;
        value.value_float.value = world.speed_mps;
        send_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, &value, SCS_VALUE_TYPE_float);
        value.value_float.value = world.input_steering;
        send_channel(SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, &value, SCS_VALUE_TYPE_float);
    }
    send_event(SCS_TELEMETRY_EVENT_frame_end);
}

// Polls the device once like the game does in a frame: one event per input, in order. Returns the
// steering value.
float poll_device(const DeviceRegistration& device, scs_u32_t flags)
{
    std::vector<scs_input_event_t> reported;
    for (int call = 0; call < 8; ++call) {
        scs_input_event_t event{};
        const scs_result_t result = device.event_callback(&event, call == 0 ? flags : 0, device.context);
        if (result == SCS_RESULT_not_found) {
            break;
        }
        CHECK(result == SCS_RESULT_ok);
        if (result != SCS_RESULT_ok) {
            break;
        }
        CHECK(event.input_index == reported.size());
        reported.push_back(event);
    }
    CHECK(reported.size() == device.input_names.size());
    for (size_t camera = 1; camera < reported.size(); ++camera) {  // camud, camlr: small steps of the sway
        CHECK(std::isfinite(reported[camera].value_float.value));
        CHECK(std::fabs(reported[camera].value_float.value) < 5.0f);
    }
    return reported.empty() ? NAN : reported[0].value_float.value;
}

// ATS re-polls a device (a new round with first_in_frame) while its value keeps changing. Returns the
// rounds it took the value to settle; a plugin that recomputes its value per call never settles.
constexpr int kMaxInputRounds = 1000;

int input_frame(const DeviceRegistration& device, float& state)
{
    for (int round = 1; round <= kMaxInputRounds; ++round) {
        const float value = poll_device(device, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame);
        if (value == state) {
            return round;
        }
        state = value;
    }
    return kMaxInputRounds + 1;
}

// input.steering while the player keeps the controls still: the sign of the total steering, which
// is only our offset, in the game's convention (positive = left).
float idle_player_steering(float offset)
{
    return offset > 0.0f ? -1.0f : offset < 0.0f ? 1.0f : 0.0f;
}

struct Drive {
    const DeviceRegistration& device;
    World& world;
    float offset = 0.0f;  // what the game got from the device
    float max_offset = 0.0f;
    int max_rounds = 0;
    bool player_idle = true;

    void frame()
    {
        max_rounds = std::max(max_rounds, input_frame(device, offset));
        max_offset = std::max(max_offset, std::fabs(offset));
        if (player_idle) {
            world.input_steering = idle_player_steering(offset);
        }
        telemetry_frame(world);
    }

    void seconds(double duration)
    {
        for (int frame_index = 0; frame_index < static_cast<int>(duration * 60.0); ++frame_index) {
            frame();
        }
    }

    bool until_drifting(double max_seconds)
    {
        for (int frame_index = 0; frame_index < static_cast<int>(max_seconds * 60.0); ++frame_index) {
            frame();
            if (std::fabs(offset) > 0.05f) {
                return true;
            }
        }
        return false;
    }
};

struct Plugin {
    decltype(&scs_telemetry_init) telemetry_init = nullptr;
    decltype(&scs_telemetry_shutdown) telemetry_shutdown = nullptr;
    decltype(&scs_input_init) input_init = nullptr;
    decltype(&scs_input_shutdown) input_shutdown = nullptr;
    scs_telemetry_init_params_v101_t telemetry_params{};
    scs_input_init_params_v100_t input_params{};

    scs_result_t init_telemetry(scs_u32_t version = SCS_TELEMETRY_VERSION_1_01)
    {
        const scs_result_t result = telemetry_init(version, &telemetry_params);
        if (result != SCS_RESULT_ok) {
            g_game.events.clear();  // the game drops registrations of a failed init
            g_game.channels.clear();
        }
        return result;
    }

    scs_result_t init_input(scs_u32_t version = SCS_INPUT_VERSION_1_00)
    {
        g_game.inside_input_init = true;
        const scs_result_t result = input_init(version, &input_params);
        g_game.inside_input_init = false;
        if (result != SCS_RESULT_ok) {
            g_game.devices.clear();
        }
        return result;
    }

    void shutdown_telemetry()
    {
        telemetry_shutdown();
        g_game.events.clear();
        g_game.channels.clear();
    }

    void shutdown_input()
    {
        g_game.devices.clear();  // devices are unregistered before scs_input_shutdown
        input_shutdown();
    }
};

void fill_common(scs_sdk_init_params_v100_t& common, scs_u32_t game_version)
{
    common.game_name = "Fake Truck Simulator";
    common.game_id = "ats";
    common.game_version = game_version;
    common.log = fake_log;
}

const DeviceRegistration* steering_device()
{
    CHECK(g_game.devices.size() == 1);
    if (g_game.devices.size() != 1) {
        return nullptr;
    }
    const DeviceRegistration& device = g_game.devices[0];
    CHECK(device.name == "long_shift");
    CHECK(device.type == SCS_INPUT_DEVICE_TYPE_semantical);
    CHECK((device.input_names == std::vector<std::string>{"steering", "camud", "camlr"}));
    CHECK((device.input_types ==
           std::vector<scs_value_type_t>{SCS_VALUE_TYPE_float, SCS_VALUE_TYPE_float, SCS_VALUE_TYPE_float}));
    if (device.active_callback) {
        device.active_callback(1, device.context);
    }
    return &device;
}

std::wstring file_next_to(const std::wstring& dll_path, const wchar_t* name)
{
    const auto slash = dll_path.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : dll_path.substr(0, slash + 1)) + name;
}

std::string read_file(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Writes long_shift.ini, then waits until the plugin's once-a-second check can see the change.
void edit_settings(const std::wstring& path, std::string_view text)
{
    std::ofstream(path, std::ios::binary) << text;
    Sleep(1100);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::printf("usage: long_shift_plugin_smoke <long_shift.dll>\n");
        return 2;
    }
    const std::wstring dll_path = argv[1];
    const std::wstring log_path = file_next_to(dll_path, L"long_shift.log");
    const std::wstring ini_path = file_next_to(dll_path, L"long_shift.ini");
    DeleteFileW(log_path.c_str());
    DeleteFileW(ini_path.c_str());

    std::printf("-- the DLL carries its version\n");
    {
        DWORD handle = 0;
        std::vector<char> info(GetFileVersionInfoSizeW(dll_path.c_str(), &handle));
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT size = 0;
        CHECK(!info.empty() && GetFileVersionInfoW(dll_path.c_str(), 0, static_cast<DWORD>(info.size()), info.data()) &&
              VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &size) && fixed);
        if (fixed) {
            CHECK(HIWORD(fixed->dwFileVersionMS) == LONG_SHIFT_VERSION_MAJOR);
            CHECK(LOWORD(fixed->dwFileVersionMS) == LONG_SHIFT_VERSION_MINOR);
            CHECK(HIWORD(fixed->dwFileVersionLS) == LONG_SHIFT_VERSION_PATCH);
        }
    }

    HMODULE dll = LoadLibraryW(dll_path.c_str());
    CHECK(dll != nullptr);
    if (!dll) {
        return 1;
    }
    Plugin plugin;
    plugin.telemetry_init = reinterpret_cast<decltype(&scs_telemetry_init)>(GetProcAddress(dll, "scs_telemetry_init"));
    plugin.telemetry_shutdown =
        reinterpret_cast<decltype(&scs_telemetry_shutdown)>(GetProcAddress(dll, "scs_telemetry_shutdown"));
    plugin.input_init = reinterpret_cast<decltype(&scs_input_init)>(GetProcAddress(dll, "scs_input_init"));
    plugin.input_shutdown = reinterpret_cast<decltype(&scs_input_shutdown)>(GetProcAddress(dll, "scs_input_shutdown"));
    if (!plugin.telemetry_init || !plugin.telemetry_shutdown || !plugin.input_init || !plugin.input_shutdown) {
        std::printf("FAIL: missing exports\n");
        return 1;
    }
    fill_common(plugin.telemetry_params.common, SCS_MAKE_VERSION(1, 5));
    plugin.telemetry_params.register_for_event = fake_register_for_event;
    plugin.telemetry_params.unregister_from_event = fake_unregister_from_event;
    plugin.telemetry_params.register_for_channel = fake_register_for_channel;
    plugin.telemetry_params.unregister_from_channel = fake_unregister_from_channel;
    fill_common(plugin.input_params.common, SCS_MAKE_VERSION(1, 0));
    plugin.input_params.register_device = fake_register_device;

    std::printf("-- unsupported API versions are refused without side effects\n");
    CHECK(plugin.init_telemetry(SCS_MAKE_VERSION(2, 0)) == SCS_RESULT_unsupported);
    CHECK(plugin.init_input(SCS_MAKE_VERSION(2, 0)) == SCS_RESULT_unsupported);
    CHECK(g_game.events.empty() && g_game.channels.empty() && g_game.devices.empty());

    std::printf("-- no long_shift.ini: the plugin writes the defaults and drifts with them\n");
    CHECK(plugin.init_telemetry() == SCS_RESULT_ok);
    CHECK(plugin.init_input() == SCS_RESULT_ok);
    CHECK(read_file(ini_path) == std::string(long_shift::default_settings_text()));
    const DeviceRegistration* device = steering_device();
    if (!device) {
        return 1;
    }
    World world;
    {
        send_event(SCS_TELEMETRY_EVENT_started);
        world.paused = false;
        world.rest_stop_min = 0;
        world.speed_mps = 25.0f;
        Drive drive{*device, world};
        drive.seconds(3 * 60.0);
        std::printf("   max |offset| %.3f\n", drive.max_offset);
        CHECK(drive.max_offset > 0.1f && drive.max_offset <= 0.2501f);  // yawns of 0.25
        CHECK(drive.max_rounds <= 2);
    }
    plugin.shutdown_input();
    plugin.shutdown_telemetry();

    std::printf("-- telemetry first, then input; yawns switched on in long_shift.ini\n");
    std::ofstream(ini_path, std::ios::binary)
        << "[general]\nstatus_log = 1\n[yawn]\nstrength = 0.25\nstart_minutes_before_rest = 120\n";
    CHECK(plugin.init_telemetry() == SCS_RESULT_ok);
    CHECK(plugin.init_input() == SCS_RESULT_ok);
    CHECK(g_game.events.size() == 4);
    CHECK(g_game.channels.size() == 3);
    device = steering_device();
    if (!device) {
        return 1;
    }
    world = World{};
    {
        Drive drive{*device, world};
        drive.seconds(1.0);  // loading, paused
        CHECK(drive.max_offset == 0.0f);

        send_event(SCS_TELEMETRY_EVENT_started);
        world.paused = false;

        std::printf("-- more than 120 minutes to rest: no drift\n");
        world.speed_mps = 25.0f;
        world.rest_stop_min = 121;
        drive.seconds(10 * 60.0);
        CHECK(drive.max_offset == 0.0f);

        std::printf("-- fatigue simulation off (no rest.stop): no drift\n");
        world.rest_stop_min.reset();
        drive.seconds(5 * 60.0);
        CHECK(drive.max_offset == 0.0f);

        std::printf("-- rest due, standing: no drift\n");
        world.rest_stop_min = 0;
        world.speed_mps = 0.0f;
        drive.seconds(5 * 60.0);
        CHECK(drive.max_offset == 0.0f);

        std::printf("-- rest due, 90 km/h: drift\n");
        world.speed_mps = 25.0f;
        drive.seconds(20 * 60.0);
        std::printf("   max |offset| %.3f, max polling rounds per frame %d\n", drive.max_offset, drive.max_rounds);
        CHECK(drive.max_offset > 0.1f && drive.max_offset <= 0.3f);
        CHECK(drive.max_rounds <= 2);

        std::printf("-- steering against the drift stops it\n");
        CHECK(drive.until_drifting(10 * 60.0));
        drive.player_idle = false;
        world.input_steering = drive.offset > 0.0f ? 1.0f : -1.0f;
        drive.seconds(1.0);
        CHECK(std::fabs(drive.offset) < 0.005f);
        drive.player_idle = true;

        std::printf("-- pause drops the drift at once\n");
        CHECK(drive.until_drifting(10 * 60.0));
        world.paused = true;
        send_event(SCS_TELEMETRY_EVENT_paused);
        CHECK(poll_device(*device, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) == 0.0f);
        drive.max_offset = 0.0f;
        drive.seconds(2.0);
        CHECK(drive.max_offset == 0.0f);

        std::printf("-- edits of long_shift.ini apply while driving\n");
        send_event(SCS_TELEMETRY_EVENT_started);
        world.paused = false;
        edit_settings(ini_path, "[general]\nenabled = 0\n");
        drive.frame();  // the plugin notices the change
        drive.max_offset = 0.0f;
        drive.seconds(60.0);
        CHECK(drive.max_offset == 0.0f);

        edit_settings(ini_path, "[yawn]\nstrength = 0.2\naverage_interval_s = lots\n");
        drive.frame();
        drive.max_offset = 0.0f;
        drive.seconds(3 * 60.0);
        std::printf("   max |offset| %.3f\n", drive.max_offset);
        CHECK(drive.max_offset > 0.1f && drive.max_offset <= 0.2001f);

        std::printf("-- after a rest the drift is off\n");
        world.rest_stop_min = 600;
        drive.frame();
        drive.max_offset = 0.0f;
        drive.seconds(60.0);
        CHECK(drive.max_offset == 0.0f);
    }
    plugin.shutdown_input();
    plugin.shutdown_telemetry();

    std::printf("-- input first; telemetry refused by the game: the device stays at 0\n");
    CHECK(plugin.init_input() == SCS_RESULT_ok);
    g_game.refuse_events = true;
    CHECK(plugin.init_telemetry() == SCS_RESULT_generic_error);
    g_game.refuse_events = false;
    device = steering_device();
    if (!device) {
        return 1;
    }
    {
        world = World{};
        world.paused = false;
        world.rest_stop_min = 0;
        world.speed_mps = 25.0f;
        Drive drive{*device, world};
        drive.seconds(60.0);
        CHECK(drive.max_offset == 0.0f);
    }
    plugin.shutdown_input();

    CHECK(FreeLibrary(dll));

    std::printf("-- log file\n");
    std::ifstream log(log_path);
    CHECK(log.good());
    std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    for (const char* expected :
         {"Long Shift " LONG_SHIFT_VERSION ", telemetry init: Fake Truck Simulator (ats)",
          "ready: semantical steering input registered",
          "settings: on; yawns 0.25 every 30 s, pause 10 s, full at 25 m/s, from 30 to 0 min before rest",
          "settings: camera sway 10 x 6, loop 12 s, randomness 0.5, standing 0.6, full at 15 m/s, from 120 to 0 min before rest",
          "settings: on; yawns 0.25 every 30 s, pause 10 s, full at 25 m/s, from 120 to 0 min before rest",
          "settings reloaded: off", "settings reloaded: on; yawns 0.20 every 30 s",
          "long_shift.ini: line 3: average_interval_s: \"lots\" is not a number",
          "driver getting tired (rest.stop 0 min)", "driver rested (rest.stop 600 min)",
          "yawn started at 25.0 m/s, yawn x1.00", "yawn ended, peak", "player steered against the yawn: offset",
          "status: speed 25.0 m/s, rest.stop 121, yawn x0.00", "status: speed 25.0 m/s, rest.stop none",
          "status: speed 0.0 m/s, rest.stop 0, yawn x0.00", " us/s, ", " polls/s", "paused",
          "cannot register telemetry events, drift disabled"}) {
        if (text.find(expected) == std::string::npos) {
            std::printf("FAIL: log has no \"%s\"\n", expected);
            ++g_failures;
        }
    }

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
