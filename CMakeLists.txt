cmake_minimum_required (VERSION 3.5)

IF(NOT CMAKE_BUILD_TYPE)
  #SET(CMAKE_BUILD_TYPE "DEBUG")
  SET(CMAKE_BUILD_TYPE "RELEASE")
  #SET(CMAKE_BUILD_TYPE "RELWITHDEBINFO")
  #SET(CMAKE_BUILD_TYPE "MINSIZEREL")
ENDIF()

set (EMU_VERSION_MAJOR 0)
set (EMU_VERSION_MINOR 1)
set (EMU_VERSION_PATCH 0)

find_package(GDAL)
find_package(ZLIB)

add_library(gdal_EMU emudataset.cpp)
# remove the leading "lib" as GDAL won't look for files with this prefix
set_target_properties(gdal_EMU PROPERTIES PREFIX "")

install (TARGETS gdal_EMU DESTINATION lib/gdalplugins)
