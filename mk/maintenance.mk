.PHONY: buildroot-menuconfig buildroot-clean buildroot-reconfigure clean fullclean
buildroot-menuconfig: | $(BUILDROOT_OUT)
	$(BUILDROOT_MAKE) menuconfig
buildroot-clean buildroot-reconfigure:
	rm -rf $(BUILDROOT_OUT)
clean:
	rm -rf $(OPENSBI_OUT) $(LINUX_OUT) $(UBOOT_OUT) $(BUILDROOT_OUT) $(RADIO_OUT) $(LP_OUT) $(RADIO_IDF_BUILD) $(IMAGES_DIR) $(GENERATED_DIR) $(STAGING_DIR) $(REPORTS_DIR)
# Cache deletion is deliberately separate from build clean.
fullclean: clean
	@echo "Download/toolchain caches retained; remove explicitly when no build needs them."
