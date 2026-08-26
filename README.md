# FastIPC-X

Adaptive High-Performance Inter-Process Communication and System-Call Optimization Engine.

## Phase 1
This starter contains:

- Real Linux `pipe()` IPC
- `fork()`
- `read()`
- `write()`
- `close()`
- `waitpid()`
- `clock_gettime()` benchmarking
- `getrusage()` context-switch measurement
- Configurable payload and chunk size

## Build

```bash
make
```

## Run

```bash
./fastipc pipe 100 64
```

This transfers 100 MB through a pipe using 64 KB chunks.

## Verify system calls

```bash
strace -f -c ./fastipc pipe 100 64
```

## First optimization experiment

Run the same 100 MB transfer with:

```bash
./fastipc pipe 100 1
./fastipc pipe 100 4
./fastipc pipe 100 16
./fastipc pipe 100 64
./fastipc pipe 100 256
```

Record runtime and throughput.

Then repeat with `strace -f -c` to compare system-call counts.

## Planned next modules

1. FIFO
2. Unix domain socket
3. POSIX shared memory
4. Automated benchmark runner
5. Adaptive IPC selector
6. CSV result export
