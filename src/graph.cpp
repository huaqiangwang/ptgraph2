#include "graph.h"
#include "perfetto.h"
#include "replay_matcher.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <zlib.h>

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
    int    argc = 0;
    char **argv = nullptr;
    if (ctx_ && perf_dlfilter_fns.args) {
        argv = perf_dlfilter_fns.args(ctx_, &argc);
    }

    //--------------------------
    // Get parameters from '--dlarg'
    // Support formats:
    //   --dlarg "-r 0 -n 4 -c id -b 100 -e 200 -j conf.json -f out.ftf"
    //   --dlarg -r --dlarg 0 --dlarg -n --dlarg 4
    //   --dlarg -r0 --dlarg -n4
    std::vector<std::string> tokens;
    for (int i = 0; i < argc; ++i) {
        if (!argv[i])
            continue;
        std::istringstream iss(argv[i]);
        std::string        tok;
        while (iss >> tok) {
            tokens.push_back(tok);
        }
    }

    std::string conf_path;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string &tok = tokens[i];
        if (tok == "-f") {
            if (i + 1 < tokens.size())
                outfilename_ = tokens[++i];
        } else if (tok.rfind("-f", 0) == 0 && tok.size() > 2) {
            outfilename_ = (tok[2] == '=') ? tok.substr(3) : tok.substr(2);
        } else if (tok == "-j") {
            if (i + 1 < tokens.size())
                conf_path = tokens[++i];
        } else if (tok.rfind("-j", 0) == 0 && tok.size() > 2) {
            conf_path = (tok[2] == '=') ? tok.substr(3) : tok.substr(2);
        } else if (tok == "-r") {
            if (i + 1 < tokens.size())
                proc_rank_ = std::stoi(tokens[++i]);
        } else if (tok.rfind("-r", 0) == 0 && tok.size() > 2) {
            proc_rank_ = std::stoi((tok[2] == '=') ? tok.substr(3) : tok.substr(2));
        } else if (tok == "-n") {
            if (i + 1 < tokens.size())
                proc_size_ = std::stoi(tokens[++i]);
        } else if (tok.rfind("-n", 0) == 0 && tok.size() > 2) {
            proc_size_ = std::stoi((tok[2] == '=') ? tok.substr(3) : tok.substr(2));
        } else if (tok == "-c") {
            if (i + 1 < tokens.size())
                coord_id_ = tokens[++i];
        } else if (tok.rfind("-c", 0) == 0 && tok.size() > 2) {
            coord_id_ = (tok[2] == '=') ? tok.substr(3) : tok.substr(2);
        } else if (tok == "-b") {
            if (i + 1 < tokens.size())
                ts_rank_begin_ns_ = std::stoull(tokens[++i]);
        } else if (tok.rfind("-b", 0) == 0 && tok.size() > 2) {
            ts_rank_begin_ns_ = std::stoull((tok[2] == '=') ? tok.substr(3) : tok.substr(2));
        } else if (tok == "-e") {
            if (i + 1 < tokens.size())
                ts_rank_end_ns_ = std::stoull(tokens[++i]);
        } else if (tok.rfind("-e", 0) == 0 && tok.size() > 2) {
            ts_rank_end_ns_ = std::stoull((tok[2] == '=') ? tok.substr(3) : tok.substr(2));
        } else if (tok == "-P" || tok == "--per-tid-output") {
            per_tid_output_ = true;
        } else if (tok == "-v" || tok == "--verbose" || tok == "--debug") {
            Logger::SetLevel(LogLevel::DEBUG);
        }
    }

    if (!conf_path.empty()) {
        LoadConfig(conf_path);
    }

    if (proc_size_ > 1) {
        shm_prefix_ += "_rank_" + std::to_string(proc_rank_);
    }

    CreateMasterZone();
    CreateNewStringZone();
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

    // Only compute from json_para_.timeZone if ts_rank_begin_ns_/ts_rank_end_ns_ were not explicitly set via -b/-e
    if (ts_rank_begin_ns_ == 0 && ts_rank_end_ns_ == 0 && json_para_.timeZone) {
        ts_global_begin_ns_ = json_para_.timeZone->begin;
        ts_global_end_ns_   = json_para_.timeZone->end;

        if (ts_global_begin_ns_ >= ts_global_end_ns_) {
            LOG_WARNING << "Invalid time zone, TimeZone failed: ts_global_begin_ns_"
                        << ts_global_begin_ns_ << " >= ts_global_end_ns_" << ts_global_end_ns_ << std::endl;
            ts_global_begin_ns_ = 0;
            ts_global_end_ns_   = 0;
        } else {
            auto ts_span_ns   = ts_global_end_ns_ - ts_global_begin_ns_;
            ts_rank_begin_ns_ = ts_global_begin_ns_ + ts_span_ns * proc_rank_ / proc_size_;
            ts_rank_end_ns_   = ts_global_begin_ns_ + ts_span_ns * (proc_rank_ + 1) / proc_size_;
        }
    }
}

