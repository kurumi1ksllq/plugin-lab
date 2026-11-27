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
    source/signal/SequentialTone.cpp
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
    source/analysis/HarmonicFamily.cpp
    source/analysis/WavExporter.cpp
    source/analysis/WavCaptureReader.cpp
    source/analysis/ChildWavAnalyzer.cpp
    source/analysis/MeasurementAnalysis.cpp
    source/analysis/StereoAnalysis.cpp
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
    # response, SequentialTone for harmonic (T1), ToneBurst for compression
    # (T2), EnvelopeSignal for gr_timeline).
    # Dependency closure checked against the headers: SweepRunner.h pulls
    # SignalGenerator.h + AudioBuffer.h; SweepRunner.cpp needs
    # utils/CrashLog.h (already above); SineSweep/Impulse/SequentialTone/
    # ToneBurst/EnvelopeSignal only need their own headers + <cmath> — no
    # MathUtils / FftHelper in this closure.
    source/capture/SweepRunner.cpp
    source/capture/AudioBuffer.cpp
    source/signal/SignalGenerator.cpp
    source/signal/SineSweep.cpp
    source/signal/Impulse.cpp
    source/signal/MultiTone.cpp
    source/signal/SequentialTone.cpp
    source/signal/ToneBurst.cpp
    source/signal/EnvelopeSignal.cpp
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
# VST3Scanner is runtime-dead (scanning happens in-process) — kept as a
# reference build, so it is gated off by default (issue #44).
option(BUILD_VST3SCANNER "Build the standalone VST3Scanner reference tool" OFF)
if(BUILD_VST3SCANNER)
    add_subdirectory(tools)
endif()

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

# ── Build-time guards (issue #118) ──────────────────────────────────────────
# These checks fail the CONFIGURE step loudly when the crash-protection wiring
# above is silently lost (e.g. a refactor drops a set_source_files_properties
# block or a translation unit from one of the dual-compile manifests).
# Guard 1 is MSVC-only (/EHa is an MSVC flag); Guard 2 is platform-independent
# (the dual-compile contract is about which files are compiled, not how).

