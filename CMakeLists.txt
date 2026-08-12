cmake_minimum_required(VERSION 3.24)
project(PluginLab VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(MSVC)
    add_compile_options(/EHsc)
endif()

# ── Fetch JUCE ──────────────────────────────────────────────────────────────
# Pinned to the release tag (not master): master moves, so CI and local builds
# would otherwise silently pull different JUCE versions (2026-08-11: master
# drifted 7dda739b → 9.0.1). Reproducible builds are a PR gate.
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# ── Standalone GUI Application ──────────────────────────────────────────────
juce_add_gui_app(PluginLab
    COMPANY_NAME "PluginLab"
    PRODUCT_NAME "Plugin Lab"
)

juce_generate_juce_header(PluginLab)

# ── Source files ────────────────────────────────────────────────────────────
target_sources(PluginLab PRIVATE
    source/Main.cpp
    source/host/PluginManager.cpp
    source/host/EditorCrashGuard.cpp
    source/host/ChildProcessCoordinator.cpp
    source/host/ChildMeasureOrchestrator.cpp
    source/signal/SignalGenerator.cpp
    source/signal/SineSweep.cpp
    source/signal/MultiTone.cpp
    source/signal/ToneBurst.cpp
    source/signal/Impulse.cpp
    source/signal/FilePlayback.cpp
    source/signal/NoiseGenerator.cpp
    source/signal/EnvelopeSignal.cpp
    source/capture/AudioBuffer.cpp
    source/capture/SweepRunner.cpp
    source/capture/MeasurementSession.cpp
    source/capture/ParameterTimeline.cpp
    source/scan/ScanEngine.cpp
    source/utils/FftHelper.cpp
    source/utils/MathUtils.cpp
    source/utils/CrashLog.cpp
    source/analysis/FreqResponse.cpp
    source/analysis/HarmonicAnalysis.cpp
    source/analysis/CompressionCurve.cpp
    source/analysis/Export.cpp
    source/analysis/GainReduction.cpp
    source/analysis/TimeConstants.cpp
    source/analysis/CompressionFamily.cpp
    source/analysis/WavExporter.cpp
    source/analysis/WavCaptureReader.cpp
    source/analysis/ChildWavAnalyzer.cpp
    source/ipc/PipeServer.cpp
    source/ipc/CommandParser.cpp
    source/ui/PlotWidget.cpp
    source/ui/PluginEditorWindow.cpp
)



target_link_libraries(PluginLab PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_gui_basics
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ── Strict warnings ─────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(PluginLab PRIVATE /W4 /permissive- /WX /utf-8)
endif()

# ── Windows-specific ────────────────────────────────────────────────────────
if(WIN32)
    target_compile_definitions(PluginLab PRIVATE
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
        # SweepRunner yields the message loop during measurement via
        # MessageManager::runDispatchLoopUntil (only declared when this is 1).
        JUCE_MODAL_LOOPS_PERMITTED=1
    )
endif()

# JUCE 9's VST3 headless scanner asserts that scanning happens on the message
# thread (JUCE_ASSERT_MESSAGE_THREAD). Commercial VST3s routinely trigger this
# in Debug builds (and the scan itself is safe on a worker thread — JUCE's own
# AudioPluginHost scans on a ThreadPool). Disable assertions so Debug builds
# behave like Release; this also avoids assertion-driven jassert breaks in the
# debugger while scanning 90+ plugins.
if(MSVC)
    target_compile_definitions(PluginLab PRIVATE JUCE_DISABLE_ASSERTIONS=1)
endif()

# ── Out-of-process VST3 host child (Block D) ────────────────────────────────
# Runs in a separate process so plugin crashes (e.g. Pianoteq calling
# ExitProcess) only kill the child; the parent Plugin Lab survives. Template:
# tools/CMakeLists.txt VST3Scanner console app. Protocol contract:
# docs/plan-block-d-out-of-process.md §子进程 IPC 协议契约.
juce_add_console_app(PluginHostChild
    PRODUCT_NAME "PluginHostChild"
)

juce_generate_juce_header(PluginHostChild)

target_sources(PluginHostChild PRIVATE
    source/child/PluginHostChild.cpp
    source/utils/CrashLog.cpp
    # D2a: measurement pipeline (frozen SweepRunner generate→process→capture
    # + the excitations it needs: SineSweep / MLS Impulse for frequency
    # response, MultiTone for harmonic (T1), ToneBurst for compression (T2)).
    # Dependency closure checked against the headers: SweepRunner.h pulls
    # SignalGenerator.h + AudioBuffer.h; SweepRunner.cpp needs
    # utils/CrashLog.h (already above); SineSweep/Impulse/MultiTone/ToneBurst
    # only need their own headers + <cmath> — no MathUtils / FftHelper in
    # this closure.
    source/capture/SweepRunner.cpp
    source/capture/AudioBuffer.cpp
    source/signal/SignalGenerator.cpp
    source/signal/SineSweep.cpp
    source/signal/Impulse.cpp
    source/signal/MultiTone.cpp
    source/signal/ToneBurst.cpp
)

target_link_libraries(PluginHostChild PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(PluginHostChild PRIVATE
    JUCE_PLUGINHOST_VST3=1
)

# runDispatchLoopUntil (pumped while waiting for createPluginInstanceAsync to
# finish on the message thread) is only declared when this is 1.
if(WIN32)
    target_compile_definitions(PluginHostChild PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1
    )
endif()

if(MSVC)
    target_compile_options(PluginHostChild PRIVATE /W4 /permissive- /WX /utf-8)
endif()

# Copy PluginHostChild.exe next to Plugin Lab.exe after build (D6 release
# layout: the parent's locateChildHostExe() finds it as a sibling first —
# same POST_BUILD pattern as tools/CMakeLists.txt VST3Scanner:35-40).
add_custom_command(TARGET PluginHostChild POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PluginHostChild>"
        "${CMAKE_CURRENT_BINARY_DIR}/PluginLab_artefacts/$<CONFIG>/PluginHostChild.exe"
    COMMENT "Copying PluginHostChild to Plugin Lab directory"
)

# PluginHostChild.cpp touches plugin-DLL code paths (VST3 scan/create) and
# needs /EHa so catch (...) also intercepts SEH faults (same mechanism as
# PluginManager.cpp:106-108).
set_source_files_properties(source/child/PluginHostChild.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# SweepRunner.cpp runs plugin processBlock/prepareToPlay inside the child too
# (D2a measure): /EHa so catch (...) also intercepts SEH faults from a
# misbehaving plugin. The set_source_files_properties call below the PluginLab
# block (line ~169) is directory-scoped and does NOT propagate to this target —
# it must be repeated for PluginHostChild explicitly.
set_source_files_properties(source/capture/SweepRunner.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# ── Tools ───────────────────────────────────────────────────────────────────
add_subdirectory(tools)

# PluginManager.cpp needs /EHa to catch hardware crashes from buggy VST3
set_source_files_properties(source/host/PluginManager.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# EditorCrashGuard.cpp also touches plugin-DLL code paths (editor create/delete)
# and needs /EHa for the same reason.
set_source_files_properties(source/host/EditorCrashGuard.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# SweepRunner.cpp runs plugin processBlock/prepareToPlay on the message thread
# (block C task 1): /EHa so catch (...) also intercepts SEH faults from a
# misbehaving plugin, turning a measurement crash into a failed run.
set_source_files_properties(source/capture/SweepRunner.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# ── Tests ───────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build PluginLab unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