// Return 0: sample will be passed to `filter_event`
// Return 1: sample is beyond the rank's time zone and should be filtered out
// Return -38: ENOSYS. stop processing further samples
int GraphDrawer::FilterByTimestamp(uint64_t timestamp)
{
    if (ts_rank_begin_ns_ == 0 && ts_rank_end_ns_ == 0) {
        return 0;
    }

    if (timestamp < ts_rank_begin_ns_) {
        return 1;
    } else if (timestamp >= ts_rank_end_ns_) {
        return -ENOSYS;
    }
    return 0;
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

    inserted_frames_count_++;
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

// Todo: make this function more clear and concise
// Add debug message to trace if any dlperf sample is not processed
void GraphDrawer::AddSample(const struct perf_dlfilter_sample *sample, void *ctx)
{
    if (!sample)
        return;

    // Check frameZone filtering
    if (json_para_.frameZone && !json_para_.frameZone->InRange(total_samples_)) {
        total_samples_++;
        return;
    }
    total_samples_++;
    LOG_TRACE << "[TID" << sample->tid << "] DlFilter: TS" << sample->time << std::hex
              << sample->flags << std::dec;

    void *active_ctx = ctx ? ctx : ctx_;

    FrameEle ele{};
    ele.timestamp = sample->time;
    ele.duration  = 0;
    ele.pid       = sample->pid >= 0 ? static_cast<uint32_t>(sample->pid) : 0;
    ele.tid       = sample->tid >= 0 ? static_cast<uint32_t>(sample->tid) : 0;
    ele.rank      = static_cast<uint32_t>(proc_rank_);
    ele.cpu       = sample->cpu >= 0 ? static_cast<uint16_t>(sample->cpu) : 0;
    ele.flags     = sample->flags;

    constexpr uint32_t FLAG_TRACE_BEGIN = PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH; // 0x101

    // Determine FrameType from sample flags
    if ((sample->flags & (PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH)) == FLAG_TRACE_BEGIN) {
        ele.type = FrameType::TRACE_BEGIN;
    } else if (sample->flags & PERF_DLFILTER_FLAG_CALL) {
        ele.type = FrameType::CALL;
    } else if (sample->flags & PERF_DLFILTER_FLAG_RETURN) {
        ele.type = FrameType::RETURN;
    } else if (sample->flags & PERF_DLFILTER_FLAG_SYSCALLRET) {
        ele.type = FrameType::SYSCALL;
    } else {
        ele.type = FrameType::SAMPLE;
    }

    std::string ip_sym;
    std::string ip_dso;
    std::string addr_sym;
    std::string addr_dso;
    std::string comm_str;

    if (active_ctx) {
        if (perf_dlfilter_fns.resolve_ip) {
            auto al_ip = perf_dlfilter_fns.resolve_ip(active_ctx);
            if (al_ip) {
                if (al_ip->sym)
                    ip_sym = al_ip->sym;
                if (al_ip->dso)
                    ip_dso = al_ip->dso;
                if (al_ip->comm && al_ip->comm[0] != '\0')
                    comm_str = al_ip->comm;
            }
        }

        if (sample->addr_correlates_sym && perf_dlfilter_fns.resolve_addr) {
            auto al_addr = perf_dlfilter_fns.resolve_addr(active_ctx);
            if (al_addr) {
                if (al_addr->sym)
                    addr_sym = al_addr->sym;
                if (al_addr->dso)
                    addr_dso = al_addr->dso;
            }
        }
    }

    // Fallbacks if symbols could not be resolved
    if (ip_sym.empty() && sample->ip) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(sample->ip));
        ip_sym = buf;
    }
    if (addr_sym.empty() && sample->addr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(sample->addr));
        addr_sym = buf;
    }

    if (comm_str.empty() && ele.pid > 0) {
        std::ifstream procComm("/proc/" + std::to_string(ele.pid) + "/comm");
        if (procComm.is_open()) {
            std::getline(procComm, comm_str);
        }
    }

    std::string sym_str;
    std::string caller_sym_str;
    std::string dso_str;

    // Distinguish CALL vs RETURN callee/caller roles:
    // - CALL: destination addr is callee being entered; ip is call site in caller
    // - RETURN: ip is returning callee; addr is return site in caller
    if (ele.type == FrameType::CALL) {
        ele.addr       = sample->addr;
        ele.ip         = sample->ip;
        sym_str        = addr_sym;
        caller_sym_str = ip_sym;
        dso_str        = addr_dso.empty() ? ip_dso : addr_dso;
    } else if (ele.type == FrameType::RETURN) {
        ele.ip         = sample->ip;
        ele.addr       = sample->addr;
        sym_str        = ip_sym;
        caller_sym_str = addr_sym;
        dso_str        = ip_dso;
    } else if (ele.type == FrameType::TRACE_BEGIN) {
        ele.ip         = sample->ip;
        ele.addr       = sample->addr;
        sym_str        = addr_sym.empty() ? ip_sym : addr_sym;
        caller_sym_str = ip_sym;
        dso_str        = addr_dso.empty() ? ip_dso : addr_dso;
    } else {
        ele.ip         = sample->ip;
        ele.addr       = sample->addr;
        sym_str        = ip_sym;
        caller_sym_str = addr_sym;
        dso_str        = ip_dso;
    }
    LOG_TRACE << ", IP " << ip_sym << ", Addr " << addr_sym << ", DSO " << dso_str << ", flags 0x"
              << std::hex << sample->flags << std::dec << std::endl;

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

    if (!comm_str.empty()) {
        auto it = thread_streams_.find(ele.tid);
        if (it != thread_streams_.end()) {
            auto *master_hdr = master_zone_.GetMasterHeader();
            if (master_hdr && it->second.catalog_index < master_hdr->thread_count) {
                auto *entries = reinterpret_cast<ThreadCatalogEntry *>(
                    static_cast<char *>(master_zone_.addr) + master_hdr->catalog_offset);
                if (entries[it->second.catalog_index].comm[0] == '\0') {
                    std::strncpy(entries[it->second.catalog_index].comm, comm_str.c_str(),
                                 sizeof(entries[it->second.catalog_index].comm) - 1);
                }
            }
        }
    }
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
        desc.rank = static_cast<uint32_t>(proc_rank_);
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
            desc.rank    = static_cast<uint32_t>(proc_rank_);
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
                desc.rank    = static_cast<uint32_t>(proc_rank_);
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

bool GraphDrawer::PublishLocalZonesToCoordinator()
{
    if (proc_size_ <= 1) {
        return true;
    }

    std::string coord_name = "/ptgraph_coord_" + coord_id_;
    int         fd         = shm_open(coord_name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        LOG_ERROR << "[Rank " << proc_rank_ << "] Failed to open coordinator SHM " << coord_name << ": " << strerror(errno) << std::endl;
        return false;
    }

    void *addr = mmap(nullptr, sizeof(ShmCoordinator), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        LOG_ERROR << "[Rank " << proc_rank_ << "] Failed to mmap coordinator SHM" << std::endl;
        return false;
    }

    auto *coord = reinterpret_cast<ShmCoordinator *>(addr);
    if (proc_rank_ >= static_cast<int>(MAX_RANKS)) {
        munmap(addr, sizeof(ShmCoordinator));
        return false;
    }

    auto &slot            = coord->slots[proc_rank_];
    slot.rank             = static_cast<uint32_t>(proc_rank_);
    slot.pid              = static_cast<uint32_t>(getpid());
    slot.total_frames     = inserted_frames_count_;
    slot.early_samples    = early_samples_count_;
    slot.filtered_samples = filtered_samples_count_;

    auto   local_descs = GetLocalZoneDescriptors();
    size_t copy_count  = std::min(local_descs.size(), MAX_ZONES_PER_RANK);
    slot.zone_count    = static_cast<uint32_t>(copy_count);
    for (size_t i = 0; i < copy_count; ++i) {
        slot.zones[i] = local_descs[i];
    }

    // Atomically signal completion
    __atomic_store_n(&slot.status, static_cast<uint32_t>(WorkerState::COMPLETED), __ATOMIC_RELEASE);
    munmap(addr, sizeof(ShmCoordinator));
    return true;
}

