# Native helpers for the Potato mod. Build-Tools.ps1 builds them into build-tools\Release.
cmake_minimum_required(VERSION 3.22)
project(VoicemeeterTools CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_options(/W4 /utf-8)

add_executable(vmtone vmtone.cpp)
target_link_libraries(vmtone PRIVATE ole32)

add_executable(vm-sessions WIN32 vm-sessions.cpp)
target_link_libraries(vm-sessions PRIVATE ole32 comctl32 version shell32)

# Softphone jump-in: the mic rules with their tests, and the hidden helper the mod DLL starts.
add_executable(softphone-tests softphone/test_engine.cpp)
add_executable(vm-softphone WIN32 softphone/vm-softphone.cpp)
target_link_libraries(vm-softphone PRIVATE ws2_32 ole32 shell32)
add_executable(overlay-preview softphone/overlay-preview.cpp)
target_link_libraries(overlay-preview PRIVATE d2d1 dwrite windowscodecs ole32)
