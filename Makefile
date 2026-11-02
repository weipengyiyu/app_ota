.DEFAULT_GOAL := help
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(PROJECT_ROOT)/.build/native
ROOTFS_BUILDDIR := $(BUILD_DIR)/work
ROOTFS_PREFIX := $(BUILD_DIR)/stage
OPEN_SOURCE_DIR := $(PROJECT_ROOT)/third_party
JOBS ?= 4
CROSS_COMPILE ?=
ifneq ($(strip $(CROSS_COMPILE)),)
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib
STRIP := $(CROSS_COMPILE)strip
CURL_HOST_ARG := --host=$(shell $(CC) -dumpmachine)
endif
CC ?= gcc
AR ?= ar
RANLIB ?= ranlib
STRIP ?= strip
mkdir = mkdir -p $(1)
include $(PROJECT_ROOT)/mk/verify.mk
include $(PROJECT_ROOT)/mk/network.mk

.PHONY: help native verify check install lab-control bundle factory
help:
	@echo 'APP-OTA standalone: make native | verify | check | install DESTDIR=/staging/rootfs'
	@echo '  CROSS_COMPILE=/path/to/arm-linux-gnueabi- BUILD_DIR=/separate/build'
	@echo '  make bundle APP_SOURCE=/app VERSION=V1.0.0 COUNTER=1 SIGN_KEY=/external/key.pem'
	@echo '  make factory APP_SOURCE=/app FACTORY_STAGE=/output/out/factory'
	@echo '  make lab-control: create flat lab-only NFS control bundle (no deployment)'
native: rootfs_unstrip
verify: $(ROOTFS_PREFIX)/usr/bin/ota_verify_sig
check: native
	python3 -m unittest discover -s $(PROJECT_ROOT)/tests -v
install:
	python3 $(PROJECT_ROOT)/tools/install_runtime.py --destdir "$(DESTDIR)" --native-stage "$(ROOTFS_PREFIX)" --public-key "$(TRUSTED_RELEASE_PUBKEY)"
lab-control:
	python3 $(PROJECT_ROOT)/tools/install_runtime.py --lab-control "$(PROJECT_ROOT)/.build/lab-control"
bundle:
	SIGN_KEY="$(SIGN_KEY)" sh $(PROJECT_ROOT)/host/release/build_app_bundle.sh "$(APP_SOURCE)" "$(VERSION)" "$(COUNTER)"
factory:
	sh $(PROJECT_ROOT)/host/release/prepare_factory_image.sh "$(APP_SOURCE)" "$(FACTORY_STAGE)"
