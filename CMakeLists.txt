cmake_minimum_required(VERSION 3.20)
project(OpenAES67 VERSION 0.1.1 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "OpenAES67 targets Windows (WASAPI + KS/PortCls).")
endif()

# /W4: the tree is warning-clean at this level (the batch build has used it since
# the warning audit). /utf-8: sources are UTF-8; without it MSVC reads them as the
# local ANSI code page and warns C4819 on any non-ASCII comment or UI string.
if(MSVC)
    add_compile_options(/W4 /utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ---- optional ASIO backend -------------------------------------------------
# The Steinberg ASIO SDK is not redistributable, so it is not bundled. Put the
# official headers in third_party/asio (see docs/AUDIO-DEVICES.md) and configure
# with -DOPENAES67_WITH_ASIO=ON. Without it, the backend compiles to stubs that
# explain why it is missing, and ASIO drivers are still listed by list-devices.
option(OPENAES67_WITH_ASIO "Build the ASIO backend (needs the Steinberg ASIO SDK)" OFF)
set(OPENAES67_ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio"
    CACHE PATH "Directory containing iasiodrv.h and asiosys.h")
if(OPENAES67_WITH_ASIO)
    if(NOT EXISTS "${OPENAES67_ASIO_SDK_DIR}/iasiodrv.h")
        message(FATAL_ERROR
            "OPENAES67_WITH_ASIO=ON but iasiodrv.h was not found in "
            "${OPENAES67_ASIO_SDK_DIR}. Download the ASIO SDK from Steinberg, "
            "copy common/iasiodrv.h, common/asiosys.h and common/asio.h into "
            "that directory, then reconfigure.")
    endif()
    message(STATUS "ASIO backend enabled (SDK: ${OPENAES67_ASIO_SDK_DIR})")
endif()

# ---- shared core: protocol + engine + audio backends -----------------------
# Globbed on purpose. A hand-maintained list is silent when it is wrong: adding
# src/audio_devices.cpp to build.bat but forgetting it here produced four
# unresolved-symbol errors that pointed at perfectly good code. CONFIGURE_DEPENDS
# re-runs CMake when files are added or removed.
file(GLOB OPENAES67_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM OPENAES67_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
list(LENGTH OPENAES67_CORE_SOURCES OPENAES67_CORE_COUNT)
message(STATUS "core sources: ${OPENAES67_CORE_COUNT} files")

add_library(openaes67_core STATIC ${OPENAES67_CORE_SOURCES})
target_compile_definitions(openaes67_core PUBLIC OPENAES67_VERSION="${PROJECT_VERSION}")

target_include_directories(openaes67_core PUBLIC include)
if(OPENAES67_WITH_ASIO)
    target_include_directories(openaes67_core PUBLIC "${OPENAES67_ASIO_SDK_DIR}")
    target_compile_definitions(openaes67_core PUBLIC OPENAES67_WITH_ASIO)
endif()
target_compile_definitions(openaes67_core PUBLIC
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(openaes67_core PUBLIC ws2_32 advapi32 ole32 uuid iphlpapi winmm shell32)

# ---- protocol self-test (no audio hardware, no network) --------------------
# The resource script includes include/openaes67/version.h for the version numbers,
# so rc.exe needs the same include path the C++ compiler gets.
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I${CMAKE_CURRENT_SOURCE_DIR}/include")

add_executable(openaes67_selftest tests/selftest.cpp assets/app.rc)
target_link_libraries(openaes67_selftest PRIVATE openaes67_core)

# ---- CLI -------------------------------------------------------------------
add_executable(openaes67 src/main.cpp assets/app.rc)
target_link_libraries(openaes67 PRIVATE openaes67_core)

# ---- optional Qt6 GUI ------------------------------------------------------
# Point CMAKE_PREFIX_PATH at the Qt kit, e.g.
#   cmake -S . -B build-qt -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
find_package(Qt6 COMPONENTS Widgets Network QUIET)
if(Qt6_FOUND)
    message(STATUS "Qt6 found: ${Qt6_VERSION} - building openaes67_gui")
    add_executable(openaes67_gui WIN32 src/gui/main_gui.cpp assets/app.rc assets/app.qrc)
    target_link_libraries(openaes67_gui PRIVATE openaes67_core Qt6::Widgets Qt6::Network)
else()
    message(STATUS "Qt6 not found - GUI target skipped (CLI and selftest still build)")
endif()
