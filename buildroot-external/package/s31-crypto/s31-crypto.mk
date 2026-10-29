################################################################################
# ESP32-S31 native crypto application backends
################################################################################
S31_CRYPTO_VERSION = 1.0
S31_CRYPTO_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../src/userspace/crypto
S31_CRYPTO_SITE_METHOD = local
S31_CRYPTO_LICENSE = Apache-2.0 (implementation), OASIS IPR Policy (interface headers)
S31_CRYPTO_LICENSE_FILES = LICENSE.Apache-2.0 pkcs11/pkcs11.h pkcs11/pkcs11t.h pkcs11/pkcs11f.h
S31_CRYPTO_DEPENDENCIES = openssl
S31_CRYPTO_INSTALL_STAGING = YES
S31_CRYPTO_UAPI = $(BR2_EXTERNAL_ESP32_S31_PATH)/../external/linux/include/uapi/linux/esp32s31-crypto.h

define S31_CRYPTO_BUILD_CMDS
	$(INSTALL) -D -m 0644 $(S31_CRYPTO_UAPI) $(@D)/include/linux/esp32s31-crypto.h
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -D_GNU_SOURCE -std=gnu11 \
		-Wall -Wextra -fPIC -shared -Wl,-z,defs -I$(@D)/include \
		$(@D)/provider.c $(@D)/ecdh.c $(@D)/hash.c $(@D)/aes.c \
		$(@D)/ecdsa.c $(@D)/ecdsa-store.c \
		$(@D)/rsa-key.c $(@D)/rsa-ops.c $(@D)/protected-mac.c $(@D)/ds-store.c -lcrypto -o $(@D)/s31.so
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -D_GNU_SOURCE -std=gnu11 \
		-Wall -Wextra -I$(@D)/include $(@D)/ds-pack.c -lcrypto -o $(@D)/s31-ds-pack
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -D_GNU_SOURCE -std=gnu11 \
		-Wall -Wextra -I$(@D)/include $(@D)/km-deploy.c -lcrypto -o $(@D)/s31-km-deploy
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -D_GNU_SOURCE -std=gnu11 \
		-Wall -Wextra -fPIC -fvisibility=hidden -shared -pthread -Wl,-z,defs \
		-Wl,-z,nodelete -Wl,-Bsymbolic-functions -Wl,-soname,s31-pkcs11.so \
		-I$(@D)/include $(@D)/p11-core.c $(@D)/p11-objects.c $(@D)/p11-ops.c \
		$(@D)/p11-functions.c -lcrypto -o $(@D)/s31-pkcs11.so
endef

define S31_CRYPTO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31.so $(TARGET_DIR)/usr/lib/ossl-modules/s31.so
	$(INSTALL) -D -m 0755 $(@D)/s31-km-deploy $(TARGET_DIR)/usr/bin/s31-km-deploy
	$(INSTALL) -D -m 0755 $(@D)/s31-ds-pack $(TARGET_DIR)/usr/bin/s31-ds-pack
	$(INSTALL) -D -m 0755 $(@D)/s31-pkcs11.so $(TARGET_DIR)/usr/lib/pkcs11/s31-pkcs11.so
	$(INSTALL) -D -m 0644 $(@D)/openssl.cnf $(TARGET_DIR)/usr/share/s31-crypto/openssl.cnf
endef

define S31_CRYPTO_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(S31_CRYPTO_UAPI) $(STAGING_DIR)/usr/include/linux/esp32s31-crypto.h
endef

$(eval $(generic-package))
