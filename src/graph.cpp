#include "graph.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <regex>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// GraphDrawer Implementation
// ============================================================================

GraphDrawer::GraphDrawer(void       *dlfilter_ctx,
                         std::string shm_prefix,
                         size_t      frame_capacity,
                         size_t      string_capacity,
                         size_t      max_threads)
    : ctx_(dlfilter_ctx),
      shm_prefix_(std::move(shm_prefix)),
      frame_capacity_(frame_capacity),
      string_capacity_(string_capacity),
      max_threads_(max_threads)
{
#if defined(PTGRAPH_HAS_MPI)
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized) {
        int rank = 0;
        int size = 1;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        MPI_Comm_size(MPI_COMM_WORLD, &size);
        mpi_rank_ = static_cast<uint32_t>(rank);
        mpi_size_ = static_cast<uint32_t>(size);
        if (mpi_size_ > 1) {
            shm_prefix_ += "_rank_" + std::to_string(mpi_rank_);
        }
    }
#endif
    CreateMasterZone();
    CreateNewStringZone();

    // Parse options from perf --dlarg if available
    if (ctx_ && perf_dlfilter_fns.args) {
        int    argc = 0;
        char **argv = perf_dlfilter_fns.args(ctx_, &argc);
        for (int i = 0; i < argc; ++i) {
            if (!argv[i])
                continue;
            std::string arg = argv[i];
            if (arg.rfind("conf=", 0) == 0) {
                LoadConfig(arg.substr(5));
            }
        }
    }
}

GraphDrawer::~GraphDrawer()
{
    Finish();
    if (unlink_on_destroy_) {
        UnlinkAll();
    }
}

void GraphDrawer::LoadConfig(const std::string &conf_file)
{
    json_para_.Parse(conf_file.c_str());
}

StringRef GraphDrawer::AppendString(const std::string &str)
{
    if (str.empty()) {
        return StringRef{0, {0}, 0};
    }

    auto it = string_cache_.find(str);
    if (it != string_cache_.end()) {
        return it->second;
    }

    size_t len_with_null = str.size() + 1;
    if (len_with_null > string_capacity_) {
        throw std::runtime_error("String length exceeds maximum string pool capacity");
    }

    ShmZone *active = &string_zones_.back();
    auto    *hdr    = active->GetStringHeader();
    if (hdr->string_size + len_with_null > hdr->string_capacity) {
        CreateNewStringZone();
        active = &string_zones_.back();
        hdr    = active->GetStringHeader();
    }

    uint64_t offset = hdr->string_size;
    std::memcpy(active->string_pool + offset, str.c_str(), len_with_null);
    __atomic_store_n(&hdr->string_size, offset + len_with_null, __ATOMIC_RELEASE);

    StringRef ref{static_cast<uint8_t>(active->zone_id), {0}, offset};
    string_cache_[str] = ref;
    return ref;
}

void GraphDrawer::AppendFrame(FrameEle frame)
{
    uint32_t tid = frame.tid;
    uint32_t pid = frame.pid;

    auto it = thread_streams_.find(tid);
    if (it == thread_streams_.end()) {
        ThreadStream stream;
        stream.pid             = pid;
        stream.tid             = tid;
        stream.current_zone_id = 0;
        CreateThreadZone(stream);

        RegisterThreadInMaster(stream);

        auto res = thread_streams_.emplace(tid, std::move(stream));
        it       = res.first;
    }

    ThreadStream &stream   = it->second;
    ShmZone      *cur_zone = &stream.zones.back();
    auto         *hdr      = cur_zone->GetThreadHeader();

    if (hdr->frame_count >= hdr->frame_capacity) {
        __atomic_store_n(&hdr->is_full, 1, __ATOMIC_RELEASE);
        stream.current_zone_id++;
        CreateThreadZone(stream);

        UpdateMasterZoneId(stream);

        cur_zone = &stream.zones.back();
        hdr      = cur_zone->GetThreadHeader();
    }

    uint64_t idx          = hdr->frame_count;
    cur_zone->frames[idx] = frame;

    __atomic_store_n(&hdr->frame_count, idx + 1, __ATOMIC_RELEASE);

    UpdateMasterTotalFrames(stream);
}

