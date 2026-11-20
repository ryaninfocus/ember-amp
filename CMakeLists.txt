cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(EmberAmp VERSION 1.1.3)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at a local JUCE checkout to skip the download:
#   cmake -B build -DJUCE_PATH=/path/to/JUCE
set(JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(JUCE_PATH)
    add_subdirectory(${JUCE_PATH} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(EmberAmp
    COMPANY_NAME                  "Ember Audio"
    BUNDLE_ID                     com.emberaudio.emberamp
    PLUGIN_MANUFACTURER_CODE      Embr
    PLUGIN_CODE                   EmAp
    FORMATS                       Standalone AU VST3
    PRODUCT_NAME                  "Ember Amp"
    ICON_BIG                      Resources/logo.png
    IS_SYNTH                      FALSE
    NEEDS_MIDI_INPUT              FALSE
    NEEDS_MIDI_OUTPUT             FALSE
    IS_MIDI_EFFECT                FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Ember Amp needs audio input access to process your guitar."
    COPY_PLUGIN_AFTER_BUILD       FALSE)

target_sources(EmberAmp PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StreamOutput.cpp)

juce_add_binary_data(EmberAmpAssets SOURCES Resources/logo.png)

target_compile_definitions(EmberAmp PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(EmberAmp
    PRIVATE
        EmberAmpAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
