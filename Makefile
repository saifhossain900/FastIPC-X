CC = gcc

BASE_CFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L -pthread

CFLAGS = $(BASE_CFLAGS) -DFASTIPCX_BUILD_FLAGS='"$(BASE_CFLAGS)"'

INCLUDES = -Iinclude

SRC = src/main.c \
      src/benchmark.c \
      src/pipe_ipc.c \
      src/fifo_ipc.c \
      src/socket_ipc.c \
      src/shm_ipc.c \
      src/benchmark_suite.c \
      src/optimizer.c \
      src/shm_ring_ipc.c \
      src/shm_optimizer.c \
      src/adaptive_selector.c \
      src/syscall_profiler.c \
      src/integrity_verifier.c \
      src/workload_profiler.c \
      src/shm_ring_slot_optimizer.c \
      src/environment_profiler.c \
      src/run_manifest.c \
      src/cpu_affinity_analyzer.c

LDLIBS = -pthread

OBJ = $(SRC:.c=.o)

TARGET = fastipc


all: $(TARGET)


$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDLIBS)


%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


clean:
	rm -f $(OBJ) $(TARGET)


.PHONY: all clean