void GraphDrawer::AppendFrameWithSymbols(FrameEle           frame,
                                         const std::string &sym,
                                         const std::string &caller_sym,
                                         const std::string &dso)
{
    frame.sym        = AppendString(sym);
    frame.caller_sym = AppendString(caller_sym);
    frame.dso        = AppendString(dso);

    AppendFrame(frame);
}

void GraphDrawer::AddSample(const struct perf_dlfilter_sample *sample, void *ctx)
{
    if (!sample)
        return;

    // Check timeZone filtering
    if (json_para_.timeZone && !json_para_.timeZone->InRange(sample->time)) {
        return;
    }

    // Check frameZone filtering
    if (json_para_.frameZone && !json_para_.frameZone->InRange(total_samples_)) {
        total_samples_++;
        return;
    }
    total_samples_++;

    void *active_ctx = ctx ? ctx : ctx_;

    FrameEle ele{};
    ele.timestamp = sample->time;
    ele.duration  = 0;
    ele.ip        = sample->ip;
    ele.caller_ip = sample->addr;
    ele.pid       = sample->pid >= 0 ? static_cast<uint32_t>(sample->pid) : 0;
    ele.tid       = sample->tid >= 0 ? static_cast<uint32_t>(sample->tid) : 0;
    ele.rank      = mpi_rank_;
    ele.cpu       = sample->cpu >= 0 ? static_cast<uint16_t>(sample->cpu) : 0;
    ele.flags     = sample->flags;

    // Determine FrameType from sample flags
    if (sample->flags & PERF_DLFILTER_FLAG_CALL) {
        ele.type = FrameType::CALL;
    } else if (sample->flags & PERF_DLFILTER_FLAG_RETURN) {
        ele.type = FrameType::RETURN;
    } else if (sample->flags & PERF_DLFILTER_FLAG_SYSCALLRET) {
        ele.type = FrameType::SYSCALL;
    } else {
        ele.type = FrameType::SAMPLE;
    }

    std::string sym_str;
    std::string caller_sym_str;
    std::string dso_str;

    if (active_ctx) {
        if (perf_dlfilter_fns.resolve_ip) {
            auto al_ip = perf_dlfilter_fns.resolve_ip(active_ctx);
            if (al_ip) {
                if (al_ip->sym)
                    sym_str = al_ip->sym;
                if (al_ip->dso)
                    dso_str = al_ip->dso;
            }
        }

        if (sample->addr_correlates_sym && perf_dlfilter_fns.resolve_addr) {
            auto al_addr = perf_dlfilter_fns.resolve_addr(active_ctx);
            if (al_addr && al_addr->sym) {
                caller_sym_str = al_addr->sym;
            }
        }
    }

    // Fallbacks if symbols could not be resolved
    if (sym_str.empty() && sample->ip) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(sample->ip));
        sym_str = buf;
    }
    if (caller_sym_str.empty() && sample->addr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(sample->addr));
        caller_sym_str = buf;
    }

    // Check funcZone filtering
    if (json_para_.funcZone) {
        if (json_para_.funcZone->IsEnoughAccumulatedFunctions()) {
            return;
        }
        uint32_t tid          = ele.tid;
        bool     in_capturing = json_para_.funcZone->IsInCapturing(tid);
        if (!in_capturing) {
            if (json_para_.funcZone->IsMatchBeginFunction(sym_str, caller_sym_str, sample->flags, tid)) {
                if (!json_para_.funcZone->CapturedEnoughFunctions(tid)) {
                    in_capturing = true;
                    json_para_.funcZone->SetCapturing(tid);
                }
            }
        }
        if (!in_capturing) {
            return;
        }
        if (in_capturing && json_para_.funcZone->IsMatchEndFunction(sym_str, caller_sym_str, sample->flags, tid)) {
            json_para_.funcZone->ClearCapturing(tid);
        }
    }

    // Check frame skipping
    if (json_para_.frameFrontSkipping && (sample->flags & PERF_DLFILTER_FLAG_CALL)) {
        for (const auto &skip_rule : *json_para_.frameFrontSkipping) {
            if (skip_rule && skip_rule->Match(sym_str, dso_str, sample->ip, sample->addr)) {
                return;
            }
        }
    }
    if (json_para_.frameEndSkipping && (sample->flags & PERF_DLFILTER_FLAG_RETURN)) {
        for (const auto &skip_rule : *json_para_.frameEndSkipping) {
            if (skip_rule && skip_rule->Match(sym_str, dso_str, sample->ip, sample->addr)) {
                return;
            }
        }
    }

    // Check frame modification
    if (json_para_.frameFrontModification && (sample->flags & PERF_DLFILTER_FLAG_CALL)) {
        for (const auto &pair : *json_para_.frameFrontModification) {
            if (pair && pair->first && pair->first->Match(sym_str, dso_str, sample->ip, sample->addr)) {
                if (pair->second) {
                    if (pair->second->sym && !pair->second->sym->empty())
                        sym_str = *pair->second->sym;
                    if (pair->second->dso && !pair->second->dso->empty())
                        dso_str = *pair->second->dso;
                }
            }
        }
    }
    if (json_para_.frameEndModification && (sample->flags & PERF_DLFILTER_FLAG_RETURN)) {
        for (const auto &pair : *json_para_.frameEndModification) {
            if (pair && pair->first && pair->first->Match(sym_str, dso_str, sample->ip, sample->addr)) {
                if (pair->second) {
                    if (pair->second->sym && !pair->second->sym->empty())
                        sym_str = *pair->second->sym;
                    if (pair->second->dso && !pair->second->dso->empty())
                        dso_str = *pair->second->dso;
                }
            }
        }
    }

    // Check endframepair replacement
    if (json_para_.endFramePair) {
        for (const auto &p : *json_para_.endFramePair) {
            if (p && p->got && p->replace && sym_str == *p->got) {
                sym_str = *p->replace;
            }
        }
    }

    AppendFrameWithSymbols(ele, sym_str, caller_sym_str, dso_str);
}

