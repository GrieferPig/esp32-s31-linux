.PHONY: uboot-esptool-check
uboot-esptool-check:
	@command -v "$(ESPTOOL)" >/dev/null || { echo "ERROR: esptool not found: $(ESPTOOL). Install standalone esptool or set ESPTOOL to its executable." >&2; exit 1; }
	@"$(ESPTOOL)" --chip "$(TARGET)" elf2image --help >/dev/null
