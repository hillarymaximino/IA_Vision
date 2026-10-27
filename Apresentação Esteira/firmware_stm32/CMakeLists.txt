# Build por linha de comando do firmware (equivalente a configuracao Debug do
# STM32CubeIDE). O projeto do CubeIDE (.project/.cproject) continua valido; este
# arquivo e usado pelo launcher (tools/) para compilar e gravar sem abrir a IDE.
#
#   cmake --preset firmware
#   cmake --build --preset firmware
#
# Saidas em build/: comEdge.elf, comEdge.bin, comEdge.hex e comEdge.map

cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake")
endif()

project(comEdge VERSION 1.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)   # gnu11, como no CubeIDE

file(GLOB FIRMWARE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Startup/*.s"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/*.c"
)

add_executable(${PROJECT_NAME}.elf ${FIRMWARE_SOURCES})

set(MCU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32F407VETX_FLASH.ld")

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE USE_HAL_DRIVER STM32F407xx DEBUG)

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    Core/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
)

target_compile_options(${PROJECT_NAME}.elf PRIVATE
    ${MCU_FLAGS}
    -O0 -g3
    -ffunction-sections -fdata-sections -fstack-usage
    --specs=nano.specs
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)

target_link_options(${PROJECT_NAME}.elf PRIVATE
    ${MCU_FLAGS}
    -T${LINKER_SCRIPT}
    --specs=nosys.specs --specs=nano.specs
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map
    -Wl,--print-memory-usage
    -static
    -Wl,--start-group -lc -lm -Wl,--end-group
)
set_target_properties(${PROJECT_NAME}.elf PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

# Imagens para gravadores que nao leem ELF (st-flash usa o .bin em 0x08000000)
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    BYPRODUCTS ${PROJECT_NAME}.bin ${PROJECT_NAME}.hex ${PROJECT_NAME}.map
    COMMENT "Gerando ${PROJECT_NAME}.bin e ${PROJECT_NAME}.hex")
