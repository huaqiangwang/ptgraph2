#ifndef __GRAPH_H__
#define __GRAPH_H__

#include "frame.h"
#include "json.h"
#include "shm.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "perf_dlfilter.h"
extern struct perf_dlfilter_fns perf_dlfilter_fns;
#ifdef __cplusplus
}
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#if __has_include(<mpi.h>)
#include <mpi.h>
#define PTGRAPH_HAS_MPI 1
#endif

//
// Structure tracking per-thread frame queue zones
//
struct ThreadStream
{
    uint32_t             pid{0};
    uint32_t             tid{0};
    uint32_t             current_zone_id{0};
    uint32_t             catalog_index{0};
    std::vector<ShmZone> zones;
};

//
// GraphDrawer is the context of dlfilter manager, responsible for:
// - managing Master Catalog SHM ("/ptgraph_master")
// - managing global String Pool SHMs ("/ptgraph_str_zone_<id>")
// - managing per-TID Frame queue SHMs ("/ptgraph_tid_<tid>_zone_<id>")
// - lock-free / atomic publishing of frame counts so other processes can read concurrently
// - converting perf_dlfilter_sample to Frame / FrameEle
//
class GraphDrawer
{
public:
    static constexpr uint32_t SHM_MAGIC               = 0x50544752; // "PTGR"
    static constexpr size_t   DEFAULT_FRAME_CAPACITY  = 65536;      // 64K frames per thread zone (~5.5 MB)
    static constexpr size_t   DEFAULT_STRING_CAPACITY = 2097152;    // 2 MB string pool
    static constexpr size_t   DEFAULT_MAX_THREADS     = 1024;       // Max threads tracked in catalog

    explicit GraphDrawer(void       *dlfilter_ctx    = nullptr,
                         std::string shm_prefix      = "ptgraph",
                         size_t      frame_capacity  = DEFAULT_FRAME_CAPACITY,
                         size_t      string_capacity = DEFAULT_STRING_CAPACITY,
                         size_t      max_threads     = DEFAULT_MAX_THREADS);

    ~GraphDrawer();

    // Non-copyable
    GraphDrawer(const GraphDrawer &)            = delete;
    GraphDrawer &operator=(const GraphDrawer &) = delete;

    // Moveable
    GraphDrawer(GraphDrawer &&) noexcept            = default;
    GraphDrawer &operator=(GraphDrawer &&) noexcept = default;

    void  SetContext(void *ctx) { ctx_ = ctx; }
    void *GetContext() const { return ctx_; }

    void     SetMpiRank(uint32_t rank) { mpi_rank_ = rank; }
    uint32_t GetMpiRank() const { return mpi_rank_; }

    void     SetMpiSize(uint32_t size) { mpi_size_ = size; }
    uint32_t GetMpiSize() const { return mpi_size_; }

    void SetUnlinkOnDestroy(bool enable) { unlink_on_destroy_ = enable; }
    bool GetUnlinkOnDestroy() const { return unlink_on_destroy_; }

    void            LoadConfig(const std::string &conf_file);
    const JsonPara &GetJsonPara() const { return json_para_; }
    JsonPara       &GetJsonPara() { return json_para_; }
    int             FilterByTimestamp(uint64_t timestamp);

    const std::string &GetShmPrefix() const { return shm_prefix_; }
    size_t             GetFrameCapacity() const { return frame_capacity_; }
    size_t             GetStringCapacity() const { return string_capacity_; }

    // Register or retrieve string {shmid, offset} in the current active string pool zone.
    StringRef AppendString(const std::string &str);

    // Append a FrameEle into the thread-specific SHM queue for frame.tid.
    void AppendFrame(FrameEle frame);

    // Helper to append a frame with string resolution for sym, caller_sym, and dso.
    void AppendFrameWithSymbols(FrameEle           frame,
                                const std::string &sym,
                                const std::string &caller_sym = "",
                                const std::string &dso        = "");

    // Convert perf_dlfilter_sample to FrameEle and append to the TID-specific SHM queue.
    void AddSample(const struct perf_dlfilter_sample *sample, void *ctx = nullptr);

    // Mark all threads and master catalog as finished
    void Finish();

