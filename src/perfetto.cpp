//
// Created by david on 1/3/25.
//

#include "perfetto.h"
// #include "graph.h"
#include <iostream>

uint64_t StringCache::GetRef(Perfetto *perfetto, std::shared_ptr<std::string> s)
{
    if (s->empty()) {
        // std::cout << "StringCache: get_ref for an empty string" << std::endl;
        return 0;
    }
    auto     it  = cache_.find(*s);
    uint16_t idx = 0;
    if (it != cache_.end()) {
        return it->second + RESERVED;
    } else {
        if (cache_.size() >= capacity_) {
            idx = cache_[lru_.back()];
            cache_.erase(lru_.back());
            lru_.pop_back();
        } else {
            idx = cache_.size();
        }
        cache_[*s] = idx;
        lru_.push_front(*s);
        // info("StringCache: cache miss");
        // info(s);
        perfetto->WriteStringRecord(idx + RESERVED, s, outstream);
        // Bug existed here?
        return idx + RESERVED;
    }
}

uint64_t ThreadCache::GetRef(Perfetto *perfetto, uint64_t pidtid)
{
    auto    pid_tid = PidTid(pidtid);
    auto    it      = cache_.find(pid_tid.pidtid);
    uint8_t idx     = 0;
    if (it != cache_.end()) {
        return it->second + RESERVED;
    } else {
        if (cache_.size() >= capacity_) {
            idx = cache_[lru_.back()];
            cache_.erase(lru_.back());
            lru_.pop_back();
        } else
            idx = cache_.size();
        cache_[pid_tid.pidtid] = idx;
        lru_.push_front(pid_tid.pidtid);
        perfetto->WriteThreadRecord(idx + RESERVED, pid_tid, outstream);
        return idx + RESERVED;
    }
}

Perfetto::Perfetto(std::string filename) : tid_w_map_active(), tid_w_map(), tid_cache_map()
{
    w = std::make_shared<std::ofstream>();
    w->open(filename, std::ios::binary);
    if (!w->is_open())
        throw std::runtime_error("Failed to open file");
    c = std::make_shared<Caches>(w);
    WriteHeader(w);
}

std::shared_ptr<std::ofstream> Perfetto::CreatePerfettoStream(std::string &filename, uint64_t pidtid)
{
    std::shared_ptr<std::ofstream> tidstream = nullptr;
    if (tid_w_map_active.find(pidtid) != tid_w_map_active.end()) {
        tidstream = tid_w_map_active[pidtid];
        return tidstream;
    }
    if (tid_w_map.find(pidtid) != tid_w_map.end()) {
        tidstream                = tid_w_map[pidtid];
        tid_w_map_active[pidtid] = tidstream;
        return tidstream;
    }
    tidstream = std::make_shared<std::ofstream>();
    tidstream->open(filename, std::ios::binary);
    if (!tidstream->is_open())
        throw std::runtime_error("Failed to open file");
    WriteHeader(tidstream);
    tid_w_map[pidtid]        = tidstream;
    tid_w_map_active[pidtid] = tidstream;

    auto tidcache         = std::make_shared<Caches>(tidstream);
    tid_cache_map[pidtid] = tidcache;

    for (const auto &proc : process_names) {
        WriteProcessRecord(proc.first, proc.second, tidstream);
    }
    for (const auto &thr : thread_names) {
        PidTid pt(thr.first);
        WriteThreadNameRecord(pt.pid, pt.tid, thr.second, tidstream);
    }
    return tidstream;
}

void Perfetto::DestoryPerfettoStream(uint64_t pidtid)
{
    if (tid_w_map_active.find(pidtid) == tid_w_map_active.end())
        return;
    tid_w_map_active.erase(pidtid);
}

void Perfetto::WriteValue(std::shared_ptr<std::ofstream> out, uint64_t value)
{
    out->write(reinterpret_cast<const char *>(&value), sizeof(value));
    if (out->fail())
        throw std::runtime_error("Failed to write u64");
}

