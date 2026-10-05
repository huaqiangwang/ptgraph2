# ptgraph2

A high-performance Linux `perf` dynamic library filter (`dlfilter`) for extracting instruction trace samples (such as Intel PT), grouping frames per thread (`tid`), storing them in lock-free shared memory (`/dev/shm`), and aggregating distributed traces via MPI.

---

## Features

- **perf dlfilter Integration**: Hooks directly into Linux `perf script --dlfilter` to capture samples and resolve instruction pointer addresses, symbols, and DSOs.
- **TID-Segregated Lock-Free Shared Memory**:
  - Automatically isolates trace streams per `tid` (`/<prefix>_tid_<tid>_zone_<id>`), preventing data interleaving across threads.
  - Dynamically provisions consecutive SHM zones upon capacity exhaustion.
  - Reader processes can consume frame queues concurrently without locks using atomic load-acquire and store-release synchronization.
- **Global Deduplicated String Pool**:
  - DSOs and symbol strings are cached in dedicated SHM string zones (`/<prefix>_str_zone_<id>`).
  - Frames store lightweight `StringRef{shmid, offset}` references to avoid duplicate strings in memory.
- **Master Catalog**:
  - Maintains `/<prefix>_master` recording active `(pid, tid)` pairs, total frames written, and active zone status for external reader discovery.
- **Distributed MPI Aggregation**:
  - Initialized in `start()` and coordinated in `stop()`.
  - Discovers and gathers all SHM zone descriptors across ranks with `MPI_Allgatherv`.
- **JSON-Driven Filtering & Weaving**:
  - Supports function window filtering (`funcZone` trigger on begin/end functions).
  - Supports timestamp window filtering (`timeZone`).
  - Supports frame index slicing (`frameZone`).
  - Supports symbol and DSO skipping / replacement (`frameFrontSkipping`, `frameEndSkipping`, `modification`).

---

## Project Structure

```text
ptgraph2/
├── CMakeLists.txt          # Build configuration (C++17, MPI, nlohmann_json)
├── conf/
│   └── ptgraph.json        # Example JSON configuration for filtering
├── include/
│   ├── frame.h             # FrameEle, FrameType, StringRef, and dump()
│   ├── graph.h             # GraphDrawer & PtGraphReader class declarations
│   ├── json.h              # JSON configuration parsing
│   ├── log.h               # Leveled logging macros (LOG_INFO, LOG_DEBUG, etc.)
│   ├── perf_dlfilter.h     # Linux perf dlfilter C API definitions
│   └── shm.h               # SHM layout headers and structures
├── src/
│   ├── dlfilter.cpp        # perf dlfilter entry points (start, filter_event, stop)
│   └── graph.cpp           # GraphDrawer and PtGraphReader implementations
└── test/
    ├── test_json.cpp       # Unit tests for JSON config filtering
    └── test_shm.cpp        # Tests for multi-TID SHM queues and MPI zone gathering
```

---

## Prerequisites

- **C++17 Compiler**: GCC $\ge$ 9 or Clang $\ge$ 10
- **CMake**: $\ge$ 3.10
- **Linux perf**: Tool with `dlfilter` support (kernel $\ge$ 5.14 recommended)
- **MPI**: OpenMPI or MPICH (`libopenmpi-dev`, `openmpi-bin`)
- **nlohmann_json**: `nlohmann-json3-dev` ($\ge$ 3.2.0)

Install dependencies on Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential libopenmpi-dev openmpi-bin nlohmann-json3-dev linux-tools-generic
```

---

## Building

```bash
mkdir -p build
cmake -B build -S .
cmake --build build
```

Artifacts generated in `build/`:
- `build/lib/libptgraph.so`: The shared library passed to `perf script --dlfilter`.
- `build/test_shm`: Test binary for multi-TID SHM streaming and MPI gathering.
- `build/test_json`: Test binary for JSON configuration parsing and filtering.

---

## Running Tests

### 1. Unit Tests for JSON Filtering
```bash
./build/test_json
```

### 2. Multi-TID SHM Tests (Single Rank)
```bash
./build/test_shm
```

### 3. Distributed MPI Gathering (Multi-Rank)
```bash
mpirun -n 3 ./build/test_shm
```

---

## Usage with Linux `perf`

### 1. Record Intel PT Trace
Record instructions of a workload:
```bash
perf record -e intel_pt//u -- filter ./my_target_app
```

### 2. Run with `ptgraph` Filter
Use `perf script` with the built dlfilter:
```bash
perf script --itrace=bcr --dlfilter build/lib/libptgraph.so --dlarg "conf=conf/ptgraph.json"
```

### 3. Read Frames from Shared Memory in an External Consumer
External processes can attach to the master catalog and read thread frames directly without copying:

```cpp
#include "graph.h"

int main() {
    PtGraphReader reader("ptgraph");
    if (!reader.OpenMaster()) {
        std::cerr << "Master catalog not found." << std::endl;
        return 1;
    }

    // Inspect registered threads
    for (const auto &entry : reader.GetThreadEntries()) {
        std::cout << "PID: " << entry.pid << ", TID: " << entry.tid
                  << ", Total Frames: " << entry.total_frames << std::endl;

        // Fetch all frames for this thread
        auto frames = reader.ReadAllFramesForThread(entry.tid);
        for (const auto &f : frames) {
            const char *sym = reader.ResolveString(f.sym);
            std::cout << "  IP: 0x" << std::hex << f.ip << " -> " << sym << std::dec << std::endl;
        }
    }
    return 0;
}
```

---

## Shared Memory Layout

| SHM Name Pattern | Description | Header Structure |
| :--- | :--- | :--- |
| `/<prefix>_master` | Registry table mapping all tracked `(pid, tid)` streams | `ShmMasterHeader` |
| `/<prefix>_str_zone_<id>` | Global string pool storing deduplicated symbols and DSOs | `ShmStringZoneHeader` |
| `/<prefix>_tid_<tid>_zone_<id>` | Dedicated `FrameEle` queue for thread `<tid>` | `ShmThreadZoneHeader` |

---

## License

This project is licensed under the MIT License.
