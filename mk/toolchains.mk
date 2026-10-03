# Source initialization is explicit and never runs concurrently with builds.
download:
	@if [ -n "$(filter-out download fetch,$(MAKECMDGOALS))" ]; then echo "Run make download separately before build targets" >&2; exit 2; fi
	@echo "--- Download ---"
	git -C $(ROOT) submodule update --init --recursive

toolchain-fetch: | $(BUILD_DIR)
	@set -eu; \
	if [ "$(TOOLCHAIN_RELEASE_TAG)" = local ]; then \
		test -x "$(CC)" || { echo "ERROR: no local toolchain installed" >&2; exit 1; }; \
		echo "Using explicitly selected local toolchain at $(TOOLCHAIN_PREFIX)"; \
		exit 0; \
	fi; \
	if [ -x "$(CC)" ] && [ "$(TOOLCHAIN_RELEASE_TAG)" = latest ]; then \
		echo "Using installed toolchain at $(TOOLCHAIN_PREFIX)"; \
		exit 0; \
	fi; \
	mkdir -p "$(dir $(TOOLCHAIN_ARCHIVE))" "$(TOOLCHAIN_DIR)"; \
	release_tag="$(TOOLCHAIN_RELEASE_TAG)"; \
	if [ "$$release_tag" = latest ]; then \
		release_tag=$$(curl --fail --location --retry 3 --silent --show-error "$(TOOLCHAIN_RELEASE_API)" | sed -n 's/^[[:space:]]*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p'); \
	fi; \
	if [ -z "$$release_tag" ]; then \
		echo "ERROR: failed to resolve the latest toolchain release tag" >&2; exit 1; \
	fi; \
	release_url="$(TOOLCHAIN_RELEASE_DOWNLOAD_BASE)/$$release_tag/$(TOOLCHAIN_RELEASE_ASSET)"; \
	release_sha256_url="$$release_url.sha256"; \
	installed_tag=$$(cat "$(TOOLCHAIN_PREFIX)/.release" 2>/dev/null || true); \
	if [ -z "$$installed_tag" ] && [ -d "$(TOOLCHAIN_PREFIX)" ]; then \
		installed_tag=$$(find "$(TOOLCHAIN_PREFIX)" -maxdepth 1 -type f -name '.release-*' -printf '%f\n' 2>/dev/null | sed 's/^\.release-//' | head -n 1); \
	fi; \
	if [ "$$installed_tag" = "$$release_tag" ] && [ -x "$(CC)" ]; then \
		echo "Toolchain release $$release_tag is already installed"; \
		exit 0; \
	fi; \
	echo "Installing toolchain release $$release_tag"; \
	curl --fail --location --retry 3 --output "$(TOOLCHAIN_ARCHIVE).part" "$$release_url"; \
	curl --fail --location --retry 3 --output "$(TOOLCHAIN_ARCHIVE).sha256.part" "$$release_sha256_url"; \
	expected_hash=$$(awk 'NR == 1 { print $$1; exit }' "$(TOOLCHAIN_ARCHIVE).sha256.part"); \
	printf '%s\n' "$$expected_hash" | grep -Eq '^[0-9a-fA-F]{64}$$' || { echo "ERROR: invalid release checksum" >&2; exit 1; }; \
	printf '%s  %s\n' "$$expected_hash" "$(TOOLCHAIN_ARCHIVE).part" | sha256sum --check -; \
	mv "$(TOOLCHAIN_ARCHIVE).part" "$(TOOLCHAIN_ARCHIVE)"; \
	rm -f "$(TOOLCHAIN_ARCHIVE).sha256.part"; \
	staging=$$(mktemp -d "$(TOOLCHAIN_DIR)/.riscv32-esp-linux-musl.XXXXXX"); \
	trap 'chmod -R u+w "$$staging" 2>/dev/null || true; rm -rf "$$staging"' EXIT; \
	tar -xJf "$(TOOLCHAIN_ARCHIVE)" -C "$$staging"; \
	test -x "$$staging/bin/riscv32-esp-linux-musl-gcc"; \
	printf '%s\n' "$$release_tag" > "$$staging/.release"; \
	printf '%s\n' "$$release_tag" > "$$staging/.release-$$release_tag"; \
	chmod u-w "$$staging"; \
	if [ -e "$(TOOLCHAIN_PREFIX)" ]; then \
		backup="$(TOOLCHAIN_PREFIX).previous.$$(date -u +%Y%m%d%H%M%S)"; \
		mv "$(TOOLCHAIN_PREFIX)" "$$backup"; \
		echo "Previous toolchain retained at $$backup"; \
	fi; \
	mv "$$staging" "$(TOOLCHAIN_PREFIX)"; \
	trap - EXIT; \
	"$(CC)" --version | head -n 1

toolchain-source:
	python3 $(ROOT)/tools/build/toolchain.py --ct-ng-dir "$(CROSSTOOL_NG_DIR)" --jobs "$(JOBS)" --prefix "$(TOOLCHAIN_PREFIX)" --work-dir "$(CACHE_DIR)/build/crosstool-ng" --sources-dir "$(CACHE_DIR)/downloads/toolchain-src" --force


.PHONY: toolchain toolchain-fetch download toolchain-source
toolchain:
	@test -x "$(CC)" || { echo "Compiler missing: run make fetch, or set TOOLCHAIN_PREFIX in local.mk" >&2; exit 1; }
	@if [ "$(TOOLCHAIN_RELEASE_TAG)" != local ] && [ "$(S31_ALLOW_UNPINNED)" != 1 ]; then \
		installed=$$(cat "$(TOOLCHAIN_PREFIX)/.release" 2>/dev/null || true); \
		test "$$installed" = "$(TOOLCHAIN_RELEASE_TAG)" || { echo "Installed toolchain $$installed does not match locked $(TOOLCHAIN_RELEASE_TAG); run make fetch or explicitly select a local experiment" >&2; exit 1; }; \
	fi
