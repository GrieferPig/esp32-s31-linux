.PHONY: check-layout check-host check-docs check-dt check-fast build-manifest
check-layout:
	python3 $(ROOT)/tools/checks/layout.py
check-host: check-layout
	python3 -m unittest discover -s $(ROOT)/tools/tests -v
	python3 -m unittest discover -s $(ROOT)/tests -v
	sh $(ROOT)/tests/esp32-config/test-gpio.sh
check-docs:
	$(MAKE) -C $(ROOT)/docs html BUILDDIR="$(REPORTS_DIR)/docs" SPHINXOPTS="-n -W --keep-going"
check-dt:
	python3 $(ROOT)/tools/checks/devicetree.py --cross-compile "$(CROSS_COMPILE)" --output "$(REPORTS_DIR)/devicetree"
check-fast: check-host check-docs check-dt
build-manifest:
	python3 $(ROOT)/tools/release/manifest.py --compiler "$(CC)" --idf "$(dir $(IDF_EXPORT))" --output-root "$(BUILD_DIR)" --output "$(IMAGES_DIR)/build-manifest.json"
