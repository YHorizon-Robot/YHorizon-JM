cmake_minimum_required(VERSION 3.15)
project(yfoc_motor_sdk_cpp LANGUAGES CXX)

set(YFOC_SDK_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/lib" CACHE PATH "YFOC native library directory")
add_library(yfoc::motor SHARED IMPORTED GLOBAL)
set_target_properties(yfoc::motor PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/include"
    INTERFACE_COMPILE_FEATURES cxx_std_17)
if(WIN32)
    if(MSVC)
        set(implib yfoc_motor_sdk.lib)
    else()
        set(implib libyfoc_motor_sdk.dll.a)
    endif()
    set_target_properties(yfoc::motor PROPERTIES
        IMPORTED_LOCATION "${YFOC_SDK_LIB_DIR}/yfoc_motor_sdk.dll"
        IMPORTED_IMPLIB "${YFOC_SDK_LIB_DIR}/${implib}")
else()
    set_target_properties(yfoc::motor PROPERTIES IMPORTED_NO_SONAME TRUE IMPORTED_LOCATION
        "${YFOC_SDK_LIB_DIR}/libyfoc_motor_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(yfoc_inspect examples/inspect.cpp)
    target_link_libraries(yfoc_inspect PRIVATE yfoc::motor)
    if(WIN32)
        add_custom_command(TARGET yfoc_inspect POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:yfoc::motor>" "$<TARGET_FILE_DIR:yfoc_inspect>")
    endif()
endif()
