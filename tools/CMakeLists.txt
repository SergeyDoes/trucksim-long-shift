# Writes the default long_shift.ini into the build folder; scripts/package.ps1 puts it into the release.
add_executable(long_shift_default_ini write_default_ini.cpp)
target_link_libraries(long_shift_default_ini PRIVATE long_shift_core long_shift_options)

add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/long_shift.ini"
                   COMMAND long_shift_default_ini "${CMAKE_BINARY_DIR}/long_shift.ini"
                   DEPENDS long_shift_default_ini
                   COMMENT "Writing the default long_shift.ini")
add_custom_target(default_ini ALL DEPENDS "${CMAKE_BINARY_DIR}/long_shift.ini")