void Perfetto::WriteString(std::shared_ptr<std::ofstream> out, std::shared_ptr<std::string> message)
{
    uint32_t len = message->size() > 32000 ? 32000 : static_cast<uint32_t>(message->size());
    out->write(message->c_str(), len);
    if (out->fail())
        throw std::runtime_error("Failed to write string");
    if (len % 8 != 0)
        out->write("\0\0\0\0\0\0\0\0", 8 - len % 8);
}

std::string Perfetto::GetInernalStringName(InternalString x)
{
    switch (x) {
    case InternalString::Empty:
        return "";
    case InternalString::Instructions:
        return "Instructions";
    case InternalString::Cycles:
        return "Cycles";
    case InternalString::Symbol:
        return "Symbol";
    case InternalString::Timespan:
        return "Timespan";
    case InternalString::DSO:
        return "DSO";
    case InternalString::FREQ:
        return "Frequency";
    case InternalString::Process:
        return "process";
    default:
        throw std::invalid_argument("Unknown InternalString value");
    }
}

int Perfetto::GetTimeStampStr(char *buffer, int buflen, uint64_t nano)
{
    int i = 0;
    while (i < 9) {
        buffer[buflen - i - 1] = '0' + (nano % 10);
        nano /= 10;
        ++i;
    }
    buffer[buflen - i - 1] = '.';
    ++i;
    while (i < 11 || nano > 0) {
        buffer[buflen - 1 - i] = '0' + (nano % 10);
        nano /= 10;
        ++i;
    }
    buflen = i;
    return buflen;
}

const char *Perfetto::GetTimeSpan(uint64_t nanostart, uint64_t nanoend)
{
    int buflen = SYM_NAME_BUFF_LEN;
    int ts1len = 0;
    int ts2len = 0;

    buf[buflen - 1]          = '\0';
    ts1len                   = GetTimeStampStr(buf, buflen - 1, nanoend);
    ts2len                   = GetTimeStampStr(buf, buflen - ts1len - 2, nanostart);
    buf[buflen - ts1len - 2] = ',';
    return buf + buflen - ts1len - ts2len - 2;
}

void Perfetto::WriteSpecificStringRecord(std::shared_ptr<std::ofstream> out, uint64_t id, std::shared_ptr<std::string> message)
{
    uint64_t len   = message->size() > 32000 ? 32000 : message->size();
    uint64_t rsize = 1 + (len + 7) / 8;
    uint64_t value = 2LL | rsize << 4 | id << 16 | len << 32;
    WriteValue(out, value);
    WriteString(out, message);
}

void Perfetto::WriteStringRecord(uint64_t id, std::shared_ptr<std::string> message, std::shared_ptr<std::ofstream> outstream)
{
    uint64_t len   = message->size() > 32000 ? 32000 : message->size();
    uint64_t rsize = 1 + (len + 7) / 8;
    uint64_t value = 2LL | rsize << 4 | id << 16 | len << 32;
    WriteValue(outstream, value);
    WriteString(outstream, message);
}

void Perfetto::WriteEventHeader(EventHeader e, [[maybe_unused]] uint64_t flag, std::shared_ptr<std::ofstream> tidstream)
{
    uint64_t rtype        = 4;
    uint64_t rsize        = 2 + e.extra_data_size;
    uint64_t name_ref     = c->string_cache.GetRef(this, e.name);
    uint64_t category_ref = c->string_cache.GetRef(this, std::make_shared<std::string>(e.category));
    uint64_t thread_ref   = c->thread_cache.GetRef(this, e.pid_tid);
    uint64_t value        = rtype | rsize << 4 | e.etype << 16 | e.nargs << 20 | thread_ref << 24 | category_ref << 32 | name_ref << 48;
    WriteValue(w, value);
    WriteValue(w, e.stamp);
    if (tidstream != nullptr) {
        name_ref     = tid_cache_map[e.pid_tid]->string_cache.GetRef(this, e.name);
        category_ref = tid_cache_map[e.pid_tid]->string_cache.GetRef(this, std::make_shared<std::string>(e.category));
        thread_ref   = tid_cache_map[e.pid_tid]->thread_cache.GetRef(this, e.pid_tid);
        value        = rtype | rsize << 4 | e.etype << 16 | e.nargs << 20 | thread_ref << 24 | category_ref << 32 | name_ref << 48;
        WriteValue(tidstream, value);
        WriteValue(tidstream, e.stamp);
    }
}