std::vector<ShmZoneDescriptor> GraphDrawer::CollectAllZonesFromCoordinator(int timeout_ms)
{
    std::vector<ShmZoneDescriptor> all_descs;
    auto                           local_descs = GetLocalZoneDescriptors();
    all_descs.insert(all_descs.end(), local_descs.begin(), local_descs.end());

    if (proc_size_ <= 1) {
        return all_descs;
    }

    std::string coord_name = "/ptgraph_coord_" + coord_id_;
    int         fd         = shm_open(coord_name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        LOG_ERROR << "[Rank 0] Cannot open coordinator SHM " << coord_name << ": " << strerror(errno) << std::endl;
        return all_descs;
    }

    void *addr = mmap(nullptr, sizeof(ShmCoordinator), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        LOG_ERROR << "[Rank 0] Cannot mmap coordinator SHM" << std::endl;
        return all_descs;
    }

    auto *coord = reinterpret_cast<ShmCoordinator *>(addr);

    // Wait for ranks 1 .. proc_size_-1
    int elapsed_ms = 0;
    for (int r = 1; r < proc_size_; ++r) {
        while (true) {
            uint32_t status = __atomic_load_n(&coord->slots[r].status, __ATOMIC_ACQUIRE);
            if (status == static_cast<uint32_t>(WorkerState::COMPLETED) ||
                status == static_cast<uint32_t>(WorkerState::FAILED)) {
                break;
            }
            if (coord->is_aborted) {
                LOG_ERROR << "[Rank 0] Coordinator marked abort." << std::endl;
                break;
            }
            usleep(10000); // 10ms
            elapsed_ms += 10;
            if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
                LOG_WARNING << "[Rank 0] Timeout waiting for worker rank " << r << std::endl;
                break;
            }
        }

        const auto &slot = coord->slots[r];
        if (slot.status == static_cast<uint32_t>(WorkerState::COMPLETED)) {
            for (uint32_t z = 0; z < slot.zone_count; ++z) {
                all_descs.push_back(slot.zones[z]);
            }
        }
    }

    munmap(addr, sizeof(ShmCoordinator));
    return all_descs;
}

// ============================================================================
// ShmStringResolver Implementation
// ============================================================================

ShmStringResolver::~ShmStringResolver()
{
    for (auto &pair : mapped_zones_) {
        if (pair.second.addr != MAP_FAILED && pair.second.addr != nullptr) {
            munmap(pair.second.addr, pair.second.total_size);
        }
        if (pair.second.fd >= 0) {
            close(pair.second.fd);
        }
    }
}

ShmStringResolver::ShmStringResolver(ShmStringResolver &&other) noexcept
    : mapped_zones_(std::move(other.mapped_zones_)),
      pools_(std::move(other.pools_))
{
}

ShmStringResolver &ShmStringResolver::operator=(ShmStringResolver &&other) noexcept
{
    if (this != &other) {
        for (auto &pair : mapped_zones_) {
            if (pair.second.addr != MAP_FAILED && pair.second.addr != nullptr) {
                munmap(pair.second.addr, pair.second.total_size);
            }
            if (pair.second.fd >= 0) {
                close(pair.second.fd);
            }
        }
        mapped_zones_ = std::move(other.mapped_zones_);
        pools_        = std::move(other.pools_);
    }
    return *this;
}

void ShmStringResolver::AddLocalStringZones(uint32_t rank, const std::vector<ShmZone> &local_zones)
{
    for (const auto &sz : local_zones) {
        auto *hdr = sz.GetStringHeader();
        if (hdr && sz.string_pool) {
            uint64_t key = (static_cast<uint64_t>(rank) << 32) | static_cast<uint64_t>(hdr->zone_id);
            pools_[key]  = {sz.string_pool, hdr->string_size};
        }
    }
}

void ShmStringResolver::LoadFromDescriptors(const std::vector<ShmZoneDescriptor> &descs)
{
    for (const auto &desc : descs) {
        if (desc.type != ShmZoneType::STRING)
            continue;

        uint64_t key = (static_cast<uint64_t>(desc.rank) << 32) | static_cast<uint64_t>(desc.zone_id);
        if (pools_.find(key) != pools_.end() || mapped_zones_.find(key) != mapped_zones_.end()) {
            continue;
        }

        int fd = shm_open(desc.shm_name, O_RDONLY, 0666);
        if (fd < 0) {
            continue;
        }

        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            close(fd);
            continue;
        }

        size_t total_size = static_cast<size_t>(st.st_size);
        void  *addr       = mmap(nullptr, total_size, PROT_READ, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            continue;
        }

        auto       *hdr  = reinterpret_cast<const ShmStringZoneHeader *>(addr);
        const char *pool = static_cast<const char *>(addr) + hdr->string_offset;

        MappedEntry entry{};
        entry.fd           = fd;
        entry.addr         = addr;
        entry.total_size   = total_size;
        mapped_zones_[key] = entry;

        pools_[key] = {pool, hdr->string_size};
    }
}

const char *ShmStringResolver::Resolve(uint32_t rank, const StringRef &ref) const
{
    uint64_t key = (static_cast<uint64_t>(rank) << 32) | static_cast<uint64_t>(ref.shmid);
    auto     it  = pools_.find(key);
    if (it != pools_.end()) {
        if (ref.offset < it->second.size) {
            return it->second.pool + ref.offset;
        }
    }
    return "";
}

namespace {

    class ProgressReporter
    {
    public:
        ProgressReporter(const std::string &task_name, uint64_t total_items, bool is_bytes = false)
            : task_name_(task_name),
              total_items_(total_items),
              is_bytes_(is_bytes),
              is_tty_(isatty(fileno(stdout)) != 0),
              last_reported_pct_(-1),
              current_items_(0)
        {
            step_ = std::max<uint64_t>(1, total_items_ / (is_tty_ ? 200 : 10));
        }

