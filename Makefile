CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra
PREFIX  ?= /usr/local
JOBS    ?= 4

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
AUDIO_SRC := src/audio_macos.c
CORE_EXT := dylib
CFLAGS += -D_DARWIN_C_SOURCE
LDFLAGS += -framework AudioToolbox -framework AudioUnit -framework CoreFoundation
LDFLAGS += -lpthread -lm
MGBA_BUILD_FLAGS := CC='cc -DHAVE_LOCALE'
else ifeq ($(UNAME_S),Linux)
AUDIO_SRC := src/audio_linux.c
CORE_EXT := so
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS += -lasound -ldl -lpthread -lm
else
$(error unsupported operating system: $(UNAME_S))
endif

SRC  := $(filter-out src/audio_linux.c src/audio_macos.c,$(wildcard src/*.c))
SRC  += $(AUDIO_SRC)
OBJ  := $(SRC:.c=.o)
BIN  := rom

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c src/rom.h src/libretro.h
	$(CC) $(CFLAGS) -c $< -o $@

kittybench: tools/kittybench.c
	$(CC) $(CFLAGS) $< -o $@

core-snes:
	mkdir -p vendor cores
	test -d vendor/snes9x/.git || git clone --depth 1 https://github.com/libretro/snes9x vendor/snes9x
	$(MAKE) -C vendor/snes9x/libretro -j$(JOBS)
	cp vendor/snes9x/libretro/snes9x_libretro.$(CORE_EXT) cores/

core-gba:
	mkdir -p vendor cores
	test -d vendor/mgba/.git || git clone --depth 1 https://github.com/libretro/mgba vendor/mgba
	$(MAKE) -C vendor/mgba -f Makefile.libretro $(MGBA_BUILD_FLAGS) -j$(JOBS)
	cp vendor/mgba/mgba_libretro.$(CORE_EXT) cores/

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f src/*.o $(BIN) kittybench

.PHONY: all clean core-snes core-gba install
