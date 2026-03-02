cmake_minimum_required(VERSION 3.20)
project(csp LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Options ---
option(CSP_TLS "Enable TLS support via mbedTLS" OFF)
option(CSP_BUILD_TESTS "Build tests" ON)

# --- Compiler flags ---
if(MSVC OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    # clang-cl or MSVC
    add_compile_options(/W4 /O2 /DDEBUG)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        add_compile_options(-stdlib=libc++)
        add_link_options(-stdlib=libc++)
    endif()
endif()

# --- Include paths ---
include_directories(include)
include_directories(vendor/include)

# --- fcontext assembly ---
set(FCONTEXT_DIR vendor/github.com/boostorg/context/src/asm)

if(WIN32)
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Native MSVC: use MASM assembly
        enable_language(ASM_MASM)
        # Boost.Context MASM files reference BOOST_CONTEXT_EXPORT; define as
        # empty for static-library builds (matches Boost's own CMakeLists.txt).
        set(CMAKE_ASM_MASM_FLAGS "${CMAKE_ASM_MASM_FLAGS} /DBOOST_CONTEXT_EXPORT=")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
                set(FCONTEXT_SRCS
                    ${FCONTEXT_DIR}/jump_arm64_aapcs_pe_armasm.asm
                    ${FCONTEXT_DIR}/make_arm64_aapcs_pe_armasm.asm)
            else()
                set(FCONTEXT_SRCS
                    ${FCONTEXT_DIR}/jump_x86_64_ms_pe_masm.asm
                    ${FCONTEXT_DIR}/make_x86_64_ms_pe_masm.asm)
            endif()
        else()
            set(FCONTEXT_SRCS
                ${FCONTEXT_DIR}/jump_i386_ms_pe_masm.asm
                ${FCONTEXT_DIR}/make_i386_ms_pe_masm.asm)
        endif()
    else()
        # clang-cl or MinGW: use GAS assembly
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
                set(FCONTEXT_SRCS
                    ${FCONTEXT_DIR}/jump_arm64_aapcs_pe_armclang.S
                    ${FCONTEXT_DIR}/make_arm64_aapcs_pe_armclang.S)
            else()
                set(FCONTEXT_SRCS
                    ${FCONTEXT_DIR}/jump_x86_64_ms_pe_clang_gas.S
                    ${FCONTEXT_DIR}/make_x86_64_ms_pe_clang_gas.S)
            endif()
        else()
            set(FCONTEXT_SRCS
                ${FCONTEXT_DIR}/jump_i386_ms_pe_clang_gas.S
                ${FCONTEXT_DIR}/make_i386_ms_pe_clang_gas.S)
        endif()
    endif()
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(FCONTEXT_SRCS
            ${FCONTEXT_DIR}/jump_arm64_aapcs_macho_gas.S
            ${FCONTEXT_DIR}/make_arm64_aapcs_macho_gas.S)
    else()
        set(FCONTEXT_SRCS
            ${FCONTEXT_DIR}/jump_x86_64_sysv_macho_gas.S
            ${FCONTEXT_DIR}/make_x86_64_sysv_macho_gas.S)
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(FCONTEXT_SRCS
            ${FCONTEXT_DIR}/jump_arm64_aapcs_elf_gas.S
            ${FCONTEXT_DIR}/make_arm64_aapcs_elf_gas.S)
    else()
        set(FCONTEXT_SRCS
            ${FCONTEXT_DIR}/jump_x86_64_sysv_elf_gas.S
            ${FCONTEXT_DIR}/make_x86_64_sysv_elf_gas.S)
    endif()
endif()

# --- Library sources ---
set(LIB_SRCS
    src/csp.cc
    src/csp_globals.cpp
    src/cancel.cc
    src/channel.cc
    src/clock.cc
    src/hamt.cc
    src/io.cc
    src/log.cc
    src/runtime.cpp
    src/timer.cc
    src/reactor.cc
    src/blocking_pool.cc
    src/stack_pool.cc
)

if(WIN32)
    list(APPEND LIB_SRCS
        src/win_signal.cc
    )
else()
    list(APPEND LIB_SRCS
        src/signal.cc
        src/stack_analysis_arm64.cc
    )
endif()

if(CSP_TLS)
    list(APPEND LIB_SRCS src/tls.cc)
endif()

# --- CSP library ---
add_library(csp STATIC ${LIB_SRCS} ${FCONTEXT_SRCS})

target_include_directories(csp PUBLIC include vendor/include)

if(CSP_TLS)
    target_compile_definitions(csp PUBLIC CSP_TLS)

    set(MBEDTLS_DIR vendor/github.com/Mbed-TLS/mbedtls)
    file(GLOB MBEDTLS_SRCS ${MBEDTLS_DIR}/library/*.c)
    add_library(mbedtls_csp STATIC ${MBEDTLS_SRCS})
    target_include_directories(mbedtls_csp PRIVATE
        include
        ${MBEDTLS_DIR}/include
    )
    target_compile_definitions(mbedtls_csp PRIVATE
        MBEDTLS_CONFIG_FILE="mbedtls_config.h"
    )
    target_include_directories(csp PRIVATE ${MBEDTLS_DIR}/include)
    target_link_libraries(csp PRIVATE mbedtls_csp)
endif()

# --- Platform link libraries ---
if(WIN32)
    target_link_libraries(csp PRIVATE synchronization ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(csp PRIVATE Threads::Threads)
endif()

# --- Tests ---
if(CSP_BUILD_TESTS)
    file(GLOB TEST_SRCS test/*.test.cc)

    # On Windows, exclude tests that depend on TLS (mbedTLS not built by default)
    if(WIN32)
        list(FILTER TEST_SRCS EXCLUDE REGEX "tls\\.test\\.cc$")
    endif()

    add_executable(csp_tests test/main.cc ${TEST_SRCS})
    target_include_directories(csp_tests PRIVATE test)
    target_link_libraries(csp_tests PRIVATE csp)
endif()
