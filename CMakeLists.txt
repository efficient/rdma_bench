cmake_minimum_required(VERSION 3.5)
project(eRPC)

set(CMAKE_CXX_COMPILER g++-7)
set(CMAKE_C_LINK_EXECUTABLE g++-7)

add_definitions(-std=c++14)

add_definitions(-Wall -Wextra -Werror -pedantic)
add_definitions(-Wsign-conversion)
add_definitions(-Wold-style-cast)

add_definitions(-Wno-unused-function)
add_definitions(-march=native)
add_definitions(-g)

set(LIBRARIES ${LIBRARIES} rt numa ibverbs pthread gflags papi memcached)

# Testing
enable_testing()
find_package(GTest REQUIRED)

include_directories(${GTEST_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR})

# Options exposed to the user
option(PERF "Compile for performance" OFF)
set(PGO "none" CACHE STRING "Profile-guided optimization (generate/use/none)")
set(LOG_LEVEL "warn" CACHE STRING "Logging level (none/error/warn/info/debug/trace)") 

# Parse the user-exposed options
if(PERF)
  MESSAGE(STATUS "Compilation optimized for performance.")
  SET(DEBUG OFF)
  SET(LTO ON)
else(PERF)
  MESSAGE(STATUS "Compilation not optimized for performance.")
  SET(DEBUG ON)
  SET(LTO OFF)
endif(PERF)

# Profile-guided optimization
if(PGO STREQUAL "generate")
  MESSAGE(STATUS "Profile-guided optimization (generate mode) is enabled. Performance will be low.")
  add_definitions(-fprofile-generate)
  SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-generate")
  SET(COMPILE_ERPC_LIB OFF)
elseif(PGO STREQUAL "use")
  MESSAGE(STATUS "Profile-guided optimization (use mode) is enabled.")
  add_definitions(-fprofile-use -fprofile-correction)
  SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-use -fprofile-correction")
elseif(PGO STREQUAL "none")
  MESSAGE(STATUS "Profile-guided optimization is disabled.")
endif()

# Debug mode
if(DEBUG)
  MESSAGE(STATUS "Debugging is enabled. Perf will be low.")
  add_definitions(-g)
else(DEBUG)
  MESSAGE(STATUS "Debugging is disabled.")
  add_definitions(-DNDEBUG)
  add_definitions(-O3)
endif(DEBUG)

# LTO
if(LTO)
  MESSAGE(STATUS "LTO is enabled. Tests won't be compiled.")
  SET(COMPILE_ERPC_LIB OFF)
  add_definitions(-flto)
  SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
else(LTO)
  MESSAGE(STATUS "LTO is disabled. Performance will be low.")
endif(LTO)

set(SOURCES
  libhrd_cpp/hrd_conn.cc
  libhrd_cpp/hrd_util.cc)

set(APP atomics-sequencer)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${APP})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${APP})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${APP})
add_executable(${APP} ${APP}/main.cc ${SOURCES})
target_link_libraries(${APP} ${GTEST_LIBRARIES} ${LIBRARIES})
