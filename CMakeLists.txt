cmake_minimum_required(VERSION 3.22)

# Before project(), and FORCE, or a build folder cached without them wins (v2's
# build-spike held an empty target). Unset, the binary silently requires the build
# machine's macOS (v1 1.0.0 shipped as 26+, 1.0.1 as 27+; v2 read minos 27.0 in the
# 10-18 audit). Check: otool -l <bundle binary> | grep minos.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)

project(LoopNest VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 17)

# JUCE is vendored inside this project (per-project checkout).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/JUCE JUCE)

# v2 (branch v2; the effect build proven in docs/spike-fx-sync.md): an AUDIO EFFECT.
# Released as plain LoopNest (Andrew, 10-18): the same bundle names and bundle id as
# v1, so installing it replaces v1's bundles, but its own plugin code (LpSx, v1 is
# LpNs), so a set that used v1 opens with that device missing rather than loading v2
# with v1's state. It still self-generates in AUDITION mode (UI Play, host input
# discarded) and runs host input through the character chain in FX mode. No MIDI bus:
# an effect doesn't need one, and with IS_SYNTH/NEEDS_MIDI_INPUT both FALSE JUCE emits
# AU type aufx. The bundle filename follows PRODUCT_NAME ("LoopNest.vst3").
juce_add_plugin(LoopNest
    PLUGIN_MANUFACTURER_CODE Wndr
    PLUGIN_CODE LpSx
    FORMATS VST3 AU
    PRODUCT_NAME "LoopNest"
    COMPANY_NAME "Wander Foster"
    COMPANY_COPYRIGHT "Copyright (c) 2026 Andrew Foster"
    BUNDLE_ID "com.wanderfoster.loopnest"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    # TRUE so typed input reaches the editor's value chips — with FALSE the macOS
    # VST3 view never becomes key window and keystrokes hit Live's key commands
    # instead ('0' deactivates the track while you're typing a value).
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES "Fx"
)

juce_generate_juce_header(LoopNest)

# Bundle JetBrains Mono (SIL OFL 1.1) so the blueprint type renders identically
# on every machine. Embedded as BinaryData::JetBrainsMono*_ttf.
# v2 stage 2: the web UI (ui/index.html) and JUCE's frontend helper module ride in
# BinaryData too, served by the editor's resource provider by original filename, so
# the plugin never loads anything from disk or the network.
juce_add_binary_data(LoopNestData SOURCES
    fonts/JetBrainsMono-Regular.ttf
    fonts/JetBrainsMono-Bold.ttf
    ui/index.html
    JUCE/modules/juce_gui_extra/native/javascript/index.js
    JUCE/modules/juce_gui_extra/native/javascript/check_native_interop.js
)

set(LOOPNEST_UI_SOURCES
    Source/UiBridge.cpp
    Source/BankDrop.cpp
    Source/WebEditor.cpp
    Source/HatchOverlay.mm
    Source/RackDrop.mm
)

target_sources(LoopNest PRIVATE
    Source/PluginProcessor.cpp
    ${LOOPNEST_UI_SOURCES}
)

target_compile_definitions(LoopNest PUBLIC
    JUCE_WEB_BROWSER=1   # stage 2: the editor is a WKWebView
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # Signalsmith Linear's FFT on Accelerate (juce_audio_basics already links it).
    SIGNALSMITH_USE_ACCELERATE=1
)

# The editor's diagnostics log (~/Library/Logs/LoopNest/hatch.log: editor opens, drags,
# drops with their file paths). Off in the public build (ruling 4, Andrew 10-18); turn it
# on for a host test: cmake -DLOOPNEST_DIAG_LOG=ON. Off, nothing is written under ~/Library/Logs.
option(LOOPNEST_DIAG_LOG "Write the editor's hatch.log diagnostics" OFF)
if(LOOPNEST_DIAG_LOG)
    target_compile_definitions(LoopNest PRIVATE LOOPNEST_DIAG_LOG=1)
endif()

# Pitch-preserving time-stretch for SYNC. Header-only, vendored as
# plain copies (MIT, LICENSE.txt alongside each): Signalsmith Stretch 1.4.0
# (a670068) + its dependency Signalsmith Linear 0.6.4 (de55e6a, the tag Stretch's
# own CMakeLists pins). Included only via Source/TimeStretch.h.
target_include_directories(LoopNest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include
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

# Ship the JetBrains Mono license inside both bundles — the SIL OFL requires the
# license text to accompany the font wherever it's distributed. With it, the binary's
# licence (ruling 2, Andrew 10-18: the binaries are AGPLv3, the source stays MIT):
# the AGPLv3 text and the third-party notices for what JUCE compiles in.
set(LOOPNEST_BUNDLE_TEXTS
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/OFL.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/AGPL-3.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt")
add_custom_command(TARGET LoopNest_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LOOPNEST_BUNDLE_TEXTS}
        "$<TARGET_FILE_DIR:LoopNest_VST3>/../Resources/"
    VERBATIM)   # quotes any space in the bundle path
add_custom_command(TARGET LoopNest_AU POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_CONTENT_DIR:LoopNest_AU>/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LOOPNEST_BUNDLE_TEXTS}
        "$<TARGET_BUNDLE_CONTENT_DIR:LoopNest_AU>/Resources/"
    VERBATIM)

