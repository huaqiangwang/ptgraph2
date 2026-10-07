#ifndef __FRAME_H__
#define __FRAME_H__

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <type_traits>

// Frame type flags for call graph weaving
enum class FrameType : uint8_t
{
    UNKNOWN = 0,
    CALL    = 1, // Function entry / branch
    RETURN  = 2, // Function exit
    SAMPLE  = 3, // Standalone / sampling event
    SYSCALL = 4, // System call entry/exit
    MARKER  = 5  // Custom trigger / zone marker
};

inline const char *FrameTypeToString(FrameType type)
{
    switch (type) {
    case FrameType::CALL:
        return "CALL";
    case FrameType::RETURN:
        return "RETURN";
    case FrameType::SAMPLE:
        return "SAMPLE";
    case FrameType::SYSCALL:
        return "SYSCALL";
    case FrameType::MARKER:
        return "MARKER";
    case FrameType::UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

//
// Reference to a string located in a specific SHM zone string pool.
//
#pragma pack(push, 8)
struct alignas(8) StringRef
{
    uint8_t  shmid{0}; // SHM zone ID where the string is stored
    uint8_t  reserved[7]{0};
    uint64_t offset{0}; // Byte offset from start of string pool in that SHM zone
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable<StringRef>::value,
              "StringRef must be trivially copyable for SHM and MPI");

//
// Structure representing a performance frame in the graph.
// To be possible to exchange data through shm and openMPI allgather.
// Information will be used in later call-graph waving. Not depending on the perf_event
// context.
// dsos, function caller and callee symbols are kept as {shmid, offset} StringRef
//
#pragma pack(push, 8)
struct alignas(8) FrameEle
{
    // Timing information (typically in nanoseconds)
    uint64_t timestamp; // Start / event timestamp
    uint64_t duration;  // Duration (end - start), or 0 if instantaneous

    // Addressing
    uint64_t ip;        // Current instruction pointer (callee entry/address)
    uint64_t addr; // Call site / caller address

    // String references ({shmid, offset}) into SHM string pools
    StringRef sym;        // Callee symbol
    StringRef caller_sym; // Caller symbol
    StringRef dso;        // DSO path/name

    // Execution context
    uint32_t pid;   // Process ID
    uint32_t tid;   // Thread ID
    uint32_t rank;  // MPI rank (useful when aggregating across nodes)
    uint16_t cpu;   // CPU core ID
    uint16_t depth; // Call stack depth (for graph weaving)

    // Event metadata
    FrameType type;        // Frame event type (CALL, RETURN, etc.)
    uint8_t   reserved[3]; // Padding to maintain alignment
    uint32_t  flags;       // Original 32-bit perf sample flags (PERF_DLFILTER_FLAG_*)

    void dump(std::ostream &os = std::cout) const
    {
        os << "FrameEle {"
           << " type=" << FrameTypeToString(type)
           << ", timestamp=" << timestamp
           << ", duration=" << duration
           << ", ip=0x" << std::hex << ip
           << ", addr=0x" << addr << std::dec
           << ", sym={shm:" << static_cast<unsigned>(sym.shmid) << ", off:" << sym.offset << "}"
           << ", caller_sym={shm:" << static_cast<unsigned>(caller_sym.shmid) << ", off:" << caller_sym.offset << "}"
           << ", dso={shm:" << static_cast<unsigned>(dso.shmid) << ", off:" << dso.offset << "}"
           << ", pid=" << pid
           << ", tid=" << tid
           << ", rank=" << rank
           << ", cpu=" << cpu
           << ", depth=" << depth
           << ", flags=0x" << std::hex << flags << std::dec
           << " }" << std::endl;
    }
};
#pragma pack(pop)

// Static assertion to ensure safety for SHM and MPI_Allgather
static_assert(std::is_trivially_copyable<FrameEle>::value,
              "FrameEle must be trivially copyable for SHM and MPI");

#include "shm.h"

#include <memory>
#include <string>

// Forward declaration of GraphDrawer
class GraphDrawer;

//
// High-level Frame wrapper around FrameEle
// Can interact with GraphDrawer to resolve string representations
//
class Frame
{
public:
    Frame() = default;
    explicit Frame(const FrameEle &ele) : ele_(ele) {}

    FrameEle       &GetEle() { return ele_; }
    const FrameEle &GetEle() const { return ele_; }

    void SetEle(const FrameEle &ele) { ele_ = ele; }

    uint64_t  GetTimestamp() const { return ele_.timestamp; }
    uint64_t  GetDuration() const { return ele_.duration; }
    uint64_t  GetIp() const { return ele_.ip; }
    uint64_t  GetAddr() const { return ele_.addr; }
    uint32_t  GetPid() const { return ele_.pid; }
    uint32_t  GetTid() const { return ele_.tid; }
    uint32_t  GetRank() const { return ele_.rank; }
    uint16_t  GetCpu() const { return ele_.cpu; }
    uint16_t  GetDepth() const { return ele_.depth; }
    FrameType GetType() const { return ele_.type; }
    uint32_t  GetFlags() const { return ele_.flags; }

    const StringRef &GetSymRef() const { return ele_.sym; }
    const StringRef &GetCallerSymRef() const { return ele_.caller_sym; }
    const StringRef &GetDsoRef() const { return ele_.dso; }

    void dump(std::ostream &os = std::cout) const { ele_.dump(os); }

private:
    FrameEle ele_{};
};

#endif // __FRAME_H__