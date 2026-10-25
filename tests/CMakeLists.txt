include(FetchContent)

# doctest is header-only: fetch it but skip its own CMake project.
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR header-only-no-cmake)
FetchContent_MakeAvailable(doctest)

add_library(doctest_header INTERFACE)
target_include_directories(doctest_header SYSTEM INTERFACE "${doctest_SOURCE_DIR}")

add_executable(long_shift_tests test_main.cpp fatigue_model_tests.cpp config_tests.cpp camera_sway_tests.cpp)
target_link_libraries(long_shift_tests PRIVATE long_shift_core doctest_header long_shift_options)

add_test(NAME unit_tests COMMAND long_shift_tests)

# Loads long_shift.dll outside the game and drives it through a fake of the SDK APIs.
add_executable(long_shift_plugin_smoke plugin_smoke.cpp)
target_link_libraries(long_shift_plugin_smoke PRIVATE long_shift_core scs_sdk long_shift_options version)
target_compile_definitions(long_shift_plugin_smoke PRIVATE LONG_SHIFT_VERSION="${PROJECT_VERSION}"
                           LONG_SHIFT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
                           LONG_SHIFT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
                           LONG_SHIFT_VERSION_PATCH=${PROJECT_VERSION_PATCH})
add_dependencies(long_shift_plugin_smoke long_shift)
add_test(NAME plugin_smoke COMMAND long_shift_plugin_smoke $<TARGET_FILE:long_shift>)
