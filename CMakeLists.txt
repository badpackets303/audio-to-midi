cmake_minimum_required(VERSION 3.22)

project(AudioToMidi VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(JUCE)

juce_add_plugin(AudioToMidi
    COMPANY_NAME "YourCompany"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Juce
    PLUGIN_CODE Atm1
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Audio To MIDI"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "This app needs microphone access to convert audio to MIDI notes.")

juce_generate_juce_header(AudioToMidi)

target_sources(AudioToMidi
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

# Cycfi Q pitch detection (header-only, optional - clone into external/q to enable):
#   git clone --recurse-submodules https://github.com/cycfi/q.git external/q
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/q/q_lib/include")
    target_include_directories(AudioToMidi PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/q/q_lib/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/q/infra/include")
endif()

target_compile_definitions(AudioToMidi
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AudioToMidi
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Pitch detector unit tests (no JUCE dependency): ctest --test-dir build
option(AUDIOTOMIDI_BUILD_TESTS "Build the pitch detector unit tests" ON)
if(AUDIOTOMIDI_BUILD_TESTS)
    enable_testing()
    add_executable(PitchDetectorTests tests/PitchDetectorTests.cpp)
    target_include_directories(PitchDetectorTests PRIVATE Source)
    add_test(NAME PitchDetectorTests COMMAND PitchDetectorTests)

    # End-to-end MIDI output tests: builds the processor into a console app
    juce_add_console_app(ProcessorTests PRODUCT_NAME "ProcessorTests")
    juce_generate_juce_header(ProcessorTests)
    target_sources(ProcessorTests
        PRIVATE
            tests/ProcessorTests.cpp
            Source/PluginProcessor.cpp
            Source/PluginEditor.cpp)
    target_include_directories(ProcessorTests PRIVATE Source)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/q/q_lib/include")
        target_include_directories(ProcessorTests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/external/q/q_lib/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/external/q/infra/include")
    endif()
    target_compile_definitions(ProcessorTests
        PRIVATE
            JucePlugin_Name="Audio To MIDI"
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
    target_link_libraries(ProcessorTests
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags)
    add_test(NAME ProcessorTests COMMAND ProcessorTests)
endif()
