cmake_minimum_required(VERSION 3.12)

# DVI output driver. Implements the same API as ../vga (vga.h) so it can be
# linked in its place. Reuses the vga mode tables for the 640x480 timing.

set(LIBRARY pico9918-dvi)

if (NOT PICO_PLATFORM STREQUAL "rp2040")
    message(FATAL_ERROR "PICO9918 DVI output is only implemented for RP2040 (PICO_PLATFORM is ${PICO_PLATFORM})")
endif()

add_library(${LIBRARY} STATIC
        dvi.c
        tmds.c
        tmds_encode_rp2040.S
        ${CMAKE_CURRENT_LIST_DIR}/../vga/vga-modes.c)

pico_generate_pio_header(${LIBRARY} ${CMAKE_CURRENT_LIST_DIR}/dvi.pio)

target_include_directories(${LIBRARY} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../vga)

target_compile_definitions(${LIBRARY} PRIVATE
        PICO9918_ENABLE_SCART=0)

target_link_libraries(${LIBRARY} PRIVATE
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_dma
        hardware_pwm
        hardware_irq
        hardware_interp)
