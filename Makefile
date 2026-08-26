CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L
INCLUDES = -Iinclude

SRC = src/main.c src/benchmark.c src/pipe_ipc.c src/fifo_ipc.c
OBJ = $(SRC:.c=.o)
TARGET = fastipc

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
