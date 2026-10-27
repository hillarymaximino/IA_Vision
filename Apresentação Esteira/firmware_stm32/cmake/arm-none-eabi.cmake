# Toolchain de compilacao cruzada para Cortex-M (GNU Arm Embedded).
#
# Procura arm-none-eabi-gcc no PATH. Para usar o compilador que vem com o
# STM32CubeIDE, informe -DARM_TOOLCHAIN_DIR=<.../gnu-tools-for-stm32.../tools/bin>.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Sem sistema operacional no alvo: os testes do CMake nao podem linkar executaveis
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARM_TOOLCHAIN_DIR "" CACHE PATH "Pasta bin/ do arm-none-eabi-gcc (vazio = PATH)")

if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
else()
    set(_exe "")
endif()

if(ARM_TOOLCHAIN_DIR)
    set(_prefix "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-")
else()
    set(_prefix "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER   "${_prefix}gcc${_exe}")
set(CMAKE_ASM_COMPILER "${_prefix}gcc${_exe}")
set(CMAKE_OBJCOPY      "${_prefix}objcopy${_exe}" CACHE FILEPATH "objcopy do toolchain ARM")
set(CMAKE_SIZE         "${_prefix}size${_exe}" CACHE FILEPATH "size do toolchain ARM")

# Bibliotecas e headers so do sysroot do toolchain, nunca do host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
