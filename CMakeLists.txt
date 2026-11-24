cmake_minimum_required(VERSION 3.28)
project(Cadence VERSION 1.0.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "Cadence targets Windows 11 with MSVC (C++23).")
endif()

# Developer / CI options (all OFF for release builds)
option(CADENCE_ENABLE_ASAN    "Build with MSVC AddressSanitizer (tests and CI only)" OFF)
option(CADENCE_BUILD_FUZZERS  "Build libFuzzer targets fuzz_json / fuzz_config (implies AddressSanitizer)" OFF)
option(CADENCE_ENABLE_ANALYZE "Run MSVC /analyze static analysis (reports warnings, does not fail the build)" OFF)

set(CADENCE_USE_ASAN OFF)
if(CADENCE_ENABLE_ASAN OR CADENCE_BUILD_FUZZERS)
    set(CADENCE_USE_ASAN ON)
endif()

if(CADENCE_USE_ASAN)
    # AddressSanitizer is built around the DLL runtime; instrumented builds are never shipped
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    # Link the MSVC runtime statically: Cadence.exe then needs no vcruntime140.dll / msvcp140.dll,
    # which Windows would otherwise search next to the exe first (e.g. in the Downloads folder).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Common C++23 compiler flags and strict security diagnostics
add_library(cadence_compile_options INTERFACE)
target_compile_features(cadence_compile_options INTERFACE cxx_std_23)
target_compile_definitions(cadence_compile_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    CADENCE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CADENCE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CADENCE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CADENCE_VERSION_STR="${PROJECT_VERSION}"
)
target_compile_options(cadence_compile_options INTERFACE
    /W4
    /WX
    /sdl
    /permissive-
    /Zc:__cplusplus
    /utf-8
    /EHsc
    /GS
    /Qspectre
    $<$<CONFIG:Release>:/O2;/Oi;/Ot;/GL;/Gw;/Gy;/guard:cf;/guard:ehcont>
    $<$<CONFIG:Debug>:/Od;/Zi>
)
target_link_options(cadence_compile_options INTERFACE
    /DYNAMICBASE
    /NXCOMPAT
    /HIGHENTROPYVA
    $<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF;/GUARD:CF;/GUARD:EHCONT>
)