void GraphDrawer::Finish()
{
    if (is_finished_)
        return;
    is_finished_ = true;

    if (master_zone_.addr != MAP_FAILED && master_zone_.addr != nullptr) {
        auto *master_hdr = master_zone_.GetMasterHeader();
        if (master_hdr) {
            auto *entries = reinterpret_cast<ThreadCatalogEntry *>(
                static_cast<char *>(master_zone_.addr) + master_hdr->catalog_offset);
            uint32_t count = master_hdr->thread_count;
            for (uint32_t i = 0; i < count; ++i) {
                __atomic_store_n(&entries[i].is_terminated, 1, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&master_hdr->is_finished, 1, __ATOMIC_RELEASE);
        }
    }
}

void GraphDrawer::UnlinkAll()
{
    if (master_zone_.addr != MAP_FAILED && !master_zone_.shm_name.empty()) {
        shm_unlink(master_zone_.shm_name.c_str());
    }
    for (auto &sz : string_zones_) {
        if (!sz.shm_name.empty()) {
            shm_unlink(sz.shm_name.c_str());
        }
    }
    for (auto &kv : thread_streams_) {
        for (auto &tz : kv.second.zones) {
            if (!tz.shm_name.empty()) {
                shm_unlink(tz.shm_name.c_str());
            }
        }
    }
}

std::vector<ShmZoneDescriptor> GraphDrawer::GetLocalZoneDescriptors() const
{
    std::vector<ShmZoneDescriptor> descs;

    // Master zone
    if (master_zone_.addr != MAP_FAILED && master_zone_.addr != nullptr) {
        ShmZoneDescriptor desc{};
        desc.rank = mpi_rank_;
        desc.pid  = static_cast<uint32_t>(getpid());
        desc.type = ShmZoneType::MASTER;
        std::strncpy(desc.shm_name, master_zone_.shm_name.c_str(), sizeof(desc.shm_name) - 1);
        auto *hdr = master_zone_.GetMasterHeader();
        if (hdr) {
            desc.element_count = hdr->thread_count;
            desc.capacity      = hdr->max_threads;
        }
        descs.push_back(desc);
    }

    // String zones
    for (const auto &sz : string_zones_) {
        if (sz.addr != MAP_FAILED && sz.addr != nullptr) {
            ShmZoneDescriptor desc{};
            desc.rank    = mpi_rank_;
            desc.pid     = static_cast<uint32_t>(getpid());
            desc.zone_id = sz.zone_id;
            desc.type    = ShmZoneType::STRING;
            std::strncpy(desc.shm_name, sz.shm_name.c_str(), sizeof(desc.shm_name) - 1);
            auto *hdr = sz.GetStringHeader();
            if (hdr) {
                desc.is_full       = hdr->is_full;
                desc.element_count = hdr->string_size;
                desc.capacity      = hdr->string_capacity;
            }
            descs.push_back(desc);
        }
    }

    // Thread frame zones
    for (const auto &kv : thread_streams_) {
        const auto &stream = kv.second;
        for (const auto &tz : stream.zones) {
            if (tz.addr != MAP_FAILED && tz.addr != nullptr) {
                ShmZoneDescriptor desc{};
                desc.rank    = mpi_rank_;
                desc.pid     = stream.pid;
                desc.tid     = stream.tid;
                desc.zone_id = tz.zone_id;
                desc.type    = ShmZoneType::THREAD;
                std::strncpy(desc.shm_name, tz.shm_name.c_str(), sizeof(desc.shm_name) - 1);
                auto *hdr = tz.GetThreadHeader();
                if (hdr) {
                    desc.is_full       = hdr->is_full;
                    desc.element_count = hdr->frame_count;
                    desc.capacity      = hdr->frame_capacity;
                }
                descs.push_back(desc);
            }
        }
    }

    return descs;
}

#if defined(PTGRAPH_HAS_MPI)
std::vector<ShmZoneDescriptor> GraphDrawer::GatherShmZonesMpi(MPI_Comm comm)
{
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (!initialized) {
        return GetLocalZoneDescriptors();
    }

    int comm_size = 1;
    int rank      = 0;
    MPI_Comm_size(comm, &comm_size);
    MPI_Comm_rank(comm, &rank);
    mpi_rank_ = static_cast<uint32_t>(rank);
    mpi_size_ = static_cast<uint32_t>(comm_size);

    std::vector<ShmZoneDescriptor> local_descs = GetLocalZoneDescriptors();
    int                            local_count = static_cast<int>(local_descs.size());

    std::vector<int> recv_counts(comm_size, 0);
    MPI_Allgather(&local_count, 1, MPI_INT, recv_counts.data(), 1, MPI_INT, comm);

    std::vector<int> displs(comm_size, 0);
    int              total_count = 0;
    for (int i = 0; i < comm_size; ++i) {
        displs[i] = total_count * static_cast<int>(sizeof(ShmZoneDescriptor));
        total_count += recv_counts[i];
    }

    std::vector<int> recv_byte_counts(comm_size, 0);
    for (int i = 0; i < comm_size; ++i) {
        recv_byte_counts[i] = recv_counts[i] * static_cast<int>(sizeof(ShmZoneDescriptor));
    }

    std::vector<ShmZoneDescriptor> all_descs(total_count);
    int                            send_bytes = local_count * static_cast<int>(sizeof(ShmZoneDescriptor));

    MPI_Allgatherv(local_descs.data(),
                   send_bytes,
                   MPI_BYTE,
                   all_descs.data(),
                   recv_byte_counts.data(),
                   displs.data(),
                   MPI_BYTE,
                   comm);

    return all_descs;
}
#endif

const char *GraphDrawer::ResolveString(const ShmZone &zone, uint64_t offset)
{
    auto *hdr = zone.GetStringHeader();
    if (!hdr || !zone.string_pool || offset >= hdr->string_size)
        return "";
    return zone.string_pool + offset;
}

const char *GraphDrawer::ResolveString(const StringRef &ref) const
{
    if (ref.shmid < string_zones_.size()) {
        auto *hdr = string_zones_[ref.shmid].GetStringHeader();
        if (hdr && ref.offset < hdr->string_size) {
            return string_zones_[ref.shmid].string_pool + ref.offset;
        }
    }
    return "";
}

void GraphDrawer::CreateMasterZone()
{
    master_zone_.zone_id  = 0;
    master_zone_.shm_name = "/" + shm_prefix_ + "_master";

    size_t header_size    = sizeof(ShmMasterHeader);
    size_t catalog_offset = (header_size + 7) & ~7ULL;
    size_t catalog_size   = max_threads_ * sizeof(ThreadCatalogEntry);
    size_t total_size     = (catalog_offset + catalog_size + 4095) & ~4095ULL;

    int fd = shm_open(master_zone_.shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        throw std::runtime_error("shm_open master failed: " + std::string(strerror(errno)));
    }

    if (ftruncate(fd, total_size) != 0) {
        close(fd);
        shm_unlink(master_zone_.shm_name.c_str());
        throw std::runtime_error("ftruncate master failed: " + std::string(strerror(errno)));
    }

    void *addr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        shm_unlink(master_zone_.shm_name.c_str());
        throw std::runtime_error("mmap master failed: " + std::string(strerror(errno)));
    }

    std::memset(addr, 0, total_size);

    master_zone_.fd         = fd;
    master_zone_.addr       = addr;
    master_zone_.total_size = total_size;
    master_zone_.header     = addr;

    auto *hdr              = master_zone_.GetMasterHeader();
    hdr->magic             = SHM_MAGIC;
    hdr->version           = 1;
    hdr->max_threads       = static_cast<uint32_t>(max_threads_);
    hdr->thread_count      = 0;
    hdr->string_zone_count = 0;
    hdr->is_finished       = 0;
    hdr->catalog_offset    = catalog_offset;
}

