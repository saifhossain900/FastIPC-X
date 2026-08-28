# FastIPC-X

## Adaptive Linux IPC Optimization & Analysis Engine

FastIPC-X is a systems-performance project for Linux that **benchmarks, optimizes, explains, and validates inter-process communication (IPC)**.

It does more than answer _“Which IPC method is fastest?”_ FastIPC-X measures multiple IPC mechanisms, finds performance bottlenecks, redesigns the shared-memory critical path, profiles kernel interaction, studies CPU scheduling and page faults, verifies data integrity, and records reproducible experiment evidence.

> **Course:** CSE323 — Operating Systems Design  
> **Core:** Native C / POSIX / Linux  
> **Dashboard:** React + Vite + Recharts  
> **Bridge:** Flask  
> **Platform tested:** Ubuntu on WSL2

---

## Why FastIPC-X?

A simple benchmark can show a timing number, but it does not explain **why** the result happened.

FastIPC-X was built around a stronger engineering question:

> **How can an IPC implementation be measured, optimized, explained at the OS level, and validated without sacrificing correctness or reproducibility?**

The project therefore combines:

- IPC implementation
- synchronization optimization
- repeated benchmarking
- syscall/futex analysis
- CPU-affinity experiments
- virtual-memory/page-fault experiments
- adaptive workload selection
- checksum-based data-integrity verification
- Git/environment experiment manifests
- a thin performance dashboard over the native engine

---

## Headline Results

Authoritative 100 MB / 64 KB evidence:

| Result | Measured Outcome |
|---|---:|
| Baseline IPC latency winner | **UNIX Socket — 9.896 ms** |
| Baseline SHM median latency | **45.752 ms** |
| Optimized SHM-RING median latency | **5.635 ms** |
| SHM latency reduction | **87.68%** |
| SHM throughput improvement | **723.73%** |
| System CPU reduction | **95.07%** |
| Voluntary context-switch reduction | **88.69%** |
| Total syscall reduction | **21.52%** |
| Futex-call reduction | **21.93%** |
| Pre-fault critical-path reduction | **81.88%** |
| Recorded integrity verification | **6 / 6 PASS** |

> The baseline UNIX Socket winner and the optimized SHM-RING result are **separate conclusions**.  
> FastIPC-X intentionally avoids presenting the optimized design as part of the original baseline comparison.

---

## Architecture

```text
React / Vite Performance Dashboard
               |
               v
          Flask API Bridge
               |
               v
        Native FastIPC-X C CLI
               |
      +--------+---------+----------+
      |        |         |          |
      v        v         v          v
    PIPE      FIFO    AF_UNIX    POSIX SHM
                                  |
                                  v
                               SHM-RING
               |
               v
 Linux Scheduler / Syscalls / Futex / VM / Page Faults
               |
               v
       CSV Evidence + Run Manifests
```

**Important:** The GUI does not reimplement the benchmark engine. It invokes the native C binary and displays measured evidence.

---

## IPC Methods

### PIPE
Anonymous kernel-managed byte stream between related processes.

### FIFO
Named pipe with filesystem-visible IPC semantics.

### UNIX Domain Socket
`AF_UNIX` + `SOCK_STREAM`, providing socket semantics without network routing.

### POSIX Shared Memory
Both processes map the same memory region and coordinate access with synchronization primitives.

### SHM-RING
Optimized shared-memory producer-consumer design using a bounded multi-slot ring to reduce synchronization on the critical path.

---

## Core Features

| Feature | What it does |
|---|---|
| Benchmark Engine | Repeated IPC trials with median latency as the main comparison |
| Chunk Optimizer | Sweeps transfer chunk sizes per IPC mechanism |
| SHM Synchronization Optimizer | Compares baseline SHM against bounded SHM-RING |
| Ring-Slot Optimizer | Evaluates ring depth without blindly adopting a microbenchmark winner |
| Adaptive Selector | Recommends IPC/configuration from measured workload profiles |
| Syscall Analyzer | Compares syscall/futex behavior before and after optimization |
| CPU Scheduler Analyzer | Measures unpinned, same-CPU, and separate-CPU placement |
| VM/Page-Fault Analyzer | Compares demand paging, pre-faulting, and `MADV_WILLNEED` |
| Integrity Verifier | Checks bytes transferred + FNV-1a 64-bit checksums |
| Environment Profiler | Records OS, kernel, CPU, compiler, page size, and build flags |
| Run Manifests | Records command, Git revision/state, environment, and outputs |
| Final Summary Generator | Consolidates canonical CSV evidence into final project conclusions |
| Performance Dashboard | React/Vite interface over the native C engine |
| Safe Video Demo Mode | Runs non-authoritative parameters so final evidence is not overwritten |