        void Update(uint64_t current)
        {
            current_items_ = current;
            if (total_items_ == 0)
                return;

            if (current >= total_items_ || current % step_ == 0) {
                Report(current);
            }
        }

        void Finish()
        {
            if (total_items_ > 0 && current_items_ < total_items_) {
                Report(total_items_);
            }
            if (is_tty_ && total_items_ > 0) {
                std::cout << std::endl;
            }
        }

    private:
        void Report(uint64_t current)
        {
            int pct = static_cast<int>((current * 100) / total_items_);
            if (pct > 100)
                pct = 100;

            if (is_tty_) {
                std::cout << "\33[2K\r[INFO] [ptgraph] " << task_name_ << ": " << pct << "% ("
                          << FormatCount(current) << "/" << FormatCount(total_items_) << ")"
                          << std::flush;
                last_reported_pct_ = pct;
            } else {
                if (last_reported_pct_ == -1 || pct / 10 != last_reported_pct_ / 10 || current >= total_items_) {
                    LOG_INFO << "[ptgraph] " << task_name_ << ": " << pct << "% ("
                             << FormatCount(current) << "/" << FormatCount(total_items_) << ")"
                             << std::endl;
                    last_reported_pct_ = pct;
                }
            }
        }

        std::string FormatCount(uint64_t count) const
        {
            if (is_bytes_) {
                double             mb = static_cast<double>(count) / (1024.0 * 1024.0);
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << mb << " MB";
                return oss.str();
            }
            return std::to_string(count);
        }

        std::string task_name_;
        uint64_t    total_items_;
        bool        is_bytes_;
        bool        is_tty_;
        int         last_reported_pct_;
        uint64_t    current_items_;
        uint64_t    step_;
    };

    struct StackFrame
    {
        uint64_t    timestamp{0};
        uint64_t    ip{0};
        uint64_t    addr{0};
        std::string sym;
        std::string dso;
        std::string caller;
        uint32_t    flags{0};
        size_t      start_event_idx{0};
    };

    inline std::string SanitizeSymbol(std::string s)
    {
        auto pos = s.find('@');
        if (pos != std::string::npos) {
            s.resize(pos);
        }
        return s;
    }

    constexpr size_t MAX_MATCH_LOOKAHEAD = 16;

