# The plugin DLL the game loads from bin/win_x64/plugins.
configure_file(version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
add_library(long_shift SHARED plugin.cpp log.cpp log.h paths.cpp paths.h settings_file.cpp settings_file.h plugin.def
            "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
target_link_libraries(long_shift PRIVATE long_shift_core scs_sdk long_shift_options)
target_compile_definitions(long_shift PRIVATE LONG_SHIFT_VERSION="${PROJECT_VERSION}")
