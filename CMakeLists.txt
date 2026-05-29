cmake_minimum_required(VERSION 3.25)

project(VSTCompare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "This MVP currently supports Windows only.")
endif()

set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/external/vst3sdk" CACHE PATH "Path to VST3 SDK root")

if(NOT EXISTS "${VST3_SDK_ROOT}/cmake/modules/SMTG_VST3_SDK.cmake")
  message(FATAL_ERROR "VST3 SDK not found. Expected at: ${VST3_SDK_ROOT}")
endif()

list(APPEND CMAKE_MODULE_PATH "${VST3_SDK_ROOT}/cmake/modules")
include(SMTG_VST3_SDK)

set(SDK_ROOT "${VST3_SDK_ROOT}")
include_directories("${SDK_ROOT}")

smtg_setup_platform_toolset()
smtg_setup_symbol_visibility()
smtg_create_lib_base_target()
smtg_create_pluginterfaces_target()
smtg_create_public_sdk_common_target()
smtg_create_public_sdk_hosting_target()

add_executable(vstcompare_cli
  src/main.cpp
  src/cli.cpp
  src/pipeline.cpp
  src/subprocess_plugin_host.cpp
  src/vst3_plugin_host.cpp
  src/worker_server.cpp
  src/worker_ipc.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/ir_test_manager.cpp
  src/frequency_test_manager.cpp
  src/phase_test_manager.cpp
  src/harmonic_test_manager.cpp
  src/thd_test_manager.cpp
  src/mono_to_stereo_width_test_manager.cpp
  src/pitch_test_manager.cpp
  src/dynamics_test_manager.cpp
  src/time_response_test_manager.cpp
  src/report_builder.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
)

target_include_directories(vstcompare_cli PRIVATE include)

target_link_libraries(vstcompare_cli PRIVATE sdk_hosting)

target_compile_definitions(vstcompare_cli PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)

enable_testing()
add_executable(vstcompare_ir_metrics_test
  tests/ir_metrics_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/ir_test_manager.cpp
)
target_include_directories(vstcompare_ir_metrics_test PRIVATE include)
add_test(NAME ir_metrics_test COMMAND vstcompare_ir_metrics_test)

add_executable(vstcompare_frequency_test
  tests/frequency_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/frequency_test_manager.cpp
)
target_include_directories(vstcompare_frequency_test PRIVATE include)
add_test(NAME frequency_test COMMAND vstcompare_frequency_test)

add_executable(vstcompare_phase_test
  tests/phase_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/frequency_test_manager.cpp
  src/phase_test_manager.cpp
)
target_include_directories(vstcompare_phase_test PRIVATE include)
add_test(NAME phase_test COMMAND vstcompare_phase_test)

add_executable(vstcompare_harmonic_test
  tests/harmonic_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/harmonic_test_manager.cpp
)
target_include_directories(vstcompare_harmonic_test PRIVATE include)
add_test(NAME harmonic_test COMMAND vstcompare_harmonic_test)

add_executable(vstcompare_latency_alignment_test
  tests/latency_alignment_test.cpp
  src/latency_alignment.cpp
)
target_include_directories(vstcompare_latency_alignment_test PRIVATE include)
add_test(NAME latency_alignment_test COMMAND vstcompare_latency_alignment_test)

add_executable(vstcompare_report_json_test
  tests/report_builder_json_test.cpp
  src/report_builder.cpp
)
target_include_directories(vstcompare_report_json_test PRIVATE include)
add_test(NAME report_json_test COMMAND vstcompare_report_json_test)

add_executable(vstcompare_thd_test
  tests/thd_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/thd_test_manager.cpp
)
target_include_directories(vstcompare_thd_test PRIVATE include)
add_test(NAME thd_test COMMAND vstcompare_thd_test)

add_executable(vstcompare_mono_to_stereo_width_test
  tests/mono_to_stereo_width_test.cpp
  src/latency_alignment.cpp
  src/mono_to_stereo_width_test_manager.cpp
)
target_include_directories(vstcompare_mono_to_stereo_width_test PRIVATE include)
add_test(NAME mono_to_stereo_width_test COMMAND vstcompare_mono_to_stereo_width_test)

add_executable(vstcompare_pitch_test
  tests/pitch_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/pitch_test_manager.cpp
)
target_include_directories(vstcompare_pitch_test PRIVATE include)
add_test(NAME pitch_test COMMAND vstcompare_pitch_test)

add_executable(vstcompare_dynamics_test
  tests/dynamics_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/dynamics_test_manager.cpp
)
target_include_directories(vstcompare_dynamics_test PRIVATE include)
add_test(NAME dynamics_test COMMAND vstcompare_dynamics_test)

add_executable(vstcompare_time_response_test
  tests/time_response_test_manager_test.cpp
  src/analysis_signal.cpp
  src/latency_alignment.cpp
  src/time_response_test_manager.cpp
)
target_include_directories(vstcompare_time_response_test PRIVATE include)
add_test(NAME time_response_test COMMAND vstcompare_time_response_test)

add_executable(vstcompare_cli_parse_test
  tests/cli_parse_test.cpp
  src/cli.cpp
)
target_include_directories(vstcompare_cli_parse_test PRIVATE include)
add_test(NAME cli_parse_test COMMAND vstcompare_cli_parse_test)
