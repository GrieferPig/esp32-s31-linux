# GNU Make is the public interface; component-native build systems own compilation.
.DEFAULT_GOAL := help
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(ROOT)/mk/config.mk
ifneq ($(filter download fetch fetch-rootfs,$(MAKECMDGOALS)),)
ifneq ($(words $(MAKECMDGOALS)),1)
$(error Run make download separately before build targets; fetch and fetch-rootfs are also standalone preparation)
endif
endif
# Serialize the single output tree across independent invocations. Native builds share JOBS.
# Preparation and read-only help/check commands never acquire a device or build.
PREPARE_GOALS := download fetch fetch-rootfs toolchain-fetch toolchain-source btstack-source
LOCK_GOALS := resolved-config check check-fast check-host check-docs check-dt check-artifacts $(PREPARE_GOALS) flash-image radio-fs initramfs bootloader radio-module coremark buildroot-menuconfig btstack-notices flash-existing-all flash-all flash-existing-radio flash-existing-rootfs flash-radio flash-rootfs flash-linux flash-dtb flash-bootloader flash-opensbi flash-persist erase fetch fetch-rootfs all fullclean build-flash build image linux opensbi uboot rootfs radio-image radio-linux-payload radio-idf-deps lp-firmware s31-pie-cases persist radio-package build-manifest clean buildroot-clean buildroot-reconfigure
ifneq ($(filter $(LOCK_GOALS),$(MAKECMDGOALS)),)
ifeq ($(S31_BUILD_LOCKED),)
.PHONY: $(MAKECMDGOALS)
$(MAKECMDGOALS):
	+@case "$(filter-out --%,$(firstword $(MAKEFLAGS)))" in *n*) $(MAKE) --no-print-directory S31_BUILD_LOCKED=1 $@ ;; *) mkdir -p "$(OUT_ROOT)/.locks"; flock $(if $(filter $(PREPARE_GOALS),$(MAKECMDGOALS)),-x,-s) "$(OUT_ROOT)/.locks/sources.lock" flock "$(OUT_ROOT)/.locks/build.lock" $(MAKE) --no-print-directory S31_BUILD_LOCKED=1 $@ ;; esac
else
include $(ROOT)/mk/rules.mk
endif
else
include $(ROOT)/mk/rules.mk
endif
