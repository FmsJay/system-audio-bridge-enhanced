# Platform-independent tests for the processor's state and engine control (WASAPI is stubbed).
#   cmake -S tests -B build-tests && cmake --build build-tests && build-tests/StateTests_artefacts/StateTests
cmake_minimum_required(VERSION 3.22)
project(SystemAudioBridgeTests VERSION 1.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_console_app(StateTests)
target_sources(StateTests PRIVATE
    StateTests.cpp
    WasapiStub.cpp
    ../Source/PluginProcessor.cpp)
target_include_directories(StateTests PRIVATE ../Source)
target_compile_definitions(StateTests PRIVATE
    JucePlugin_Name="StateTests"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(StateTests PRIVATE
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_recommended_config_flags)