# Guard 1: the /EHa crash-protection blocks must all still be present.
# /EHa is wired via per-source COMPILE_FLAGS on 5 call sites in this file
# (PluginManager.cpp / EditorCrashGuard.cpp / SweepRunner.cpp for the PluginLab
# target, PluginHostChild.cpp / SweepRunner.cpp again for PluginHostChild — the
# property is directory-scoped, so SweepRunner.cpp must be repeated per target)
# and 3 sites in tests/CMakeLists.txt (SweepRunner.cpp / EditorCrashGuard.cpp /
# ChildHostParityTests.cpp). The guard collects (file, COMPILE_FLAGS "/EHa")
# pairs from uncommented blocks and compares the resulting file list against
# the authorized table above, multiplicity included.
if(MSVC)
    # A bare call count is not enough: /EHa could be swapped for /EHsc, a block
    # could be commented out, or the source file could change — all silently.
    # So assert BOTH the flag text and the authorized file list.
    set(EHA_ROOT_AUTHORIZED
        source/child/PluginHostChild.cpp
        source/capture/SweepRunner.cpp
        source/host/PluginManager.cpp
        source/host/EditorCrashGuard.cpp
        source/capture/SweepRunner.cpp)
    set(EHA_TESTS_AUTHORIZED
        ../source/capture/SweepRunner.cpp
        ../source/host/EditorCrashGuard.cpp
        ChildHostParityTests.cpp)

    # Collect every (file, flag) pair from a real, uncommented call block:
    # "set_source_files_properties(<file> PROPERTIES" on one line, then the
    # COMPILE_FLAGS line that follows. Commented-out blocks start with "#"
    # and are dropped by the regex anchor.
    function(pluginlab_collect_eha_blocks list_file out_pairs)
        file(STRINGS "${list_file}" _calls
             REGEX "^[ \t]*set_source_files_properties\\([^)]*PROPERTIES")
        file(STRINGS "${list_file}" _flags
             REGEX "^[ \t]*COMPILE_FLAGS[ \t]+\"/EHa\"")
        set(_pairs "")
        foreach(_call IN LISTS _calls)
            string(REGEX REPLACE
                   "^[ \t]*set_source_files_properties\\(([^)]*) PROPERTIES.*$"
                   "\\1" _file "${_call}")
            string(STRIP "${_file}" _file)
            list(APPEND _pairs "${_file}")
        endforeach()
        list(LENGTH _calls _n_calls)
        list(LENGTH _flags _n_flags)
        set(${out_pairs} "${_pairs}" PARENT_SCOPE)
        set(${out_pairs}_CALLS ${_n_calls} PARENT_SCOPE)
        set(${out_pairs}_FLAGS ${_n_flags} PARENT_SCOPE)
    endfunction()

    pluginlab_collect_eha_blocks("${CMAKE_CURRENT_LIST_FILE}" ROOT_EHA)
    if(NOT ROOT_EHA_FLAGS EQUAL ROOT_EHA_CALLS)
        message(FATAL_ERROR
            "[EHa guard] Root CMakeLists.txt: ${ROOT_EHA_CALLS} "
            "set_source_files_properties(...PROPERTIES) blocks but "
            "${ROOT_EHA_FLAGS} COMPILE_FLAGS \"/EHa\" lines. A /EHa flag was "
            "changed or dropped — crash protection (/EHa + catch(...) + "
            "CRASH_LOG) would be silently lost. Restore it.")
    endif()
    # Multiset comparison, not set membership: SweepRunner.cpp must appear
    # TWICE in the root file (the property is directory-scoped, so it has to be
    # repeated for the PluginHostChild target). A list(FIND) subset check would
    # pass with one of the two blocks deleted, silently dropping SEH
    # protection from the child process. Sorted copies compare both the
    # membership and the multiplicity; an extra unauthorized TU also fails.
    set(_root_want ${EHA_ROOT_AUTHORIZED})
    set(_root_have ${ROOT_EHA})
    list(SORT _root_want)
    list(SORT _root_have)
    if(NOT "${_root_want}" STREQUAL "${_root_have}")
        message(FATAL_ERROR
            "[EHa guard] Root CMakeLists.txt: authorized /EHa files are"
            " [${_root_want}] but found [${_root_have}]. A block was removed,"
            " duplicated or added for an unauthorized translation unit — crash"
            " protection (/EHa + catch(...) + CRASH_LOG) would be silently lost."
            " Note SweepRunner.cpp must appear once per target (PluginLab and"
            " PluginHostChild).")
    endif()

    pluginlab_collect_eha_blocks("${CMAKE_CURRENT_LIST_DIR}/tests/CMakeLists.txt"
                                 TESTS_EHA)
    if(NOT TESTS_EHA_FLAGS EQUAL TESTS_EHA_CALLS)
        message(FATAL_ERROR
            "[EHa guard] tests/CMakeLists.txt: ${TESTS_EHA_CALLS} blocks but "
            "${TESTS_EHA_FLAGS} COMPILE_FLAGS \"/EHa\" lines. Restore them.")
    endif()
    set(_tests_want ${EHA_TESTS_AUTHORIZED})
    set(_tests_have ${TESTS_EHA})
    list(SORT _tests_want)
    list(SORT _tests_have)
    if(NOT "${_tests_want}" STREQUAL "${_tests_have}")
        message(FATAL_ERROR
            "[EHa guard] tests/CMakeLists.txt: authorized /EHa files are"
            " [${_tests_want}] but found [${_tests_have}]. Restore the missing"
            " block(s).")
    endif()
endif()

# Guard 2: dual-compile manifest consistency for the key modules.
# With no static library every production .cpp must be listed in BOTH the
# PluginLab target_sources (root) and the unit_tests target_sources
# (tests/CMakeLists.txt); missing one side silently changes the compile
# context (flags, defines) between the app and the tests.
#
# Matching only inside the unit_tests target_sources block, with comment lines
# removed and the path escaped: searching the whole file would also hit the
# /EHa property blocks and commented-out entries, which is how the first
# version of this guard passed while a real manifest entry was deleted.
set(GUARD_MODULES capture analysis ipc signal scan host)
# Exclusions and why: child/ and ui/ are compiled into their own targets with
# no unit_tests counterpart by design; utils/ is excluded because the tests
# stub CrashLog via CommandParserStubs.cpp (tests/AGENTS.md).
set(GUARD_EXCLUDED_DIRS child ui utils)

