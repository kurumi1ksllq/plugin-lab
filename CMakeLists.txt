cmake_minimum_required(VERSION 3.24)
project(PluginLab VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(MSVC)
    add_compile_options(/EHsc)
endif()

# ── Fetch JUCE ──────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG master
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
    source/signal/SignalGenerator.cpp
    source/signal/SineSweep.cpp
    source/signal/MultiTone.cpp
    source/signal/ToneBurst.cpp
    source/signal/Impulse.cpp
    source/capture/AudioBuffer.cpp
    source/capture/SweepRunner.cpp
    source/capture/MeasurementSession.cpp
    source/utils/FftHelper.cpp
    source/utils/MathUtils.cpp
    source/utils/CrashLog.cpp
    source/analysis/FreqResponse.cpp
    source/analysis/HarmonicAnalysis.cpp
    source/analysis/CompressionCurve.cpp
    source/analysis/Export.cpp
    source/ipc/PipeServer.cpp
    source/ipc/CommandParser.cpp
    source/ui/PlotWidget.cpp
    source/ui/PluginEditorWindow.cpp
)



target_link_libraries(PluginLab PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
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
    )
endif()

# ── Tools ───────────────────────────────────────────────────────────────────
add_subdirectory(tools)

# PluginManager.cpp needs /EHa to catch hardware crashes from buggy VST3
set_source_files_properties(source/host/PluginManager.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# ── Tests ───────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build PluginLab unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
