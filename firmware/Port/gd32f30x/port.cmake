set(GD32_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/GD32F30x")
set(GD32_CMSIS "${GD32_ROOT}/CMSIS")
set(GD32_SPL   "${GD32_ROOT}/GD32F30x_standard_peripheral")
set(YFOC_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Port/gd32f30x")

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${YFOC_PORT_DIR}/startup_gd32f30x_hd.S
    ${GD32_CMSIS}/GD/GD32F30x/Source/system_gd32f30x.c
    ${GD32_CMSIS}/GD/GD32F30x/Source/syscalls.c
    ${YFOC_PORT_DIR}/board.c
    ${YFOC_PORT_DIR}/irq.c
    ${YFOC_PORT_DIR}/pwm.c
    ${YFOC_PORT_DIR}/encoder.c
    ${YFOC_PORT_DIR}/can.c
    ${YFOC_PORT_DIR}/debug.c
    ${YFOC_PORT_DIR}/adc.c
    ${YFOC_PORT_DIR}/nv_flash.c
    ${YFOC_PORT_DIR}/clock_diag.c
    ${YFOC_PORT_DIR}/factory_hw.c

    ${GD32_SPL}/Source/gd32f30x_adc.c
    ${GD32_SPL}/Source/gd32f30x_can.c
    ${GD32_SPL}/Source/gd32f30x_dbg.c
    ${GD32_SPL}/Source/gd32f30x_fmc.c
    ${GD32_SPL}/Source/gd32f30x_gpio.c
    ${GD32_SPL}/Source/gd32f30x_misc.c
    ${GD32_SPL}/Source/gd32f30x_pmu.c
    ${GD32_SPL}/Source/gd32f30x_rcu.c
    ${GD32_SPL}/Source/gd32f30x_spi.c
    ${GD32_SPL}/Source/gd32f30x_timer.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${YFOC_PORT_DIR}
    ${GD32_CMSIS}
    ${GD32_CMSIS}/GD/GD32F30x/Include
    ${GD32_SPL}/Include
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    GD32F30X_HD
    USE_STDPERIPH_DRIVER
    HXTAL_VALUE=${YFOC_HXTAL_VALUE}
)

set_source_files_properties(
    ${YFOC_PORT_DIR}/encoder.c
    ${YFOC_PORT_DIR}/pwm.c
    PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Release,RelWithDebInfo>:-O3;-fno-math-errno;-fno-trapping-math>"
)

file(TO_CMAKE_PATH "${YFOC_PORT_DIR}/gd32f30x_flash.ld" YFOC_LINKER_SCRIPT)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T "${YFOC_LINKER_SCRIPT}"
    "LINKER:-Map=${CMAKE_PROJECT_NAME}.map"
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    LINK_DEPENDS "${YFOC_LINKER_SCRIPT}"
)