void Perfetto::WriteHeader(std::shared_ptr<std::ofstream> out)
{
    uint64_t          rtype       = 0;
    uint64_t          mtype       = 1;
    const std::string name        = "scylla";
    uint64_t          name_len    = name.size();
    uint64_t          rsize       = 1 + (name.size() + 7) / 8;
    uint64_t          provider_id = 0; // The only provider
    uint64_t          value       = rtype | (rsize << 4) | (mtype << 16) | (provider_id << 20) | (name_len << 52);

    WriteValue(out, 0x0016547846040010LL);
    WriteValue(out, value);
    WriteString(out, std::make_shared<std::string>(name));
    rtype = 0;
    rsize = 1;
    mtype = 2;
    value = rtype | (rsize << 4) | (mtype << 16) | (provider_id << 20);
    WriteValue(out, value);

    // Internal strings
    for (auto i = 1; i < static_cast<int>(InternalString::COUNT); ++i) {
        auto message = std::make_shared<std::string>(
            GetInernalStringName(static_cast<InternalString>(i)));
        WriteSpecificStringRecord(out, i, message);
    }
}

void Perfetto::WriteThreadRecord(uint64_t id, PidTid pid_tid, std::shared_ptr<std::ofstream> outstream)
{
    uint64_t rsize  = 3;
    uint64_t pid64  = pid_tid.pid;
    uint64_t tid64  = pid_tid.tid;
    uint64_t record = 3LL | rsize << 4 | id << 16;
    WriteValue(outstream, record);
    WriteValue(outstream, pid64);
    WriteValue(outstream, tid64);
}

void Perfetto::WriteProcessRecord(uint32_t pid, std::shared_ptr<std::string> name)
{
    if (!name || name->empty())
        return;
    process_names[pid] = name;
    if (written_proc_names.find(pid) != written_proc_names.end() && written_proc_names[pid] == *name)
        return;
    written_proc_names[pid] = *name;

    WriteProcessRecord(pid, name, w);
    for (auto &pair : tid_w_map_active) {
        if (pair.second) {
            WriteProcessRecord(pid, name, pair.second);
        }
    }
}

void Perfetto::WriteProcessRecord(uint32_t pid, std::shared_ptr<std::string> name, std::shared_ptr<std::ofstream> outstream)
{
    if (!name || name->empty())
        return;
    auto                    target_stream = outstream ? outstream : w;
    std::shared_ptr<Caches> target_caches = (target_stream == w) ? c : nullptr;
    if (!target_caches) {
        for (auto &pair : tid_w_map) {
            if (pair.second == target_stream) {
                target_caches = tid_cache_map[pair.first];
                break;
            }
        }
    }
    if (!target_caches)
        target_caches = c;

    uint64_t name_ref = target_caches->string_cache.GetRef(this, name);
    uint64_t rtype    = 7;
    uint64_t rsize    = 2;
    uint64_t obj_type = 1; // kZxObjTypeProcess
    uint64_t header   = rtype | (rsize << 4) | (obj_type << 16) | (name_ref << 24);
    WriteValue(target_stream, header);
    WriteValue(target_stream, static_cast<uint64_t>(pid));
}

void Perfetto::WriteThreadNameRecord(uint32_t pid, uint32_t tid, std::shared_ptr<std::string> thread_name)
{
    if (!thread_name || thread_name->empty())
        return;
    PidTid pt(pid, tid);
    thread_names[pt.pidtid] = thread_name;
    if (written_thread_names.find(pt.pidtid) != written_thread_names.end() && written_thread_names[pt.pidtid] == *thread_name)
        return;
    written_thread_names[pt.pidtid] = *thread_name;

    WriteThreadNameRecord(pid, tid, thread_name, w);
    for (auto &pair : tid_w_map_active) {
        if (pair.second) {
            WriteThreadNameRecord(pid, tid, thread_name, pair.second);
        }
    }
}

