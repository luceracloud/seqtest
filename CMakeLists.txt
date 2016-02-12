#
# Copyright 2016 Lucera Financial Infrastructures, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use file except in compliance with the License.
# You may obtain a copy of the license at
#
#  http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# This program is used to stress test TCP connections, verifying that
# ordering constraints are preserved across a connection.  The intent
# is to validate correct function of a TCP proxy.
#

cmake_minimum_required(VERSION 2.8)

project(seqtest C)

include(CheckLibraryExists)
include(CheckFunctionExists)

add_executable(seqtest seqtest.c)

check_library_exists(socket getaddrinfo "" HAVE_LIBSOCKET)
if (HAVE_LIBSOCKET)
    target_link_libraries(seqtest socket)
    LIST(APPEND CMAKE_REQUIRED_LIBRARIES socket)
endif (HAVE_LIBSOCKET)

check_library_exists(nsl gethostbyname "" HAVE_LIBNSL)
if (HAVE_LIBNSL)
    target_link_libraries(seqtest nsl)
    LIST(APPEND CMAKE_REQUIRED_LIBRARIES nsl)
endif (HAVE_LIBNSL)

check_library_exists(rt clock_gettime "" HAVE_LIBRT)
if (HAVE_LIBRT)
    target_link_libraries(seqtest rt)
    LIST(APPEND CMAKE_REQUIRED_LIBRARIES rt)
endif (HAVE_LIBRT)

check_library_exists(pthread pthread_create "" HAVE_LIBPTHREAD)
if (HAVE_LIBPTHREAD)
    target_link_libraries(seqtest pthread)
    LIST(APPEND CMAKE_REQUIRED_LIBRARIES pthread)
endif (HAVE_LIBPTHREAD)

check_library_exists(m sqrt "" HAVE_LIBM)
if (HAVE_LIBM)
    target_link_libraries(seqtest m)
    LIST(APPEND CMAKE_REQUIRED_LIBRARIES m)
endif (HAVE_LIBM)

check_function_exists(strlcpy HAVE_STRLCPY)
if (HAVE_STRLCPY)
    add_definitions(-DHAVE_STRLCPY)
endif (HAVE_STRLCPY)

check_function_exists(gethrtime HAVE_GETHRTIME)
if (HAVE_GETHRTIME)
    add_definitions(-DHAVE_GETHRTIME)
endif (HAVE_GETHRTIME)

check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)
if (HAVE_CLOCK_GETTIME)
    add_definitions(-DHAVE_CLOCK_GETTIME)
endif (HAVE_CLOCK_GETTIME)

check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
if (HAVE_GETTIMEOFDAY)
    add_definitions(-DHAVE_GETTIMEOFDAY)
endif (HAVE_GETTIMEOFDAY)

install(TARGETS seqtest DESTINATION bin)
