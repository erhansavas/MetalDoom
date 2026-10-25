SHELL := /bin/sh
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
CC := xcrun clang
CXX := xcrun clang++
CONFIG ?= release
BUILD ?= build/$(CONFIG)
ifeq ($(CONFIG),release)
OPT := -O3 -mcpu=apple-m1 -flto=full
else ifeq ($(CONFIG),debug)
OPT := -O0
else ifeq ($(CONFIG),sanitize)
OPT := -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
else
$(error CONFIG must be release, debug or sanitize)
endif
APP := $(BUILD)/MetalDoom.app
VERSION := $(shell /usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' src/platform/macos/Info.plist)
DMG := MetalDoom-$(VERSION)-macOS-arm64.dmg
COMMON := -arch arm64 -isysroot $(SDK) -mmacosx-version-min=13.0 -Isrc/doom -Isrc/platform/macos -fwrapv -fno-strict-aliasing -fsigned-char -g $(OPT)
CFLAGS := $(COMMON) -std=gnu11 -DNORMALUNIX -DNATIVE_MACOS -Wall -Wextra -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-missing-field-initializers -Wno-sign-compare
CXXFLAGS := $(COMMON) -std=c++17 -fobjc-arc -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unknown-pragmas
CORE := $(filter-out src/doom/i_%.c,$(wildcard src/doom/*.c))
OBJS := $(patsubst src/doom/%.c,$(BUILD)/core/%.o,$(CORE))
PLATFORM := $(wildcard src/platform/macos/*.mm) $(wildcard src/platform/macos/*.cpp)
POBJS := $(patsubst src/platform/macos/%.mm,$(BUILD)/platform/%.o,$(filter %.mm,$(PLATFORM))) $(patsubst src/platform/macos/%.cpp,$(BUILD)/platform/%.o,$(filter %.cpp,$(PLATFORM))) $(BUILD)/test/core_checks.o
FRAMEWORKS := -framework AppKit -framework Metal -framework QuartzCore -framework CoreGraphics -framework AudioToolbox -framework CoreAudio -framework GameController -framework CoreHaptics
RESOURCES := $(APP)/Contents/Resources
.DELETE_ON_ERROR:
.DEFAULT_GOAL := all
.PHONY: all test check release dmg

all: $(APP)/Contents/MacOS/MetalDoom $(RESOURCES)/doom.metallib $(APP)/Contents/Info.plist $(APP)/Contents/PkgInfo $(RESOURCES)/LICENSE $(RESOURCES)/NOTICE.txt $(RESOURCES)/AppIcon.icns

$(BUILD)/core/%.o: src/doom/%.c Makefile
	@mkdir -p $(@D)
	@echo CC $<
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/platform/%.o: src/platform/macos/%.mm Makefile
	@mkdir -p $(@D)
	@echo CXX $<
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/platform/%.o: src/platform/macos/%.cpp Makefile
	@mkdir -p $(@D)
	@echo CXX $<
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/test/core_checks.o: tests/core_checks.c Makefile
	@mkdir -p $(@D)
	@echo CC $<
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(APP)/Contents/MacOS/MetalDoom: $(OBJS) $(POBJS)
	@mkdir -p $(@D)
	@echo LINK $@
	@$(CXX) $(COMMON) $^ $(FRAMEWORKS) -o $@
$(BUILD)/doom.air: shaders/doom.metal
	@mkdir -p $(@D)
	xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=13.0 -fmodules-cache-path=$(abspath $(BUILD))/metal-modules -c $< -o $@
$(RESOURCES)/doom.metallib: $(BUILD)/doom.air
	@mkdir -p $(@D)
	xcrun -sdk macosx metallib $^ -o $@
$(APP)/Contents/Info.plist: src/platform/macos/Info.plist
	@mkdir -p $(@D)
	cp $< $@
# The app icon: an original picture drawn by tools/icon.mm.
$(BUILD)/icon: tools/icon.mm
	@mkdir -p $(@D)
	$(CXX) -arch arm64 -isysroot $(SDK) -mmacosx-version-min=13.0 -std=c++17 -fobjc-arc $< -framework Foundation -framework CoreGraphics -framework ImageIO -o $@
$(RESOURCES)/AppIcon.icns: $(BUILD)/icon
	@mkdir -p $(@D)
	$(BUILD)/icon $(BUILD)/AppIcon.iconset
	iconutil -c icns $(BUILD)/AppIcon.iconset -o $@
$(APP)/Contents/PkgInfo:
	@mkdir -p $(@D)
	printf 'APPL????' > $@
$(RESOURCES)/LICENSE: LICENSE
	@mkdir -p $(@D)
	cp $< $@
$(RESOURCES)/NOTICE.txt: NOTICE.txt
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/tests/unit: tests/unit.cpp src/platform/macos/Portable.cpp src/platform/macos/MIDI.cpp src/platform/macos/Content.cpp src/platform/macos/Saves.cpp src/platform/macos/Feedback.cpp
	@mkdir -p $(@D)
	$(CXX) -arch arm64 -isysroot $(SDK) -mmacosx-version-min=13.0 -Isrc/platform/macos -std=c++17 -O1 -g -fsanitize=address,undefined $^ -o $@
test: all $(BUILD)/tests/unit
	$(BUILD)/tests/unit
	$(APP)/Contents/MacOS/MetalDoom --core-test
	$(APP)/Contents/MacOS/MetalDoom --audio-test
	$(APP)/Contents/MacOS/MetalDoom --metal-test

# Checks that need no GPU, audio device or display (continuous integration).
check: all $(BUILD)/tests/unit
	$(BUILD)/tests/unit
	$(APP)/Contents/MacOS/MetalDoom --core-test

# An ad-hoc signed app bundle for sharing (not notarized).
release: all
	codesign --force --sign - $(APP)
	codesign --verify --strict $(APP)

# The download: a disk image with the signed app and a link to Applications.
dmg: release
	@mkdir -p $(BUILD)/dmg
	ditto $(APP) $(BUILD)/dmg/MetalDoom.app
	ln -sfn /Applications $(BUILD)/dmg/Applications
	hdiutil create -volname MetalDoom -srcfolder $(BUILD)/dmg -fs HFS+ -format UDZO -ov $(BUILD)/$(DMG)
	cd $(BUILD) && shasum -a 256 $(DMG) > $(DMG).sha256

-include $(OBJS:.o=.d) $(POBJS:.o=.d)