void GraphDrawer::CreateNewStringZone()
{
    if (!string_zones_.empty()) {
        auto *prev_hdr = string_zones_.back().GetStringHeader();
        if (prev_hdr) {
            __atomic_store_n(&prev_hdr->is_full, 1, __ATOMIC_RELEASE);
        }
        current_string_zone_id_++;
    }

    ShmZone new_zone;
    new_zone.zone_id  = current_string_zone_id_;
    new_zone.shm_name = "/" + shm_prefix_ + "_str_zone_" + std::to_string(current_string_zone_id_);

    size_t header_size   = sizeof(ShmStringZoneHeader);
    size_t string_offset = (header_size + 7) & ~7ULL;
    size_t total_size    = (string_offset + string_capacity_ + 4095) & ~4095ULL;

    int fd = shm_open(new_zone.shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        throw std::runtime_error("shm_open string zone failed: " + std::string(strerror(errno)));
    }

    if (ftruncate(fd, total_size) != 0) {
        close(fd);
        shm_unlink(new_zone.shm_name.c_str());
        throw std::runtime_error("ftruncate string zone failed: " + std::string(strerror(errno)));
    }

    void *addr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        shm_unlink(new_zone.shm_name.c_str());
        throw std::runtime_error("mmap string zone failed: " + std::string(strerror(errno)));
    }

    std::memset(addr, 0, total_size);

    new_zone.fd          = fd;
    new_zone.addr        = addr;
    new_zone.total_size  = total_size;
    new_zone.header      = addr;
    new_zone.string_pool = static_cast<char *>(addr) + string_offset;

    auto *hdr            = new_zone.GetStringHeader();
    hdr->magic           = SHM_MAGIC;
    hdr->zone_id         = static_cast<uint8_t>(current_string_zone_id_);
    hdr->is_full         = 0;
    hdr->reserved        = 0;
    hdr->string_capacity = string_capacity_;
    hdr->string_size     = 0;
    hdr->string_offset   = string_offset;

    string_zones_.push_back(std::move(new_zone));

    auto *master_hdr = master_zone_.GetMasterHeader();
    if (master_hdr) {
        __atomic_store_n(&master_hdr->string_zone_count,
                         static_cast<uint32_t>(string_zones_.size()),
                         __ATOMIC_RELEASE);
    }
}

