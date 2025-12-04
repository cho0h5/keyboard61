# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(firmware C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

option(BUILD_BOOTLOADER "Build the USB CDC bootloader image" ON)
option(BUILD_FIRMWARE   "Build the main keyboard firmware image" ON)

# Flash size: set to 2MB for a stock Pico. Adjust if your board has larger flash.
set(PICO_FLASH_SIZE_BYTES 2097152)

if(BUILD_BOOTLOADER)
    # Bootloader target at offset 0x0
    set(PICO_FLASH_TARGET_OFFSET 0 CACHE STRING "Bootloader offset" FORCE)

    add_executable(bootloader
            bootloader.c
            bootloader_descriptors.c)

    pico_set_program_name(bootloader "bootloader")
    pico_set_program_version(bootloader "0.1")
    pico_enable_stdio_uart(bootloader 0)
    pico_enable_stdio_usb(bootloader 0)

    # Ensure linker offset is pinned to 0 even if cache gets stale
    target_link_options(bootloader PRIVATE "-Wl,--defsym=FLASH_TARGET_OFFSET=0x0")

    target_link_libraries(bootloader
            pico_stdlib
            tinyusb_device
            tinyusb_board
            hardware_flash
            hardware_sync)

    target_compile_definitions(bootloader PRIVATE BOOTLOADER_BUILD=1)

    target_include_directories(bootloader PRIVATE
            ${CMAKE_CURRENT_LIST_DIR})

    pico_add_extra_outputs(bootloader)
endif()

if(BUILD_FIRMWARE)
    # Application firmware lives after the bootloader (offset 0x20000)
    set(PICO_FLASH_TARGET_OFFSET 0x20000 CACHE STRING "Firmware offset" FORCE)

    add_executable(firmware
            firmware.c
            usb_descriptors.c)

    pico_set_program_name(firmware "firmware")
    pico_set_program_version(firmware "0.1")

    pico_enable_stdio_uart(firmware 0)
    pico_enable_stdio_usb(firmware 0)

    target_compile_definitions(firmware PRIVATE FIRMWARE_BUILD=1)

    # Force linker offset to 0x20000 even if cache was stale
    target_link_options(firmware PRIVATE "-Wl,--defsym=FLASH_TARGET_OFFSET=0x20000")

    # Use custom linker script with flash origin at 0x10020000
    pico_set_linker_script(firmware ${CMAKE_CURRENT_LIST_DIR}/memmap_firmware.ld)
    configure_file(${CMAKE_CURRENT_LIST_DIR}/pico_flash_region_firmware.ld
                   ${CMAKE_CURRENT_BINARY_DIR}/pico_flash_region_firmware.ld COPYONLY)

    target_link_libraries(firmware
            pico_stdlib
            tinyusb_device
            tinyusb_board)

    target_include_directories(firmware PRIVATE
            ${CMAKE_CURRENT_LIST_DIR})

    pico_add_extra_outputs(firmware)
endif()
