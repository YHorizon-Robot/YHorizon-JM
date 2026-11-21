cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake"
        CACHE FILEPATH "ARM GCC toolchain" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_PROJECT_NAME YHorizon-JM)

project(${CMAKE_PROJECT_NAME} C ASM)

set(CMAKE_C_FLAGS_RELEASE        "-O2 -g0 -DNDEBUG" CACHE STRING "Release C flags" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g3 -DNDEBUG" CACHE STRING "RelWithDebInfo C flags" FORCE)

set(YFOC_BOARD "YH_JM" CACHE STRING "Board directory under Boards/")
set(YFOC_FACTORY_TEST OFF CACHE BOOL "Build the factory self-test instead of the servo")
set(CAN_PROTOCOL "v1" CACHE STRING "CAN frontend (v1 or dm4310)")
set_property(CACHE CAN_PROTOCOL PROPERTY STRINGS v1 dm4310)
if(NOT CAN_PROTOCOL STREQUAL "v1" AND NOT CAN_PROTOCOL STREQUAL "dm4310")
    message(FATAL_ERROR "Unsupported CAN_PROTOCOL '${CAN_PROTOCOL}'")
endif()
if(YFOC_FACTORY_TEST AND NOT CAN_PROTOCOL STREQUAL "v1")
    message(FATAL_ERROR "Factory self-test requires CAN_PROTOCOL=v1")
endif()
message(STATUS "JM1 CAN protocol: ${CAN_PROTOCOL}")

if(NOT YFOC_BOARD STREQUAL "YH_JM")
    message(FATAL_ERROR "This repository only builds YH_JM (got '${YFOC_BOARD}')")
endif()

set(YFOC_BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Boards/${YFOC_BOARD}")
if(NOT EXISTS "${YFOC_BOARD_DIR}/board.cmake")
    message(FATAL_ERROR "Unknown board '${YFOC_BOARD}' (missing ${YFOC_BOARD_DIR}/board.cmake)")
endif()
include("${YFOC_BOARD_DIR}/board.cmake")

add_executable(${CMAKE_PROJECT_NAME})

include("${CMAKE_CURRENT_SOURCE_DIR}/Port/${YFOC_PORT}/port.cmake")

if(CAN_PROTOCOL STREQUAL "dm4310")
    set(comm_sources App/compat/comm_dm4310.c App/compat/dm4310.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CFG_DM4310=1)
else()
    set(comm_sources App/multi_axis.c App/comm.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CFG_MULTIAXIS=1)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Hw/RTT/SEGGER_RTT.c
    Hw/RTT/SEGGER_RTT_printf.c

    Foc/foc_math.c
    Foc/foc_svpwm.c
    Foc/foc_encoder.c
    Foc/foc_vernier.c
    Foc/foc_current.c

    App/main.c
    App/servo.c
    App/cali.c
    App/cali_nv.c
    App/commission.c
    ${comm_sources}
    App/node.c
)

if(YFOC_FACTORY_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE App/factory.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CFG_FACTORY_TEST=1)
endif()

target_include_directories(${CMAKE_PROJECT_NAME} BEFORE PRIVATE
    "${YFOC_BOARD_DIR}"
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App
    Foc
    Hw/include
    Hw/RTT
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    CFG_COMMISSION=1
    $<$<CONFIG:Debug>:DEBUG>
)

set_source_files_properties(
    Foc/foc_math.c
    Foc/foc_svpwm.c
    Foc/foc_encoder.c
    Foc/foc_vernier.c
    Foc/foc_current.c
    App/servo.c
    PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Release,RelWithDebInfo>:-O3;-fno-math-errno;-fno-trapping-math>"
)

target_link_libraries(${CMAKE_PROJECT_NAME} m)

if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "CMAKE_OBJCOPY is not set (arm-none-eabi-objcopy not found)")
endif()
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND "${CMAKE_OBJCOPY}" -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMENT "Generating bin/hex"
)