    // Statistics tracking
    void     IncEarlySamples() { early_samples_count_++; }
    uint64_t GetEarlySamples() const { return early_samples_count_; }

    void     IncFilteredSamples() { filtered_samples_count_++; }
    uint64_t GetFilteredSamples() const { return filtered_samples_count_; }

    uint64_t GetInsertedFrames() const { return inserted_frames_count_; }

    // Unlink all SHM segments created by this instance from /dev/shm
    void UnlinkAll();

    // Accessors
    const ShmZone                                    &GetMasterZone() const { return master_zone_; }
    const std::vector<ShmZone>                       &GetStringZones() const { return string_zones_; }
    const std::unordered_map<uint32_t, ThreadStream> &GetThreadStreams() const { return thread_streams_; }

    // Collect local zone descriptors representing all SHM zones managed by this instance
    std::vector<ShmZoneDescriptor> GetLocalZoneDescriptors() const;

#if defined(PTGRAPH_HAS_MPI)
    // Use MPI_Allgather / MPI_Allgatherv to collect SHM zone descriptors from all ranks.
    std::vector<ShmZoneDescriptor> GatherShmZonesMpi(MPI_Comm comm = MPI_COMM_WORLD);
#endif

    // Resolve string from zone and offset
    static const char *ResolveString(const ShmZone &zone, uint64_t offset);

    // Resolve string using StringRef and available string zones
    const char *ResolveString(const StringRef &ref) const;

private:
    void CreateMasterZone();
    void CreateNewStringZone();
    void CreateThreadZone(ThreadStream &stream);
    void RegisterThreadInMaster(ThreadStream &stream);
    void UpdateMasterZoneId(const ThreadStream &stream);
    void UpdateMasterTotalFrames(const ThreadStream &stream);

    void       *ctx_{nullptr};
    std::string shm_prefix_;
    size_t      frame_capacity_;
    size_t      string_capacity_;
    size_t      max_threads_;
    uint32_t    current_string_zone_id_{0};
    int         mpi_rank_{0};
    int         mpi_size_{1};
    bool        unlink_on_destroy_{false};
    bool        is_finished_{false};
    std::string outfilename_;
    uint64_t    ts_global_begin_ns_{0};
    uint64_t    ts_global_end_ns_{0};
    uint64_t    ts_rank_begin_ns_{0};
    uint64_t    ts_rank_end_ns_{0};

    JsonPara json_para_;
    uint64_t total_samples_{0};
    uint64_t early_samples_count_{0};
    uint64_t filtered_samples_count_{0};
    uint64_t inserted_frames_count_{0};

    ShmZone                                    master_zone_;
    std::vector<ShmZone>                       string_zones_;
    std::unordered_map<uint32_t, ThreadStream> thread_streams_; // tid -> stream
    std::unordered_map<std::string, StringRef> string_cache_;
};

//
// PtGraphReader: Helper class for external processes to read frames and
// symbols grouped by TID from shared memory.
//
class PtGraphReader
{
public:
    explicit PtGraphReader(std::string shm_prefix = "ptgraph");
    ~PtGraphReader();

    // Connect to the master catalog SHM
    bool OpenMaster();

    // Get snapshot of all registered threads in the master catalog
    std::vector<ThreadCatalogEntry> GetThreadEntries();

    bool GetThreadEntry(uint32_t tid, ThreadCatalogEntry &out_entry);

    // Check if the writer has finished tracing
    bool IsFinished();

    // Ensure string zone with index `zone_id` is mapped
    bool EnsureStringZone(uint8_t zone_id);

    // Resolve StringRef to string literal
    const char *ResolveString(const StringRef &ref);

    // Map a specific thread frame zone for reading
    bool OpenThreadZone(uint32_t tid, uint32_t zone_id, ShmZone &out_zone);

    // Read all available frames for a TID across all its zones
    std::vector<FrameEle> ReadAllFramesForThread(uint32_t tid);

    // Unlink all SHM zones belonging to prefix using master catalog
    static void UnlinkAll(const std::string &shm_prefix = "ptgraph");

private:
    std::string          shm_prefix_;
    ShmZone              master_zone_;
    std::vector<ShmZone> string_zones_;
};

#endif // __GRAPH_H__
