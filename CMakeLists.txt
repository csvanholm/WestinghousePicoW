# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
    file(TO_CMAKE_PATH "${USERHOME}" USERHOME)
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS "${picoVscode}")
    include("${picoVscode}")
endif()

# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")
# ====================================================================================

cmake_minimum_required(VERSION 3.13)
option(PICO_MAIL_USE_LOCAL_SDK "Prefer bundled forked SDK from third_party/pico-sdk" ON)
if (PICO_MAIL_USE_LOCAL_SDK)
    set(PICO_MAIL_LOCAL_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
    if (EXISTS "${PICO_MAIL_LOCAL_SDK_PATH}/pico_sdk_init.cmake")
        set(PICO_SDK_PATH "${PICO_MAIL_LOCAL_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
        message(STATUS "Using local Pico SDK: ${PICO_SDK_PATH}")
    else()
        message(WARNING "PICO_MAIL_USE_LOCAL_SDK is ON but third_party/pico-sdk is missing; falling back to external PICO_SDK_PATH.")
    endif()
endif()



# 1. Pull in SDK build functions
include(pico_sdk_import.cmake)
project(westinghouse_wifi_rtos C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands" FORCE)

# 2. Initialize the SDK
pico_sdk_init()

set(PICO_MAIL_USE_FREERTOS TRUE)
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel)
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})

if(PICO_PLATFORM MATCHES "^rp2040")
    set(PICO_MAIL_FREERTOS_PORT "RP2040")
elseif(PICO_PLATFORM MATCHES "^rp2350")
    if(PICO_PLATFORM MATCHES "riscv|rv")
        set(PICO_MAIL_FREERTOS_PORT "RP2350_RISC-V")
    else()
        set(PICO_MAIL_FREERTOS_PORT "RP2350_ARM_NTZ")
    endif()
else()
    message(FATAL_ERROR "Unsupported PICO_PLATFORM='${PICO_PLATFORM}' for bundled FreeRTOS integration")
endif()

include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/${PICO_MAIL_FREERTOS_PORT}/library.cmake)
message(STATUS "Using bundled FreeRTOS port: ${PICO_MAIL_FREERTOS_PORT}")

# Check if we're building for a Pico W variant
if(PICO_BOARD MATCHES "pico_w|pico2_w")
    set(_PICO_W TRUE)
    message(STATUS "Building for Pico W variant: ${PICO_BOARD}")
else()
    set(_PICO_W FALSE)
    message(STATUS "Building for non-WiFi Pico: ${PICO_BOARD}")
endif()

if(_PICO_W)
    find_package(Perl REQUIRED)

    set(PICO_MAIL_FSDATA_DIR ${CMAKE_CURRENT_LIST_DIR}/Pico-W_Wi-Fi_Configure/wifi_setup)
    set(PICO_MAIL_MAKEFSDATA_SCRIPT ${PICO_MAIL_FSDATA_DIR}/external/makefsdata)

    file(GLOB_RECURSE PICO_MAIL_FSDATA_INPUTS CONFIGURE_DEPENDS
        "${PICO_MAIL_FSDATA_DIR}/fs/*"
    )

    add_custom_command(
        OUTPUT ${PICO_MAIL_FSDATA_DIR}/my_fsdata.c
        COMMAND ${CMAKE_COMMAND} -E echo "[fsdata] Regenerating my_fsdata.c from wifi_setup/fs"
        COMMAND ${PERL_EXECUTABLE} ${PICO_MAIL_MAKEFSDATA_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E rename ${PICO_MAIL_FSDATA_DIR}/fsdata.c ${PICO_MAIL_FSDATA_DIR}/my_fsdata.c
        COMMAND ${CMAKE_COMMAND} -E echo "[fsdata] Wrote ${PICO_MAIL_FSDATA_DIR}/my_fsdata.c"
        WORKING_DIRECTORY ${PICO_MAIL_FSDATA_DIR}
        DEPENDS ${PICO_MAIL_MAKEFSDATA_SCRIPT} ${PICO_MAIL_FSDATA_INPUTS}
        COMMENT "Generating my_fsdata.c from wifi_setup/fs"
        VERBATIM
    )

    add_custom_target(pico_mail_generate_fsdata
        DEPENDS ${PICO_MAIL_FSDATA_DIR}/my_fsdata.c
    )
endif()

if(PICO_PLATFORM MATCHES "^rp2350")
    set(PICO_MAIL_TIME_LIB pico_aon_timer)
    message(STATUS "Using time library: pico_aon_timer (RP2350)")
elseif(PICO_PLATFORM MATCHES "^rp2040")
    set(PICO_MAIL_TIME_LIB hardware_rtc)
    message(STATUS "Using time library: hardware_rtc (RP2040)")
else()
    message(FATAL_ERROR "Unsupported PICO_PLATFORM='${PICO_PLATFORM}' for time library selection")
endif()


# 3. Define the executable
add_executable(${PROJECT_NAME} 
    main.cpp
    mail_service.cpp
    generator_controller.cpp
)

if(_PICO_W)
    add_dependencies(${PROJECT_NAME} pico_mail_generate_fsdata)
endif()

if(_PICO_W)
    target_sources(${PROJECT_NAME} PRIVATE
        Pico-W_Wi-Fi_Configure/wifi_setup/access_point.c
        Pico-W_Wi-Fi_Configure/wifi_setup/dhcp_server.c
        Pico-W_Wi-Fi_Configure/wifi_setup/http_server.c
        Pico-W_Wi-Fi_Configure/wifi_setup/flash_program.c
    )
endif()

# 4. Include project directories (for lwipopts.h)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Pico-W_Wi-Fi_Configure/wifi_setup
)


target_compile_definitions(${PROJECT_NAME} PRIVATE
    #this is just to supress some of the verbose logging from the Wi-Fi driver, which can be quite chatty and is not relevant to this example
    PICO_CYW43_LOGGING_ENABLED=0
    PICO_CYW43_ARCH_DEBUG_ENABLED=0
    SMTP_CHECK_DATA=0
    #PICO_STACK_SIZE=0x1000
        LWIP_HTTPD=1
        LWIP_HTTPD_SSI=1
        LWIP_HTTPD_CGI=1
        LWIP_HTTPD_SSI_INCLUDE_TAG=0
        HTTPD_FSDATA_FILE=\"my_fsdata.c\"
  )  

target_compile_definitions(${PROJECT_NAME} PRIVATE USE_FREERTOS_APP=1)

if(_PICO_W)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _PICO_W=1)
endif()
    

# 5. Link required libraries
# - pico_stdlib: Standard Pico functions
# - pico_cyw43_arch_lwip_threadsafe_background: Wi-Fi stack for Pico W/2W

target_link_libraries(${PROJECT_NAME} 
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_smtp
    pico_lwip_http
    pico_lwip_mbedtls
    pico_mbedtls
    pico_lwip_sntp
    hardware_flash
    ${PICO_MAIL_TIME_LIB}
)

target_link_libraries(${PROJECT_NAME}
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)


# 6. Enable USB output for printf debugging
pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 1)
# 7. Create extra outputs (bin, hex, uf2)
pico_add_extra_outputs(${PROJECT_NAME})
