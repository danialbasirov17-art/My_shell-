CC ?= cc
CFLAGS ?= -std=c23 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc
SOURCES := $(wildcard src/*.c)
OBJ := $(SOURCES:.c=.o)

.PHONY: all debug clean
all: mysh
mysh: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -g -fsanitize=address,undefined" mysh
clean:
	$(RM) $(OBJ) mysh
