cmake_minimum_required(VERSION 3.13)

# Initialize SDK
include(pico_sdk_import.cmake)

project(pico-tpm-spider C CXX ASM)

# Initialize Pico SDK
pico_sdk_init()

# Create executable
add_executable(pico-tpm-spider
    main.c
)

# Generate PIO header
pico_generate_pio_header(pico-tpm-spider ${CMAKE_CURRENT_LIST_DIR}/sniffer.pio)

# Link necessary libraries
target_link_libraries(pico-tpm-spider
    hardware_clocks
    hardware_dma
    hardware_pio
    pico_multicore
    pico_stdlib
)

# The Pi Pico can safely be clocked up to 200MHz
add_compile_definitions(SYS_CLK_MHZ=200)

# Enable USB stdio
pico_enable_stdio_usb(pico-tpm-spider 1)
pico_enable_stdio_uart(pico-tpm-spider 0)

# Generate UF2 and other outputs
pico_add_extra_outputs(pico-tpm-spider)
