cmake_minimum_required(VERSION 3.10)

project(AudioProject)

set(CMAKE_CXX_STANDARD 17)

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