# Re-seal both bundles last. JUCE signs at link time, then its own POST_BUILD
# step writes moduleinfo.json into the VST3, and the text copies above mutate both
# bundles — leaving a stale seal Ableton silently rejects. POST_BUILD commands run
# in the order they're added, and juce_add_plugin registered JUCE's (moduleinfo
# included) above, so these run after everything else. The install-time re-sign
# in CLAUDE.md stays as belt-and-braces.
foreach(fmt VST3 AU)
    add_custom_command(TARGET LoopNest_${fmt} POST_BUILD
        COMMAND codesign -s - --force --deep "$<TARGET_BUNDLE_DIR:LoopNest_${fmt}>"
        VERBATIM)
endforeach()

# POST_BUILD only runs when a bundle relinks, so an edited licence text alone would never
# reach it: this re-copies the texts and re-seals both bundles whenever a text (or a
# bundle's binary) is newer than its stamp.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_texts.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LOOPNEST_BUNDLE_TEXTS} "$<TARGET_BUNDLE_CONTENT_DIR:LoopNest_VST3>/Resources/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LOOPNEST_BUNDLE_TEXTS} "$<TARGET_BUNDLE_CONTENT_DIR:LoopNest_AU>/Resources/"
    COMMAND codesign -s - --force --deep "$<TARGET_BUNDLE_DIR:LoopNest_VST3>"
    COMMAND codesign -s - --force --deep "$<TARGET_BUNDLE_DIR:LoopNest_AU>"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/bundle_texts.stamp"
    DEPENDS ${LOOPNEST_BUNDLE_TEXTS} LoopNest_VST3 LoopNest_AU
            "$<TARGET_FILE:LoopNest_VST3>" "$<TARGET_FILE:LoopNest_AU>"
    VERBATIM)
add_custom_target(LoopNest_BundleTexts ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bundle_texts.stamp")

# v2 stage 0: console test target (snapshot round-trips, restore gestures, the async
# loader, get/setState). Compiles the processor + editor sources directly, under its
# own product name: the processor's settings and Prints dirs follow JucePlugin_Name,
# so a test run never touches the plugin's. Run: build/LoopNestTests_artefacts/
# <config>/LoopNestTests (exit 0 = pass).
juce_add_console_app(LoopNestTests PRODUCT_NAME "LoopNest Tests")
juce_generate_juce_header(LoopNestTests)
target_sources(LoopNestTests PRIVATE
    tests/LoopNestTests.cpp
    Source/PluginProcessor.cpp
    ${LOOPNEST_UI_SOURCES}
)
target_compile_definitions(LoopNestTests PRIVATE
    JucePlugin_Name="LoopNest Tests"
    LOOPNEST_PRINTS_ENV=1          # the UiBridge HATCH tests print into the temp root
    JUCE_MODAL_LOOPS_PERMITTED=1   # runDispatchLoopUntil: the tests deliver the AsyncUpdater install for real
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    SIGNALSMITH_USE_ACCELERATE=1
)
target_include_directories(LoopNestTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include
)
target_link_libraries(LoopNestTests PRIVATE
    LoopNestData
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# v2 audit: a measurement tool (prints numbers, asserts nothing). Each rack knob's
# loudness, peak, DC and delay through CharacterChain, and GAIN MATCH / BYPASS through
# the real processor in AUDITION and FX. Its own product name, like the tests.
# Run: build-spike/LoopNestMeasure_artefacts/<config>/LoopNestMeasure [44.1 kHz loops...]
juce_add_console_app(LoopNestMeasure PRODUCT_NAME "LoopNest Measure")
juce_generate_juce_header(LoopNestMeasure)
target_sources(LoopNestMeasure PRIVATE
    tests/DspMeasure.cpp
    Source/PluginProcessor.cpp
    ${LOOPNEST_UI_SOURCES}
)
target_compile_definitions(LoopNestMeasure PRIVATE
    JucePlugin_Name="LoopNest Measure"
    LOOPNEST_PRINTS_ENV=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    SIGNALSMITH_USE_ACCELERATE=1
)
target_include_directories(LoopNestMeasure PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include
)
target_link_libraries(LoopNestMeasure PRIVATE
    LoopNestData
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# v2 stage 2: a windowed harness for the web editor, outside any host. It hosts the
# processor (a silent render thread stands in for the host's audio callback, so the
# playhead and meters move) and shows the real editor: WKWebView, bridge and the
# HATCH overlay. For proving the drag-out into a scratch Finder folder and for
# WKWebView snapshots. Never installed. Its own product name keeps its settings and
# Prints dirs apart from the plugin's.
juce_add_gui_app(LoopNestUiHarness PRODUCT_NAME "LoopNest UI Harness")
juce_generate_juce_header(LoopNestUiHarness)
target_sources(LoopNestUiHarness PRIVATE
    tests/UiHarness.mm
    Source/PluginProcessor.cpp
    ${LOOPNEST_UI_SOURCES}
)
target_compile_definitions(LoopNestUiHarness PRIVATE
    JucePlugin_Name="LoopNest UI Harness"
    LOOPNEST_PRINTS_ENV=1   # honour LOOPNEST_PRINTS_DIR (scratch prints; the plugin never reads it)
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    SIGNALSMITH_USE_ACCELERATE=1
)
target_include_directories(LoopNestUiHarness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include
)
target_link_libraries(LoopNestUiHarness PRIVATE
    LoopNestData
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