void GraphDrawer::CreateThreadZone(ThreadStream &stream)
{
    ShmZone new_zone;
    new_zone.zone_id  = stream.current_zone_id;
    new_zone.shm_name = "/" + shm_prefix_ + "_tid_" + std::to_string(stream.tid) +
                        "_zone_" + std::to_string(stream.current_zone_id);

    size_t header_size   = sizeof(ShmThreadZoneHeader);
    size_t frames_offset = (header_size + 7) & ~7ULL;
    size_t frames_size   = frame_capacity_ * sizeof(FrameEle);
    size_t total_size    = (frames_offset + frames_size + 4095) & ~4095ULL;

    int fd = shm_open(new_zone.shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        throw std::runtime_error("shm_open thread zone failed: " + std::string(strerror(errno)));
    }

    if (ftruncate(fd, total_size) != 0) {
        close(fd);
        shm_unlink(new_zone.shm_name.c_str());
        throw std::runtime_error("ftruncate thread zone failed: " + std::string(strerror(errno)));
    }

    void *addr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        shm_unlink(new_zone.shm_name.c_str());
        throw std::runtime_error("mmap thread zone failed: " + std::string(strerror(errno)));
    }

    std::memset(addr, 0, total_size);

    new_zone.fd         = fd;
    new_zone.addr       = addr;
    new_zone.total_size = total_size;
    new_zone.header     = addr;
    new_zone.frames     = reinterpret_cast<FrameEle *>(static_cast<char *>(addr) + frames_offset);

    auto *hdr        = new_zone.GetThreadHeader();
    hdr->magic       = SHM_MAGIC;
    hdr->pid         = stream.pid;
    hdr->tid         = stream.tid;
    hdr->zone_id     = stream.current_zone_id;
    hdr->is_full     = 0;
    hdr->reserved[0] = hdr->reserved[1] = hdr->reserved[2] = 0;
    hdr->reserved2                                         = 0;
    hdr->frame_capacity                                    = frame_capacity_;
    hdr->frame_count                                       = 0;
    hdr->frame_offset                                      = frames_offset;

    stream.zones.push_back(std::move(new_zone));
}

