cmake_minimum_required(VERSION 3.21)
project(VisionTest VERSION 2.3.0 LANGUAGES CXX)

# ------------------------------------------------------------------------------
# Dependencias
# ------------------------------------------------------------------------------

# OpenCV: no Windows aponte a variavel de ambiente OpenCV_DIR para a pasta que
# contem OpenCVConfig.cmake (ex: C:/opencv/build).
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui dnn)
find_package(Threads REQUIRED)

# Galaxy SDK (Daheng). Caminho padrao do instalador de cada sistema; sobrescreva
# com -DGALAXY_SDK_DIR=... se instalou em outro lugar.
if(WIN32)
    set(_galaxy_default "C:/Program Files/Daheng Imaging/GalaxySDK/Development/VC SDK")
else()
    set(_galaxy_default "/opt/Galaxy_camera")
endif()
set(GALAXY_SDK_DIR "${_galaxy_default}" CACHE PATH "Raiz do Galaxy SDK (pasta com inc/ e lib/)")

find_path(GALAXY_INCLUDE_DIR GxIAPI.h
    HINTS "${GALAXY_SDK_DIR}/inc" "${GALAXY_SDK_DIR}/include")
find_library(GALAXY_LIBRARY NAMES GxIAPI gxiapi
    HINTS "${GALAXY_SDK_DIR}/lib/x64" "${GALAXY_SDK_DIR}/lib" /usr/lib /usr/local/lib)

if(NOT GALAXY_INCLUDE_DIR OR NOT GALAXY_LIBRARY)
    message(FATAL_ERROR
        "Galaxy SDK nao encontrado em '${GALAXY_SDK_DIR}'.\n"
        "Instale o Galaxy SDK da Daheng ou informe -DGALAXY_SDK_DIR=<pasta com inc/ e lib/>.")
endif()

# Bridge serial com o STM32 (ja adicionada pelo projeto de topo, se houver)
if(NOT TARGET serial_lib)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../bridge_host ${CMAKE_CURRENT_BINARY_DIR}/bridge_host)
endif()

# ------------------------------------------------------------------------------
# Executavel
# ------------------------------------------------------------------------------
add_executable(VisionTest
    src/main.cpp
    src/VisionModule.cpp
    src/CameraModule.cpp
    src/Orchestrator.cpp
)

target_compile_features(VisionTest PRIVATE cxx_std_17)
target_include_directories(VisionTest PRIVATE inc ${GALAXY_INCLUDE_DIR})
target_link_libraries(VisionTest PRIVATE ${OpenCV_LIBS} ${GALAXY_LIBRARY} serial_lib Threads::Threads)

if(WIN32)
    target_compile_definitions(VisionTest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
    target_compile_options(VisionTest PRIVATE /W4 /utf-8)
    # F5 no Visual Studio roda a partir desta pasta (models/ e roi_config.yml)
    set_target_properties(VisionTest PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    target_compile_options(VisionTest PRIVATE -Wall -Wextra)
endif()

# Windows: copia as DLLs do OpenCV para junto do .exe (o Galaxy SDK registra as
# suas no PATH durante a instalacao).
if(WIN32)
    add_custom_command(TARGET VisionTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:VisionTest> $<TARGET_FILE_DIR:VisionTest>
        COMMAND_EXPAND_LISTS
        COMMENT "Copiando DLLs de runtime para a pasta do executavel")
endif()
