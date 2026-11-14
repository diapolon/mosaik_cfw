# Prototipo frontend arcade: launcher + runner.
# Sul Mac: make && ./run-mac.sh
# Per la console servirà CROSS=aarch64-linux-gnu- con il sysroot dell'SDK TrimUI.

CC      ?= cc
PKGS    := sdl2 SDL2_ttf SDL2_image
CFLAGS  += -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -D_DEFAULT_SOURCE -DARCADE_VERSION='"$(shell cat ../VERSION)"' -DUPDATE_DEFAULT_URL='"$(shell cat ../UPDATE_URL 2>/dev/null)"' $(shell pkg-config --cflags $(PKGS) SDL2_mixer)
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

BUILD   := build
# Video: FFmpeg di Homebrew (brew install ffmpeg); senza, l'app non si compila e basta
FFMPEG_PKGS := libavformat libavcodec libswscale libswresample libavutil
VIDEO_MAC := $(if $(shell pkg-config --exists $(FFMPEG_PKGS) && echo si),$(BUILD)/video)
COMMON  := src/common/ui.c src/common/json.c src/common/settings.c src/common/lang.c src/common/filters.c src/common/platform.c src/common/wifi.c

all: $(BUILD)/launcher $(BUILD)/runner $(BUILD)/gamewatch $(BUILD)/gamemenu $(BUILD)/files $(BUILD)/viewer $(BUILD)/music $(BUILD)/sdtest $(BUILD)/scraper $(BUILD)/sdgrow $(BUILD)/diag $(VIDEO_MAC)

LAUNCHER := src/launcher/launcher.c src/launcher/games.c src/launcher/scrape_client.c src/launcher/romcheck.c src/launcher/music.c src/launcher/update.c src/launcher/stickcal.c src/launcher/savesplit.c src/launcher/fileserver.c src/launcher/zipread.c src/launcher/store.c src/launcher/qrcode.c src/runner/romzip.c src/runner/gamecfg.c

# VERSION e UPDATE_URL entrano solo come -D: sono prerequisiti, così cambiarli ricompila (audit BUG-010).
# UPDATE_URL passa da .update-url, che cambia solo quando cambia il valore, anche se il file
# viene tolto (con $(wildcard) togliendolo non si ricompilava)
$(BUILD)/.update-url: FORCE | $(BUILD)
	@v="$$(cat ../UPDATE_URL 2>/dev/null)"; [ -f $@ ] && [ "$$(cat $@)" = "$$v" ] || printf '%s' "$$v" > $@
.PHONY: FORCE
FORCE:

$(BUILD)/launcher: $(COMMON) $(LAUNCHER) src/common/*.h src/launcher/*.h ../VERSION $(BUILD)/.update-url | $(BUILD)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags openssl) -o $@ $(COMMON) $(LAUNCHER) $(LDLIBS) $(shell pkg-config --libs SDL2_mixer) -lcurl -lz $(shell pkg-config --libs openssl)

RUNNER := src/runner/runner.c src/runner/glshader.c src/runner/romzip.c src/runner/gamecfg.c

$(BUILD)/runner: $(COMMON) $(RUNNER) src/runner/*.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(RUNNER) $(LDLIBS) -ldl -lz

$(BUILD)/gamemenu: $(COMMON) src/tools/gamemenu.c src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) src/tools/gamemenu.c $(LDLIBS)

# app dello Store (pacchetti in Arcade/Apps): Esplora file
APPKIT := src/apps/appkit.c

$(BUILD)/files: $(COMMON) $(APPKIT) src/apps/files.c src/launcher/zipread.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(APPKIT) src/apps/files.c src/launcher/zipread.c $(LDLIBS) -lpthread -lz

$(BUILD)/viewer: $(COMMON) $(APPKIT) src/apps/viewer.c src/launcher/zipread.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(APPKIT) src/apps/viewer.c src/launcher/zipread.c $(LDLIBS) -lz

$(BUILD)/music: $(COMMON) $(APPKIT) src/apps/music.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(APPKIT) src/apps/music.c $(LDLIBS) $(shell pkg-config --libs SDL2_mixer)

$(BUILD)/video: $(COMMON) $(APPKIT) src/apps/video.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(FFMPEG_PKGS)) -o $@ $(COMMON) $(APPKIT) src/apps/video.c $(LDLIBS) $(shell pkg-config --libs $(FFMPEG_PKGS))

# Diagnostica (scarica 4 MB per la prova del Wi-Fi: libcurl)
$(BUILD)/diag: $(COMMON) $(APPKIT) src/apps/diag.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(APPKIT) src/apps/diag.c $(LDLIBS) -lcurl -lpthread

# Spazio della SD (contiene m2/sdgrow.c, solo su Linux; sul Mac una scheda simulata)
$(BUILD)/sdgrow: $(COMMON) $(APPKIT) src/apps/sdgrow.c ../m2/sdgrow.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(APPKIT) src/apps/sdgrow.c $(LDLIBS)

# Scarica immagini (app dello Store senza schermata, la avvia il launcher: launcher/scrape_client.c)
SCRAPER_APP := src/apps/scraper.c src/launcher/scraper.c src/common/json.c src/runner/romzip.c
$(BUILD)/scraper: $(SCRAPER_APP) src/launcher/scraper.h src/launcher/games.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SCRAPER_APP) $(LDLIBS) -lcurl -lz

$(BUILD)/sdtest: $(COMMON) $(APPKIT) src/apps/sdtest.c src/apps/appkit.h src/common/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(APPKIT) src/apps/sdtest.c $(LDLIBS) -lpthread

$(BUILD)/gamewatch: src/tools/gamewatch.c | $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
