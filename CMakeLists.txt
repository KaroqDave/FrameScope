cmake_minimum_required(VERSION 3.25)

project(FrameScope
    VERSION 0.1.0
    DESCRIPTION "Stutter diagnosis: system vs. game"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FRAMESCOPE_BUILD_GUI   "Build the Qt GUI"            ON)
option(FRAMESCOPE_BUILD_TESTS "Build the unit tests"        ON)
option(FRAMESCOPE_BUILD_CLI   "Build the command-line tool" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Core: pure C++23, no Qt. Parsing, spike detection, statistics.
# ---------------------------------------------------------------------------
add_library(framescope_core STATIC
    src/core/FrameRecord.h
    src/core/PresentMonCsvParser.h
    src/core/PresentMonCsvParser.cpp
    src/core/SpikeDetector.h
    src/core/SpikeDetector.cpp
    src/core/SessionStats.h
    src/core/SessionStats.cpp
    src/core/CaptureSession.h
    src/core/CaptureSession.cpp
)
target_include_directories(framescope_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# CLI: analyse an existing PresentMon CSV offline.
# ---------------------------------------------------------------------------
if(FRAMESCOPE_BUILD_CLI)
    add_executable(framescope-cli src/cli/main.cpp)
    target_link_libraries(framescope-cli PRIVATE framescope_core)
endif()

# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
if(FRAMESCOPE_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
    qt_standard_project_setup()

    set(FRAMESCOPE_GUI_SOURCES
        src/main.cpp
        src/capture/PresentMonCapture.h
        src/capture/PresentMonCapture.cpp
        src/ui/MainWindow.h
        src/ui/MainWindow.cpp
        src/ui/FrameTimeView.h
        src/ui/FrameTimeView.cpp
    )

    # PresentMon (and the ETW kernel session in M2) needs admin rights,
    # so the Windows build asks for elevation via its manifest.
    if(WIN32)
        if(MSVC)
            list(APPEND FRAMESCOPE_GUI_SOURCES resources/windows/framescope.manifest)
        else()
            enable_language(RC)
            set(FRAMESCOPE_MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/framescope.manifest)
            configure_file(resources/windows/framescope.rc.in
                           ${CMAKE_CURRENT_BINARY_DIR}/framescope.rc @ONLY)
            list(APPEND FRAMESCOPE_GUI_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/framescope.rc)
        endif()
    endif()

    qt_add_executable(framescope WIN32 ${FRAMESCOPE_GUI_SOURCES})
    target_link_libraries(framescope PRIVATE framescope_core Qt6::Widgets)
endif()

# ---------------------------------------------------------------------------
# Tests (no Qt needed)
# ---------------------------------------------------------------------------
if(FRAMESCOPE_BUILD_TESTS)
    enable_testing()
    add_executable(framescope_tests tests/test_main.cpp)
    target_link_libraries(framescope_tests PRIVATE framescope_core)
    add_test(NAME framescope_tests COMMAND framescope_tests)
endif()
