# ViPERFX_RE
# Builds libv4a_re.so for Android via NDK/CMake and packages Magisk module

# NDK: ANDROID_NDK_HOME > ANDROID_NDK_ROOT > ANDROID_HOME/ndk/<latest>
ANDROID_NDK_HOME ?= $(or $(ANDROID_NDK_ROOT),$(wildcard $(ANDROID_HOME)/ndk/$(shell ls $(ANDROID_HOME)/ndk 2>/dev/null | sort -V | tail -1)))
ifeq ($(ANDROID_NDK_HOME),)
  $(warning ANDROID_NDK_HOME not set. Set it to your NDK installation path.)
endif

NDK_TOOLCHAIN := $(ANDROID_NDK_HOME)/build/cmake/android.toolchain.cmake
MIN_SDK        := 21
ALL_ABIS       := armeabi-v7a arm64-v8a
BUILD_TYPE     := Release

# Version defaults from module.prop (overridable via CLI)
VERSION_NAME   ?= v2.1.2
VERSION_CODE   ?= 20261116

# ABI selection: pass ABI= to build a subset (comma or space separated)
COMMA := ,
ifdef ABI
  ABIS := $(subst $(COMMA), ,$(ABI))
else
  ABIS := $(ALL_ABIS)
endif

BUILD_DIR      := build
OUT_DIR        := out
MODULE_DIR     := module
MODULE_OUT     := $(OUT_DIR)/magisk_module
MODULE_ZIP     := $(OUT_DIR)/ViPER4Android-RE-$(VERSION_NAME).zip

ADB_DEVICE     := $(shell adb devices 2>/dev/null | awk 'NR==2 && $$2=="device"{print $$1}')
ADB            := adb$(if $(ADB_DEVICE), -s $(ADB_DEVICE),)

.PHONY: all clean libs module zip update-json deploy $(ALL_ABIS)

all: libs

# Build selected ABIs
libs: $(ABIS)

# Per-ABI build targets
$(ABIS):
	@echo "Building for ABI: $@"
	@mkdir -p $(BUILD_DIR)/$@
	cmake -B $(BUILD_DIR)/$@ \
		-DCMAKE_TOOLCHAIN_FILE=$(NDK_TOOLCHAIN) \
		-DANDROID_ABI=$@ \
		-DANDROID_PLATFORM=android-$(MIN_SDK) \
		-DANDROID_ARM_NEON=TRUE \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DVERSION_CODE=$(VERSION_CODE) \
		-DVERSION_NAME=$(VERSION_NAME) \
		.
	cmake --build $(BUILD_DIR)/$@ -- -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
	@mkdir -p $(OUT_DIR)
	cp $(BUILD_DIR)/$@/libv4a_re.so $(OUT_DIR)/libv4a_re_$@.so

# Prepare Magisk module directory structure
module: libs
	@echo "Preparing Magisk module..."
	@rm -rf $(MODULE_OUT)
	@mkdir -p $(MODULE_OUT)/common/files
	@cp -r $(MODULE_DIR)/META-INF $(MODULE_OUT)/
	@cp $(MODULE_DIR)/module.prop $(MODULE_OUT)/
	@cp $(MODULE_DIR)/customize.sh $(MODULE_OUT)/
	@cp $(MODULE_DIR)/service.sh $(MODULE_OUT)/
	@cp $(MODULE_DIR)/uninstall.sh $(MODULE_OUT)/
	@touch $(MODULE_OUT)/skip_mount
	@touch $(MODULE_OUT)/skip_mountify
	@cp $(MODULE_DIR)/LICENSE $(MODULE_OUT)/
	@cp -r $(MODULE_DIR)/common/* $(MODULE_OUT)/common/
	@sed -i.bak \
		-e 's/^version=.*/version=$(VERSION_NAME)/' \
		-e 's/^versionCode=.*/versionCode=$(VERSION_CODE)/' \
		$(MODULE_OUT)/module.prop && rm -f $(MODULE_OUT)/module.prop.bak
	@for abi in $(ABIS); do \
		cp $(OUT_DIR)/libv4a_re_$$abi.so $(MODULE_OUT)/common/files/; \
	done

# Create flashable zip
zip: update-json module
	@echo "Creating Magisk module zip..."
	@mkdir -p $(OUT_DIR)
	@rm -f $(MODULE_ZIP)
	@cd $(MODULE_OUT) && zip -r ../../$(MODULE_ZIP) .
	@echo "Module: $(MODULE_ZIP)"

# Update Magisk update metadata in-place
update-json:
	@echo "Updating Magisk update metadata..."
	@for file in magisk/update.json magisk/update-aidl.json; do \
		sed -i.bak \
			-e 's/"version": "[^"]*"/"version": "$(VERSION_NAME)"/' \
			-e 's/"versionCode": [0-9][0-9]*/"versionCode": $(VERSION_CODE)/' \
			-e 's|/download/[^/]*/|/download/$(VERSION_NAME)/|' \
			-e 's|ViPER4Android-RE-\(AIDL-\)\{0,1\}[^/"]*\.zip|ViPER4Android-RE-\1$(VERSION_NAME).zip|' \
			$$file && rm -f $$file.bak; \
	done

# Build single ABI
arm64: arm64-v8a
arm32: armeabi-v7a

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)

format:
	@echo "Formatting code with clang-format..."
	@find src/ -name '*.[ch]' -o -name '*.cpp' | xargs clang-format -i

help:
	@echo "ViPERFX_RE Build System"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - Android NDK (set ANDROID_NDK_HOME)"
	@echo "  - CMake"
	@echo ""
	@echo "Targets:"
	@echo "  make libs          Build libv4a_re.so for all ABIs (default)"
	@echo "  make arm64-v8a     Build for arm64 only"
	@echo "  make armeabi-v7a   Build for arm32 only"
	@echo "  make module        Prepare Magisk module directory"
	@echo "  make zip           Build + package Magisk flashable zip"
	@echo "  make update-json   Update Magisk update JSON version, versionCode, and zipUrl"
	@echo "  make clean         Remove build artifacts"
	@echo ""
	@echo "Options:"
	@echo "  ANDROID_NDK_HOME=<path>  NDK installation path"
	@echo "  MIN_SDK=<num>            Minimum SDK version (default: 21)"
	@echo "  BUILD_TYPE=<type>        CMake build type (default: Release)"
	@echo "  VERSION_NAME=<str>       Version name (default: from module.prop)"
	@echo "  VERSION_CODE=<num>       Version code (default: from module.prop)"
	@echo "  ABI=<abi[,abi]>          Build specific ABI(s) (default: all)"