if(CADENCE_USE_ASAN)
    target_compile_options(cadence_compile_options INTERFACE /fsanitize=address)

    # The Visual Studio generator only adds the sanitizer library folder to the linker search path when
    # ASan is enabled through the IDE project setting. With a plain /fsanitize=address flag (as here) the
    # linker reports LNK1104 for clang_rt.asan_*.lib, so locate the MSVC sanitizer files ourselves.
    get_filename_component(CADENCE_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)            # ...\MSVC\<ver>\bin\Hostx64\x64
    get_filename_component(CADENCE_MSVC_ROOT    "${CADENCE_MSVC_BIN_DIR}/../../.." ABSOLUTE)     # ...\MSVC\<ver>
    get_filename_component(CADENCE_VC_ROOT      "${CADENCE_MSVC_ROOT}/../../.." ABSOLUTE)        # ...\VC

    # Finds file_name below the active MSVC toolset (preferred) or anywhere below VC\
    function(cadence_find_msvc_file out_var file_name required)
        foreach(search_root IN ITEMS "${CADENCE_MSVC_BIN_DIR}" "${CADENCE_MSVC_ROOT}" "${CADENCE_VC_ROOT}")
            file(GLOB_RECURSE hits LIST_DIRECTORIES false "${search_root}/${file_name}")
            if(hits)
                list(GET hits 0 first_hit)
                set(${out_var} "${first_hit}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
        if(required)
            message(FATAL_ERROR "${file_name} was not found below ${CADENCE_VC_ROOT}. "
                                "Install the Visual Studio component 'MSVC AddressSanitizer' "
                                "(Microsoft.VisualStudio.Component.VC.ASAN).")
        endif()
        set(${out_var} "" PARENT_SCOPE)
    endfunction()

    set(CADENCE_SANITIZER_LIBS clang_rt.asan_dynamic-x86_64.lib clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
    if(CADENCE_BUILD_FUZZERS)
        list(APPEND CADENCE_SANITIZER_LIBS clang_rt.fuzzer_MD-x86_64.lib)
    endif()
    foreach(sanitizer_lib IN LISTS CADENCE_SANITIZER_LIBS)
        cadence_find_msvc_file(sanitizer_lib_path ${sanitizer_lib} TRUE)
        get_filename_component(sanitizer_lib_dir "${sanitizer_lib_path}" DIRECTORY)
        target_link_directories(cadence_compile_options INTERFACE "${sanitizer_lib_dir}")
        message(STATUS "Sanitizer library ${sanitizer_lib}: ${sanitizer_lib_dir}")
    endforeach()

    # The ASan runtime DLL must sit next to instrumented executables (copied after each build below)
    cadence_find_msvc_file(CADENCE_ASAN_DLL     clang_rt.asan_dynamic-x86_64.dll     TRUE)
    cadence_find_msvc_file(CADENCE_ASAN_DBG_DLL clang_rt.asan_dbg_dynamic-x86_64.dll FALSE)
else()
    target_link_options(cadence_compile_options INTERFACE
        /CETCOMPAT
        # Resolve static imports from System32 only (LOAD_LIBRARY_SEARCH_SYSTEM32), never from the exe folder.
        # Not used with ASan: its runtime DLL lives next to the test executables.
        /DEPENDENTLOADFLAG:0x800
    )
endif()

if(CADENCE_ENABLE_ANALYZE)
    # Code analysis findings are reported in the build log but never fail the build
    target_compile_options(cadence_compile_options INTERFACE /analyze /analyze:WX-)
endif()

# ---------------------------------------------------------------------------
# Core Engine Library (100% UI-independent)
# ---------------------------------------------------------------------------
add_library(cadence_core STATIC
    src/core/Types.hpp
    src/core/Units.hpp
    src/core/StringUtil.hpp
    src/core/StringUtil.cpp
    src/core/JsonParser.hpp
    src/core/JsonParser.cpp
    src/core/TimingEngine.hpp
    src/core/TimingEngine.cpp
    src/core/MouseInputEngine.hpp
    src/core/MouseInputEngine.cpp
    src/core/KeyboardInputEngine.hpp
    src/core/KeyboardInputEngine.cpp
    src/core/PerformanceMonitor.hpp
    src/core/PerformanceMonitor.cpp
    src/core/HotkeyManager.hpp
    src/core/HotkeyManager.cpp
    src/core/ConfigManager.hpp
    src/core/ConfigManager.cpp
    src/core/ProfileManager.hpp
    src/core/ProfileManager.cpp
    src/core/UpdateChecker.hpp
    src/core/UpdateChecker.cpp
    src/core/EngineController.hpp
    src/core/EngineController.cpp
)
target_include_directories(cadence_core PUBLIC src)
target_link_libraries(cadence_core PUBLIC
    cadence_compile_options
    user32
    kernel32
    avrt
    psapi
    shell32
    advapi32
    bcrypt
    winhttp
)

# ---------------------------------------------------------------------------
# Main Windows 11 Native Application (Pure C++23 + Direct2D + DirectWrite)
# ---------------------------------------------------------------------------
add_executable(Cadence WIN32
    src/main.cpp
    src/ui/Theme.hpp
    src/ui/UiHelpers.hpp
    src/ui/D2DRenderer.hpp
    src/ui/D2DRenderer.cpp
    src/ui/MainWindow.hpp
    src/ui/MainWindow.cpp
    src/ui/MainWindowRender.cpp
    src/ui/MainWindowControls.cpp
    src/ui/MainWindowWidgets.cpp
    src/ui/MainWindowProfiles.cpp
    src/ui/MainWindowUpdate.cpp
    src/ui/MainWindowInput.cpp
    resources/app.manifest
    resources/app.rc
)
target_link_libraries(Cadence PRIVATE
    cadence_core
    d2d1
    dwrite
    dwmapi
    gdi32
    comdlg32
    ole32
)
set_target_properties(Cadence PROPERTIES
    OUTPUT_NAME "Cadence"
    WIN32_EXECUTABLE TRUE
)
# DLLs that are not Windows KnownDLLs are loaded on first use, i.e. after wWinMain has called
# SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32). delayimp ships with MSVC (no third-party code).
target_link_options(Cadence PRIVATE
    /DELAYLOAD:d2d1.dll
    /DELAYLOAD:dwrite.dll
    /DELAYLOAD:dwmapi.dll
    /DELAYLOAD:winhttp.dll
    /DELAYLOAD:bcrypt.dll
    /DELAYLOAD:avrt.dll
)
target_link_libraries(Cadence PRIVATE delayimp)

# ---------------------------------------------------------------------------
# Timing & Performance Benchmark Executable
# ---------------------------------------------------------------------------
add_executable(bench_timing
    tests/bench_timing.cpp
)
target_link_libraries(bench_timing PRIVATE cadence_core)

# ---------------------------------------------------------------------------
# Unit & Integration Tests (self-contained runner, one CTest entry per test case)
# ---------------------------------------------------------------------------
add_executable(test_core
    tests/TestHarness.hpp
    tests/test_core.cpp
)
target_link_libraries(test_core PRIVATE cadence_core)

enable_testing()
set(CADENCE_TEST_CASES
    ConfigRoundTrip
    ConfigParserHardening
    ConfigSequenceAndCoordsCapped
    ConfigLoadErrorsAndBackup
    ConfigVersioning
    ProfileActivationIsolation
    ProfileImportPreviewAndAdd
    TimingUnitConversions
    TimingPrecisionAndJitter
    PerformanceMonitorRates
    StringUtilRoundTrip
    UpdateSemVer
    UpdateReleaseJson
    UpdateSha256AndPeChecks
    UpdateErrorReasons
)
foreach(test_case IN LISTS CADENCE_TEST_CASES)
    add_test(NAME ${test_case} COMMAND test_core ${test_case})
endforeach()
# Fails when a test exists in test_core.cpp but is missing from the list above (or the other way round)
list(JOIN CADENCE_TEST_CASES "," CADENCE_TEST_CASES_CSV)
add_test(NAME TestListInSync COMMAND test_core --expect-tests=${CADENCE_TEST_CASES_CSV})

# ---------------------------------------------------------------------------
# Fuzzing (libFuzzer, built into MSVC)
# ---------------------------------------------------------------------------
if(CADENCE_BUILD_FUZZERS)
    add_executable(fuzz_json
        tests/fuzz_json.cpp
        src/core/JsonParser.cpp
    )
    add_executable(fuzz_config
        tests/fuzz_config.cpp
        src/core/JsonParser.cpp
        src/core/StringUtil.cpp
        src/core/ConfigManager.cpp
        src/core/UpdateChecker.cpp
    )
    foreach(fuzzer IN ITEMS fuzz_json fuzz_config)
        target_include_directories(${fuzzer} PRIVATE src)
        target_link_libraries(${fuzzer} PRIVATE cadence_compile_options shell32 ole32 bcrypt winhttp)
        target_compile_options(${fuzzer} PRIVATE /fsanitize=fuzzer)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer runtime next to instrumented executables (so ctest / fuzz runs need no PATH setup)
# ---------------------------------------------------------------------------
if(CADENCE_USE_ASAN)
    set(CADENCE_ASAN_TARGETS test_core bench_timing Cadence)
    if(CADENCE_BUILD_FUZZERS)
        list(APPEND CADENCE_ASAN_TARGETS fuzz_json fuzz_config)
    endif()
    foreach(asan_dll IN ITEMS "${CADENCE_ASAN_DLL}" "${CADENCE_ASAN_DBG_DLL}")
        if(asan_dll)
            foreach(asan_target IN LISTS CADENCE_ASAN_TARGETS)
                add_custom_command(TARGET ${asan_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${asan_dll}" "$<TARGET_FILE_DIR:${asan_target}>"
                    VERBATIM)
            endforeach()
        endif()
    endforeach()
endif()
