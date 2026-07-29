# Helper programs installed into the S31 root filesystem.
OUT_DIR ?= .

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS ?= -march=rv32imac_zicsr_zifencei -mabi=ilp32 -static -O2
DESTDIR ?= ../build/s31-rootfs

ESP_HOSTED_DIR := $(abspath ../esp-hosted-fg)
ESP_HOSTED_PROTO_DIR := $(ESP_HOSTED_DIR)/common/proto
ESP_HOSTED_PROTOBUF_DIR := $(ESP_HOSTED_DIR)/common/protobuf-c

PROGRAMS := $(OUT_DIR)/segfault $(OUT_DIR)/forktest $(OUT_DIR)/membench \
	$(OUT_DIR)/esp-hosted-ctl $(OUT_DIR)/ble-scan

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

$(OUT_DIR)/membench: membench.c
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $@
	-$(STRIP) $@

$(OUT_DIR)/ble-scan: ble_scan.c
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $@
	-$(STRIP) $@

$(OUT_DIR)/esp-hosted-ctl: esp_hosted_ctl.c \
		$(ESP_HOSTED_PROTO_DIR)/esp_hosted_rpc.pb-c.c \
		$(ESP_HOSTED_PROTOBUF_DIR)/protobuf-c/protobuf-c.c
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) \
		-I$(ESP_HOSTED_PROTO_DIR) \
		-I$(ESP_HOSTED_PROTOBUF_DIR) \
		$^ -o $@
	-$(STRIP) $@

install: all
	install -d $(DESTDIR)/sbin
	install -m 755 $(OUT_DIR)/segfault $(DESTDIR)/sbin/segfault
	install -m 755 $(OUT_DIR)/forktest $(DESTDIR)/sbin/forktest
	install -m 755 $(OUT_DIR)/membench $(DESTDIR)/sbin/membench
	install -m 755 $(OUT_DIR)/ble-scan $(DESTDIR)/sbin/ble-scan
	install -m 755 $(OUT_DIR)/esp-hosted-ctl $(DESTDIR)/sbin/esp-hosted-ctl
	ln -sfn esp-hosted-ctl $(DESTDIR)/sbin/test.out

clean:
	rm -f $(PROGRAMS)