    std::string ReturnSymbol(const FrameEle          &frame,
                             const ShmStringResolver &resolver,
                             const JsonPara          &json_para)
    {
        std::string sym = resolver.Resolve(frame.rank, frame.sym);
        if (sym.empty() && frame.ip) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(frame.ip));
            sym = buf;
        }
        if (sym.empty())
            sym = "[unknown]";
        sym = SanitizeSymbol(sym);

        if (json_para.endFramePair) {
            for (const auto &pair : *json_para.endFramePair) {
                if (pair && pair->got && pair->replace && sym == *pair->got) {
                    sym = *pair->replace;
                    break;
                }
            }
        }
        return sym;
    }

    std::vector<std::string> FutureReturnSymbols(const std::vector<FrameEle> &frames,
                                                 size_t                       current_idx,
                                                 const ShmStringResolver     &resolver,
                                                 const JsonPara              &json_para)
    {
        std::vector<std::string> result;
        for (size_t i = current_idx + 1; i < frames.size() && result.size() < MAX_MATCH_LOOKAHEAD; ++i) {
            if (frames[i].type == FrameType::RETURN) {
                result.push_back(ReturnSymbol(frames[i], resolver, json_para));
            }
        }
        return result;
    }

    void ReplayOneThread(uint64_t                              pidtid,
                         const std::vector<ShmZoneDescriptor> &thread_descs,
                         const ShmStringResolver              &resolver,
                         const JsonPara                       &json_para,
                         std::vector<TimelineEvent>           &out_events,
                         const std::function<void(uint64_t)>  &on_progress = nullptr,
                         std::mutex                           *log_mutex   = nullptr)
    {
        std::vector<FrameEle> frames;

        // Load frames from each thread zone
        for (const auto &desc : thread_descs) {
            if (desc.element_count == 0)
                continue;

            int fd = shm_open(desc.shm_name, O_RDONLY, 0666);
            if (fd < 0) {
                continue;
            }

            struct stat st{};
            if (fstat(fd, &st) != 0 || st.st_size <= 0) {
                close(fd);
                continue;
            }

            void *addr = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
            if (addr == MAP_FAILED) {
                close(fd);
                continue;
            }

            auto       *hdr        = reinterpret_cast<const ShmThreadZoneHeader *>(addr);
            const auto *frames_ptr = reinterpret_cast<const FrameEle *>(
                static_cast<const char *>(addr) + hdr->frame_offset);

            uint64_t count = std::min(hdr->frame_count, desc.element_count);
            for (uint64_t i = 0; i < count; ++i) {
                frames.push_back(frames_ptr[i]);
            }

            munmap(addr, st.st_size);
            close(fd);
        }

        if (frames.empty())
            return;

        // Ensure strictly chronological ordering while preserving arrival sequence for equal timestamps
        std::stable_sort(frames.begin(), frames.end(), [](const FrameEle &a, const FrameEle &b) {
            return a.timestamp < b.timestamp;
        });

        std::vector<StackFrame> stack;

        uint64_t last_seen_time = frames.back().timestamp;
        uint32_t tid            = static_cast<uint32_t>(pidtid & 0xFFFFFFFFULL);

        uint64_t calls_count          = 0;
        uint64_t rets_count           = 0;
        uint64_t exact_match_count    = 0;
        uint64_t deep_match_count     = 0;
        uint64_t missing_call_count   = 0;
        uint64_t fallback_match_count = 0;
        uint64_t recovered_ret_count  = 0;
        uint64_t empty_rets_count     = 0;
        uint64_t syscalls_count       = 0;
        uint64_t interrupts_count     = 0;
        uint64_t tail_pops_count      = 0;
        size_t   max_depth            = 0;
        uint64_t local_count          = 0;

        for (size_t frame_idx = 0; frame_idx < frames.size(); ++frame_idx) {
            const auto &ele            = frames[frame_idx];
            std::string sym_str        = resolver.Resolve(ele.rank, ele.sym);
            std::string caller_sym_str = resolver.Resolve(ele.rank, ele.caller_sym);
            std::string dso_str        = resolver.Resolve(ele.rank, ele.dso);

            local_count++;
            if (local_count >= 20000) {
                if (on_progress) {
                    on_progress(local_count);
                }
                local_count = 0;
            }

            LOG_DEBUG << std::endl
                      << "-----------------------------------------------" << std::endl;
            LOG_DEBUG << "[TID" << tid << "] FrameEle: TS " << ele.timestamp << ", flags " << std::hex
                      << ele.flags << std::dec << ", CPU " << ele.cpu
                      << ", caller " << caller_sym_str << ", callee " << sym_str << std::endl;

            if (sym_str.empty() && ele.ip) {
                std::cout << "[TID " << tid << "] Empty sym_str for IP 0x" << std::hex << ele.ip << std::dec << std::endl;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(ele.ip));
                sym_str = buf;
            }
            if (caller_sym_str.empty() && ele.addr) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(ele.addr));
                caller_sym_str = buf;
            }
            if (sym_str.empty())
                sym_str = "[unknown]";
            if (caller_sym_str.empty())
                caller_sym_str = "[unknown]";

            if (ele.type == FrameType::CALL) {
                calls_count++;
                sym_str = SanitizeSymbol(sym_str);

                if (json_para.frameFrontSkipping) {
                    bool skip = false;
                    for (const auto &rule : *json_para.frameFrontSkipping) {
                        if (rule && rule->Match(sym_str, dso_str, ele.ip, ele.addr)) {
                            skip = true;
                            break;
                        }
                    }
                    if (skip) {
                        LOG_DEBUG << "[TID " << tid << "] Skip CALL frame: " << sym_str << std::endl;
                        continue;
                    }
                }

                if (json_para.frameFrontModification) {
                    for (const auto &pair : *json_para.frameFrontModification) {
                        if (pair && pair->first && pair->first->Match(sym_str, dso_str, ele.ip, ele.addr)) {
                            if (pair->second) {
                                if (pair->second->sym && !pair->second->sym->empty())
                                    sym_str = *pair->second->sym;
                                if (pair->second->dso && !pair->second->dso->empty())
                                    dso_str = *pair->second->dso;
                            }
                            break;
                        }
                    }
                }

                //
                // TODO: Add support of JIT
                // JIT
                // Most likely the 'ret' in a JIT function will not be captured by intel_pt
                // sym only is not nullptr when sample is 'SYSCALL' and 'TRACEDIS'
                // if 'last symbol in stack' is JIT and current sample is NOT 'SYSCALL' or 'TRACEDIS',
                // then close last frame before push the next one

                //
                // Normal case: push the current frame onto the stack
                size_t start_event_idx = out_events.size();
                stack.push_back({ele.timestamp, ele.ip, ele.addr, sym_str, dso_str, caller_sym_str, ele.flags, start_event_idx});
                if (stack.size() > max_depth)
                    max_depth = stack.size();

                LOG_DEBUG << "[TID " << tid << "] ++PUSH CALL: " << sym_str << " (caller: " << caller_sym_str << ", ts=" << ele.timestamp << ", dso=" << dso_str << ")" << std::endl;

                out_events.push_back({TimelineEventType::START, ele.timestamp, 0, pidtid, sym_str, dso_str, caller_sym_str, ele.cpu});
            } else if (ele.type == FrameType::RETURN) {
                rets_count++;
                sym_str = SanitizeSymbol(sym_str);

                if (json_para.endFramePair) {
                    for (const auto &p : *json_para.endFramePair) {
                        if (p && p->got && p->replace && sym_str == *p->got) {
                            sym_str = *p->replace;
                            break;
                        }
                    }
                }

                if (json_para.frameEndSkipping) {
                    bool skip = false;
                    for (const auto &rule : *json_para.frameEndSkipping) {
                        if (rule && rule->Match(sym_str, dso_str, ele.ip, ele.addr)) {
                            skip = true;
                            break;
                        }
                    }
                    if (skip) {
                        LOG_DEBUG << "[TID " << tid << "] Skip RET frame: " << sym_str << std::endl;
                        continue;
                    }
                }

                if (json_para.frameEndModification) {
                    for (const auto &pair : *json_para.frameEndModification) {
                        if (pair && pair->first && pair->first->Match(sym_str, dso_str, ele.ip, ele.addr)) {
                            if (pair->second) {
                                if (pair->second->sym && !pair->second->sym->empty())
                                    sym_str = *pair->second->sym;
                                if (pair->second->dso && !pair->second->dso->empty())
                                    dso_str = *pair->second->dso;
                            }
                            break;
                        }
                    }
                }

                std::vector<ReplayStackEntry> stack_top_first;
                size_t                        stack_depth = std::min(stack.size(), MAX_MATCH_LOOKAHEAD);
                stack_top_first.reserve(stack_depth);
                for (size_t i = 0; i < stack_depth; ++i) {
                    const auto &frame = stack[stack.size() - 1 - i];
                    stack_top_first.push_back({frame.sym, frame.caller});
                }

                auto future_returns = FutureReturnSymbols(frames, frame_idx, resolver, json_para);
                auto decision       = ChooseReturnRecovery(stack_top_first, sym_str, caller_sym_str,
                                                           future_returns, MAX_MATCH_LOOKAHEAD);
                if (!stack.empty() && decision.pop_count != 1) {
                    LOG_DEBUG << "[TID " << tid << "] RET mismatch: " << sym_str
                              << ", keep score=" << decision.keep_score
                              << ", pop score=" << decision.pop_score
                              << ", decision=" << (decision.keep_stack ? "missing CALL" : "missing RETURN(s)")
                              << std::endl;
                }

                if (decision.keep_stack) {
                    if (stack.empty()) {
                        empty_rets_count++;
                        LOG_DEBUG << "[TID " << tid << "] RET isolated (empty stack, called pre-trace): "
                                  << sym_str << " (ts=" << ele.timestamp << "), instantaneous closure" << std::endl;
                    } else {
                        missing_call_count++;
                        LOG_DEBUG << "[TID " << tid << "] RET isolated (missing CALL): "
                                  << sym_str << " (ts=" << ele.timestamp << "), instantaneous closure" << std::endl;
                    }
                    // Instantaneous closure for isolated RETURN
                    out_events.push_back({TimelineEventType::FULL, ele.timestamp, ele.timestamp,
                                          pidtid, sym_str, dso_str, caller_sym_str, ele.cpu});
                    continue;
                }

                if (decision.pop_count > 0) {
                    size_t num_popped = decision.pop_count;
                    if (decision.fallback_pop) {
                        fallback_match_count++;
                    } else if (num_popped == 1) {
                        exact_match_count++;
                    } else {
                        deep_match_count++;
                        recovered_ret_count += num_popped - 1;
                    }

                    for (size_t i = 0; i < num_popped; ++i) {
                        auto popped = stack.back();
                        stack.pop_back();

                        uint64_t dur = ele.timestamp >= popped.timestamp ? (ele.timestamp - popped.timestamp) : 0;
                        LOG_DEBUG << "[TID " << tid << "][Depth " << stack.size() + 1 << "] --POP RET: "
                                  << popped.sym << " (matched " << sym_str << ", dur=" << dur << " ns)" << std::endl;

                        out_events.push_back({TimelineEventType::END, ele.timestamp, popped.timestamp,
                                              pidtid, popped.sym, popped.dso, popped.caller, ele.cpu});
                    }
                }
            } else if (ele.type == FrameType::TRACE_BEGIN ||
                       ((ele.flags & (PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH)) ==
                        (PERF_DLFILTER_FLAG_TRACE_BEGIN | PERF_DLFILTER_FLAG_BRANCH))) {
                constexpr uint32_t FLAG_INTERRUPT = PERF_DLFILTER_FLAG_BRANCH | PERF_DLFILTER_FLAG_CALL |
                                                    PERF_DLFILTER_FLAG_ASYNC | PERF_DLFILTER_FLAG_INTERRUPT; // 0x63
                if (!stack.empty() &&
                    ((stack.back().flags & FLAG_INTERRUPT) == FLAG_INTERRUPT || stack.back().flags == FLAG_INTERRUPT)) {
                    auto popped = stack.back();
                    stack.pop_back();

                    interrupts_count++;
                    if (calls_count > 0) {
                        calls_count--;
                    }
                    uint64_t dur = ele.timestamp >= popped.timestamp ? (ele.timestamp - popped.timestamp) : 0;

                    std::string irq_sym = "[interrupt]";

                    // Mark the start event as interrupt
                    if (popped.start_event_idx < out_events.size()) {
                        out_events[popped.start_event_idx].sym = irq_sym;
                    }

                    LOG_DEBUG << "[TID " << tid << "][Depth " << stack.size() + 1 << "] --POP INTERRUPT: "
                              << irq_sym << " (dur=" << dur << " ns, started at " << popped.timestamp << ")" << std::endl;

                    out_events.push_back({TimelineEventType::END, ele.timestamp, popped.timestamp,
                                          pidtid, irq_sym, popped.dso, popped.caller, ele.cpu});
                }
            } else if (ele.type == FrameType::SYSCALL) {
                syscalls_count++;
                uint64_t end_time = ele.duration > 0 ? (ele.timestamp + ele.duration) : (ele.timestamp + 100);

                LOG_DEBUG << "[TID " << tid << "][Depth " << stack.size() << "] SYSCALL: "
                          << sym_str << " (dur=" << (end_time - ele.timestamp) << " ns)" << std::endl;

                out_events.push_back({TimelineEventType::FULL, end_time, ele.timestamp,
                                      pidtid, sym_str, dso_str, caller_sym_str, ele.cpu});
            }
        }

        if (local_count > 0 && on_progress) {
            on_progress(local_count);
        }

        // Pop any remaining unreturned frames at stream end
        while (!stack.empty()) {
            tail_pops_count++;
            auto popped = stack.back();
            stack.pop_back();
            uint64_t end_time = last_seen_time > popped.timestamp ? last_seen_time : (popped.timestamp + 100);

            LOG_DEBUG << "[TID " << tid << "][Depth " << stack.size() + 1 << "] Tail pop unclosed frame: "
                      << popped.sym << " (dur=" << (end_time - popped.timestamp) << " ns)" << std::endl;

            out_events.push_back({TimelineEventType::END, end_time, popped.timestamp,
                                  pidtid, popped.sym, popped.dso, popped.caller, 0});
        }

        if (log_mutex) {
            log_mutex->lock();
        }
        if (isatty(fileno(stdout))) {
            std::cout << "\33[2K\r";
        }
        LOG_INFO << "[ptgraph] Thread " << tid << " replay summary: " << frames.size() << " frames ("
                 << calls_count << " calls, " << rets_count << " rets ["
                 << exact_match_count << " exact, " << deep_match_count << " deep, "
                 << recovered_ret_count << " recovered missing returns, "
                 << missing_call_count << " missing calls, " << fallback_match_count << " fallback, "
                 << empty_rets_count << " pre-trace empty], "
                 << syscalls_count << " syscalls, "
                 << interrupts_count << " interrupts, "
                 << tail_pops_count << " tail pops, max stack depth: "
                 << max_depth << ")" << std::endl;
        if (log_mutex) {
            log_mutex->unlock();
        }
    }

} // namespace

