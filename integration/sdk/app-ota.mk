# Include from a product rootfs build after assigning APP_OTA_ROOT,
# APP_OTA_ROOTFS and APP_OTA_PUBLIC_KEY. Does not replace rcS or board networking.
# Native tools must have been built with the matching target libc/toolchain.
APP_OTA_NATIVE_STAGE ?= $(APP_OTA_ROOT)/.build/native/stage
.PHONY: app-ota-overlay
app-ota-overlay:
	python3 $(APP_OTA_ROOT)/tools/install_runtime.py \
		--destdir "$(APP_OTA_ROOTFS)" --native-stage "$(APP_OTA_NATIVE_STAGE)" \
		--public-key "$(APP_OTA_PUBLIC_KEY)"
