# ptgraph2

A high-performance Linux `perf` dynamic library filter (`dlfilter`) and call-graph reconstruction engine for Intel PT (Processor Trace). It captures branch/call/return/interrupt events, segregates frames per thread (`tid`) in lock-free shared memory (`/dev/shm`), coordinates multi-process parallel decoding, and reconstructs robust timeline traces exported directly to the Perfetto format (`.ftf` / `.ftf.gz`).

---

## Features

- **perf dlfilter Integration**: Hooks directly into Linux `perf script --dlfilter` to capture samples and resolve instruction pointer addresses, symbols, and DSOs.
- **TID-Segregated Lock-Free Shared Memory**:
  - Automatically isolates trace streams per `tid` (`/<prefix>_tid_<tid>_zone_<id>`), preventing data interleaving across threads.
  - Dynamically provisions consecutive SHM zones upon capacity exhaustion.
  - Reader processes consume frame queues concurrently without locks using atomic load-acquire and store-release synchronization.
- **Global Deduplicated String Pool**:
  - DSOs and symbol strings are cached in dedicated SHM string zones (`/<prefix>_str_zone_<id>`).
  - Frames store lightweight `StringRef{shmid, offset}` references to avoid duplicate strings in memory.
- **Master Catalog & Multi-Process Coordinator**:
  - Maintains `/<prefix>_master` recording active `(pid, tid)` pairs, total frames written, and active zone status.
  - Coordinated via `ShmCoordinator` across parallel worker processes without external IPC dependencies.
- **Multi-Process Parallel Acceleration (`mperf`)**:
  - Automatically slices trace time intervals and launches parallel worker processes to decode large `perf.data` files concurrently.
  - Rank 0 acts as the aggregator to assemble zones and generate unified timelines.
- **Robust Call & Return Matching with Lookahead Recovery**:
  - Resilient against trace drops, buffer overflows, tail-call optimizations, and runtime aliases.
  - Uses LCS dynamic-programming lookahead (`ChooseReturnRecovery`) to arbitrate between missing CALLs and missing RETURNs.
  - Preserves exact instruction retirement sequence across identical timestamps using stable sorting.
  - Generates zero-duration `FULL` slices for isolated pre-trace returns without polluting call stack depth.
- **Hardware Interrupt & Trace Resume Pairing**:
  - Automatically correlates asynchronous interrupt events (`flags = 0x63`: `BRANCH | CALL | ASYNC | INTERRUPT`) with trace resumption (`flags = 0x101`: `TRACE_BEGIN | BRANCH`).
  - Closes interrupt intervals cleanly as `[interrupt]` slices, preventing false stack explosions and eliminating trailing unclosed frames.
- **Native Perfetto Timeline Export**:
  - Generates Fuchsia Trace Format (`.ftf`) files compatible with [ui.perfetto.dev](https://ui.perfetto.dev).
  - Built-in multi-threaded timeline replay and streaming Gzip compression (`.ftf.gz`).
  - Supports unified single traces or per-TID output (`out_tid_<tid>.ftf`).
- **JSON-Driven Filtering & Weaving**:
  - Supports function window filtering (`funcZone` trigger on begin/end functions).
  - Supports timestamp window filtering (`timeZone`).
  - Supports frame index slicing (`frameZone`).
  - Supports symbol and DSO skipping / replacement (`frameFrontSkipping`, `frameEndSkipping`, `modification`, `endFramePair`).

---

## Prerequisites

- **C++17 Compiler**: GCC $\ge$ 9 or Clang $\ge$ 10
- **CMake**: $\ge$ 3.10
- **Linux perf**: Tool with `dlfilter` support (Linux kernel $\ge$ 5.14 recommended)
- **ZLIB**: `zlib1g-dev`
- **nlohmann_json**: `nlohmann-json3-dev` ($\ge$ 3.2.0)

Install dependencies on Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential zlib1g-dev nlohmann-json3-dev linux-tools-generic
```

---

## Building

```bash
cmake -B build -S .
cmake --build build -j
```
---

## Usage

### 1. Record an Intel PT Trace
Record instruction branches of your workload:
```bash
perf record -e intel_pt//u -- ./my_target_app
```

### 2. Run with `mperf` (Parallel Multi-Process Acceleration)
`mperf` partitions the trace time window across $N$ worker processes:
```bash
./build/mperf -i perf.data -n 4 -f out.ftf -j conf/ptgraph.json
```

Options:
- `-i <file>`: Input `perf.data` file (default: `perf.data`).
- `-n <num>`: Number of parallel `perf script` worker processes (default: `4`).
- `-f <file>`: Output Perfetto trace file (default: `out.ftf`, automatically generates `out.ftf.gz`).
- `-j <file>`: Configuration JSON file.
- `-P`: Generate per-TID Perfetto trace files (`out_tid_<tid>.ftf`).
- `-v`: Enable verbose debug logging.

### 3. Run Standalone with Linux `perf script`
Alternatively, invoke `perf script` directly with the dlfilter:
```bash
perf script -i perf.data --itrace=cr \
    --dlfilter build/lib/libptgraph.so \
    --dlarg "-f out.ftf -j conf/ptgraph.json"
```

### 4. View Trace in Perfetto UI
Open [https://ui.perfetto.dev](https://ui.perfetto.dev) in Chrome or Edge and drag-and-drop the generated `out.ftf` or `out.ftf.gz` file.

---

## License

This project is licensed under the MIT License.