void GraphDrawer::GeneratePerfettoTrace(const std::string &outfile, const std::vector<ShmZoneDescriptor> &all_zones)
{
    LOG_INFO << "[ptgraph] Generating Perfetto trace into " << outfile
             << " (mode: " << (GetPerTidOutput() ? "Per-TID separate files" : "Unified single trace") << ")..." << std::endl;

    // 1. Build cross-rank string resolver
    ShmStringResolver resolver;
    resolver.AddLocalStringZones(proc_rank_, string_zones_);
    resolver.LoadFromDescriptors(all_zones);

    // 2. Discover process and thread names from Master zones
    std::unordered_map<uint32_t, std::string> pid_to_comm;
    std::unordered_map<uint64_t, std::string> tid_to_comm;

    for (const auto &desc : all_zones) {
        if (desc.type != ShmZoneType::MASTER)
            continue;

        int fd = shm_open(desc.shm_name, O_RDONLY, 0666);
        if (fd < 0)
            continue;

        struct stat st{};
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            void *addr = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
            if (addr != MAP_FAILED) {
                auto       *hdr     = reinterpret_cast<const ShmMasterHeader *>(addr);
                const auto *entries = reinterpret_cast<const ThreadCatalogEntry *>(
                    static_cast<const char *>(addr) + hdr->catalog_offset);
                for (uint32_t i = 0; i < hdr->thread_count; ++i) {
                    if (entries[i].comm[0] != '\0') {
                        pid_to_comm[entries[i].pid] = entries[i].comm;
                        uint64_t pt                 = (static_cast<uint64_t>(entries[i].pid) << 32) | entries[i].tid;
                        tid_to_comm[pt]             = entries[i].comm;
                    }
                }
                munmap(addr, st.st_size);
            }
        }
        close(fd);
    }

    // 3. Group thread zones by (pid, tid)
    std::unordered_map<uint64_t, std::vector<ShmZoneDescriptor>> thread_zones_map;
    std::vector<uint64_t>                                        thread_keys;

    for (const auto &desc : all_zones) {
        if (desc.type != ShmZoneType::THREAD)
            continue;
        uint64_t pidtid = (static_cast<uint64_t>(desc.pid) << 32) | static_cast<uint64_t>(desc.tid);
        if (thread_zones_map.find(pidtid) == thread_zones_map.end()) {
            thread_keys.push_back(pidtid);
        }
        thread_zones_map[pidtid].push_back(desc);
    }

    // Sort zone descriptors for each thread by (rank, zone_id)
    for (auto &pair : thread_zones_map) {
        std::sort(pair.second.begin(), pair.second.end(), [](const ShmZoneDescriptor &a, const ShmZoneDescriptor &b) {
            if (a.rank != b.rank)
                return a.rank < b.rank;
            return a.zone_id < b.zone_id;
        });
    }

    // Ensure fallback process and thread names for any missing
    for (uint64_t pidtid : thread_keys) {
        uint32_t pid = static_cast<uint32_t>(pidtid >> 32);
        uint32_t tid = static_cast<uint32_t>(pidtid & 0xFFFFFFFFULL);

        if (pid_to_comm.find(pid) == pid_to_comm.end() || pid_to_comm[pid].empty()) {
            std::string   pcomm;
            std::ifstream procComm("/proc/" + std::to_string(pid) + "/comm");
            if (procComm.is_open()) {
                std::getline(procComm, pcomm);
            }
            if (pcomm.empty()) {
                pcomm = "pid_" + std::to_string(pid);
            }
            pid_to_comm[pid] = pcomm;
        }

        if (tid_to_comm.find(pidtid) == tid_to_comm.end() || tid_to_comm[pidtid].empty()) {
            tid_to_comm[pidtid] = pid_to_comm[pid].empty() ? ("thread_" + std::to_string(tid)) : pid_to_comm[pid];
        }
    }

    bool   is_per_tid = GetPerTidOutput();
    size_t num_tids   = thread_keys.size();

    uint64_t total_replay_frames = 0;
    for (const auto &pair : thread_zones_map) {
        for (const auto &desc : pair.second) {
            total_replay_frames += desc.element_count;
        }
    }

    LOG_INFO << "[ptgraph] Replaying timelines for " << num_tids << " threads concurrently ("
             << total_replay_frames << " frames)..." << std::endl;

    ProgressReporter      replay_progress("Replaying timelines", total_replay_frames);
    std::atomic<uint64_t> replayed_frames_total{0};
    std::mutex            replay_progress_mutex;
    if (total_replay_frames > 0) {
        replay_progress.Update(0);
    }

    auto on_replay_progress = [&](uint64_t count) {
        uint64_t                    current = replayed_frames_total.fetch_add(count, std::memory_order_relaxed) + count;
        std::lock_guard<std::mutex> lock(replay_progress_mutex);
        replay_progress.Update(current);
    };

    unsigned int num_workers = std::thread::hardware_concurrency();
    if (num_workers == 0)
        num_workers = 4;
    num_workers = std::min(num_workers, static_cast<unsigned int>(num_tids));
    if (num_workers == 0)
        num_workers = 1;

    std::vector<std::vector<TimelineEvent>> per_thread_results(num_tids);
    std::atomic<size_t>                     next_thread_idx{0};
    std::vector<std::thread>                workers;

    for (unsigned int w = 0; w < num_workers; ++w) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = next_thread_idx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= num_tids)
                    break;

                uint64_t pidtid = thread_keys[idx];
                uint32_t pid    = static_cast<uint32_t>(pidtid >> 32);
                uint32_t tid    = static_cast<uint32_t>(pidtid & 0xFFFFFFFFULL);

                ReplayOneThread(pidtid, thread_zones_map[pidtid], resolver, json_para_,
                                per_thread_results[idx],
                                on_replay_progress, &replay_progress_mutex);

                if (is_per_tid) {
                    Perfetto writer("out_tid_" + std::to_string(tid) + ".ftf");
                    writer.WriteProcessRecord(pid, std::make_shared<std::string>(pid_to_comm[pid]));
                    writer.WriteThreadNameRecord(pid, tid, std::make_shared<std::string>(tid_to_comm[pidtid]));
                    for (const auto &event : per_thread_results[idx]) {
                        if (event.type == TimelineEventType::START) {
                            writer.WriteFrameStart(event.timestamp, event.pidtid,
                                                   std::make_shared<std::string>(event.sym), 0);
                        } else if (event.type == TimelineEventType::END) {
                            writer.WriteFrameEnd(event.timestamp, event.pidtid, event.start_timestamp,
                                                 std::make_shared<std::string>(event.dso),
                                                 std::make_shared<std::string>(event.sym), 0);
                        } else {
                            writer.WriteFrameFull(event.start_timestamp, event.timestamp, event.pidtid,
                                                  std::make_shared<std::string>(event.dso),
                                                  std::make_shared<std::string>(event.sym), 0);
                        }
                    }
                    writer.Flush();
                }
            }
        });
    }

    for (auto &t : workers) {
        t.join();
    }
    replay_progress.Finish();

    if (is_per_tid) {
        LOG_INFO << "[ptgraph] Per-TID Perfetto trace files generated successfully for "
                 << num_tids << " threads." << std::endl;
        return;
    }

    // 4. In unified mode: serialize all buffered events chronologically into outfile
    bool        ends_with_gz      = (outfile.size() >= 3 && outfile.substr(outfile.size() - 3) == ".gz");
    std::string uncompressed_file = ends_with_gz ? (outfile + ".tmp") : outfile;
    std::string gz_file           = ends_with_gz ? outfile : (outfile + ".gz");

    uint64_t total_events_to_write = 0;
    for (size_t i = 0; i < num_tids; ++i) {
        total_events_to_write += per_thread_results[i].size();
    }

    LOG_INFO << "[ptgraph] Serializing unified trace into " << uncompressed_file
             << " (" << total_events_to_write << " events across " << num_tids << " threads)..." << std::endl;
    Perfetto unified(uncompressed_file);

    for (const auto &pair : pid_to_comm) {
        unified.WriteProcessRecord(pair.first, std::make_shared<std::string>(pair.second));
    }
    for (const auto &pair : tid_to_comm) {
        uint32_t p = static_cast<uint32_t>(pair.first >> 32);
        uint32_t t = static_cast<uint32_t>(pair.first & 0xFFFFFFFFULL);
        unified.WriteThreadNameRecord(p, t, std::make_shared<std::string>(pair.second));
    }

    struct QueueItem
    {
        uint64_t timestamp;
        size_t   tid_idx;
        size_t   event_idx;

        bool operator>(const QueueItem &other) const
        {
            return timestamp > other.timestamp;
        }
    };

    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;
    for (size_t i = 0; i < num_tids; ++i) {
        if (!per_thread_results[i].empty()) {
            pq.push({per_thread_results[i][0].timestamp, i, 0});
        }
    }

    ProgressReporter ser_progress("Writing " + uncompressed_file, total_events_to_write);
    if (total_events_to_write > 0) {
        ser_progress.Update(0);
    }

    uint64_t total_events_written = 0;
    while (!pq.empty()) {
        auto top = pq.top();
        pq.pop();

        const auto &ev = per_thread_results[top.tid_idx][top.event_idx];
        if (ev.type == TimelineEventType::START) {
            unified.WriteFrameStart(ev.timestamp, ev.pidtid, std::make_shared<std::string>(ev.sym), 0);
        } else if (ev.type == TimelineEventType::END) {
            unified.WriteFrameEnd(ev.timestamp, ev.pidtid, ev.start_timestamp,
                                  std::make_shared<std::string>(ev.dso),
                                  std::make_shared<std::string>(ev.sym), 0);
        } else if (ev.type == TimelineEventType::FULL) {
            unified.WriteFrameFull(ev.start_timestamp, ev.timestamp, ev.pidtid,
                                   std::make_shared<std::string>(ev.dso),
                                   std::make_shared<std::string>(ev.sym), 0);
        }
        total_events_written++;
        ser_progress.Update(total_events_written);

        if (top.event_idx + 1 < per_thread_results[top.tid_idx].size()) {
            pq.push({per_thread_results[top.tid_idx][top.event_idx + 1].timestamp,
                     top.tid_idx, top.event_idx + 1});
        }
    }

    ser_progress.Finish();
    unified.Flush();
    LOG_INFO << "[ptgraph] Unified Perfetto trace written to " << uncompressed_file
             << " (" << total_events_written << " events across " << num_tids << " threads)." << std::endl;

    // 5. In unified mode: gzip compress the generated trace file
    CompressFileGzip(uncompressed_file, gz_file);
    if (ends_with_gz) {
        std::remove(uncompressed_file.c_str());
    }
}

