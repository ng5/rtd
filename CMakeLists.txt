cmake_minimum_required(VERSION 3.24)
project(RtdTickCPP LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0601)
# If a conda environment is active, expose its include/lib paths so CMake can find packages installed there.
if(DEFINED ENV{CONDA_PREFIX})
    set(CONDA_PREFIX $ENV{CONDA_PREFIX})
    message(STATUS "Conda environment detected: ${CONDA_PREFIX}")
else()
    message(STATUS "No CONDA_PREFIX env var found. If you want CMake to use a conda env, activate it before configuring.")
endif()
message(STATUS "INCLUDE_DIRECTORIES: ${CONDA_INCLUDE_DIRS}")
include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/third_party SYSTEM "${CONDA_PREFIX}/include" "${CONDA_PREFIX}/Library/include" )
link_directories("${CONDA_PREFIX}/lib" "${CONDA_PREFIX}/Library/lib")

# --- MIDL (IDL -> headers + IID C + TLB) ---
set(MIDL_IDL ${CMAKE_SOURCE_DIR}/idl/TypeLibrary.idl)
set(MIDL_OUT_DIR ${CMAKE_BINARY_DIR}/midl)
file(MAKE_DIRECTORY ${MIDL_OUT_DIR})
set(MIDL_ENV x64)

# Locate midl.exe
find_program(MIDL_EXECUTABLE NAMES midl HINTS ENV PATH)
if(NOT MIDL_EXECUTABLE)
    message(FATAL_ERROR "MIDL executable not found. Install Windows SDK / Visual Studio or ensure 'midl.exe' is on PATH.")
endif()

# Run MIDL at configure time so generated files are available to IDE/projects immediately
execute_process(
        COMMAND "${MIDL_EXECUTABLE}" /nologo /env ${MIDL_ENV} /h "${MIDL_OUT_DIR}/RtdTickLib_i.h" /iid "${MIDL_OUT_DIR}/RtdTickLib_i.c" /tlb "${MIDL_OUT_DIR}/TypeLibrary.tlb" "${MIDL_IDL}"
        WORKING_DIRECTORY "${MIDL_OUT_DIR}"
        RESULT_VARIABLE MIDL_RESULT
        OUTPUT_VARIABLE MIDL_STDOUT
        ERROR_VARIABLE MIDL_STDERR
)
if(NOT MIDL_RESULT EQUAL 0)
    message(FATAL_ERROR "MIDL failed (exit ${MIDL_RESULT}).\nSTDOUT:\n${MIDL_STDOUT}\nSTDERR:\n${MIDL_STDERR}")
endif()

# --- Generate RC that embeds the TLB and the .rgs (absolute paths) ---
set(MIDL_TLB_PATH ${MIDL_OUT_DIR}/TypeLibrary.tlb)
set(RGS_PATH ${CMAKE_SOURCE_DIR}/res/RtdTick.rgs)
configure_file(${CMAKE_SOURCE_DIR}/res/RtdTick.rc.in ${CMAKE_BINARY_DIR}/RtdTick_gen.rc @ONLY)

# --- Sources ---
file(GLOB SRC_CPP src/*.cpp)
set(SRC ${SRC_CPP} include/third_party/simdjson.cpp ${CMAKE_BINARY_DIR}/RtdTick_gen.rc MyRtd.def)

# Require the MIDL-generated C file (should exist after execute_process)
if(EXISTS "${MIDL_OUT_DIR}/RtdTickLib_i.c")
    list(APPEND SRC ${MIDL_OUT_DIR}/RtdTickLib_i.c)
else()
    message(FATAL_ERROR "MIDL-generated file '${MIDL_OUT_DIR}/RtdTickLib_i.c' not found after running midl.")
endif()

add_library(RtdTickCPP SHARED ${SRC})
target_include_directories(RtdTickCPP PRIVATE ${MIDL_OUT_DIR} ${CMAKE_SOURCE_DIR}/res)
target_link_libraries(RtdTickCPP PRIVATE ole32 oleaut32 uuid user32 winhttp)
set_target_properties(RtdTickCPP PROPERTIES OUTPUT_NAME "MyRtd")

file(GLOB TESTS tests/*.cpp)
foreach(file ${TESTS})
    get_filename_component(x ${file} NAME_WLE)
    add_executable("${x}" ${file})
    message(STATUS "Adding test executable: ${x}")
    target_link_libraries("${x}" PRIVATE websockets)
endforeach()


# Automatically register the built DLL after each build (post-build step)
if(WIN32)
    add_custom_command(TARGET RtdTickCPP POST_BUILD
            COMMAND regsvr32 /s /n /i:user "$<TARGET_FILE:RtdTickCPP>"
            COMMENT "Per-user COM registration (HKCU) of MyRtd.dll (post-build)"
    )
endif()
