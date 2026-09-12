cmake_minimum_required(VERSION 3.22)
project(aquamarine VERSION 0.15.0 LANGUAGES C CXX)
if(NOT ANDROID)
  message(FATAL_ERROR "This entry point builds the Android platform backend")
endif()
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET hyprutils>=0.14.0 pixman-1 libdrm)
set(AQ_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(aquamarine SHARED
  ${AQ_ROOT}/src/backend/Backend.cpp
  ${AQ_ROOT}/src/backend/Android.cpp
  ${AQ_ROOT}/src/backend/Logger.cpp
  ${AQ_ROOT}/src/allocator/Allocator.cpp
  ${AQ_ROOT}/src/allocator/Swapchain.cpp
  ${AQ_ROOT}/src/buffer/Buffer.cpp
  ${AQ_ROOT}/src/input/Input.cpp
  ${AQ_ROOT}/src/misc/Attachment.cpp
  ${AQ_ROOT}/src/output/Output.cpp
  ${AQ_ROOT}/src/output/OutputTiming.cpp
  ${AQ_ROOT}/src/utils/Shared.cpp
  ${AQ_ROOT}/src/utils/FormatUtils.cpp)
target_include_directories(aquamarine PUBLIC ${AQ_ROOT}/include PRIVATE ${AQ_ROOT}/src/include)
target_compile_definitions(aquamarine PRIVATE AQUAMARINE_VERSION="0.15.0-android")
target_link_libraries(aquamarine PUBLIC PkgConfig::deps PRIVATE android log)
set(PREFIX ${CMAKE_INSTALL_PREFIX})
set(INCLUDE ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR})
set(AQUAMARINE_VERSION 0.15.0)
configure_file(aquamarine.pc.in aquamarine.pc @ONLY)
install(TARGETS aquamarine)
install(DIRECTORY ${AQ_ROOT}/include/aquamarine DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aquamarine.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
