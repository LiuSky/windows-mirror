cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(valeria_activate LANGUAGES CXX)
endif()

add_executable(valeria-activate
    src/main.cpp
)

target_compile_features(valeria-activate PRIVATE cxx_std_17)

if(WIN32)
    target_compile_definitions(valeria-activate PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(valeria-activate PRIVATE
        setupapi
        winusb
    )
    if(MINGW)
        # wmain is used so serial/instance selectors remain Unicode on Windows.
        target_link_options(valeria-activate PRIVATE -municode)
    endif()
endif()

if(MSVC)
    target_compile_options(valeria-activate PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(valeria-activate PRIVATE -Wall -Wextra -Wpedantic)
endif()
