cmake_minimum_required(VERSION 3.22)
project(plasma-nm-ts VERSION 0.1.2 LANGUAGES C CXX)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

option(BUILD_PLASMA_NM_PLUGIN "Build the plasma-nm VPN UI plugin" ON)

add_subdirectory(service)
if(BUILD_PLASMA_NM_PLUGIN)
    add_subdirectory(ui)
endif()
