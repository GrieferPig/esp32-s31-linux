OUT_DIR ?= .

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS ?= -march=rv32imac_zicsr_zifencei -mabi=ilp32 -static -O2
DESTDIR ?= ../build/s31-initramfs-root

PROGRAMS := $(OUT_DIR)/segfault $(OUT_DIR)/forktest

.PHONY: all clean install

all: $(PROGRAMS)

$(OUT_DIR)/segfault: segfault.c
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $@
	-$(STRIP) $@

$(OUT_DIR)/forktest: forktest.c
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $@
	-$(STRIP) $@

install: all
	install -d $(DESTDIR)/sbin
	install -m 755 $(OUT_DIR)/segfault $(DESTDIR)/sbin/segfault
	install -m 755 $(OUT_DIR)/forktest $(DESTDIR)/sbin/forktest

clean:
	rm -f $(PROGRAMS)
