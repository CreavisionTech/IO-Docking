cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(io_dock C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

add_executable(io_dock
    src/main.c
    src/protocol.c
    src/usb_descriptors.c
    src/text_cmd.c
    src/bin_proto.c
    src/drivers.c
    src/seq.c
    src/config.c
)

# src/ is on the include path so "#include <tusb_config.h>" and friends resolve
target_include_directories(io_dock PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_options(io_dock PRIVATE -Wall -Wextra -Wformat=2)

target_link_libraries(io_dock
    pico_stdlib
    tinyusb_device
    tinyusb_board
    hardware_flash
    pico_flash
    hardware_adc
    hardware_dma
    hardware_i2c
    hardware_pio
    hardware_pwm
    hardware_spi
    hardware_uart
    pico_multicore
    pico_unique_id
)

# Board carries an 8 MB (W25Q64) flash; the parameter-storage sector lives at
# the very end of it. The SDK default (2 MB) would make flash_range_erase()
# reject that offset, so tell the target the real flash size.
target_compile_definitions(io_dock PRIVATE PICO_FLASH_SIZE_BYTES=8388608)

pico_enable_stdio_uart(io_dock 0)
pico_enable_stdio_usb(io_dock 0)

# RAM-resident flash diagnostic (see src/flash_diag.c). copy_to_ram so it runs
# even if quad/XIP boot is broken; single-I/O commands identify the flash chip.
add_executable(flash_diag
    src/flash_diag.c
)
target_include_directories(flash_diag PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(flash_diag pico_stdlib)
pico_enable_stdio_uart(flash_diag 0)
pico_enable_stdio_usb(flash_diag 1)
pico_set_binary_type(flash_diag copy_to_ram)

# Note: pico_add_extra_outputs is intentionally omitted - it needs picotool,
# which the host compiler here (STM32 mingw GCC 6.3) cannot build. Generate
# .bin/.hex/.uf2 afterwards with objcopy + uf2conv.py (see build notes).
