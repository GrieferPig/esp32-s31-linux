# Generate an empty, NOR-compatible JFFS2 image for the persist partition.
# This is separate from normal firmware updates so user data is not erased.
persist: check-layout | $(BUILD_DIR) $(IMAGES_DIR)
	@command -v mkfs.jffs2 >/dev/null || { echo "ERROR: mkfs.jffs2 is required" >&2; exit 1; }
	@staging=$$(mktemp -d "$(BUILD_DIR)/persist.XXXXXX"); \
	trap 'rmdir "$$staging"' EXIT; \
	mkfs.jffs2 -q -e 0x2000 --pad=$(PERSIST_PARTITION_SIZE) \
		-d "$$staging" -o $(PERSIST_IMG)


.PHONY: image
image: build
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && $(ROOT)/tools/build/flash_image.sh $(S31_LAYOUT_CFG) $(IMAGES_DIR)'
	$(MAKE) --no-print-directory S31_BUILD_LOCKED=1 build-manifest
	python3 $(ROOT)/tools/release/assets.py --prefix "$(IMAGES_DIR)" --checksums --output-root "$(BUILD_DIR)" --publish "$(ROOT)/dist"
