cmake_minimum_required(VERSION 3.20)
project(csp LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Options ---
option(CSP_TLS "Enable TLS support via PicoTLS" OFF)
option(CSP_BUILD_TESTS "Build tests" ON)

# --- Compiler flags ---
if(MSVC OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    # clang-cl or MSVC — use generator expressions so /W4 /O2 don't leak to MASM
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W4> $<$<COMPILE_LANGUAGE:C,CXX>:/O2>)
    add_compile_definitions(DEBUG NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
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
        # Native MSVC: ARM64 uses armasm64 (ASM_MARMASM), x86/x64 uses ml/ml64 (ASM_MASM).
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            enable_language(ASM_MARMASM)
        else()
            enable_language(ASM_MASM)
        endif()
        # BOOST_CONTEXT_EXPORT define is applied via target_compile_definitions
        # below (after add_library) — CMAKE_ASM_MASM_FLAGS doesn't reliably
        # pass trailing = in VS generators.
        # Use CSP's local make_fcontext copies (src/asm/) which have
        # trampoline/finish separated into their own PROCs with correct
        # .pdata/.xdata for MSVC x64 exception unwinding on imp stacks.
        # jump_fcontext is unchanged, so it comes from the vendor submodule.
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
                set(FCONTEXT_SRCS
                    ${FCONTEXT_DIR}/jump_arm64_aapcs_pe_armasm.asm
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/asm/make_arm64_aapcs_pe_armasm.asm)
            else()
                set(FCONTEXT_SRCS
                    ${FCONTEXT_DIR}/jump_x86_64_ms_pe_masm.asm
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/asm/make_x86_64_ms_pe_masm.asm)
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
    src/imp_exit.cc
    src/supervisor.cc
    src/byte_reader.cc
    src/net.cc
    src/file.cc
    src/http.cc
)

if(WIN32)
    list(APPEND LIB_SRCS
        src/win_signal.cc
        # stack_analysis_arm64.cc has a portable #else branch that returns
        # conservative defaults on non-ARM64. Include it on Windows so the
        # csp::analyze_stack_depth symbol resolves for callers (notably the
        # T3.4.4 spawn-time consultation and the T3.4.5 audit test).
        src/stack_analysis_arm64.cc
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

# --- llhttp (vendored HTTP parser) ---
set(LLHTTP_DIR vendor/github.com/nodejs/llhttp)
add_library(llhttp_csp STATIC
    ${LLHTTP_DIR}/src/llhttp.c
    ${LLHTTP_DIR}/src/api.c
    ${LLHTTP_DIR}/src/http.c
)
target_include_directories(llhttp_csp PUBLIC ${LLHTTP_DIR}/include)

# --- CSP library ---
add_library(csp STATIC ${LIB_SRCS} ${FCONTEXT_SRCS})

target_include_directories(csp PUBLIC include vendor/include)
target_include_directories(csp PRIVATE ${LLHTTP_DIR}/include)
target_link_libraries(csp PRIVATE llhttp_csp)

# Boost.Context MASM files reference BOOST_CONTEXT_EXPORT; define as empty
# for static-library builds.
if(WIN32 AND MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_definitions(csp PRIVATE "BOOST_CONTEXT_EXPORT=")
endif()

if(CSP_TLS)
    target_compile_definitions(csp PUBLIC CSP_TLS)

    set(PICOTLS_DIR vendor/github.com/h2o/picotls)
    set(CIFRA_DIR ${PICOTLS_DIR}/deps/cifra/src)
    set(UECC_DIR ${PICOTLS_DIR}/deps/micro-ecc)

    set(PICOTLS_SRCS
        ${PICOTLS_DIR}/lib/picotls.c
        ${PICOTLS_DIR}/lib/hpke.c
        ${PICOTLS_DIR}/lib/pembase64.c
        ${PICOTLS_DIR}/lib/cifra.c
        ${PICOTLS_DIR}/lib/cifra/x25519.c
        ${PICOTLS_DIR}/lib/cifra/chacha20.c
        ${PICOTLS_DIR}/lib/cifra/aes128.c
        ${PICOTLS_DIR}/lib/cifra/aes256.c
        ${PICOTLS_DIR}/lib/cifra/random.c
        ${PICOTLS_DIR}/lib/uecc.c
        ${PICOTLS_DIR}/lib/minicrypto-pem.c
        ${PICOTLS_DIR}/lib/asn1.c
        ${PICOTLS_DIR}/lib/ffx.c
        ${UECC_DIR}/uECC.c
        ${CIFRA_DIR}/aes.c
        ${CIFRA_DIR}/blockwise.c
        ${CIFRA_DIR}/chacha20.c
        ${CIFRA_DIR}/chash.c
        ${CIFRA_DIR}/curve25519.c
        ${CIFRA_DIR}/drbg.c
        ${CIFRA_DIR}/hmac.c
        ${CIFRA_DIR}/gcm.c
        ${CIFRA_DIR}/gf128.c
        ${CIFRA_DIR}/modes.c
        ${CIFRA_DIR}/poly1305.c
        ${CIFRA_DIR}/sha256.c
        ${CIFRA_DIR}/sha512.c
    )
    add_library(picotls_csp STATIC ${PICOTLS_SRCS})
    target_include_directories(picotls_csp PRIVATE
        ${PICOTLS_DIR}/include
        ${CIFRA_DIR}
        ${CIFRA_DIR}/ext
        ${UECC_DIR}
    )
    target_include_directories(csp PRIVATE
        ${PICOTLS_DIR}/include
        ${CIFRA_DIR}
        ${CIFRA_DIR}/ext
        ${UECC_DIR}
    )
    target_link_libraries(csp PRIVATE picotls_csp)
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

    # Exclude tests for features not in the dist amalgamation.
    list(FILTER TEST_SRCS EXCLUDE REGEX "net\\.test\\.cc$")

    if(WIN32)
        # Exclude tests that depend on TLS (PicoTLS not built by default on Windows)
        list(FILTER TEST_SRCS EXCLUDE REGEX "tls\\.test\\.cc$")
        # HTTP tests not yet verified on Windows
        list(FILTER TEST_SRCS EXCLUDE REGEX "http\\.test\\.cc$")
        # HTTP/2 not built on Windows (nghttp2 not wired through CMake yet)
        list(FILTER TEST_SRCS EXCLUDE REGEX "http2\\.test\\.cc$")
        # HTTP/3 not built on Windows (nghttp3/ngtcp2 not wired through CMake yet)
        list(FILTER TEST_SRCS EXCLUDE REGEX "http3\\.test\\.cc$")
        # WebSocket not built on Windows (wslay not wired through CMake yet)
        list(FILTER TEST_SRCS EXCLUDE REGEX "ws\\.test\\.cc$")
    endif()

    add_executable(csp_tests test/main.cc ${TEST_SRCS})
    target_include_directories(csp_tests PRIVATE test)
    target_link_libraries(csp_tests PRIVATE csp)
endif()
