# Makefile for TDRiveTOP - a TouchDesigner Custom TOP that renders .riv files
# via the Rive Renderer (Metal). Produces TDRiveTOP.plugin which TouchDesigner
# loads from its custom-operator plugin search paths.
#
# Build:    make
# Clean:    make clean
# Rebuild:  make rebuild
#
# Prereqs:  scripts/build_rive.sh has been run first so the Rive static libs
#           exist under third_party/rive-runtime/{out,renderer/out}/<config>.

CONFIG       ?= release
RIVE_DIR     := third_party/rive-runtime
# All Rive static libs land here - the renderer's premake5.lua builds the
# core runtime, decoders, harfbuzz, sheenbidi, etc. into a single out/.
RIVE_OUT     := $(RIVE_DIR)/renderer/out/$(CONFIG)

SRC_DIR  := TDRive
BUILD_DIR := build/$(CONFIG)
PLUGIN_NAME := TDRiveTOP
BUNDLE_DIR  := $(BUILD_DIR)/$(PLUGIN_NAME).plugin
BIN_DIR     := $(BUNDLE_DIR)/Contents/MacOS
PLUGIN_BIN  := $(BIN_DIR)/$(PLUGIN_NAME)

# Architecture. The Rive runtime's build_rive.sh only builds for the host
# arch by default, so we match it. To produce a universal plugin you'd have
# to build Rive twice (arm64 + x86_64) and lipo the archives together;
# easier to just override ARCHS on the command line when you need it.
ARCHS ?= -arch arm64

CXX       := clang++
CXXFLAGS  := -std=c++17 -fobjc-arc -fvisibility=hidden -O2 \
             -mmacosx-version-min=13.0 $(ARCHS) \
             -Wall -Wno-unused-parameter \
             -I$(SRC_DIR) \
             -I$(RIVE_DIR)/include \
             -I$(RIVE_DIR)/renderer/include \
             -I$(RIVE_DIR)/dependencies/skia/dependencies/depot_tools \
             -I$(RIVE_DIR)/dependencies

LDFLAGS  := -bundle $(ARCHS) -mmacosx-version-min=13.0 \
            -framework Foundation \
            -framework Metal \
            -framework QuartzCore \
            -framework CoreGraphics \
            -framework CoreFoundation \
            -framework CoreText \
            -framework ImageIO

# The Rive static libs end up in the per-project out/ directory. Glob them
# all - link order doesn't matter much for clang on macOS.
RIVE_LIBS := $(wildcard $(RIVE_OUT)/*.a)

OBJ := $(BUILD_DIR)/TDRiveTOP.o

.PHONY: all clean rebuild check-rive inspect

all: check-rive $(PLUGIN_BIN) $(BUNDLE_DIR)/Contents/Info.plist
	@echo ""
	@echo ">> Built $(BUNDLE_DIR)"

check-rive:
	@if [ -z "$(strip $(RIVE_LIBS))" ]; then \
		echo "ERROR: no static libs found under $(RIVE_OUT)."; \
		echo "Run scripts/build_rive.sh first."; \
		exit 1; \
	fi

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.mm
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PLUGIN_BIN): $(OBJ) $(RIVE_LIBS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(OBJ) $(RIVE_LIBS) -o $@

$(BUNDLE_DIR)/Contents/Info.plist: $(SRC_DIR)/Info.plist
	@mkdir -p $(BUNDLE_DIR)/Contents
	cp $< $@

clean:
	rm -rf build

rebuild: clean all

# Build and run the .riv inspector against demo.riv. Usage:
#   make inspect                 # uses demo.riv
#   make inspect RIV=other.riv
RIV ?= demo.riv

inspect: check-rive
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -fobjc-arc -O2 $(ARCHS) -mmacosx-version-min=13.0 \
	    -I$(SRC_DIR) \
	    -I$(RIVE_DIR)/include \
	    -I$(RIVE_DIR)/renderer/include \
	    scripts/inspect_riv.mm \
	    $(RIVE_LIBS) \
	    -framework Foundation -framework CoreFoundation \
	    -framework Metal -framework QuartzCore \
	    -framework CoreGraphics -framework CoreText -framework ImageIO \
	    -o $(BUILD_DIR)/inspect_riv
	@echo ""
	$(BUILD_DIR)/inspect_riv $(RIV)
