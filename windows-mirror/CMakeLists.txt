cmake_minimum_required(VERSION 3.21)

project(iphone_valeria_mirror LANGUAGES CXX)

option(VALERIA_BUILD_TESTS "Build protocol/parser tests" ON)
option(VALERIA_BUILD_ACTIVATOR "Build the Apple MI_01 activation diagnostic" ON)

if(VALERIA_BUILD_ACTIVATOR)
    add_subdirectory(activation)
endif()

add_library(valeria_core STATIC
    src/BinaryDictionary.cpp
    src/Clock.cpp
    src/CoreMedia.cpp
    src/PacketFramer.cpp
    src/QuickTimeSession.cpp
)

target_include_directories(valeria_core PUBLIC include)
target_compile_features(valeria_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(valeria_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(valeria_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_library(valeria_winusb STATIC src/WinUsbTransport.cpp)
    target_include_directories(valeria_winusb PUBLIC include)
    target_compile_features(valeria_winusb PUBLIC cxx_std_17)
    target_compile_definitions(valeria_winusb PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(valeria_winusb PUBLIC valeria_core setupapi winusb)

    if(MSVC)
        target_compile_options(valeria_winusb PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(valeria_winusb PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(iphone-valeria-mirror src/main.cpp)
    target_compile_features(iphone-valeria-mirror PRIVATE cxx_std_17)
    target_compile_definitions(iphone-valeria-mirror PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(iphone-valeria-mirror PRIVATE valeria_core valeria_winusb)
    if(MSVC)
        target_compile_options(iphone-valeria-mirror PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(iphone-valeria-mirror PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(VALERIA_BUILD_TESTS)
    enable_testing()
    add_executable(valeria_core_tests tests/core_tests.cpp)
    target_compile_features(valeria_core_tests PRIVATE cxx_std_17)
    target_link_libraries(valeria_core_tests PRIVATE valeria_core)
    target_compile_definitions(valeria_core_tests PRIVATE
        VALERIA_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/quicktime_video_hack"
    )
    add_test(NAME valeria_core_tests COMMAND valeria_core_tests)
endif()
