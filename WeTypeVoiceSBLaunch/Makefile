# Match RootHide + DockX: do NOT ship a broken old-ABI arm64e slice.
# Measured on iphone-10089-root (iOS 16.1.2): working SpringBoard tweaks (Atria,
# Choicy) carry an arm64e slice with caps USR00, while both local toolchains
# (clang 10 / L1ghtmann clang 11) emit caps 0x00. dyld prefers the arm64e slice
# of a fat image and refuses that ABI -> SpringBoard fails to boot -> RootHide
# safe mode. So ship arm64 only until a new-ABI-capable clang is available.
THEOS_PACKAGE_SCHEME ?= roothide
ARCHS ?= arm64
# dpkg on this device only accepts iphoneos-arm64e; the payload staying
# arm64-only is deliberate (see the ABI note above).
THEOS_PACKAGE_ARCH = iphoneos-arm64e
TARGET ?= iphone:clang:16.5:15.0
FINALPACKAGE ?= 1
include $(THEOS)/makefiles/common.mk
TWEAK_NAME = WeTypeVoiceSBLaunch
WeTypeVoiceSBLaunch_FILES = TweakEmpty.c
WeTypeVoiceSBLaunch_CFLAGS = -fno-modules -fblocks
WeTypeVoiceSBLaunch_LDFLAGS = -undefined dynamic_lookup
WeTypeVoiceSBLaunch_INSTALL_TARGET_PROCESSES = SpringBoard
include $(THEOS_MAKE_PATH)/tweak.mk

# Tools resolve from PATH first (macOS/Xcode ships install_name_tool, otool and
# ldid via brew), then fall back to the Linux theos toolchain.
INT := $(shell command -v install_name_tool 2>/dev/null || echo /opt/theos/toolchain/ios-arm64e-clang-toolchain-new/bin/install_name_tool)
LDID := $(shell command -v ldid 2>/dev/null || echo /opt/theos/toolchain/ios-arm64e-clang-toolchain-new/bin/ldid)
OT := $(shell command -v otool 2>/dev/null || echo /opt/theos/toolchain/ios-arm64e-clang-toolchain-new/bin/otool)
define WKS_PATCH_DYLIB
	$(INT) -id /Library/MobileSubstrate/DynamicLibraries/WeTypeVoiceSBLaunch.dylib "$(1)" || true
	$(INT) -change @rpath/CydiaSubstrate.framework/CydiaSubstrate @loader_path/.jbroot/usr/lib/libsubstrate.dylib "$(1)" || true
	$(INT) -delete_rpath /var/jb/Library/Frameworks "$(1)" 2>/dev/null || true
	$(INT) -delete_rpath /var/jb/usr/lib "$(1)" 2>/dev/null || true
	$(INT) -delete_rpath @loader_path/.jbroot/Library/Frameworks "$(1)" 2>/dev/null || true
	$(INT) -add_rpath @loader_path/.jbroot/usr/lib "$(1)" 2>/dev/null || true
	$(LDID) -S "$(1)"
endef

# Injection通路自检：id 必须是安装目录、不得残留 /var/jb 绝对依赖、
# 打印每片 cpusubtype/caps 以便和已知能注入的 tweak 对照。
define WKS_CHECK_DYLIB
	@echo "== WKS CHECK $(1)"; \
	$(OT) -D "$(1)" 2>/dev/null | sed 's/^/   id: /'; \
	$(OT) -L "$(1)" 2>/dev/null | sed 's/^/   dep: /'; \
	if $(OT) -L "$(1)" 2>/dev/null | grep -q "/var/jb"; then \
		echo "   !! absolute /var/jb dependency left"; \
	fi; \
	$(OT) -hf "$(1)" 2>/dev/null | grep -E "architecture|cpusubtype|capabilities" | sed 's/^/   abi: /'
endef

after-all::
	$(call WKS_PATCH_DYLIB,$(THEOS_OBJ_DIR)/WeTypeVoiceSBLaunch.dylib)
	$(call WKS_CHECK_DYLIB,$(THEOS_OBJ_DIR)/WeTypeVoiceSBLaunch.dylib)