bool GraphDrawer::CompressFileGzip(const std::string &src_path, const std::string &dst_path)
{
    FILE *in = fopen(src_path.c_str(), "rb");
    if (!in) {
        LOG_ERROR << "[ptgraph] Failed to open " << src_path << " for gzip compression: "
                  << strerror(errno) << std::endl;
        return false;
    }

    gzFile out = gzopen(dst_path.c_str(), "wb6");
    if (!out) {
        LOG_ERROR << "[ptgraph] Failed to create " << dst_path << " for gzip compression: "
                  << strerror(errno) << std::endl;
        fclose(in);
        return false;
    }

    gzbuffer(out, 512 * 1024);

    struct stat st{};
    uint64_t    total_bytes = 0;
    if (fstat(fileno(in), &st) == 0 && st.st_size > 0) {
        total_bytes = static_cast<uint64_t>(st.st_size);
    }

    LOG_INFO << "[ptgraph] Compressing " << src_path << " to " << dst_path << "..." << std::endl;
    ProgressReporter gz_progress("Compressing " + src_path, total_bytes, true);
    if (total_bytes > 0) {
        gz_progress.Update(0);
    }

    const size_t      buf_size = 512 * 1024;
    std::vector<char> buffer(buf_size);
    uint64_t          bytes_read_total = 0;

    while (true) {
        size_t n = fread(buffer.data(), 1, buf_size, in);
        if (n == 0)
            break;
        int written = gzwrite(out, buffer.data(), static_cast<unsigned int>(n));
        if (written <= 0) {
            LOG_ERROR << "[ptgraph] Error writing to gzip file " << dst_path << std::endl;
            gzclose(out);
            fclose(in);
            return false;
        }
        bytes_read_total += n;
        gz_progress.Update(bytes_read_total);
    }

    gz_progress.Finish();
    gzclose(out);
    fclose(in);

    struct stat dst_st{};
    if (stat(dst_path.c_str(), &dst_st) == 0) {
        double orig_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
        double comp_mb = static_cast<double>(dst_st.st_size) / (1024.0 * 1024.0);
        double ratio   = total_bytes > 0 ? (static_cast<double>(dst_st.st_size) * 100.0 / total_bytes) : 0.0;
        LOG_INFO << "[ptgraph] Gzip compression finished: " << dst_path
                 << " (" << std::fixed << std::setprecision(1) << orig_mb << " MB -> "
                 << comp_mb << " MB, " << ratio << "%)" << std::endl;
    }
    return true;
}

void GraphDrawer::GenerateGraph(const std::string &outfile, const std::vector<ShmZoneDescriptor> &gathered_zones)
{
    std::string target_file = outfile.empty() ? outfilename_ : outfile;
    if (target_file.empty()) {
        target_file = "out.ftf";
    }

    std::vector<ShmZoneDescriptor> all_zones = gathered_zones;
    if (all_zones.empty()) {
        if (proc_size_ > 1 && proc_rank_ == 0) {
            all_zones = CollectAllZonesFromCoordinator();
        } else {
            all_zones = GetLocalZoneDescriptors();
        }
    }

    GeneratePerfettoTrace(target_file, all_zones);
}

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
    std::memset(entries[current_threads].comm, 0, sizeof(entries[current_threads].comm));

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