void GraphDrawer::RegisterThreadInMaster(ThreadStream &stream)
{
    auto *master_hdr = master_zone_.GetMasterHeader();
    if (!master_hdr)
        return;

    uint32_t current_threads = master_hdr->thread_count;
    if (current_threads >= master_hdr->max_threads) {
        return;
    }

    auto *entries = reinterpret_cast<ThreadCatalogEntry *>(
        static_cast<char *>(master_zone_.addr) + master_hdr->catalog_offset);

    entries[current_threads].pid            = stream.pid;
    entries[current_threads].tid            = stream.tid;
    entries[current_threads].active_zone_id = stream.current_zone_id;
    entries[current_threads].is_terminated  = 0;
    entries[current_threads].total_frames   = 0;

    stream.catalog_index = current_threads;
    __atomic_store_n(&master_hdr->thread_count, current_threads + 1, __ATOMIC_RELEASE);
}

void GraphDrawer::UpdateMasterZoneId(const ThreadStream &stream)
{
    auto *master_hdr = master_zone_.GetMasterHeader();
    if (!master_hdr || stream.catalog_index >= master_hdr->thread_count)
        return;

    auto *entries = reinterpret_cast<ThreadCatalogEntry *>(
        static_cast<char *>(master_zone_.addr) + master_hdr->catalog_offset);

    __atomic_store_n(&entries[stream.catalog_index].active_zone_id,
                     stream.current_zone_id,
                     __ATOMIC_RELEASE);
}

