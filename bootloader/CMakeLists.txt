# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(S31LINUX_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
if(EXISTS "$ENV{HOME}/s31linux")
	set(S31LINUX_DIR "$ENV{HOME}/s31linux")
endif()
get_filename_component(S31LINUX_DIR "${S31LINUX_DIR}" ABSOLUTE)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/esp_hosted_slave")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# IDF's unversioned xespv spelling selects mixed 2p1/2p2 assembler variants.
# Keep every bootloader object and the final link on the S31-validated 2p2 ISA.
set(S31_XESPV2P2_MARCH
    "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2")
idf_build_set_property(COMPILE_OPTIONS "${S31_XESPV2P2_MARCH}" APPEND)
idf_build_set_property(LINK_OPTIONS "${S31_XESPV2P2_MARCH}" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-mespv-spec=2p2" APPEND)
idf_build_set_property(LINK_OPTIONS "-mespv-spec=2p2" APPEND)
# IDF's generated project_elf_src file is not a component source, so it only
# inherits directory options rather than the IDF build properties above.
add_compile_options("${S31_XESPV2P2_MARCH}" "-mespv-spec=2p2")
add_link_options("${S31_XESPV2P2_MARCH}" "-mespv-spec=2p2")
# The second-stage bootloader is an IDF external subproject.  Pass it a local
# component so it receives the same ISA flags without modifying ESP-IDF.
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/bootloader_components/s31_xespv2p2" APPEND)
# ESP32-S31 maps instruction and read-only flash through the same unified XIP
# virtual window.  The two linker MEMORY aliases intentionally overlap.
idf_build_set_property(LINK_OPTIONS "-Wl,--no-check-sections" APPEND)
project(hello_world)

# The IDF application and the Linux payload execute at the same time on the two
# HP harts.  Keep the IDF XIP image out of the full Flash mapping
# (0x40000000..0x41000000).  Generate this from IDF's S31 script so that IDF
# remains the sole source for the rest of the memory layout.
set(IDF_S31_MEMORY_LD
    "$ENV{IDF_PATH}/components/esp_system/ld/esp32s31/memory.ld.in")
file(READ "${IDF_S31_MEMORY_LD}" S31_MEMORY_LD_CONTENT)
string(REPLACE "org = 0x40000020" "org = 0x42000020"
       S31_MEMORY_LD_CONTENT "${S31_MEMORY_LD_CONTENT}")
string(REPLACE "#include \"ld.common\""
       "#include \"$ENV{IDF_PATH}/components/esp_system/ld/ld.common\""
       S31_MEMORY_LD_CONTENT "${S31_MEMORY_LD_CONTENT}")
string(REPLACE "#include \"ld.hp_mem_defs\""
       "#include \"$ENV{IDF_PATH}/components/esp_system/ld/esp32s31/ld.hp_mem_defs\""
       S31_MEMORY_LD_CONTENT "${S31_MEMORY_LD_CONTENT}")
set(S31_MEMORY_LD "${CMAKE_CURRENT_BINARY_DIR}/s31linux.memory.ld.in")
file(WRITE "${S31_MEMORY_LD}" "${S31_MEMORY_LD_CONTENT}")

idf_component_get_property(ESP_SYSTEM_LIB esp_system COMPONENT_LIB)
get_target_property(ESP_SYSTEM_LINK_OPTIONS
                    "${ESP_SYSTEM_LIB}" INTERFACE_LINK_OPTIONS)
list(REMOVE_ITEM ESP_SYSTEM_LINK_OPTIONS "SHELL:-T memory.ld")
set_property(TARGET "${ESP_SYSTEM_LIB}" PROPERTY INTERFACE_LINK_OPTIONS
             "${ESP_SYSTEM_LINK_OPTIONS}")
target_linker_script("${ESP_SYSTEM_LIB}" INTERFACE "${S31_MEMORY_LD}")
# MEMORY and REGION_ALIAS declarations must precede IDF's sections.ld.
get_target_property(ESP_SYSTEM_LINK_OPTIONS
                    "${ESP_SYSTEM_LIB}" INTERFACE_LINK_OPTIONS)
list(REMOVE_ITEM ESP_SYSTEM_LINK_OPTIONS "SHELL:-T s31linux.memory.ld")
list(PREPEND ESP_SYSTEM_LINK_OPTIONS "SHELL:-T s31linux.memory.ld")
set_property(TARGET "${ESP_SYSTEM_LIB}" PROPERTY INTERFACE_LINK_OPTIONS
             "${ESP_SYSTEM_LINK_OPTIONS}")
