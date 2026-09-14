# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2025 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


find_package(libralf REQUIRED)
include(GNUInstallDirs)

set (PACKAGE_SRCS RalfPackageHandler.cpp)
set (LIB_VER 2.0.1)
add_library(Package SHARED ${PACKAGE_SRCS})
if(DEFINED BUILD_VER)
    message(STATUS "Using build version: ${BUILD_VER}")
    set_target_properties(Package PROPERTIES VERSION ${BUILD_VER} SOVERSION 1)
else()
    set_target_properties(Package PROPERTIES VERSION ${LIB_VER} SOVERSION 1)
endif()

if(DISABLE_DEPENDENCY_CHECK)
    message(STATUS "Dependency check is disabled")
    target_compile_definitions(Package PRIVATE DISABLE_DEPENDENCY_CHECK)
else()
    message(STATUS "Dependency check is enabled")
endif()

if(DEFINED RDK_PACKAGE_CERT_PATH AND NOT RDK_PACKAGE_CERT_PATH STREQUAL "")
    message(STATUS "Using custom package certificate path: ${RDK_PACKAGE_CERT_PATH}")
    target_compile_definitions(Package PRIVATE RDK_PACKAGE_CERT_PATH="${RDK_PACKAGE_CERT_PATH}")
else()
    message(STATUS "Using default package certificate path")
endif()


if(DAC_APP_PATH)
    target_compile_definitions(Package PRIVATE DAC_APP_PATH="${DAC_APP_PATH}")
endif()

if(BUILD_REFERENCE)
    target_compile_definitions(Package PRIVATE BUILD_REFERENCE="${BUILD_REFERENCE}")
endif()

target_link_libraries(Package PRIVATE ralf jsoncpp)
target_include_directories(Package PUBLIC
    $<BUILD_INTERFACE:${LIBPACKAGE_BASE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS Package DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES
        ${LIBPACKAGE_BASE_DIR}/include/RalfPackageImpl.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

