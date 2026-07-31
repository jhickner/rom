CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra
PREFIX  ?= $(HOME)/.local

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
AUDIO_SRC := src/audio_macos.c
HWGL_SRC  := src/hwgl_macos.c
CFLAGS += -D_DARWIN_C_SOURCE
LDFLAGS += -framework AudioToolbox -framework AudioUnit -framework CoreFoundation
LDFLAGS += -framework OpenGL
LDFLAGS += -lpthread -lm
else ifeq ($(UNAME_S),Linux)
AUDIO_SRC := src/audio_linux.c
HWGL_SRC  := src/hwgl_null.c
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS += -lasound -ldl -lpthread -lm
else
$(error unsupported operating system: $(UNAME_S))
endif

PLATFORM_SRC := src/audio_linux.c src/audio_macos.c \
                src/hwgl_macos.c src/hwgl_null.c
SRC  := $(filter-out $(PLATFORM_SRC),$(wildcard src/*.c))
SRC  += $(AUDIO_SRC) $(HWGL_SRC)
OBJ  := $(SRC:.c=.o)
BIN  := rom

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c src/rom.h src/libretro.h src/hwgl.h
	$(CC) $(CFLAGS) -c $< -o $@

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f src/*.o $(BIN)

.PHONY: all clean install
