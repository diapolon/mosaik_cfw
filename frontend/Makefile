# Prototipo frontend arcade: launcher + runner.
# Sul Mac: make && ./run-mac.sh
# Per la console servirà CROSS=aarch64-linux-gnu- con il sysroot dell'SDK TrimUI.

CC      ?= cc
PKGS    := sdl2 SDL2_ttf SDL2_image
CFLAGS  += -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -D_DEFAULT_SOURCE -DARCADE_VERSION='"$(shell cat ../VERSION)"' -DUPDATE_DEFAULT_URL='"$(shell cat ../UPDATE_URL 2>/dev/null)"' $(shell pkg-config --cflags $(PKGS) SDL2_mixer)
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

BUILD   := build
COMMON  := src/common/ui.c src/common/json.c src/common/settings.c src/common/lang.c src/common/filters.c src/common/platform.c src/common/wifi.c

all: $(BUILD)/launcher $(BUILD)/runner $(BUILD)/gamewatch $(BUILD)/gamemenu

LAUNCHER := src/launcher/launcher.c src/launcher/games.c src/launcher/scraper.c src/launcher/romcheck.c src/launcher/music.c src/launcher/update.c src/launcher/stickcal.c

$(BUILD)/launcher: $(COMMON) $(LAUNCHER) src/common/*.h src/launcher/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(LAUNCHER) $(LDLIBS) $(shell pkg-config --libs SDL2_mixer) -lcurl

RUNNER := src/runner/runner.c src/runner/glshader.c

$(BUILD)/runner: $(COMMON) $(RUNNER) src/runner/*.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(RUNNER) $(LDLIBS) -ldl

$(BUILD)/gamemenu: $(COMMON) src/tools/gamemenu.c src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) src/tools/gamemenu.c $(LDLIBS)

$(BUILD)/gamewatch: src/tools/gamewatch.c | $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
