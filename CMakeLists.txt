cmake_minimum_required(VERSION 3.22)
project(plasma-nm-ts VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

add_subdirectory(service)
