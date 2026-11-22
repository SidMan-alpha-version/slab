CC = clang
CFLAGS = -O2 -Wall -Wextra -std=c11 -pthread -Iinclude -Isrc -fsanitize=address -g
SRC = src/lexer.c src/ast.c src/parser.c src/analyzer.c src/codegen.c src/main.c
OBJ = $(SRC:.c=.o)
TARGET = slab

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	bash tests/run_tests.sh

clean:
	rm -f $(OBJ) $(TARGET) _slab_* tests/valid/*.c tests/valid/*.bin tests/valid/*.csv tests/invalid/*.c

.PHONY: all clean test
