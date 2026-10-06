#ifndef __SHM_H__
#define __SHM_H__

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

//
// Shared Memory layout:
//
// 1. Master Catalog Zone: "/<prefix>_master"
// +-----------------+------------------------------------+
// | ShmMasterHeader | ThreadCatalogEntry[0..max_threads] |
// +-----------------+------------------------------------+
//
// 2. Global String Pool Zones: "/<prefix>_str_zone_<id>"
// +---------------------+-----------------------+
// | ShmStringZoneHeader | Deduplicated Strings  |
// +---------------------+-----------------------+
//
// 3. Per-TID Frame Queue Zones: "/<prefix>_tid_<tid>_zone_<id>"
// +---------------------+-----------------------------------+
// | ShmThreadZoneHeader | FrameEle[0..frame_capacity - 1]   |
// +---------------------+-----------------------------------+
//
// 4. Legacy Zone Layout (backward compatibility):
// +-----------+------------------+-----------------+
// | ShmHeader | FrameEle Queue   | ShmStringPool   |
// +-----------+------------------+-----------------+
//
#pragma pack(push, 8)
struct alignas(8) ThreadCatalogEntry
{
    uint32_t pid{0};
    uint32_t tid{0};
    uint32_t active_zone_id{0}; // Current / latest zone ID written for this thread
    uint32_t is_terminated{0};  // 1 if thread has finished or filter stopped
    uint64_t total_frames{0};   // Cumulative frames written across all zones for this thread
};

//
// Descriptor representing a single SHM zone published by a worker process/rank
//
enum class ShmZoneType : uint8_t
{
    MASTER = 0,
    STRING = 1,
    THREAD = 2
};

#pragma pack(push, 8)
struct alignas(8) ShmZoneDescriptor
{
    uint32_t    rank{0};                   // Worker rank / process ID index
    uint32_t    pid{0};                    // Process ID
    uint32_t    tid{0};                    // Thread ID (for thread frame zones)
    uint32_t    zone_id{0};                // Zone ID
    ShmZoneType type{ShmZoneType::THREAD}; // Master, String, or Thread zone
    uint8_t     is_full{0};
    uint8_t     reserved[3]{0};
    uint64_t    element_count{0}; // frame_count or string_size
    uint64_t    capacity{0};      // frame_capacity or string_capacity
    char        shm_name[128]{0}; // Name in /dev/shm (e.g. "/ptgraph_tid_1001_zone_0")
};

enum class WorkerState : uint32_t
{
    INIT       = 0,
    PROCESSING = 1,
    COMPLETED  = 2,
    FAILED     = 3
};

constexpr size_t MAX_RANKS          = 128;
constexpr size_t MAX_ZONES_PER_RANK = 256;

// Each worker writes its finished zone list and stats here
struct alignas(8) RankCoordSlot
{
    uint32_t          status{0};       // WorkerState (atomic)
    uint32_t          rank{0};         // Rank ID
    uint32_t          pid{0};          // Process PID
    uint32_t          zone_count{0};   // Number of zones published in zones[]
    uint64_t          total_frames{0}; // Total frames inserted by this rank
    uint64_t          early_samples{0};
    uint64_t          filtered_samples{0};
    ShmZoneDescriptor zones[MAX_ZONES_PER_RANK];
};

// Coordinator header mapped at "/<prefix>_coord_<coord_id>"
struct alignas(8) ShmCoordinator
{
    uint32_t      magic;       // 0x50544752 ("PTGR")
    uint32_t      version;     // 1
    uint32_t      total_ranks; // Total worker processes expected
    uint32_t      is_aborted;  // 1 if any process aborts/fails
    uint64_t      job_id;      // Job / run identifier
    uint64_t      reserved[2];
    RankCoordSlot slots[MAX_RANKS];
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable<ShmZoneDescriptor>::value,
              "ShmZoneDescriptor must be trivially copyable");
static_assert(std::is_trivially_copyable<RankCoordSlot>::value,
              "RankCoordSlot must be trivially copyable");
static_assert(std::is_trivially_copyable<ShmCoordinator>::value,
              "ShmCoordinator must be trivially copyable");

struct alignas(8) ShmMasterHeader
{
    uint32_t magic;             // 0x50544752 ("PTGR")
    uint32_t version;           // Layout version (1)
    uint32_t max_threads;       // Max entries in catalog
    uint32_t thread_count;      // Active entries registered in catalog
    uint32_t string_zone_count; // Number of string pool zones allocated
    uint32_t is_finished;       // 1 if tracing has stopped
    uint64_t catalog_offset;    // Byte offset to ThreadCatalogEntry array
    uint64_t reserved[2];
};