void GraphDrawer::UpdateMasterTotalFrames(const ThreadStream &stream)
{
    auto *master_hdr = master_zone_.GetMasterHeader();
    if (!master_hdr || stream.catalog_index >= master_hdr->thread_count)
        return;

    auto *entries = reinterpret_cast<ThreadCatalogEntry *>(
        static_cast<char *>(master_zone_.addr) + master_hdr->catalog_offset);

    __atomic_fetch_add(&entries[stream.catalog_index].total_frames, 1, __ATOMIC_RELEASE);
}

// ============================================================================
// PtGraphReader Implementation
// ============================================================================

PtGraphReader::PtGraphReader(std::string shm_prefix)
    : shm_prefix_(std::move(shm_prefix))
{
}

PtGraphReader::~PtGraphReader() = default;

bool PtGraphReader::OpenMaster()
{
    if (master_zone_.addr != MAP_FAILED && master_zone_.addr != nullptr) {
        return true;
    }
    std::string name = "/" + shm_prefix_ + "_master";
    int         fd   = shm_open(name.c_str(), O_RDONLY, 0666);
    if (fd < 0)
        return false;

    struct stat sb{};
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return false;
    }

    void *addr = mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return false;
    }

    master_zone_.zone_id    = 0;
    master_zone_.shm_name   = name;
    master_zone_.fd         = fd;
    master_zone_.addr       = addr;
    master_zone_.total_size = sb.st_size;
    master_zone_.header     = addr;
    return true;
}

std::vector<ThreadCatalogEntry> PtGraphReader::GetThreadEntries()
{
    std::vector<ThreadCatalogEntry> result;
    if (!OpenMaster())
        return result;

    auto *hdr = master_zone_.GetMasterHeader();
    if (!hdr || hdr->magic != GraphDrawer::SHM_MAGIC)
        return result;

    uint32_t count   = __atomic_load_n(&hdr->thread_count, __ATOMIC_ACQUIRE);
    auto    *entries = reinterpret_cast<const ThreadCatalogEntry *>(
        static_cast<const char *>(master_zone_.addr) + hdr->catalog_offset);

    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ThreadCatalogEntry entry;
        entry.pid            = entries[i].pid;
        entry.tid            = entries[i].tid;
        entry.active_zone_id = __atomic_load_n(&entries[i].active_zone_id, __ATOMIC_ACQUIRE);
        entry.is_terminated  = __atomic_load_n(&entries[i].is_terminated, __ATOMIC_ACQUIRE);
        entry.total_frames   = __atomic_load_n(&entries[i].total_frames, __ATOMIC_ACQUIRE);
        result.push_back(entry);
    }
    return result;
}

bool PtGraphReader::GetThreadEntry(uint32_t tid, ThreadCatalogEntry &out_entry)
{
    auto entries = GetThreadEntries();
    for (const auto &e : entries) {
        if (e.tid == tid) {
            out_entry = e;
            return true;
        }
    }
    return false;
}

bool PtGraphReader::IsFinished()
{
    if (!OpenMaster())
        return false;
    auto *hdr = master_zone_.GetMasterHeader();
    return hdr && (__atomic_load_n(&hdr->is_finished, __ATOMIC_ACQUIRE) == 1);
}