void Perfetto::WriteThreadNameRecord(uint32_t pid, uint32_t tid, std::shared_ptr<std::string> thread_name, std::shared_ptr<std::ofstream> outstream)
{
    if (!thread_name || thread_name->empty())
        return;
    auto                    target_stream = outstream ? outstream : w;
    std::shared_ptr<Caches> target_caches = (target_stream == w) ? c : nullptr;
    if (!target_caches) {
        for (auto &pair : tid_w_map) {
            if (pair.second == target_stream) {
                target_caches = tid_cache_map[pair.first];
                break;
            }
        }
    }
    if (!target_caches)
        target_caches = c;

    uint64_t name_ref          = target_caches->string_cache.GetRef(this, thread_name);
    uint64_t proc_arg_name_ref = static_cast<uint64_t>(InternalString::Process);
    uint64_t rtype             = 7;
    uint64_t rsize             = 4;
    uint64_t obj_type          = 2; // kZxObjTypeThread
    uint64_t n_args            = 1;
    uint64_t header            = rtype | (rsize << 4) | (obj_type << 16) | (name_ref << 24) | (n_args << 40);
    uint64_t arg_hdr           = 8ULL | (2ULL << 4) | (proc_arg_name_ref << 16);

    WriteValue(target_stream, header);
    WriteValue(target_stream, static_cast<uint64_t>(tid));
    WriteValue(target_stream, arg_hdr);
    WriteValue(target_stream, static_cast<uint64_t>(pid));
}

void Perfetto::WriteInfoArgs(uint64_t insns, uint64_t cycles, std::shared_ptr<std::string> dso,
                             std::shared_ptr<std::string> timespan, uint64_t flag, std::shared_ptr<std::ofstream> tidstream)
{
    uint64_t name_ref = static_cast<uint64_t>(InternalString::Instructions);
    uint64_t value    = 4LL | 2LL << 4 | (name_ref << 16);
    WriteValue(w, value);
    WriteValue(w, insns);
    if (tidstream != nullptr) {
        WriteValue(tidstream, value);
        WriteValue(tidstream, insns);
    }

    name_ref = static_cast<uint64_t>(InternalString::Cycles);
    value    = 4LL | 2LL << 4 | (name_ref << 16);
    WriteValue(w, value);
    WriteValue(w, cycles);
    if (tidstream != nullptr) {
        WriteValue(tidstream, value);
        WriteValue(tidstream, insns);
    }

    if ((flag & FLAG_DISABLE_ARG_DSO) == 0) {
        name_ref          = static_cast<uint64_t>(InternalString::DSO);
        uint64_t dso_size = 1 + (dso->size() + 7) / 8;
        value             = 6LL | dso_size << 4 | (name_ref << 16) | dso->size() << 32 | 1LL << 47;
        WriteValue(w, value);
        WriteString(w, dso);
        if (tidstream != nullptr) {
            WriteValue(tidstream, value);
            WriteString(tidstream, dso);
        }
    }

    if ((flag & FLAG_DISABLE_ARG_TIMESPAN) == 0) {
        name_ref         = static_cast<uint64_t>(InternalString::Timespan);
        uint64_t ts_size = 1 + (timespan->size() + 7) / 8;
        value            = 6LL | ts_size << 4 | (name_ref << 16) | timespan->size() << 32 | 1LL << 47;
        WriteValue(w, value);
        WriteString(w, timespan);
        if (tidstream != nullptr) {
            WriteValue(tidstream, value);
            WriteString(tidstream, timespan);
        }
    }
}

void Perfetto::WriteFrameStart(uint64_t stamp, uint64_t pid_tid, std::shared_ptr<std::string> sym, uint64_t flag)
{
    std::shared_ptr<std::ofstream> tidstream = nullptr;
    if (tid_w_map_active.find(pid_tid) != tid_w_map_active.end())
        tidstream = tid_w_map_active[pid_tid];

    EventHeader e = {2, 0, stamp, pid_tid, "Misc", sym, 0};
    WriteEventHeader(e, flag, tidstream);
}