struct alignas(8) ShmStringZoneHeader
{
    uint32_t magic;   // 0x50544752 ("PTGR")
    uint8_t  zone_id; // Sequential zone id (0, 1, 2, ...)
    uint8_t  is_full; // Set to 1 when string pool is full
    uint16_t reserved;

    uint64_t string_capacity; // Max byte capacity for string pool
    uint64_t string_size;     // Current byte usage in string pool
    uint64_t string_offset;   // Byte offset from start of SHM to string pool
};

struct alignas(8) ShmThreadZoneHeader
{
    uint32_t magic;   // 0x50544752 ("PTGR")
    uint32_t pid;     // Process ID
    uint32_t tid;     // Thread ID
    uint32_t zone_id; // Sequential zone id for this thread (0, 1, 2, ...)
    uint8_t  is_full; // Set to 1 when zone is full and cannot take more data
    uint8_t  reserved[3];
    uint32_t reserved2{0};

    uint64_t frame_capacity; // Max elements in this zone's FrameEle queue
    uint64_t frame_count;    // Current number of FrameEle elements written
    uint64_t frame_offset;   // Byte offset from start of SHM to FrameEle queue
};

struct alignas(8) ShmHeader
{
    uint32_t magic;   // 0x50544752 ("PTGR")
    uint8_t  zone_id; // Sequential zone id (0, 1, 2, ...)
    uint8_t  is_full; // Set to 1 when zone is full and cannot take more data
    uint16_t reserved;

    uint64_t frame_capacity; // Max elements in this zone's FrameEle queue
    uint64_t frame_count;    // Current number of FrameEle elements written
    uint64_t frame_offset;   // Byte offset from start of SHM to FrameEle queue

    uint64_t string_capacity; // Max byte capacity for string pool
    uint64_t string_size;     // Current byte usage in string pool
    uint64_t string_offset;   // Byte offset from start of SHM to string pool
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable<ThreadCatalogEntry>::value, "ThreadCatalogEntry must be trivially copyable");
static_assert(std::is_trivially_copyable<ShmMasterHeader>::value, "ShmMasterHeader must be trivially copyable");
static_assert(std::is_trivially_copyable<ShmStringZoneHeader>::value, "ShmStringZoneHeader must be trivially copyable");
static_assert(std::is_trivially_copyable<ShmThreadZoneHeader>::value, "ShmThreadZoneHeader must be trivially copyable");
static_assert(std::is_trivially_copyable<ShmHeader>::value, "ShmHeader must be trivially copyable");

// Forward declaration of FrameEle for pointer member in ShmZone
struct FrameEle;

struct ShmZone
{
    uint32_t    zone_id{0};
    std::string shm_name;
    int         fd{-1};
    void       *addr{MAP_FAILED};
    size_t      total_size{0};
    void       *header{nullptr};
    FrameEle   *frames{nullptr};
    char       *string_pool{nullptr};

    ShmThreadZoneHeader *GetThreadHeader() const
    {
        return reinterpret_cast<ShmThreadZoneHeader *>(header);
    }
    ShmStringZoneHeader *GetStringHeader() const
    {
        return reinterpret_cast<ShmStringZoneHeader *>(header);
    }
    ShmMasterHeader *GetMasterHeader() const
    {
        return reinterpret_cast<ShmMasterHeader *>(header);
    }
    ShmHeader *GetLegacyHeader() const
    {
        return reinterpret_cast<ShmHeader *>(header);
    }

    ~ShmZone()
    {
        if (addr != MAP_FAILED && addr != nullptr) {
            munmap(addr, total_size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    // Move-only semantics
    ShmZone()                           = default;
    ShmZone(const ShmZone &)            = delete;
    ShmZone &operator=(const ShmZone &) = delete;
    ShmZone(ShmZone &&other) noexcept
        : zone_id(other.zone_id),
          shm_name(std::move(other.shm_name)),
          fd(other.fd),
          addr(other.addr),
          total_size(other.total_size),
          header(other.header),
          frames(other.frames),
          string_pool(other.string_pool)
    {
        other.fd   = -1;
        other.addr = MAP_FAILED;
    }
    ShmZone &operator=(ShmZone &&other) noexcept
    {
        if (this != &other) {
            if (addr != MAP_FAILED && addr != nullptr)
                munmap(addr, total_size);
            if (fd >= 0)
                close(fd);

            zone_id     = other.zone_id;
            shm_name    = std::move(other.shm_name);
            fd          = other.fd;
            addr        = other.addr;
            total_size  = other.total_size;
            header      = other.header;
            frames      = other.frames;
            string_pool = other.string_pool;

            other.fd   = -1;
            other.addr = MAP_FAILED;
        }
        return *this;
    }
};

#endif // __SHM_H__
