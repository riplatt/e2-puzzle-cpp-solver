cmake_minimum_required(VERSION 3.20)
project(EdgeMatchingPuzzleSolver VERSION 1.0.0 LANGUAGES CXX)

# Set platform indicator variables.
find_program(NIXOS "nixos-version")
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(DARWIN TRUE)
endif()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Aggressive optimization flags for maximum performance
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_BUILD_TYPE Release)

# Static linking for portability
if(!NIXOS AND !DARWIN)
  # MacOSX does not support -static, see: https://stackoverflow.com/questions/3801011/ld-library-not-found-for-lcrt0-o-on-osx-10-6-with-gcc-clang-static-flag
  set(CMAKE_EXE_LINKER_FLAGS "-static")
endif()

# Find required packages
if(NIXOS OR DARWIN)
  # MacOSX has "built-in" pthreads, see: https://stackoverflow.com/questions/54587052/cmake-on-mac-could-not-find-threads-missing-threads-found
  set(CMAKE_THREAD_LIBS_INIT "-lpthread")
  set(CMAKE_HAVE_THREADS_LIBRARY 1)
  set(CMAKE_USE_WIN32_THREADS_INIT 0)
  set(CMAKE_USE_PTHREADS_INIT 1)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()
find_package(Threads REQUIRED)

# Production single-threaded solver with hints support
add_executable(solver src/solver.cpp)
target_link_libraries(solver PRIVATE Threads::Threads)

# Production parallel solver with hints support
add_executable(parallel_solver src/parallel_solver.cpp)
target_link_libraries(parallel_solver PRIVATE Threads::Threads)


# Set executable properties for all solvers
set_target_properties(solver parallel_solver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

# Optional: Enable Link Time Optimization if available
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)
if(supported)
    set_property(TARGET solver parallel_solver PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "Link Time Optimization enabled")
else()
    message(STATUS "Link Time Optimization not supported: ${error}")
endif()

# Display build configuration
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Installation rules
install(TARGETS solver parallel_solver DESTINATION bin)
install(DIRECTORY data/ DESTINATION share/edge-puzzle-solver/data)
