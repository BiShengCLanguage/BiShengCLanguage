cmake_minimum_required(VERSION 3.13.4)
add_compile_options(-fbsc-experimental)

# for macOS
set(CMAKE_BUILD_WITH_INSTALL_NAME_DIR ON)

# standalone or in-tree
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(LIBCBS_STANDALONE ON)
else()
  set(LIBCBS_STANDALONE OFF)
endif()

#standalone build
if(LIBCBS_STANDALONE)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "
      No build type selected. You need to pass -DCMAKE_BUILD_TYPE=<type> in order to configure libcbs.")
  endif()

  # Let cmake understand .cbs file extension and its features.It has to be set before project(...)
  set(CMAKE_C_SYSROOT_FLAG_CODE "list(APPEND CMAKE_C_SOURCE_FILE_EXTENSIONS cbs)")
  project(libcbs LANGUAGES C)

  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

  message(STATUS "Standalone libcbs build")
endif()

# set compiler path
if(LIBCBS_STANDALONE)
  if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${CMAKE_BINARY_DIR}/bin/clang" CACHE FILEPATH "Path to the C compiler")
  else()
    message(STATUS "Using user-provided CMAKE_C_COMPILER: ${CMAKE_C_COMPILER}")
  endif()
  if(NOT EXISTS "${CMAKE_C_COMPILER}")
    message(FATAL_ERROR "Compiler not found at: '${CMAKE_C_COMPILER}'")
  endif()
# in tree
else()
  set(CMAKE_C_COMPILER "${CMAKE_BINARY_DIR}/bin/clang")
endif()

# Options : cmake -DENABLE_BSC_LIB_FUTURE=ON
option(ENABLE_BSC_LIB_FUTURE "Enable BSC Lib future features" OFF)
if(ENABLE_BSC_LIB_FUTURE)
  message(STATUS "BSC Lib future features is enabled")
endif()

set(LIBCBS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(LIBC_TARGET_OS ${CMAKE_SYSTEM_NAME})
string(TOLOWER ${LIBC_TARGET_OS} LIBC_TARGET_OS)
set(CMAKE_EXE_LINKER_FLAGS "-lpthread")

foreach(flag "-fcoverage-mapping" "-fno-lifetime-dse")
  string(REPLACE "${flag}" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE "${flag}" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
include(LLVMLibCBSRules)

# Compile the source code, create multiple object files from each folder.
add_subdirectory(src)
add_subdirectory(test)

#stand alone install, must after add_subdirectory(src)
if(LIBCBS_STANDALONE)
  include(GNUInstallDirs)
endif()