---

## Optimization Story

### 1. Establish a baseline

100 MB / 64 KB baseline median latency:

```text
PIPE    : 44.293 ms
FIFO    : 49.855 ms
SOCKET  :  9.896 ms  <- baseline winner
SHM     : 53.884 ms
```

The result showed an important lesson:

**Shared memory is not automatically fast if synchronization dominates the transfer path.**

### 2. Tune chunk size

FastIPC-X sweeps candidate chunk sizes instead of assuming one universal value.

Different IPC mechanisms preferred different configurations under the tested workload.

### 3. Redesign SHM synchronization

The original SHM implementation used a **single-slot producer-consumer hand-off**.

FastIPC-X replaced that path with **SHM-RING**, allowing multiple chunks to be in flight.

Production validation:

```text
Baseline SHM
Median latency      : 45.752 ms
Throughput          : 2174.01 MB/s
System CPU          : 35.934
Voluntary CS        : 3169.8

SHM-RING
Median latency      : 5.635 ms
Throughput          : 17908.03 MB/s
System CPU          : 1.770
Voluntary CS        : 358.6
```

Measured change:

```text
Latency               -87.68%
Throughput            +723.73%
System CPU            -95.07%
Voluntary switches    -88.69%
```

### 4. Explain the result at the kernel level

Tracing showed:

```text
Total syscalls : 3299 -> 2589   (-21.52%)
Futex calls    : 3238 -> 2528   (-21.93%)
```

`strace` is treated as **behavioral evidence**, not the primary performance timer, because tracing changes execution behavior.

---

## CPU Scheduler Experiment

FastIPC-X compares:

- unpinned execution
- producer + consumer on the same CPU
- producer + consumer on separate CPUs

Measured on the test system:

```text
UNPINNED     : 6.171 ms
SAME-CPU     : 4.826 ms
SEPARATE-CPU : 7.252 ms
```

Same-CPU placement measured about **21.80% lower latency** than unpinned execution.

This is intentionally labeled **experimental** because scheduler/cache behavior depends on hardware, kernel, and workload. Production remains unpinned.

---

## Virtual Memory / Page Faults

FastIPC-X separates setup work from the timed critical path.

### Demand paging
The timed path experienced **25,600 minor faults**.

### Pre-faulting
The same first-touch faults were moved into setup, leaving **0 recorded timed-path minor faults** and reducing timed-path latency by about **81.88%**.

> Pre-faulting does **not** remove the memory cost. It moves that work outside the timed critical path.

### `MADV_WILLNEED`
A successful hint does not guarantee that pages were pre-populated. FastIPC-X reports the measured behavior without making that claim.

---

## Adaptive Workload Selection

Recorded workload profile:

| Payload | Selected configuration |
|---:|---|
| 1 MB | FIFO / 4 KB |
| 10 MB | SHM-RING / 64 KB |
| 100 MB | SHM-RING / 64 KB |
| 500 MB | SHM-RING / 64 KB |

The adaptive layer uses **persisted measured evidence**, not a hardcoded assumption that one IPC method always wins.

---

## Data Integrity

FastIPC-X verifies:

- bytes sent
- bytes received
- sender checksum
- receiver checksum

Checksum algorithm:

```text
FNV-1a 64-bit
```

Recorded authoritative verification:

```text
6 / 6 PASS
```

This ensures the optimized transfer remains correct.

---

## Reproducible Experiments

Important runs create manifests containing:

- Run ID
- timestamp
- experiment category
- exact command
- command exit code/status
- Git commit
- Git branch
- source-tree state
- system environment profile
- generated result files

This makes it possible to distinguish:

```text
clean authoritative evidence
```

from:

```text
temporary / dirty development experiments
```

---

## Scientific Guardrails

FastIPC-X deliberately avoids several common benchmarking mistakes:

- Baseline and optimized results are reported separately.
- Median results are preferred over one lucky timing run.
- `strace` timing is not used as primary benchmark proof.
- A fast ring-slot microbenchmark is not automatically adopted in production.
- CPU-affinity results are labeled system-specific.
- Pre-faulting is described as **work shifting**, not free performance.
- `MADV_WILLNEED` is treated as a hint, not guaranteed pre-population.
- Live GUI demos use different parameters so canonical evidence is protected.

---

## Skills Demonstrated

