CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L -pthread
INCLUDES = -Iinclude

SRC = src/main.c src/benchmark.c src/pipe_ipc.c src/fifo_ipc.c src/socket_ipc.c src/shm_ipc.c src/benchmark_suite.c src/optimizer.c src/shm_ring_ipc.c src/shm_optimizer.c
LDLIBS = -pthread
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
