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

add_executable(seqtest seqtest.c)

check_library_exists(socket getaddrinfo "" HAVE_LIBSOCKET)
if (HAVE_LIBSOCKET)
    target_link_libraries(seqtest socket)
endif (HAVE_LIBSOCKET)

check_library_exists(nsl gethostbyname "" HAVE_LIBNSL)
if (HAVE_LIBNSL)
    target_link_libraries(seqtest nsl)
endif (HAVE_LIBNSL)

check_library_exists(rt clock_gettime "" HAVE_LIBRT)
if (HAVE_LIBRT)
    target_link_libraries(seqtest rt)
endif (HAVE_LIBRT)

check_library_exists(pthread pthread_create "" HAVE_LIBPTHREAD)
if (HAVE_LIBPTHREAD)
    target_link_libraries(seqtest pthread)
endif (HAVE_LIBPTHREAD)

check_library_exists(m sqrt "" HAVE_LIBM)
if (HAVE_LIBM)
    target_link_libraries(seqtest m)
endif (HAVE_LIBM)

install(TARGETS seqtest DESTINATION bin)
