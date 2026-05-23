CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=

BIN := macsm
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

.PHONY: all clean test examples

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

test: $(BIN)
	python3 -m unittest discover -s tests

examples: $(BIN)
	./$(BIN) examples/hello.asm
	./$(BIN) examples/sum_loop.asm

clean:
	rm -f $(BIN)
	rm -rf build
