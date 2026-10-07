cmake_minimum_required(VERSION 3.22)
project(SystemAudioBridgeEnhanced VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(SystemAudioBridgeEnhanced
    COMPANY_NAME "FmsJay"
    PRODUCT_NAME "System Audio Bridge (enhanced)"
    PLUGIN_MANUFACTURER_CODE Fmsj
    PLUGIN_CODE Sabe
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Tools
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SystemAudioBridgeEnhanced PRIVATE
    Source/PluginProcessor.cpp
    Source/Wasapi.cpp)

target_compile_definitions(SystemAudioBridgeEnhanced PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SystemAudioBridgeEnhanced
    PRIVATE
        juce::juce_audio_processors
        juce::juce_gui_basics
        mmdevapi ole32 avrt
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
