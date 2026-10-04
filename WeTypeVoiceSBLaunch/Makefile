# SpringBoard refuses images the injector does not recognise: our earlier builds
# linked only libSystem and carried an absolute install-name, while every tweak
# that actually loads here (ExtVoice, Atria) links CydiaSubstrate via @rpath and
# keeps theos' default @rpath id. Build shape is now identical to ExtVoice.
THEOS_PACKAGE_SCHEME ?= roothide
ARCHS ?= arm64 arm64e
# device dpkg only accepts iphoneos-arm64e
THEOS_PACKAGE_ARCH = iphoneos-arm64e
TARGET ?= iphone:clang:16.5:14.0
FINALPACKAGE ?= 1
include $(THEOS)/makefiles/common.mk
TWEAK_NAME = WeTypeVoiceSBLaunch
WeTypeVoiceSBLaunch_FILES = TweakEmpty.c
WeTypeVoiceSBLaunch_CFLAGS = -fno-modules -fblocks
WeTypeVoiceSBLaunch_FRAMEWORKS = Foundation UIKit CydiaSubstrate
WeTypeVoiceSBLaunch_LDFLAGS = -undefined dynamic_lookup
WeTypeVoiceSBLaunch_INSTALL_TARGET_PROCESSES = SpringBoard
include $(THEOS_MAKE_PATH)/tweak.mk

OT := $(shell command -v otool 2>/dev/null || echo /opt/theos/toolchain/ios-arm64e-clang-toolchain-new/bin/otool)

after-all::
	@echo "== WKS CHECK"; \
	$(OT) -hv $(THEOS_OBJ_DIR)/$(TWEAK_NAME).dylib 2>/dev/null | sed 's/^/   abi: /'; \
	$(OT) -D $(THEOS_OBJ_DIR)/$(TWEAK_NAME).dylib 2>/dev/null | sed 's/^/   id: /'; \
	$(OT) -L $(THEOS_OBJ_DIR)/$(TWEAK_NAME).dylib 2>/dev/null | sed 's/^/   dep: /'