bool PtGraphReader::EnsureStringZone(uint8_t zone_id)
{
    if (zone_id < string_zones_.size() && string_zones_[zone_id].addr != MAP_FAILED &&
        string_zones_[zone_id].addr != nullptr) {
        return true;
    }
    if (zone_id >= string_zones_.size()) {
        string_zones_.resize(zone_id + 1);
    }
    std::string name = "/" + shm_prefix_ + "_str_zone_" + std::to_string(zone_id);
    int         fd   = shm_open(name.c_str(), O_RDONLY, 0666);
    if (fd < 0)
        return false;

    struct stat sb{};
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return false;
    }

    void *addr = mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return false;
    }

    auto *shdr                         = reinterpret_cast<ShmStringZoneHeader *>(addr);
    string_zones_[zone_id].zone_id     = zone_id;
    string_zones_[zone_id].shm_name    = name;
    string_zones_[zone_id].fd          = fd;
    string_zones_[zone_id].addr        = addr;
    string_zones_[zone_id].total_size  = sb.st_size;
    string_zones_[zone_id].header      = addr;
    string_zones_[zone_id].string_pool = static_cast<char *>(addr) + shdr->string_offset;
    return true;
}

const char *PtGraphReader::ResolveString(const StringRef &ref)
{
    if (!EnsureStringZone(ref.shmid))
        return "";
    auto &zone = string_zones_[ref.shmid];
    auto *hdr  = zone.GetStringHeader();
    if (!hdr || ref.offset >= hdr->string_size)
        return "";
    return zone.string_pool + ref.offset;
}

bool PtGraphReader::OpenThreadZone(uint32_t tid, uint32_t zone_id, ShmZone &out_zone)
{
    std::string name = "/" + shm_prefix_ + "_tid_" + std::to_string(tid) +
                       "_zone_" + std::to_string(zone_id);
    int         fd   = shm_open(name.c_str(), O_RDONLY, 0666);
    if (fd < 0)
        return false;

    struct stat sb{};
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return false;
    }

    void *addr = mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return false;
    }

    auto *thdr          = reinterpret_cast<ShmThreadZoneHeader *>(addr);
    out_zone.zone_id    = zone_id;
    out_zone.shm_name   = name;
    out_zone.fd         = fd;
    out_zone.addr       = addr;
    out_zone.total_size = sb.st_size;
    out_zone.header     = addr;
    out_zone.frames     = reinterpret_cast<FrameEle *>(static_cast<char *>(addr) + thdr->frame_offset);
    return true;
}

std::vector<FrameEle> PtGraphReader::ReadAllFramesForThread(uint32_t tid)
{
    std::vector<FrameEle> all_frames;
    uint32_t              zone_id = 0;
    while (true) {
        ShmZone zone;
        if (!OpenThreadZone(tid, zone_id, zone)) {
            break;
        }
        auto    *thdr  = zone.GetThreadHeader();
        uint64_t count = __atomic_load_n(&thdr->frame_count, __ATOMIC_ACQUIRE);
        for (uint64_t i = 0; i < count; ++i) {
            all_frames.push_back(zone.frames[i]);
        }
        bool is_full = __atomic_load_n(&thdr->is_full, __ATOMIC_ACQUIRE) == 1;
        if (!is_full) {
            break;
        }
        zone_id++;
    }
    return all_frames;
}

void PtGraphReader::UnlinkAll(const std::string &shm_prefix)
{
    PtGraphReader reader(shm_prefix);
    if (reader.OpenMaster()) {
        auto *mhdr = reader.master_zone_.GetMasterHeader();
        if (mhdr) {
            uint32_t str_count = __atomic_load_n(&mhdr->string_zone_count, __ATOMIC_ACQUIRE);
            for (uint32_t s = 0; s < str_count; ++s) {
                std::string sname = "/" + shm_prefix + "_str_zone_" + std::to_string(s);
                shm_unlink(sname.c_str());
            }
            auto entries = reader.GetThreadEntries();
            for (const auto &e : entries) {
                for (uint32_t z = 0; z <= e.active_zone_id; ++z) {
                    std::string tname = "/" + shm_prefix + "_tid_" + std::to_string(e.tid) +
                                        "_zone_" + std::to_string(z);
                    shm_unlink(tname.c_str());
                }
            }
        }
    }
    std::string mname = "/" + shm_prefix + "_master";
    shm_unlink(mname.c_str());
}
