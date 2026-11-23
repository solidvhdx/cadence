cmake_minimum_required(VERSION 3.28)
project(Cadence VERSION 1.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "Cadence targets Windows 11 with MSVC (C++23).")
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
    /CETCOMPAT
    $<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF;/GUARD:CF;/GUARD:EHCONT>
)

# ---------------------------------------------------------------------------
# Core Engine Library (100% UI-independent)
# ---------------------------------------------------------------------------
add_library(cadence_core STATIC
    src/core/Types.hpp
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
    src/ui/D2DRenderer.hpp
    src/ui/D2DRenderer.cpp
    src/ui/MainWindow.hpp
    src/ui/MainWindow.cpp
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

# ---------------------------------------------------------------------------
# Timing & Performance Benchmark Executable
# ---------------------------------------------------------------------------
add_executable(bench_timing
    tests/bench_timing.cpp
)
target_link_libraries(bench_timing PRIVATE cadence_core)

# ---------------------------------------------------------------------------
# Unit & Integration Test Executable
# ---------------------------------------------------------------------------
add_executable(test_core
    tests/test_core.cpp
)
target_link_libraries(test_core PRIVATE cadence_core)

enable_testing()
add_test(NAME CadenceCoreTests COMMAND test_core)