# Slice tests/CMakeLists.txt down to the unit_tests target_sources block.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/tests/CMakeLists.txt" _tests_lines)
set(_in_block FALSE)
set(_tests_manifest "")
foreach(_line IN LISTS _tests_lines)
    if(_line MATCHES "^[ \t]*target_sources\\(unit_tests")
        set(_in_block TRUE)
        continue()
    endif()
    if(_in_block)
        # The closing paren usually shares a line with the last file
        # ("ChildMeasureOrchestratorTests.cpp)"), so a line-anchored "\)"
        # never fires and the slice would swallow the rest of the file —
        # including the /EHa property blocks, which is how a deleted manifest
        # entry still matched. Accept a trailing paren anywhere on the line.
        if(_line MATCHES "\\)[ \t]*$")
            set(_in_block FALSE)
            continue()
        endif()
        # Drop comment lines so a commented-out entry does not count as listed.
        if(NOT _line MATCHES "^[ \t]*#")
            string(APPEND _tests_manifest "${_line} ")
        endif()
    endif()
endforeach()
if(_tests_manifest STREQUAL "")
    message(FATAL_ERROR
        "[Dual-compile guard] Could not locate target_sources(unit_tests ...) in"
        " tests/CMakeLists.txt — the guard cannot verify the dual-compile"
        " contract. Fix the guard rather than removing it.")
endif()
# Self-check: if the slice ran past the block it would contain the /EHa
# property calls below it, and every path would match itself. Refuse to
# report success on a slice that swallowed them.
if(_tests_manifest MATCHES "set_source_files_properties")
    message(FATAL_ERROR
        "[Dual-compile guard] The target_sources(unit_tests ...) slice contains"
        " set_source_files_properties lines — the slice ran past the closing"
        " paren and the guard would pass on any path. Fix the guard.")
endif()

# Same slice for the root PluginLab target_sources, to check the other direction.
file(STRINGS "${CMAKE_CURRENT_LIST_FILE}" _root_lines)
set(_in_block FALSE)
set(_root_manifest "")
foreach(_line IN LISTS _root_lines)
    if(_line MATCHES "^[ \t]*target_sources\\(PluginLab")
        set(_in_block TRUE)
        continue()
    endif()
    if(_in_block)
        if(_line MATCHES "\\)[ \t]*$")
            set(_in_block FALSE)
            continue()
        endif()
        if(NOT _line MATCHES "^[ \t]*#")
            string(APPEND _root_manifest "${_line} ")
        endif()
    endif()
endforeach()

set(DUAL_COMPILE_MISSING "")
foreach(_mod IN LISTS GUARD_MODULES)
    # CONFIGURE_DEPENDS: a newly added .cpp must re-run configure, otherwise
    # the "new file missing from a manifest" case is never checked locally.
    file(GLOB _mod_cpps CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/source/${_mod}/*.cpp")
    foreach(_cpp IN LISTS _mod_cpps)
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_cpp}")
        file(TO_CMAKE_PATH "${_rel}" _rel)
        # Escape the path before using it as a regex (dots and slashes).
        string(REPLACE "." "\\." _rel_re "${_rel}")
        string(REPLACE "/" "\\/" _rel_re "${_rel_re}")
        if(NOT _root_manifest MATCHES "${_rel_re}")
            list(APPEND DUAL_COMPILE_MISSING "${_rel} (missing from PluginLab)")
        endif()
        if(NOT _tests_manifest MATCHES "${_rel_re}")
            list(APPEND DUAL_COMPILE_MISSING "${_rel} (missing from unit_tests)")
        endif()
    endforeach()
    unset(_mod_cpps)
endforeach()
if(DUAL_COMPILE_MISSING)
    message(FATAL_ERROR
        "[Dual-compile guard] Dual-compile contract violated (AGENTS.md"
        " CONVENTIONS): ${DUAL_COMPILE_MISSING}. Every production .cpp in"
        " ${GUARD_MODULES} must be listed in BOTH the PluginLab and unit_tests"
        " target_sources. Excluded by design: ${GUARD_EXCLUDED_DIRS}.")
endif()


# ── Tests ───────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build PluginLab unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
