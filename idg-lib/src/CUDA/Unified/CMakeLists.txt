# Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
# SPDX-License-Identifier: GPL-3.0-or-later

project(cuda-unified)

# Set headers
include_directories (
    ${IDG_CUDA_INCLUDE_DIR}
    ${CUDA_INCLUDE_DIR}
)

# Set headers
set (
    ${PROJECT_NAME}_headers
    Unified.h
)

# Set sources
set (
    ${PROJECT_NAME}_sources
    Unified.cpp
)

# Set build target
add_library(
    ${PROJECT_NAME} OBJECT ${${PROJECT_NAME}_headers}
                           ${${PROJECT_NAME}_sources}
)
if (NOT BUILD_STATIC_LIBS)
set_target_properties (
    ${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "-fPIC" SOVERSION 0)
endif()

# Set install
install (
   FILES
   ${${PROJECT_NAME}_headers}
   DESTINATION
   include/CUDA/Unified
)
