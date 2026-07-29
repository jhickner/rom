CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE
LDFLAGS += -framework AudioToolbox -framework AudioUnit -framework CoreFoundation
LDFLAGS += -lpthread

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:.c=.o)
BIN  := emu

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c src/emu.h src/libretro.h
	$(CC) $(CFLAGS) -c $< -o $@

kittybench: tools/kittybench.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJ) $(BIN) kittybench

.PHONY: all clean
