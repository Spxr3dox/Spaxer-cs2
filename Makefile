CXX      := g++
CXXFLAGS := -std=c++20 -O2 -pipe -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -MMD -MP

INCLUDES := -Isrc

PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gtk-layer-shell-0 cairo x11 xtst)
PKG_LIBS   := $(shell pkg-config --libs   gtk+-3.0 gtk-layer-shell-0 cairo x11 xtst)

GUI_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 x11)
GUI_LIBS   := $(shell pkg-config --libs   gtk+-3.0 x11)

SRC := \
  src/memory/process.cpp \
  src/sdk/offsets.cpp \
  src/sdk/dumper.cpp \
  src/config/settings.cpp \
  src/input/input.cpp \
  src/features/bomb_update.cpp \
  src/features/esp_update.cpp \
  src/features/cs2_crosshair.cpp \
  src/features/triggerbot.cpp \
  src/features/aimbot.cpp \
  src/features/rcs.cpp \
  src/features/unsafe.cpp \
  src/features/glow.cpp \
  src/features/chams.cpp \
  src/features/radar_hack.cpp \
  src/features/hitmarker.cpp \
  src/features/sound_esp.cpp \
  src/features/movement.cpp \
  src/render/camera.cpp \
  src/render/model_chams.cpp \
  src/overlay/world.cpp \
  src/overlay/main.cpp

GUI_SRC := \
  src/config/settings.cpp \
  src/gui/main.cpp

OBJDIR := build
OBJ := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SRC))
GUI_OBJ := $(patsubst %.cpp,$(OBJDIR)/gui-%.o,$(GUI_SRC))
TARGET := spaxer
GUI_TARGET := spaxer-gui

all: $(TARGET) $(GUI_TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PKG_LIBS) -lpthread

$(GUI_TARGET): $(GUI_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(GUI_LIBS) -lpthread

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PKG_CFLAGS) -c $< -o $@

$(OBJDIR)/gui-%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(GUI_CFLAGS) -c $< -o $@

-include $(OBJ:.o=.d) $(GUI_OBJ:.o=.d)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(GUI_TARGET)

.PHONY: all clean
