CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE
LDFLAGS += -framework AudioToolbox -framework AudioUnit -framework CoreFoundation
LDFLAGS += -lpthread -lm
PREFIX  ?= /usr/local
JOBS    ?= 4

SRC  := $(wildcard src/*.c)
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
	cp vendor/snes9x/libretro/snes9x_libretro.dylib cores/

core-gba:
	mkdir -p vendor cores
	test -d vendor/mgba/.git || git clone --depth 1 https://github.com/libretro/mgba vendor/mgba
	$(MAKE) -C vendor/mgba -f Makefile.libretro CC='cc -DHAVE_LOCALE' -j$(JOBS)
	cp vendor/mgba/mgba_libretro.dylib cores/

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(BIN) kittybench

.PHONY: all clean core-snes core-gba install
