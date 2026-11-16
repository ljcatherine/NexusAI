# stable-diffusion.cpp static library for the NexusAI PS5 title.
#
# Derived from PSllama app/sd/CMakeLists.txt
# (https://github.com/mansanitizer/psllama, commit d079717), GPL-3.0-or-later.
# NexusAI is GPL-3.0-or-later.
#
# Builds stable-diffusion.cpp against the same ggml that the llama.cpp
# cross-build already produced, so the title links one ggml. Configure with the
# payload SDK's prospero.cmake toolchain. prepare-build.sh copies this file to
# <destination>/third_party/sd-ps5/CMakeLists.txt, so the defaults resolve against
# the clones under <destination>/third_party. Override on the command line:
#   -DSDCPP_SOURCE=<stable-diffusion.cpp checkout>   (default ../stable-diffusion.cpp)
#   -DGGML_SOURCE=<llama.cpp>/ggml                   (default ../llama.cpp/ggml, ggml headers)
#   -DGGML_BUILD=<llama cross-build>/ggml/src        (required, contains libggml.a)
#
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(sd_ps5 C CXX)

# Absolute paths so the imported ggml target does not depend on the working directory.
get_filename_component(NEXUS_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(SDCPP_SOURCE "${NEXUS_THIRD_PARTY}/stable-diffusion.cpp" CACHE PATH "stable-diffusion.cpp source tree")
set(GGML_SOURCE "${NEXUS_THIRD_PARTY}/llama.cpp/ggml" CACHE PATH "ggml source directory (headers)")
# U-44 decides the llama.cpp build directory per host (NixOS), so there is no default.
set(GGML_BUILD "" CACHE PATH "Directory containing libggml.a from the llama.cpp cross-build (required)")
if(NOT GGML_BUILD)
    message(FATAL_ERROR "GGML_BUILD is unset: pass -DGGML_BUILD=<llama cross-build>/ggml/src (the directory holding libggml.a)")
endif()

add_library(ggml STATIC IMPORTED GLOBAL)
set_target_properties(ggml PROPERTIES
    IMPORTED_LOCATION "${GGML_BUILD}/libggml.a"
    INTERFACE_INCLUDE_DIRECTORIES "${GGML_SOURCE}/include"
    INTERFACE_COMPILE_DEFINITIONS "GGML_MAX_NAME=160")

set(SD_GGML_SOURCE_DIR "${GGML_SOURCE}" CACHE PATH "" FORCE)
set(SD_USE_UPSTREAM_GGML ON CACHE BOOL "" FORCE)
set(SD_USE_SYSTEM_GGML ON CACHE BOOL "" FORCE)
set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SD_WEBP OFF CACHE BOOL "" FORCE)
set(SD_WEBM OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDCPP_SOURCE}" sdcpp)