```text
Linux / POSIX Systems Programming
Process Creation & Coordination
PIPE / FIFO / AF_UNIX Socket / POSIX SHM
Semaphores & Producer-Consumer Synchronization
Bounded Ring Buffers
Performance Benchmarking
Latency / Throughput / CPU Analysis
Context-Switch Analysis
System Calls & futex Profiling
CPU Affinity & Scheduler Experiments
Virtual Memory & Page-Fault Analysis
Data Integrity Verification
FNV-1a 64-bit Checksums
Experimental Reproducibility
Git-Based Run Manifests
C + Flask + React/Vite Integration
Evidence-Driven Optimization
```

---

## Build

### Requirements

Linux or WSL2 with:

```text
GCC
make
Python 3
Node.js / npm
```

Build native engine:

```bash
make
```

---

## Native CLI Examples

Baseline benchmark:

```bash
./fastipc benchmark 100 5
```

Shared-memory synchronization optimization:

```bash
./fastipc optimize-shm 100 64 5
```

Integrity verification:

```bash
./fastipc verify shm-opt 17 72
```

CPU-affinity analysis:

```bash
./fastipc analyze-affinity 100 64 5
```

Virtual-memory analysis:

```bash
./fastipc optimize-memory 100 64 5
```

Generate final evidence summary:

```bash
./fastipc final-summary
```

---

## Dashboard

### Backend

```bash
cd dashboard/backend
python3 -m venv .venv
source .venv/bin/activate
pip install flask flask-cors
cd ../..
python3 dashboard/backend/app.py
```

Backend:

```text
http://127.0.0.1:5050
```

### Frontend

```bash
cd dashboard/frontend
npm install
npm run dev -- --host 0.0.0.0 --port 5175
```

Open:

```text
http://localhost:5175/
```

Dashboard sections:

```text
Dashboard
Benchmark Lab
Optimization
Adaptive Engine
Syscall Analyzer
CPU Scheduler
Virtual Memory
Integrity
Video Demo
Final Results
Evidence
```

---

## Safe Video Demo Mode

The dashboard contains a recording-oriented demo page.

Safe presets intentionally use non-authoritative parameters such as:

```text
Integrity      : 17 MB / 72 KB
SHM demo       : 37 MB / 72 KB
Scheduler demo : 37 MB / 72 KB
Memory demo    : 37 MB / 72 KB
```

This lets the native C system run live during a project video without overwriting the final 100 MB / 64 KB evidence files.

---

## Technical Challenges

The technical report documents the major development challenges in the **STAR format**:

1. single-slot SHM synchronization bottleneck
2. IPC-specific chunk-size tuning
3. microbenchmark candidate vs production validation
4. explaining optimization through syscall/futex evidence
5. CPU-affinity results varying with scheduler/topology
6. page faults affecting the timed critical path
7. maintaining correctness and reproducibility while optimizing

---

## Project Structure

```text
FastIPC-X/
├── include/                 # C headers
├── src/                     # native IPC/benchmark/analysis engine
├── results/                 # CSV/TXT evidence + manifests
├── dashboard/
│   ├── backend/
│   │   └── app.py           # thin Flask bridge
│   └── frontend/
│       ├── src/
│       │   ├── App.jsx
│       │   └── index.css
│       └── public/
├── Makefile
└── README.md
```

---

## Limitations

- Results depend on hardware, Linux kernel, compiler, WSL/native environment, and system load.
- Adaptive selection currently uses persisted benchmark profiles rather than online learning.
- CPU-affinity conclusions are experimental and system-specific.
- Cross-machine/network IPC is outside the project scope.
- Larger trial sets and confidence intervals would strengthen publication-quality statistical analysis.

---

## Project Demo

**Video:** _Add final 2–5 minute project demo link here._

Suggested embed pattern after uploading:

```markdown
[![FastIPC-X Project Demo](VIDEO_THUMBNAIL_URL)](VIDEO_URL)
```

---

## Technical Report

**Report:** _Add the final technical-report link here._

The report includes:

- project overview
- system architecture
- IPC implementation
- optimization results
- technical challenges in STAR format
- kernel/scheduler/VM analysis
- scientific limitations
- lessons learned
- conclusion

---

## Final Takeaway

> **FastIPC-X shows that IPC performance is not only about choosing a mechanism — it is about synchronization, kernel interaction, scheduling, memory behavior, correctness, and evidence.**

The strongest validated result is the bounded **SHM-RING** design, which reduced shared-memory median latency by **87.68%** while preserving data integrity and substantially reducing system/synchronization overhead.
