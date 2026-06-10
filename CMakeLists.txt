cmake_minimum_required(VERSION 3.22)
project(LoopNest VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)

# JUCE lives alongside this project inside the ClaudeCodeVault folder.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

# LoopNest is a self-generating instrument that ignores MIDI for sound: it
# sits on a track, auditions its loop via a UI Play button, and exports via
# drag-out. It DOES declare a MIDI input bus, though — Ableton refuses to open an
# Instrument-category VST3 that has no event input. The MIDI is simply ignored.
juce_add_plugin(LoopNest
    PLUGIN_MANUFACTURER_CODE Wndr
    PLUGIN_CODE LpNs
    FORMATS VST3 AU
    PRODUCT_NAME "LoopNest"
    COMPANY_NAME "Wander Foster"
    BUNDLE_ID "com.wanderfoster.loopnest"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    # TRUE so typed input reaches the editor's value chips — with FALSE the macOS
    # VST3 view never becomes key window and keystrokes hit Live's key commands
    # instead ('0' deactivates the track while you're typing a value).
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES "Instrument"
)

juce_generate_juce_header(LoopNest)

# Bundle JetBrains Mono (SIL OFL 1.1) so the blueprint type renders identically
# on every machine. Embedded as BinaryData::JetBrainsMono*_ttf.
juce_add_binary_data(LoopNestData SOURCES
    fonts/JetBrainsMono-Regular.ttf
    fonts/JetBrainsMono-Bold.ttf
    fonts/JetBrainsMono-ExtraBold.ttf
)

target_sources(LoopNest PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(LoopNest PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(LoopNest PRIVATE
    LoopNestData
    juce::juce_audio_utils
    juce::juce_audio_processors
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Ship the JetBrains Mono license inside the bundle — the SIL OFL requires the
# license text to accompany the font wherever it's distributed.
add_custom_command(TARGET LoopNest_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts/OFL.txt"
        "$<TARGET_FILE_DIR:LoopNest_VST3>/../Resources/OFL.txt")
