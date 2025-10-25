cmake_minimum_required(VERSION 3.24)
project(RtdTickCPP LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0601)

# --- MIDL (IDL -> headers + IID C + TLB) ---
set(MIDL_IDL ${CMAKE_SOURCE_DIR}/idl/TypeLibrary.idl)
set(MIDL_OUT_DIR ${CMAKE_BINARY_DIR}/midl)
file(MAKE_DIRECTORY ${MIDL_OUT_DIR})

set(MIDL_ENV x64)

add_custom_command(
        OUTPUT ${MIDL_OUT_DIR}/RtdTickLib_i.h
        ${MIDL_OUT_DIR}/RtdTickLib_i.c
        ${MIDL_OUT_DIR}/TypeLibrary.tlb
        COMMAND ${CMAKE_COMMAND} -E echo "MIDL ${MIDL_IDL}"
        COMMAND midl /nologo /env ${MIDL_ENV}
        /h "${MIDL_OUT_DIR}/RtdTickLib_i.h"
        /iid "${MIDL_OUT_DIR}/RtdTickLib_i.c"
        /tlb "${MIDL_OUT_DIR}/TypeLibrary.tlb"
        "${MIDL_IDL}"
        DEPENDS ${MIDL_IDL}
        WORKING_DIRECTORY ${MIDL_OUT_DIR}
        VERBATIM)

add_custom_target(midl_gen
        DEPENDS ${MIDL_OUT_DIR}/RtdTickLib_i.h ${MIDL_OUT_DIR}/RtdTickLib_i.c ${MIDL_OUT_DIR}/TypeLibrary.tlb)

# --- Generate RC that embeds the TLB and the .rgs (absolute paths) ---
set(MIDL_TLB_PATH ${MIDL_OUT_DIR}/TypeLibrary.tlb)
set(RGS_PATH ${CMAKE_SOURCE_DIR}/res/RtdTick.rgs)
configure_file(${CMAKE_SOURCE_DIR}/res/RtdTick.rc.in
        ${CMAKE_BINARY_DIR}/RtdTick_gen.rc @ONLY)

# --- Sources ---
set(SRC
        src/dllmain.cpp
        src/dllmain.h
        src/RtdTick.cpp
        src/third_party/simdjson.cpp
        ${CMAKE_BINARY_DIR}/RtdTick_gen.rc
        ${MIDL_OUT_DIR}/RtdTickLib_i.c
        MyRtd.def
)

add_library(RtdTickCPP SHARED ${SRC})
add_dependencies(RtdTickCPP midl_gen)

target_include_directories(RtdTickCPP PRIVATE
        ${MIDL_OUT_DIR}
        ${CMAKE_SOURCE_DIR}/res
)

target_link_libraries(RtdTickCPP PRIVATE ole32 oleaut32 uuid user32 winhttp)

set_target_properties(RtdTickCPP PROPERTIES OUTPUT_NAME "MyRtd")

# --- Convenience targets ---
add_custom_target(register
        COMMAND regsvr32 /s /n /i:user "$<TARGET_FILE:RtdTickCPP>"
        DEPENDS RtdTickCPP
        COMMENT "Per-user COM registration (HKCU) of MyRtd.dll")

add_custom_target(unregister
        COMMAND regsvr32 /s /u /n /i:user "$<TARGET_FILE:RtdTickCPP>"
        COMMENT "Per-user COM unregistration (HKCU) of MyRtd.dll")
