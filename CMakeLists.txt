cmake_minimum_required(VERSION 3.10)

project(AudioProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    include
)


add_executable(
    audio_project

    src/audio_gain.cpp
    src/audio_filter.cpp
    src/read_wav_file.cpp
    src/write_wav_file.cpp
)

target_compile_definitions(audio_project PRIVATE
    DEFAULT_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
    DEFAULT_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

    add_executable(realtime_audio
        src/realtime_audio.cpp
    )
    target_link_libraries(realtime_audio PRIVATE PkgConfig::ALSA)
    target_compile_options(realtime_audio PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(STATUS "realtime_audio is only built on Linux (ALSA required)")
endif()

