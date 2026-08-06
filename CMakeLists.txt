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
    find_package(Threads REQUIRED)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

    add_executable(realtime_audio
        src/realtime_audio.cpp
        src/audio_config.cpp
        src/command_interface.cpp
        src/deepfilter_processor.cpp
        src/pcm_device.cpp
        src/realtime_processor.cpp
        src/web_control_server.cpp
    )
    target_link_libraries(realtime_audio PRIVATE PkgConfig::ALSA Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(realtime_audio PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(deepfilter_benchmark
        src/deepfilter_benchmark.cpp
        src/read_wav_file.cpp
        src/write_wav_file.cpp
    )
    target_link_libraries(deepfilter_benchmark PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(deepfilter_benchmark PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(STATUS "realtime_audio is only built on Linux (ALSA required)")
endif()

