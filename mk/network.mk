PAHO_EMBEDDED := paho.mqtt.embedded-c-1.1.0
PAHO_ARCHIVE := $(OPEN_SOURCE_DIR)/mqtt/paho.mqtt.embedded-c-v1.1.0.tar.gz
PAHO_SHA256 := 20fd257add47988bde01168feaf250c3fccf5fc366fd3b19ca915dde360c3ede
PAHO_SRCDIR := $(ROOTFS_BUILDDIR)/$(PAHO_EMBEDDED)
PAHO_PATCH := $(PROJECT_ROOT)/native/mqtt/paho-embedded-gcc12.patch

CURL := curl-8.21.0
CURL_ARCHIVE := $(OPEN_SOURCE_DIR)/http/$(CURL).tar.xz
CURL_SHA256 := aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
CURL_SRCDIR := $(ROOTFS_BUILDDIR)/$(CURL)
CURL_BUILDDIR := $(ROOTFS_BUILDDIR)/curl-build
CURL_BUILD_STAMP := $(CURL_BUILDDIR)/.built

OTA_NET_SRCDIR := $(PROJECT_ROOT)/native/mqtt
OTA_BOARD_DIR := $(PROJECT_ROOT)/runtime/agent
OTA_MQTT_BUILDDIR := $(ROOTFS_BUILDDIR)/ota-net
OTA_MQTT_BIN := $(OTA_MQTT_BUILDDIR)/ota-mqtt-sub
MBEDTLS_PREFIX := $(ROOTFS_BUILDDIR)/mbedtls-prefix
MBEDTLS_PREFIX_STAMP := $(MBEDTLS_PREFIX)/.installed
MBEDTLS_TLS_LIBRARY := $(MBEDTLS_BUILDDIR)/library/libmbedtls.a

OTA_TARGET_CC := $(CC)
OTA_TARGET_AR := $(AR)
OTA_TARGET_RANLIB := $(RANLIB)
OTA_TARGET_STRIP := $(STRIP)

PAHO_PACKET_SRC := \
	$(PAHO_SRCDIR)/MQTTPacket/src/MQTTPacket.c \
	$(PAHO_SRCDIR)/MQTTPacket/src/MQTTDeserializePublish.c \
	$(PAHO_SRCDIR)/MQTTPacket/src/MQTTConnectClient.c \
	$(PAHO_SRCDIR)/MQTTPacket/src/MQTTSubscribeClient.c \
	$(PAHO_SRCDIR)/MQTTPacket/src/MQTTSerializePublish.c \
	$(PAHO_SRCDIR)/MQTTPacket/src/MQTTUnsubscribeClient.c

rootfs_unstrip: \
	$(ROOTFS_PREFIX)/usr/bin/ota-mqtt-sub \
	$(ROOTFS_PREFIX)/usr/bin/ota-http \
	$(ROOTFS_PREFIX)/usr/sbin/ota-agentd \
	$(ROOTFS_PREFIX)/etc/ota/agent.conf

clean: ota_net.clean

ota_net.clean:
	rm -rf $(PAHO_SRCDIR) $(CURL_SRCDIR) $(CURL_BUILDDIR) \
		$(OTA_MQTT_BUILDDIR) $(MBEDTLS_PREFIX)

$(ROOTFS_PREFIX)/usr/bin/ota-mqtt-sub: $(OTA_MQTT_BIN)
	$(call mkdir,$(@D))
	cp -af $< $@
	chmod 0755 $@

$(ROOTFS_PREFIX)/usr/bin/ota-http: $(CURL_BUILD_STAMP)
	$(call mkdir,$(@D))
	cp -af $(CURL_BUILDDIR)/src/curl $@
	chmod 0755 $@

$(ROOTFS_PREFIX)/usr/sbin/ota-agentd: $(OTA_BOARD_DIR)/ota-agentd
	$(call mkdir,$(@D))
	cp -af $< $@
	chmod 0755 $@

$(ROOTFS_PREFIX)/etc/ota/agent.conf: $(PROJECT_ROOT)/config/agent.conf.example
	$(call mkdir,$(@D))
	cp -af $< $@
	chmod 0600 $@
	$(call mkdir,$(@D)/pki)
	chmod 0700 $(@D)/pki

