# SPDX-License-Identifier: BSD-2-Clause
P4_ESP_HOSTED_VERSION = 1df17f74d62eede4127785ae9414e03e48e62ebd
P4_ESP_HOSTED_SITE = $(call github,espressif,esp-hosted,$(P4_ESP_HOSTED_VERSION))
P4_ESP_HOSTED_LICENSE = GPL-2.0
P4_ESP_HOSTED_MODULE_SUBDIRS = esp_hosted_ng/host
P4_ESP_HOSTED_MODULE_MAKE_OPTS = target=sdio

$(eval $(kernel-module))
$(eval $(generic-package))
