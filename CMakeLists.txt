cmake_minimum_required(VERSION 3.22)

if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SculptKey VERSION 0.1.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A local JUCE checkout can be selected for offline builds and verification.
set(SCULPT_JUCE_SOURCE_DIR "" CACHE PATH "Optional local JUCE 8.0.4 source directory")
if(SCULPT_JUCE_SOURCE_DIR)
  add_subdirectory("${SCULPT_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

file(GLOB_RECURSE SCULPT_KEY_WAVS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Samples/*.wav")
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Samples/manifest.json" OR NOT SCULPT_KEY_WAVS)
  message(FATAL_ERROR "The complete Samples/manifest.json and WAV sample bank are required.")
endif()

juce_add_binary_data(SculptKeySamples
  HEADER_NAME KeySampleData.h
  NAMESPACE KeySamples
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Samples/manifest.json" ${SCULPT_KEY_WAVS})
set_target_properties(SculptKeySamples PROPERTIES POSITION_INDEPENDENT_CODE ON)

juce_add_plugin(SculptKey
  COMPANY_NAME "Sculpt"
  BUNDLE_ID "com.sculpt.key"
  PRODUCT_NAME "Sculpt Key"
  PLUGIN_MANUFACTURER_CODE Scpt
  PLUGIN_CODE Skey
  FORMATS VST3
  VST3_CATEGORIES Instrument Synth
  IS_SYNTH TRUE
  IS_MIDI_EFFECT FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SculptKey PRIVATE
  Source/KeyEngine.h
  Source/PluginProcessor.h
  Source/PluginProcessor.cpp
  Source/PluginEditor.h
  Source/PluginEditor.cpp)

target_compile_definitions(SculptKey PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SculptKey
  PRIVATE
    SculptKeySamples
    juce::juce_audio_processors
    juce::juce_audio_formats
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
