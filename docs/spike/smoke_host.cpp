// Smoke test for fatigue_drift_spike.dll: loads the plugin outside the game and drives
// it through fake SDK telemetry/input APIs the way the game does (see readme.txt of the SDK).
// Usage: fatigue_drift_spike_smoke <path to fatigue_drift_spike.dll>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

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

    void reset_telemetry()
    {
        events.clear();
        channels.clear();
    }
};

FakeGame g_game;

bool valid_config_name(const char* text)
{
    if (!text || !*text) {
        return false;
    }
    for (; *text; ++text) {
        if (!((*text >= 'a' && *text <= 'z') || (*text >= '0' && *text <= '9') || *text == '_')) {
            return false;
        }
    }
    return true;
}

bool valid_display_name(const char* text)
{
    if (!text || !*text) {
        return false;
    }
    for (; *text; ++text) {
        const char c = *text;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ' ' || c == '.')) {
            return false;
        }
    }
    return true;
}

SCSAPI_VOID fake_log(const scs_log_type_t type, const scs_string_t message)
{
    std::printf("  game log [%d] %s\n", type, message);
}

SCSAPI_RESULT fake_register_for_event(const scs_event_t event, const scs_telemetry_event_callback_t callback, const scs_context_t context)
{
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
                                        const scs_u32_t, const scs_telemetry_channel_callback_t callback, const scs_context_t context)
{
    static const std::map<std::string, scs_value_type_t> known_channels = {
        {SCS_TELEMETRY_CHANNEL_next_rest_stop, SCS_VALUE_TYPE_s32},
        {SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float},
        {SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, SCS_VALUE_TYPE_float},
        {SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, SCS_VALUE_TYPE_float},
    };
    CHECK(index == SCS_U32_NIL);
    const auto known = known_channels.find(name);
    if (known == known_channels.end()) {
        return SCS_RESULT_not_found;
    }
    if (known->second != type) {
        return SCS_RESULT_unsupported_type;
    }
    if (!callback) {
        return SCS_RESULT_invalid_parameter;
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

// Validates the device the way scssdk_input_device.h documents it and copies it: the plugin's
// structure only has to live for the duration of the call.
SCSAPI_RESULT fake_register_device(const scs_input_device_t* const info)
{
    if (!g_game.inside_input_init) {
        return SCS_RESULT_not_now;
    }
    if (!info || !valid_config_name(info->name) || !valid_display_name(info->display_name) || !info->input_event_callback ||
        (info->type != SCS_INPUT_DEVICE_TYPE_generic && info->type != SCS_INPUT_DEVICE_TYPE_semantical) ||
        info->input_count == 0 || info->input_count > SCS_INPUT_MAX_INPUT_COUNT || !info->inputs) {
        return SCS_RESULT_invalid_parameter;
    }
    for (const auto& device : g_game.devices) {
        if (device.name == info->name) {
            return SCS_RESULT_already_registered;
        }
    }
    DeviceRegistration device{info->name, info->type, {}, {}, info->callback_context, info->input_active_callback, info->input_event_callback};
    for (scs_u32_t i = 0; i < info->input_count; ++i) {
        const scs_input_device_input_t& input = info->inputs[i];
        if (!valid_config_name(input.name) || !valid_display_name(input.display_name) ||
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

void send_channel(const char* name, const scs_value_t& value)
{
    for (const auto& channel : g_game.channels) {
        if (channel.name == name && channel.type == value.type) {
            channel.callback(name, SCS_U32_NIL, &value, channel.context);
        }
    }
}

void send_float(const char* name, float number)
{
    scs_value_t value{};
    value.type = SCS_VALUE_TYPE_float;
    value.value_float.value = number;
    send_channel(name, value);
}

void send_s32(const char* name, scs_s32_t number)
{
    scs_value_t value{};
    value.type = SCS_VALUE_TYPE_s32;
    value.value_s32.value = number;
    send_channel(name, value);
}

void send_telemetry_frame()
{
    scs_telemetry_frame_start_t frame_start{};
    send_event(SCS_TELEMETRY_EVENT_frame_start, &frame_start);
    send_float(SCS_TELEMETRY_TRUCK_CHANNEL_speed, 15.0f);
    send_float(SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, 0.05f);
    send_float(SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, 0.04f);
    send_s32(SCS_TELEMETRY_CHANNEL_next_rest_stop, 420);
    send_event(SCS_TELEMETRY_EVENT_frame_end);
}

// Polls a device like the game does once per frame. Returns the single reported value.
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
        reported.push_back(event);
    }
    CHECK(reported.size() == 1);
    if (reported.empty()) {
        return NAN;
    }
    CHECK(reported[0].input_index == 0);
    return reported[0].value_float.value;
}

// ATS re-polls a device right away (new round with first_in_frame) while its reported value keeps
// changing: in ATS 1.61 a value recomputed from the clock on every call made the game poll the spike
// ~6 million times per second and freeze. Models that loop; returns the rounds it took to settle.
constexpr int kMaxInputRounds = 1000;

int run_input_frame(const DeviceRegistration& device, float& state, scs_u32_t extra_flags = 0)
{
    for (int round = 1; round <= kMaxInputRounds; ++round) {
        const float value =
            poll_device(device, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame | (round == 1 ? extra_flags : 0));
        if (value == state) {
            return round;
        }
        state = value;
    }
    return kMaxInputRounds + 1;
}

const DeviceRegistration* find_device(const char* name)
{
    for (const auto& device : g_game.devices) {
        if (device.name == name) {
            return &device;
        }
    }
    return nullptr;
}

struct Plugin {
    decltype(&scs_telemetry_init) telemetry_init = nullptr;
    decltype(&scs_telemetry_shutdown) telemetry_shutdown = nullptr;
    decltype(&scs_input_init) input_init = nullptr;
    decltype(&scs_input_shutdown) input_shutdown = nullptr;
    scs_telemetry_init_params_v101_t telemetry_params{};
    scs_input_init_params_v100_t input_params{};

    scs_result_t init_telemetry(scs_u32_t version = SCS_TELEMETRY_VERSION_1_01)
    {
        return telemetry_init(version, &telemetry_params);
    }

    scs_result_t init_input(scs_u32_t version = SCS_INPUT_VERSION_1_00)
    {
        g_game.inside_input_init = true;
        const scs_result_t result = input_init(version, &input_params);
        g_game.inside_input_init = false;
        if (result != SCS_RESULT_ok) {
            g_game.devices.clear();  // the game drops registrations of a failed init
        }
        return result;
    }

    // The game unregisters everything automatically around shutdown.
    void shutdown_telemetry()
    {
        telemetry_shutdown();
        g_game.reset_telemetry();
    }

    void shutdown_input()
    {
        g_game.devices.clear();
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

std::wstring log_path_next_to(const std::wstring& dll_path)
{
    const auto slash = dll_path.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : dll_path.substr(0, slash + 1)) + L"fatigue_drift.log";
}

void check_devices_registered()
{
    CHECK(g_game.devices.size() == 2);
    const DeviceRegistration* const semantical = find_device("fatigue_drift_sem");
    const DeviceRegistration* const generic = find_device("fatigue_drift");
    CHECK(semantical && semantical->type == SCS_INPUT_DEVICE_TYPE_semantical && semantical->input_names.size() == 1 &&
          semantical->input_names[0] == "steering" && semantical->input_types[0] == SCS_VALUE_TYPE_float);
    CHECK(generic && generic->type == SCS_INPUT_DEVICE_TYPE_generic && generic->input_names.size() == 1 &&
          generic->input_names[0] == "steer_offset" && generic->input_types[0] == SCS_VALUE_TYPE_float);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::printf("usage: fatigue_drift_spike_smoke <fatigue_drift_spike.dll>\n");
        return 2;
    }
    const std::wstring dll_path = argv[1];
    const std::wstring log_path = log_path_next_to(dll_path);
    DeleteFileW(log_path.c_str());

    HMODULE dll = LoadLibraryW(dll_path.c_str());
    CHECK(dll != nullptr);
    if (!dll) {
        return 1;
    }

    Plugin plugin;
    plugin.telemetry_init = reinterpret_cast<decltype(&scs_telemetry_init)>(GetProcAddress(dll, "scs_telemetry_init"));
    plugin.telemetry_shutdown = reinterpret_cast<decltype(&scs_telemetry_shutdown)>(GetProcAddress(dll, "scs_telemetry_shutdown"));
    plugin.input_init = reinterpret_cast<decltype(&scs_input_init)>(GetProcAddress(dll, "scs_input_init"));
    plugin.input_shutdown = reinterpret_cast<decltype(&scs_input_shutdown)>(GetProcAddress(dll, "scs_input_shutdown"));
    CHECK(plugin.telemetry_init && plugin.telemetry_shutdown && plugin.input_init && plugin.input_shutdown);
    if (!plugin.telemetry_init || !plugin.telemetry_shutdown || !plugin.input_init || !plugin.input_shutdown) {
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

    std::printf("-- cycle 1: telemetry first, mode semantical by default\n");
    SetEnvironmentVariableW(L"FATIGUE_DRIFT_SPIKE_MODE", nullptr);
    CHECK(plugin.init_telemetry() == SCS_RESULT_ok);
    CHECK(plugin.init_input() == SCS_RESULT_ok);
    CHECK(g_game.events.count(SCS_TELEMETRY_EVENT_frame_start) && g_game.events.count(SCS_TELEMETRY_EVENT_paused) &&
          g_game.events.count(SCS_TELEMETRY_EVENT_started));
    CHECK(g_game.channels.size() == 4);
    check_devices_registered();
    const DeviceRegistration* semantical = find_device("fatigue_drift_sem");
    const DeviceRegistration* generic = find_device("fatigue_drift");
    if (!semantical || !generic) {
        return 1;
    }
    for (const auto& device : g_game.devices) {
        if (device.active_callback) {
            device.active_callback(1, device.context);
        }
    }

    // Telemetry starts paused: nothing may leak into steering yet.
    float semantical_state = 0.0f;
    float generic_state = 0.0f;
    for (int frame = 0; frame < 5; ++frame) {
        const scs_u32_t flags = frame == 0 ? SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation : 0;
        CHECK(run_input_frame(*semantical, semantical_state, flags) == 1);
        CHECK(run_input_frame(*generic, generic_state, flags) == 1);
        CHECK(semantical_state == 0.0f && generic_state == 0.0f);
        send_telemetry_frame();
    }

    send_event(SCS_TELEMETRY_EVENT_started);
    float max_semantical = 0.0f;
    float max_generic = 0.0f;
    int max_rounds = 0;
    const auto drive_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
    while (std::chrono::steady_clock::now() < drive_until) {
        max_rounds = std::max(max_rounds, run_input_frame(*semantical, semantical_state));
        max_rounds = std::max(max_rounds, run_input_frame(*generic, generic_state));
        max_semantical = std::max(max_semantical, std::fabs(semantical_state));
        max_generic = std::max(max_generic, std::fabs(generic_state));
        send_telemetry_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("   max |semantical| = %.4f, max |generic| = %.4f, max polling rounds per frame = %d\n",
                max_semantical, max_generic, max_rounds);
    CHECK(max_rounds <= 2);
    CHECK(max_semantical > 0.2f && max_semantical <= 0.3001f);
    CHECK(max_generic == 0.0f);

    send_event(SCS_TELEMETRY_EVENT_paused);
    send_telemetry_frame();
    CHECK(poll_device(*semantical, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) == 0.0f);

    plugin.shutdown_input();
    plugin.shutdown_telemetry();

    std::printf("-- cycle 2: input first, telemetry shut down first\n");
    CHECK(plugin.init_input() == SCS_RESULT_ok);
    CHECK(plugin.init_telemetry() == SCS_RESULT_ok);
    check_devices_registered();
    semantical = find_device("fatigue_drift_sem");
    if (!semantical) {
        return 1;
    }
    send_event(SCS_TELEMETRY_EVENT_started);
    send_telemetry_frame();
    poll_device(*semantical, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame);
    // A frame that starts late is reported as a stall.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    poll_device(*semantical, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame);
    plugin.shutdown_telemetry();
    // Input keeps running without telemetry: treated as paused, so no offset.
    CHECK(poll_device(*semantical, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) == 0.0f);
    plugin.shutdown_input();

    std::printf("-- cycle 3: FATIGUE_DRIFT_SPIKE_MODE=off starts the spike in mode off\n");
    SetEnvironmentVariableW(L"FATIGUE_DRIFT_SPIKE_MODE", L"off");
    CHECK(plugin.init_telemetry() == SCS_RESULT_ok);
    CHECK(plugin.init_input() == SCS_RESULT_ok);
    semantical = find_device("fatigue_drift_sem");
    if (!semantical) {
        return 1;
    }
    send_event(SCS_TELEMETRY_EVENT_started);
    for (int frame = 0; frame < 10; ++frame) {
        send_telemetry_frame();
        CHECK(poll_device(*semantical, SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) == 0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    plugin.shutdown_input();
    plugin.shutdown_telemetry();

    CHECK(FreeLibrary(dll));

    std::printf("-- log file\n");
    std::ifstream log(log_path);
    CHECK(log.good());
    int sessions = 0;
    int header_rows = 0;
    int data_rows = 0;
    bool started_logged = false;
    bool stall_logged = false;
    for (std::string line; std::getline(log, line);) {
        if (line.rfind("# ==== session", 0) == 0) {
            ++sessions;
        } else if (line.rfind("t,mode,amp,offset,", 0) == 0) {
            ++header_rows;
        } else if (line.find(" started, mode semantical") != std::string::npos) {
            started_logged = true;
        } else if (line.find(" stall: semantical device not polled for 0.4") != std::string::npos) {
            stall_logged = true;
        } else if (!line.empty() && line[0] != '#') {
            ++data_rows;
        }
    }
    std::printf("   sessions %d, data rows %d\n", sessions, data_rows);
    CHECK(sessions == 3 && header_rows == 3);
    CHECK(data_rows >= 10);
    CHECK(started_logged);
    CHECK(stall_logged);

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
