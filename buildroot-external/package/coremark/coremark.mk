# Keep the upstream CoreMark build system untouched. Its native command-line
# variables select the board flags and two pthread contexts. This packaged
# size-optimized result is not comparable to the former -O3/unrolled baseline.
define COREMARK_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" -C $(@D) \
		PORT_CFLAGS="$(TARGET_CFLAGS)" \
		XCFLAGS="-DMULTITHREAD=2 -DUSE_PTHREAD=1 -pthread" \
		PORT_DIR=linux$(if $(BR2_ARCH_IS_64),64) EXE= link
endef