void Perfetto::WriteFrameEnd(uint64_t stamp, uint64_t pid_tid, uint64_t stamp_start, std::shared_ptr<std::string> dso,
                             [[maybe_unused]] std::shared_ptr<std::string> sym, uint64_t flag)
{
    std::shared_ptr<std::string>   timespan  = nullptr;
    std::shared_ptr<std::ofstream> tidstream = nullptr;
    uint64_t                       rsize     = 4;
    uint64_t                       nargs     = 2;
    if (tid_w_map_active.find(pid_tid) != tid_w_map_active.end())
        tidstream = tid_w_map_active[pid_tid];
    if ((flag & FLAG_DISABLE_ARG_TIMESPAN) == 0) {
        timespan = std::make_shared<std::string>(GetTimeSpan(stamp_start, stamp));
        rsize    = rsize + (7 + timespan->size()) / 8 + 1;
        nargs++;
    }
    if ((flag & FLAG_DISABLE_ARG_DSO) == 0) {
        rsize = rsize + (7 + dso->size()) / 8 + 1;
        nargs++;
    }
    EventHeader e = {3, nargs, stamp, pid_tid, "Misc", std::make_shared<std::string>(""), rsize};
    // EventHeader e = {3, nargs, stamp, pid_tid, "Misc", sym, rsize};

    WriteEventHeader(e, flag, tidstream);
    WriteInfoArgs(0, 0, dso, timespan, flag, tidstream);
}

// https://fuchsia.dev/fuchsia-src/reference/tracing/trace-format#duration-complete-event
void Perfetto::WriteFrameFull(uint64_t stamp_start, uint64_t stamp_end, uint64_t pidtid,
                              std::shared_ptr<std::string> dso, std::shared_ptr<std::string> sym, uint64_t flag)
{
    std::shared_ptr<std::string>   ts        = nullptr;
    std::shared_ptr<std::ofstream> tidstream = nullptr;
    if (tid_w_map_active.find(pidtid) != tid_w_map_active.end())
        tidstream = tid_w_map_active[pidtid];
    uint64_t rsize = 5;
    uint64_t nargs = 2;

    if ((flag & FLAG_DISABLE_ARG_TIMESPAN) == 0) {
        ts    = std::make_shared<std::string>(GetTimeSpan(stamp_start, stamp_end));
        rsize = rsize + (7 + ts->size()) / 8 + 1;
        nargs++;
    }

    if ((flag & FLAG_DISABLE_ARG_DSO) == 0) {
        rsize = rsize + (7 + dso->size()) / 8 + 1;
        nargs++;
    }

    EventHeader e = {4, nargs, stamp_start, pidtid, "Misc", sym, rsize};
    WriteEventHeader(e, flag, tidstream);
    WriteInfoArgs(0, 0, dso, ts, flag, tidstream);
    WriteValue(w, stamp_end);
    if (tidstream != nullptr)
        WriteValue(tidstream, stamp_end);
}

/* Counter value will not be shown in chart for specific 'taged' thread */
void Perfetto::WriteCounter(uint64_t timestamp, uint64_t pidtid, std::shared_ptr<std::string> eventname,
                            uint64_t counter, uint32_t id)
{
    uint64_t nargs      = 1;
    uint64_t extra_size = 1 + 2;
    uint64_t flag       = 0;

    EventHeader e = {1, nargs, timestamp, pidtid, "Misc", eventname, extra_size};
    WriteEventHeader(e, flag, nullptr);
    uint64_t name_ref = static_cast<uint64_t>(InternalString::FREQ);
    WriteValue(w, 4LL | 2LL << 4 | (name_ref << 16));
    WriteValue(w, counter);
    WriteValue(w, id);
}

void Perfetto::Flush()
{
    if (w && w->is_open()) {
        w->flush();
        w->close();
    }
    for (auto &it : tid_w_map_active) {
        if (it.second && it.second->is_open()) {
            it.second->flush();
            it.second->close();
        }
    }
}

Perfetto::~Perfetto()
{
    Flush();
}