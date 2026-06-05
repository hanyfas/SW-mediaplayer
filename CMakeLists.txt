cmake_minimum_required(VERSION 3.25)
project(SignagePlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ── Sources ──────────────────────────────────────────────────────────────────
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.h"
)

add_executable(SignagePlayer WIN32 ${SOURCES})

# ── Include paths ─────────────────────────────────────────────────────────────
target_include_directories(SignagePlayer PRIVATE
    src
)

# ── vcpkg / find_package ─────────────────────────────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)

# FFmpeg via vcpkg — uses old-style variables: FFMPEG_LIBRARIES / FFMPEG_INCLUDE_DIRS
find_package(FFMPEG REQUIRED)
message(STATUS "FFmpeg include dirs: ${FFMPEG_INCLUDE_DIRS}")
message(STATUS "FFmpeg libraries:    ${FFMPEG_LIBRARIES}")

target_include_directories(SignagePlayer PRIVATE ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(SignagePlayer PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(SignagePlayer PRIVATE
    nlohmann_json::nlohmann_json

    # FFmpeg (old-style variable from vcpkg's FindFFMPEG.cmake)
    ${FFMPEG_LIBRARIES}

    # Direct3D / DXGI
    d3d11
    d3dcompiler
    dxgi
    dxguid

    # Media Foundation (for MFT colour conversion fallback)
    mf
    mfplat
    mfuuid
    wmcodecdspuuid

    # Windows Imaging Component
    windowscodecs

    # Audio
    winmm

    # Shell / COM / UI
    ole32
    oleaut32
    uuid
    shell32
    user32
    gdi32
    comctl32
    shlwapi
    comdlg32
)

target_compile_definitions(SignagePlayer PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00   # Windows 10
    UNICODE
    _UNICODE
)

target_compile_options(SignagePlayer PRIVATE
    /W4 /WX-
    /MP                   # parallel build
    $<$<CONFIG:Release>:/O2 /Oi /GL /arch:AVX2>
    $<$<CONFIG:Debug>:/Od /Zi /RTC1>
)

target_link_options(SignagePlayer PRIVATE
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

# ── Post-build: copy assets ───────────────────────────────────────────────────
add_custom_command(TARGET SignagePlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:SignagePlayer>/shaders"
    COMMENT "Copying HLSL shaders"
)

add_custom_command(TARGET SignagePlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/playlist.json"
        "$<TARGET_FILE_DIR:SignagePlayer>/playlist.json"
    COMMENT "Copying playlist"
)
