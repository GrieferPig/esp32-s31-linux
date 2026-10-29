cmake_minimum_required(VERSION 3.16)

list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(s31_lp_firmware)
