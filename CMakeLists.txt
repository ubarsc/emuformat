cmake_minimum_required (VERSION 3.5)
project(EMU)

IF(NOT CMAKE_BUILD_TYPE)
  #SET(CMAKE_BUILD_TYPE "DEBUG")
  SET(CMAKE_BUILD_TYPE "RELEASE")
  #SET(CMAKE_BUILD_TYPE "RELWITHDEBINFO")
  #SET(CMAKE_BUILD_TYPE "MINSIZEREL")
ENDIF()

set (EMU_VERSION_MAJOR 0)
set (EMU_VERSION_MINOR 1)
set (EMU_VERSION_PATCH 0)

find_package(GDAL REQUIRED)
find_package(ZLIB REQUIRED)

option (BUILD_SHARED_LIBS "Build with shared library" ON)

include_directories("include")
add_library(gdal_EMU src/emudataset.cpp src/emuband.cpp src/emucompress.cpp 
    include/emudataset.h include/emuband.h include/emucompress.h)
# remove the leading "lib" as GDAL won't look for files with this prefix
set_target_properties(gdal_EMU PROPERTIES PREFIX "")
target_compile_features(gdal_EMU PUBLIC cxx_std_11)
target_link_libraries(gdal_EMU PUBLIC ${GDAL_LIBRARIES} ${ZLIB_LIBRARIES})

install (TARGETS gdal_EMU DESTINATION lib/gdalplugins)
