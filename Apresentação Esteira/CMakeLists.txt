cmake_minimum_required(VERSION 3.21)
project(SicovEsteira VERSION 1.1.0 LANGUAGES CXX)

# Projeto de topo da esteira SICOV: visao computacional (PC) + bridge serial STM32.
# O firmware do STM32 (firmware_stm32/) e compilacao cruzada para ARM e tem projeto
# proprio: use o preset 'firmware' daquela pasta, o STM32CubeIDE ou o launcher (tools/).

option(SICOV_BUILD_VISION "Compila a aplicacao de visao (requer OpenCV e Galaxy SDK)" ON)
option(BRIDGE_BUILD_EXAMPLES "Compila os terminais de teste da bridge" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(bridge_host)

if(SICOV_BUILD_VISION)
    add_subdirectory(visao)
endif()
