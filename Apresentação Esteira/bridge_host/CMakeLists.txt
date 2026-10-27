cmake_minimum_required(VERSION 3.16)
project(Stm32UartBridge VERSION 1.3.0 LANGUAGES CXX)

# Exemplos so por padrao quando este diretorio e o projeto principal
# (consumido via add_subdirectory, o projeto pai decide).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_bridge_examples_default ON)
else()
    set(_bridge_examples_default OFF)
endif()
option(BRIDGE_BUILD_EXAMPLES "Compila os exemplos de terminal da bridge" ${_bridge_examples_default})

find_package(Threads REQUIRED)

add_library(serial_lib STATIC
    src/PortEnumerator.cpp
    src/SerialPort.cpp
    src/AsyncSerialReceiver.cpp
    src/MessageBufferObserver.cpp
    src/SerialClient.cpp
    src/Stm32Bridge.cpp
)
add_library(stm32_bridge::serial_lib ALIAS serial_lib)

target_compile_features(serial_lib PUBLIC cxx_std_17)
target_include_directories(serial_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serial_lib PUBLIC Threads::Threads)

if(WIN32)
    # Backend Win32: COM via CreateFile e enumeracao via SetupAPI
    target_link_libraries(serial_lib PRIVATE setupapi advapi32)
    target_compile_definitions(serial_lib PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
    target_compile_options(serial_lib PRIVATE /W4 /utf-8)
else()
    target_compile_options(serial_lib PRIVATE -Wall -Wextra)
endif()

if(BRIDGE_BUILD_EXAMPLES)
    add_executable(serial_example example/main.cpp)
    target_link_libraries(serial_example PRIVATE serial_lib)

    add_executable(bridge_example example/demo_bridge.cpp)
    target_link_libraries(bridge_example PRIVATE serial_lib)

    if(MSVC)
        target_compile_options(serial_example PRIVATE /utf-8)
        target_compile_options(bridge_example PRIVATE /utf-8)
    endif()
endif()
