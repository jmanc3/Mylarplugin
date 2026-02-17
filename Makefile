#MORE_DEBUG_FLAGS := -DDEBUGTITLEBAR

#MORE_DEBUG_FLAGS := -DDEBUGCONTAINERS
#when we upgrade to a new version these need to be handled one by one
#MORE_DEBUG_FLAGS := -DFORK_WARN


PLUGIN_NAME = mylar-desktop
MAKEFLAGS += -j16

# --- Source discovery ---
SOURCE_FILES := $(wildcard ./src/*.cpp ./src/*/*.cpp)
OBJECT_FILES := $(patsubst ./%, out/%, $(SOURCE_FILES:.cpp=.o))

# --- Pkg-config dependencies ---
PKG_FLAGS := $(shell pkg-config --cflags librsvg-2.0 libdrm hyprland pangocairo wayland-server  wayland-client wayland-cursor xkbcommon cairo dbus-1 alsa libpipewire-0.3)
PKG_LIBS  := $(shell pkg-config --libs   librsvg-2.0 libdrm hyprland pangocairo wayland-server wayland-client wayland-cursor xkbcommon cairo dbus-1 alsa libpipewire-0.3)

# --- Include paths ---
INCLUDE_FLAGS := -I./include

# --- Common compiler flags ---
COMMON_FLAGS := -std=c++2b -fPIC --no-gnu-unique $(INCLUDE_FLAGS) $(PKG_FLAGS)

# --- Build type flags ---
DEBUG_FLAGS := -g -O0 $(MORE_DEBUG_FLAGS)
RELEASE_FLAGS := -O3 -DNDEBUG

# --- Output ---
OUTPUT = out/$(PLUGIN_NAME).so

.PHONY: all debug release clean load unload

# --- Default target ---
all: debug

# --- Build modes ---
debug:   CXX_FLAGS := $(COMMON_FLAGS) $(DEBUG_FLAGS)
release: CXX_FLAGS := $(COMMON_FLAGS) $(RELEASE_FLAGS)

debug release: $(OUTPUT)

# --- Link step ---
$(OUTPUT): $(OBJECT_FILES)
	@echo "Linking $@"
	$(CXX) -shared $^ -o $@ $(PKG_LIBS) -lrt

# --- Compile step ---
out/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<"
	$(CXX) $(CXX_FLAGS) -c $< -o $@

# --- Utility targets ---
clean:
	$(RM) $(OUTPUT) $(OBJECT_FILES)

load: all unload
	hyprctl plugin load ${PWD}/$(OUTPUT)

unload:
	hyprctl plugin unload ${PWD}/$(OUTPUT)

