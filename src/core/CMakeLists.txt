# Game-independent logic, shared by the plugin and the unit tests.
add_library(long_shift_core STATIC FatigueModel.cpp FatigueModel.h Config.cpp Config.h CameraSway.cpp CameraSway.h)
target_include_directories(long_shift_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(long_shift_core PRIVATE long_shift_options)
