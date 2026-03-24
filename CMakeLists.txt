cmake_minimum_required(VERSION 3.16)
project(432Hz_BitPerfect CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------
# RtAudio path — override with -DRTAUDIO_DIR=<path> if needed.
# Default: C:/libs/rtaudio (where the repo was cloned)
# --------------------------------------------------------------------------
if(NOT DEFINED RTAUDIO_DIR)
    set(RTAUDIO_DIR "C:/libs/rtaudio")
endif()

# --------------------------------------------------------------------------
# Dear ImGui path — override with -DIMGUI_DIR=<path> if needed.
# Default: C:/libs/imgui  (clone https://github.com/ocornut/imgui)
# Required branch/tag: docking  (or any recent version >= 1.89)
# --------------------------------------------------------------------------
if(NOT DEFINED IMGUI_DIR)
    set(IMGUI_DIR "C:/libs/imgui")
endif()

# RtAudio core + bundled ASIO sources (all under RTAUDIO_DIR/include)
set(RTAUDIO_SOURCES
    ${RTAUDIO_DIR}/RtAudio.cpp
    ${RTAUDIO_DIR}/include/asio.cpp
    ${RTAUDIO_DIR}/include/asiodrivers.cpp
    ${RTAUDIO_DIR}/include/asiolist.cpp
    ${RTAUDIO_DIR}/include/iasiothiscallresolver.cpp
)

# Dear ImGui core + Win32/DX11 backends
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(432hz_player
    src/main.cpp
    src/lut.cpp
    ${RTAUDIO_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(432hz_player PRIVATE
    src
    ${RTAUDIO_DIR}
    ${RTAUDIO_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(432hz_player PRIVATE
    __WINDOWS_ASIO__          # Enable RtAudio ASIO backend
    __WINDOWS_WASAPI__        # Enable RtAudio WASAPI backend (bit-perfect exclusive mode)
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    # Note: WIN32_LEAN_AND_MEAN is intentionally omitted —
    # the ASIO headers and RtAudio's CoInitialize need full Windows COM headers.
)

# Link against needed Windows libs (audio + UI + shell)
target_link_libraries(432hz_player PRIVATE
    ole32
    winmm
    ksuser
    mfplat
    mfuuid
    wmcodecdspuuid
    d3d11
    dxgi
    d3dcompiler
    shell32
    shlwapi
    dwmapi
)

# High-performance release flags
if(MSVC)
    target_compile_options(432hz_player PRIVATE
        /O2 /Oi /GL /arch:AVX2 /fp:precise /W3
    )
    target_link_options(432hz_player PRIVATE /LTCG /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup)
else()
    target_compile_options(432hz_player PRIVATE
        -O3 -march=native -ffast-math
    )
endif()
