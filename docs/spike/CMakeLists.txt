# Stage 1 spike: checks in-game that an SDK input device can add an offset to steering.

add_library(fatigue_drift_spike SHARED spike.cpp spike.def)
target_link_libraries(fatigue_drift_spike PRIVATE scs_sdk long_shift_options)

# Loads the DLL outside the game and drives it through fake SDK APIs.
add_executable(fatigue_drift_spike_smoke smoke_host.cpp)
target_link_libraries(fatigue_drift_spike_smoke PRIVATE scs_sdk long_shift_options)
add_dependencies(fatigue_drift_spike_smoke fatigue_drift_spike)

add_test(NAME spike_smoke COMMAND fatigue_drift_spike_smoke $<TARGET_FILE:fatigue_drift_spike>)
