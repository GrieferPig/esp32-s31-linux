# Device actions are explicit and consume verified existing outputs only.
.PHONY: flash-existing-all flash-all build-flash flash-existing-radio flash-existing-rootfs flash-radio flash-rootfs flash-linux flash-dtb flash-bootloader flash-opensbi flash-persist erase
flash-existing-all:
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && python3 $(ROOT)/tools/device/flash.py --artifact-dir "$(ROOT)/dist/current" --slot all --port "$(PORT)" --baud "$(BAUD)"'
flash-all: flash-existing-all
build-flash: image
	$(MAKE) flash-existing-all
flash-existing-radio flash-radio flash-existing-rootfs flash-rootfs flash-linux flash-dtb flash-bootloader flash-opensbi:
	@echo 'Partial update refused: installed companion identity is unknown. Use flash-existing-all to write the verified matched slot set while preserving persist.' >&2
	@exit 2
flash-persist erase:
	@echo 'Destructive persist/erase is not part of firmware update. Use an explicit device maintenance procedure.' >&2
	@exit 2
