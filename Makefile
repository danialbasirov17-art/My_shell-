CC      ?= gcc
TARGET  := mysh
SRCS    := main.c lexer.c parser.c ast.c executor.c builtins.c jobs.c

# C23: gcc < 14 знает его как c2x
STD := $(shell echo 'int main(void){return 0;}' | $(CC) -std=c23 -x c - -fsyntax-only 2>/dev/null && echo c23 || echo c2x)

WARN     := -Wall -Wextra
BASE     := -std=$(STD) $(WARN) -D_POSIX_C_SOURCE=200809L
CFLAGS   ?= -O2
CFLAGS   := $(BASE) $(CFLAGS)

.PHONY: all debug clean test

all: $(TARGET)

$(TARGET): $(SRCS) shell.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

debug: clean
	$(MAKE) $(TARGET) CFLAGS='-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer' \
	    LDFLAGS='-fsanitize=address,undefined'

test: $(TARGET)
	sh tests/run_tests.sh ./$(TARGET)
	python3 tests/pty_jobs.py ./$(TARGET)

clean:
	rm -f $(TARGET)
