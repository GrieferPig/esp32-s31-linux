# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(S31LINUX_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
if(EXISTS "$ENV{HOME}/s31linux")
	set(S31LINUX_DIR "$ENV{HOME}/s31linux")
endif()
get_filename_component(S31LINUX_DIR "${S31LINUX_DIR}" ABSOLUTE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(hello_world)

