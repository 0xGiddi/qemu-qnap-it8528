QEMU_VERSION ?= v10.0.0
QEMU_REPO := https://gitlab.com/qemu-project/qemu.git
QEMU_DIR := qemu
BUILD_DIR := $(QEMU_DIR)/build
INSTALL_DIR := $(CURDIR)/install
NPROC := $(shell nproc)

SOURCE_DIR := source
PATCH_DIR := patches

DEVICE_C := $(QEMU_DIR)/hw/misc/qnap_it8528.c
DEVICE_H := $(QEMU_DIR)/hw/misc/qnap_it8528.h

STAGE_FETCH     := .done_fetch
STAGE_COPY      := .done_copy
STAGE_PATCH     := .done_patch
STAGE_CONFIGURE := .done_configure

.PHONY: all fetch copy_sources patch configure build install clean distclean help

all: build

help:
	@echo "Targets:"
	@echo "  fetch - Clone QEMU sources"
	@echo "  copy_sources - Copy device files into QEMU tree"
	@echo "  patch - Apply patches to QEMU source"
	@echo "  configure - Run QEMU configure"
	@echo "  build - Build QEMU (default)"
	@echo "  install - Install to $(INSTALL_DIR)"
	@echo "  clean - Clean build artifacts"
	@echo "  distclean - Remove everything including QEMU clone"
	@echo ""
	@echo "Variables:"
	@echo "  QEMU_VERSION=$(QEMU_VERSION)"


$(STAGE_FETCH):
	@echo "Cloning QEMU $(QEMU_VERSION)"
	git clone --depth 1 --branch $(QEMU_VERSION) --recurse-submodules $(QEMU_REPO) $(QEMU_DIR)
	touch $@

fetch: $(STAGE_FETCH)

$(STAGE_COPY): $(STAGE_FETCH) $(SOURCE_DIR)/qnap_it8528.c $(SOURCE_DIR)/qnap_it8528.h
	@echo "Copying device sources into QEMU tree"
	cp $(SOURCE_DIR)/qnap_it8528.c $(DEVICE_C)
	cp $(SOURCE_DIR)/qnap_it8528.h $(DEVICE_H)
	touch $@

copy_sources: $(STAGE_COPY)

$(STAGE_PATCH): $(STAGE_COPY)
	@echo "Applying patches"
	@for p in $(sort $(wildcard $(PATCH_DIR)/*.patch)); do \
		echo "Applying $$p"; \
		patch -d $(QEMU_DIR) -p1 --forward < $$p || { \
			echo "ERROR: patch $$p failed"; exit 1; \
		}; \
	done
	touch $@

patch: $(STAGE_PATCH)

$(STAGE_CONFIGURE): $(STAGE_PATCH)
	@echo "Configuring QEMU"
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && ../configure --target-list=x86_64-softmmu --prefix=$(INSTALL_DIR) --disable-docs
	touch $@

configure: $(STAGE_CONFIGURE)

build: $(STAGE_CONFIGURE)
	@echo "Building QEMU"
	$(MAKE) -C $(BUILD_DIR) -j$(NPROC)
	@echo "Build complete"

install: build
	@echo "Installing to $(INSTALL_DIR)"
	$(MAKE) -C $(BUILD_DIR) install

update_sources:
	@echo "Updating device sources"
	cp $(SOURCE_DIR)/qnap_it8528.c $(DEVICE_C)
	cp $(SOURCE_DIR)/qnap_it8528.h $(DEVICE_H)
	$(MAKE) -C $(BUILD_DIR) -j$(NPROC)

clean:
	@echo "Cleaning build"
	$(MAKE) -C $(BUILD_DIR) clean
	rm -f $(STAGE_CONFIGURE)

distclean:
	@echo "Removing QEMU clone and stage files"
	rm -rf $(QEMU_DIR) $(INSTALL_DIR)
	rm -f $(STAGE_FETCH) $(STAGE_COPY) $(STAGE_PATCH) $(STAGE_CONFIGURE)
