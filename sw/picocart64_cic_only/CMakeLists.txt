add_executable(picocart64_cic_only
    picocart64_cic_only.c
)

if(NOT DEFINED REGION)
    message(FATAL_ERROR "Please set the variable REGION to either NTSC or PAL")
endif()

if(REGION STREQUAL "NTSC")
    set(CONFIG_REGION_NTSC 1)
    set(CONFIG_REGION_PAL 0)
elseif(REGION STREQUAL "PAL")
    set(CONFIG_REGION_NTSC 0)
    set(CONFIG_REGION_PAL 1)
else()
    message(FATAL_ERROR "Please set the variable REGION to either NTSC or PAL")
endif()

if(NOT DEFINED CPU_FREQ_MHZ)
    set(CONFIG_CPU_FREQ_MHZ 266)
else()
    set(CONFIG_CPU_FREQ_MHZ ${CPU_FREQ_MHZ})
endif()

target_compile_definitions(picocart64_cic_only PRIVATE
    PICO_STDOUT_MUTEX=0
    CONFIG_REGION_NTSC=${CONFIG_REGION_NTSC}
    CONFIG_REGION_PAL=${CONFIG_REGION_PAL}
    CONFIG_CPU_FREQ_MHZ=${CONFIG_CPU_FREQ_MHZ}
)

# Generate git_info.h
set(GIT_INFO_H ${CMAKE_CURRENT_BINARY_DIR}/git_info.h)

add_custom_target(update_git_info_cic_only DEPENDS ${GIT_INFO_H})

add_custom_command(OUTPUT ${GIT_INFO_H}
        COMMENT "Generating ${GIT_INFO_H}"
        COMMAND ${CMAKE_COMMAND} -E echo_append " #define GIT_REV 0x" > ${GIT_INFO_H}
        COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} rev-parse --short=8 HEAD >> ${GIT_INFO_H}
)

# Shares FreeRTOSConfig.h, picocart64_pins.h and sram.h with picocart64_v1
target_include_directories(picocart64_cic_only PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../picocart64_v1
    ${CMAKE_CURRENT_BINARY_DIR}
)

add_dependencies(picocart64_cic_only update_git_info_cic_only)

# pull in common dependencies
target_link_libraries(picocart64_cic_only
    n64_cic
    pico_stdlib
    stdio_async_uart
    FreeRTOS-Kernel-Static # FreeRTOS kernel, static allocations
)

# disable usb output (interrupt heavy)
pico_enable_stdio_usb(picocart64_cic_only 0)

# disable uart output
pico_enable_stdio_uart(picocart64_cic_only 0)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(picocart64_cic_only)
