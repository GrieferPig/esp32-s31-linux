# Minimal ESP-IDF project used only to compile the Wi-Fi, Bluetooth,
# coexistence and PHY dependency closure consumed by the Linux radio driver.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only main and its declared radio dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
# IDF's unversioned xespv spelling selects mixed 2p1/2p2 assembler variants.
# Keep all generated objects and the final link on the S31-validated 2p2 ISA.
set(S31_XESPV2P2_MARCH
    "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2")
idf_build_set_property(COMPILE_OPTIONS "${S31_XESPV2P2_MARCH}" APPEND)
idf_build_set_property(LINK_OPTIONS "${S31_XESPV2P2_MARCH}" APPEND)
# IDF's generated project_elf_src file is not a component source, so it only
# inherits directory options rather than the IDF build properties above.
# The second-stage bootloader is an IDF external subproject.  Pass it a local
# component so it receives the same ISA flags without modifying ESP-IDF.
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/bootloader_components/s31_xespv2p2" APPEND)
# ESP32-S31 maps instruction and read-only flash through the same unified XIP
# virtual window.  The two linker MEMORY aliases intentionally overlap.
idf_build_set_property(LINK_OPTIONS "-Wl,--no-check-sections" APPEND)
project(s31_radio_idf_deps)
