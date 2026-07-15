# color-mixer-batch — standalone FS vs Prusa color-mix reverse-match CLI.
#
# No external dependencies beyond the C++17 standard library. The two vendored
# algorithm files (filament_mixer.cpp + prusa_fdm_mixer.cpp) are pulled in
# directly — they do not link against libslic3r or anything else.
cmake_minimum_required(VERSION 3.15)
project(color_mixer_batch CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(color_match_batch
    src/main.cpp
    src/match_search.cpp
    src/color_io.cpp
    vendor/filament_mixer.cpp
    vendor/prusa_fdm_mixer.cpp
)

target_include_directories(color_match_batch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

if(MSVC)
    target_compile_options(color_match_batch PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(color_match_batch PRIVATE -Wall -Wextra -Wpedantic)
endif()
