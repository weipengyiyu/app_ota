MBEDTLS := mbedtls-3.6.4
MBEDTLS_ARCHIVE := $(OPEN_SOURCE_DIR)/mbedtls/$(MBEDTLS).tar.bz2
MBEDTLS_SHA256 := ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
OTA_VERIFY_BUILDDIR := $(ROOTFS_BUILDDIR)/ota_verify
MBEDTLS_SRCDIR := $(ROOTFS_BUILDDIR)/$(MBEDTLS)
MBEDTLS_BUILDDIR := $(MBEDTLS_SRCDIR)/build
MBEDTLS_BUILD_STAMP := $(MBEDTLS_BUILDDIR)/.built
OTA_VERIFY_BIN := $(OTA_VERIFY_BUILDDIR)/ota_verify_sig
TARGET_CC := $(CC)
TARGET_AR := $(AR)
TARGET_RANLIB := $(RANLIB)
TARGET_CC_PATH := $(shell command -v $(TARGET_CC))
TARGET_AR_PATH := $(shell command -v $(TARGET_AR))
TARGET_RANLIB_PATH := $(shell command -v $(TARGET_RANLIB))

rootfs_unstrip: $(ROOTFS_PREFIX)/usr/bin/ota_verify_sig

clean: ota_verify.clean

ota_verify.clean:
	rm -rf $(OTA_VERIFY_BUILDDIR) $(MBEDTLS_SRCDIR)

$(ROOTFS_PREFIX)/usr/bin/ota_verify_sig: $(OTA_VERIFY_BIN)
	$(call mkdir,$(@D))
	cp -af $< $@
	chmod 0755 $@

$(ROOTFS_PREFIX)/etc/ota_pubkey.pem: $(TRUSTED_RELEASE_PUBKEY)
	$(call mkdir,$(@D))
	cp -af $< $@
	chmod 0644 $@

$(OTA_VERIFY_BIN): $(MBEDTLS_BUILD_STAMP) \
	$(PROJECT_ROOT)/native/verify/ota_verify_sig.c
	$(call mkdir,$(@D))
	$(TARGET_CC) -Os -static -ffunction-sections -fdata-sections \
		-I$(MBEDTLS_SRCDIR)/include \
		$(PROJECT_ROOT)/native/verify/ota_verify_sig.c \
		$(MBEDTLS_BUILDDIR)/library/libmbedx509.a \
		$(MBEDTLS_BUILDDIR)/library/libmbedcrypto.a \
		-Wl,--gc-sections -o $@

$(MBEDTLS_BUILDDIR)/library/libmbedcrypto.a \
$(MBEDTLS_BUILDDIR)/library/libmbedx509.a: $(MBEDTLS_BUILD_STAMP)

$(MBEDTLS_BUILD_STAMP): $(MBEDTLS_SRCDIR)/.extracted
	cmake -S $(MBEDTLS_SRCDIR) -B $(MBEDTLS_BUILDDIR) \
		-DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_C_COMPILER=$(TARGET_CC_PATH) \
		-DCMAKE_AR=$(TARGET_AR_PATH) \
		-DCMAKE_RANLIB=$(TARGET_RANLIB_PATH) \
		-DCMAKE_BUILD_TYPE=MinSizeRel \
		-DBUILD_SHARED_LIBS=OFF \
		-DENABLE_PROGRAMS=OFF \
		-DENABLE_TESTING=OFF \
		-DMBEDTLS_FATAL_WARNINGS=OFF \
		-DCMAKE_C_FLAGS='-Os -ffunction-sections -fdata-sections'
	$(MAKE) -C $(MBEDTLS_BUILDDIR) mbedcrypto mbedx509
	touch $@

$(MBEDTLS_SRCDIR)/.extracted: $(MBEDTLS_ARCHIVE)
	$(call mkdir,$(@D))
	echo "$(MBEDTLS_SHA256)  $(MBEDTLS_ARCHIVE)" | sha256sum -c -
	tar xf $(MBEDTLS_ARCHIVE) -C $(ROOTFS_BUILDDIR)
	touch $@