$(OTA_MQTT_BIN): $(PAHO_SRCDIR)/.extracted $(MBEDTLS_PREFIX_STAMP) \
		$(OTA_NET_SRCDIR)/ota_mqtt_sub.c \
		$(OTA_NET_SRCDIR)/ota_mqtt_platform.c \
		$(OTA_NET_SRCDIR)/ota_mqtt_platform.h
	$(call mkdir,$(@D))
	$(OTA_TARGET_CC) -Os -Wall -Wextra -ffunction-sections -fdata-sections \
		-DMQTTCLIENT_PLATFORM_HEADER=ota_mqtt_platform.h \
		-I$(OTA_NET_SRCDIR) \
		-I$(PAHO_SRCDIR)/MQTTClient-C/src \
		-I$(PAHO_SRCDIR)/MQTTPacket/src \
		-I$(MBEDTLS_PREFIX)/include \
		$(OTA_NET_SRCDIR)/ota_mqtt_sub.c \
		$(OTA_NET_SRCDIR)/ota_mqtt_platform.c \
		$(PAHO_SRCDIR)/MQTTClient-C/src/MQTTClient.c \
		$(PAHO_PACKET_SRC) \
		$(MBEDTLS_PREFIX)/lib/libmbedtls.a \
		$(MBEDTLS_PREFIX)/lib/libmbedx509.a \
		$(MBEDTLS_PREFIX)/lib/libmbedcrypto.a \
		-Wl,--gc-sections -o $@

$(PAHO_SRCDIR)/.extracted: $(PAHO_ARCHIVE) $(PAHO_PATCH)
	$(call mkdir,$(@D))
	echo "$(PAHO_SHA256)  $(PAHO_ARCHIVE)" | sha256sum -c -
	tar xf $(PAHO_ARCHIVE) -C $(ROOTFS_BUILDDIR)
	patch -d $(PAHO_SRCDIR) -p1 < $(PAHO_PATCH)
	touch $@

$(CURL_SRCDIR)/.extracted: $(CURL_ARCHIVE)
	$(call mkdir,$(@D))
	echo "$(CURL_SHA256)  $(CURL_ARCHIVE)" | sha256sum -c -
	tar xf $(CURL_ARCHIVE) -C $(ROOTFS_BUILDDIR)
	touch $@

$(MBEDTLS_PREFIX_STAMP): $(MBEDTLS_BUILD_STAMP) $(MBEDTLS_TLS_LIBRARY)
	$(call mkdir,$(@D)/include)
	$(call mkdir,$(@D)/lib)
	cp -a $(MBEDTLS_SRCDIR)/include/mbedtls $(MBEDTLS_PREFIX)/include/
	cp -a $(MBEDTLS_SRCDIR)/include/psa $(MBEDTLS_PREFIX)/include/
	cp -a $(MBEDTLS_BUILDDIR)/library/libmbedtls.a \
		$(MBEDTLS_BUILDDIR)/library/libmbedx509.a \
		$(MBEDTLS_BUILDDIR)/library/libmbedcrypto.a $(MBEDTLS_PREFIX)/lib/
	touch $@

$(MBEDTLS_TLS_LIBRARY): $(MBEDTLS_BUILD_STAMP)
	$(MAKE) -C $(MBEDTLS_BUILDDIR) mbedtls

$(CURL_BUILD_STAMP): $(CURL_SRCDIR)/.extracted $(MBEDTLS_PREFIX_STAMP)
	$(call mkdir,$(@D))
	cd $(CURL_BUILDDIR) && \
		CC=$(OTA_TARGET_CC) AR=$(OTA_TARGET_AR) \
		RANLIB=$(OTA_TARGET_RANLIB) STRIP=$(OTA_TARGET_STRIP) \
		CPPFLAGS="-I$(MBEDTLS_PREFIX)/include" \
		LDFLAGS="-L$(MBEDTLS_PREFIX)/lib" \
		$(CURL_SRCDIR)/configure \
			$(CURL_HOST_ARG) \
			--disable-shared --enable-static --disable-docs --disable-manual \
			--enable-http \
			--disable-dict --disable-file --disable-ftp --disable-gopher \
			--disable-imap --disable-ipfs --disable-ldap --disable-ldaps \
			--disable-mqtt --disable-pop3 --disable-rtsp --disable-smb \
			--disable-smtp --disable-telnet --disable-tftp --disable-websockets \
			--disable-proxy --disable-doh --disable-netrc \
			--disable-alt-svc --disable-hsts \
			--disable-threaded-resolver --disable-ipv6 \
			--disable-socketpair \
			--with-mbedtls=$(MBEDTLS_PREFIX) \
			--without-zlib --without-brotli --without-zstd \
			--without-libpsl --without-libidn2 --without-nghttp2 \
			--without-ca-bundle --without-ca-path
	$(MAKE) -C $(CURL_BUILDDIR) -j4
	touch $@
