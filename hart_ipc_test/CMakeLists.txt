# 跨hart共享SRAM和中断测试项目
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(LINK_OPTIONS "-Wl,--no-check-sections" APPEND)
project(hart_ipc_test)
