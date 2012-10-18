# **********************************************************
# Copyright (c) 2011 Google, Inc.    All rights reserved.
# Copyright (c) 2009-2010 VMware, Inc.    All rights reserved.
# **********************************************************

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
# 
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
# 
# * Neither the name of VMware, Inc. nor the names of its contributors may be
#   used to endorse or promote products derived from this software without
#   specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
# DAMAGE.

# Packages up a DynamoRIO release via ctest command mode.
# This is a cross-platform script that replaces package.sh and
# package.bat: though we assume can use Unix Makefiles on Windows,
# in order to build in parallel.

# Usage: invoke via "ctest -S package.cmake,build=<build#>"
# See other available args below

cmake_minimum_required (VERSION 2.2)

# arguments are a ;-separated list (must escape as \; from ctest_run_script())
# required args:
set(arg_build "")      # build #
# optional args:
set(arg_ubuild "")     # unique build #: if not specified, set to arg_build
set(arg_version "")    # version #
set(arg_outdir ".")    # directory in which to place deliverables
set(arg_cacheappend "")# string to append to every build's cache
set(arg_no64 OFF)      # skip the 64-bit builds?
# also takes args parsed by runsuite_common_pre.cmake, in particular:
set(arg_preload "")    # cmake file to include prior to each 32-bit build
set(arg_preload64 "")  # cmake file to include prior to each 64-bit build

foreach (arg ${CTEST_SCRIPT_ARG})
  if (${arg} MATCHES "^build=")
    string(REGEX REPLACE "^build=" "" arg_build "${arg}")
  endif ()
  if (${arg} MATCHES "^ubuild=")
    string(REGEX REPLACE "^ubuild=" "" arg_ubuild "${arg}")
  endif ()
  if (${arg} MATCHES "^version=")
    string(REGEX REPLACE "^version=" "" arg_version "${arg}")
  endif ()
  if (${arg} MATCHES "^outdir=")
    string(REGEX REPLACE "^outdir=" "" arg_outdir "${arg}")
  endif ()
  if (${arg} MATCHES "^cacheappend=")
    string(REGEX REPLACE "^cacheappend=" "" arg_cacheappend "${arg}")
  endif ()
  if (${arg} MATCHES "^no64")
    set(arg_no64 ON)
  endif ()
endforeach (arg)

if ("${arg_build}" STREQUAL "")
  message(FATAL_ERROR "build number not set: pass as build= arg")
endif()
if ("${arg_ubuild}" STREQUAL "")
  set(arg_ubuild "${arg_build}")
endif()

set(CTEST_PROJECT_NAME "DynamoRIO")
set(cpack_project_name "DynamoRIO")
set(run_tests OFF)
set(CTEST_SOURCE_DIRECTORY "${CTEST_SCRIPT_DIRECTORY}/..")
include("${CTEST_SCRIPT_DIRECTORY}/../suite/runsuite_common_pre.cmake")

set(base_cache "
  BUILD_NUMBER:STRING=${arg_build}
  UNIQUE_BUILD_NUMBER:STRING=${arg_ubuild}
  ${arg_cacheappend}
  ${base_cache}
  ")

# version is optional
if (arg_version)
  set(base_cache "${base_cache}
    VERSION_NUMBER:STRING=${arg_version}")
endif (arg_version)

# open-source now, no reason for debug to not be internal as well
testbuild_ex("release-32" OFF "" OFF ON "")
# note that we do build TOOLS so our DynamoRIOTarget{32,64}.cmake files match
# and the DynamoRIOTarget{32,64}-debug.cmake file is complete (i#390)
testbuild_ex("debug-32" OFF "
  DEBUG:BOOL=ON
  INTERNAL:BOOL=ON
  BUILD_DOCS:BOOL=OFF
  BUILD_DRGUI:BOOL=OFF
  BUILD_SAMPLES:BOOL=OFF
  " OFF ON "")
if (NOT arg_no64)
  testbuild_ex("release-64" ON "" OFF ON "")
  testbuild_ex("debug-64" ON "
    DEBUG:BOOL=ON
    INTERNAL:BOOL=ON
    BUILD_DOCS:BOOL=OFF
    BUILD_DRGUI:BOOL=OFF
    BUILD_SAMPLES:BOOL=OFF
    " OFF ON "")
endif (NOT arg_no64)

set(build_package ON)
include("${CTEST_SCRIPT_DIRECTORY}/../suite/runsuite_common_post.cmake")

# copy the final archive into cur dir
# "cmake -E copy" only takes one file so use 'z' => .tar.gz or .zip
file(GLOB results ${last_build_dir}/DynamoRIO-*z*)
if (EXISTS "${results}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${results} "${arg_outdir}")
else (EXISTS "${results}")
  message(FATAL_ERROR "failed to create package")
endif (EXISTS "${results}")
