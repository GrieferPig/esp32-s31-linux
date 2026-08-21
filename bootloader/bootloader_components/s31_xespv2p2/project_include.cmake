set(S31_XESPV2P2_MARCH
    "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2")

idf_build_set_property(COMPILE_OPTIONS "${S31_XESPV2P2_MARCH}" APPEND)
idf_build_set_property(LINK_OPTIONS "${S31_XESPV2P2_MARCH}" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=bootloader_console_init" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,-u,__wrap_bootloader_console_init" APPEND)

# This also covers IDF's generated project_elf_src file, which is outside of
# all component targets.
