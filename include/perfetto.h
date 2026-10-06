//
// Created by david on 1/3/25.
//
#ifndef PERFDLFILTERLIB_PERFETTO_H
#define PERFDLFILTERLIB_PERFETTO_H
#include <cstdint>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#define FLAG_DISABLE_ARG_DSO       (1 << 0)
#define FLAG_DISABLE_ARG_TIMESPAN  (1 << 1)
#define FLAG_WITH_STARTSTOP_FILTER (1 << 2)

enum class InternalString
{
    Empty,
    Instructions,
    Cycles,
    Symbol,
    Timespan,
    DSO,
    FREQ,
    Process,
    COUNT
};

class Perfetto;

class StringCache
{
public:
    StringCache(std::shared_ptr<std::ofstream> stream) : capacity_(32 * 1024), cache_(), lru_(), outstream(stream) {}
    uint64_t GetRef(Perfetto *perfetto, std::shared_ptr<std::string> s);

private:
    static const uint64_t                     RESERVED = static_cast<uint64_t>(InternalString::COUNT);
    size_t                                    capacity_;
    std::unordered_map<std::string, uint16_t> cache_;
    std::list<std::string>                    lru_;
    std::shared_ptr<std::ofstream>            outstream;
};

class ThreadCache
{
public:
    ThreadCache(std::shared_ptr<std::ofstream> stream) : capacity_(254), outstream(stream) {}
    uint64_t GetRef(Perfetto *perfetto, uint64_t pidtid);

private:
    // TODO: record tid 0 for what?
    static const uint64_t                 RESERVED = 1;
    size_t                                capacity_;
    std::unordered_map<uint64_t, uint8_t> cache_;
    std::list<uint64_t>                   lru_{};
    std::shared_ptr<std::ofstream>        outstream;
};

class Caches
{
public:
    Caches(std::shared_ptr<std::ofstream> outstream) : string_cache(outstream), thread_cache(outstream) {};
    StringCache string_cache;
    ThreadCache thread_cache;
};

union PidTid
{
    struct
    {
        uint32_t tid;
        uint32_t pid;
    };
    uint64_t pidtid;
    PidTid(uint32_t p, uint32_t t) : tid(t), pid(p) {}
    PidTid(uint64_t pidtid) : pidtid(pidtid) {}
};

struct EventHeader
{
    uint64_t                     etype;
    uint64_t                     nargs;
    uint64_t                     stamp;
    uint64_t                     pid_tid; // (pid, tid)
    std::string                  category;
    std::shared_ptr<std::string> name;
    uint64_t                     extra_data_size;
};

class Perfetto
{
public:
    Perfetto(std::string filename);
    ~Perfetto();
    void WriteStringRecord(uint64_t id, std::shared_ptr<std::string> message, std::shared_ptr<std::ofstream> outstream);
    void WriteThreadRecord(uint64_t id, PidTid pidtid, std::shared_ptr<std::ofstream> outstream);
    void WriteProcessRecord(uint32_t pid, std::shared_ptr<std::string> name);
    void WriteProcessRecord(uint32_t pid, std::shared_ptr<std::string> name, std::shared_ptr<std::ofstream> outstream);
    void WriteThreadNameRecord(uint32_t pid, uint32_t tid, std::shared_ptr<std::string> thread_name);
    void WriteThreadNameRecord(uint32_t pid, uint32_t tid, std::shared_ptr<std::string> thread_name, std::shared_ptr<std::ofstream> outstream);

    void WriteEventHeader(EventHeader e, uint64_t flag, std::shared_ptr<std::ofstream> tidstream);
    void WriteInfoArgs(uint64_t insns, uint64_t cycles, std::shared_ptr<std::string> dso,
                       std::shared_ptr<std::string> timespan, uint64_t flag, std::shared_ptr<std::ofstream> tidstream);

    void WriteFrameStart(uint64_t stamp, uint64_t pid_tid, std::shared_ptr<std::string> sym, [[maybe_unused]] uint64_t flag);
    void WriteFrameEnd(uint64_t stamp, uint64_t pid_tid, uint64_t satmp_start, std::shared_ptr<std::string> dso,
                       std::shared_ptr<std::string> sym, uint64_t flag);
    void WriteFrameFull(uint64_t stamp_start, uint64_t stamp_end, uint64_t pidtid, std::shared_ptr<std::string> dso,
                        std::shared_ptr<std::string> sym, uint64_t flag);

    void                           WriteCounter(uint64_t timestamp, uint64_t pidtid, std::shared_ptr<std::string> eventname,
                                                uint64_t counter, uint32_t id);
    std::shared_ptr<std::ofstream> CreatePerfettoStream(std::string &filename, uint64_t pidtid);
    void                           DestoryPerfettoStream(uint64_t pidtid);

    void Flush();

private:
#define SYM_NAME_BUFF_LEN 256
    char                                                         buf[SYM_NAME_BUFF_LEN];
    std::shared_ptr<std::ofstream>                               w;
    std::unordered_map<uint64_t, std::shared_ptr<std::ofstream>> tid_w_map_active;
    std::unordered_map<uint64_t, std::shared_ptr<std::ofstream>> tid_w_map;
    std::shared_ptr<Caches>                                      c;
    std::unordered_map<uint64_t, std::shared_ptr<Caches>>        tid_cache_map;
    std::unordered_map<uint32_t, std::shared_ptr<std::string>>   process_names;
    std::unordered_map<uint32_t, std::string>                    written_proc_names;
    std::unordered_map<uint64_t, std::shared_ptr<std::string>>   thread_names;
    std::unordered_map<uint64_t, std::string>                    written_thread_names;

    std::string GetInernalStringName(InternalString x);
    int         GetTimeStampStr(char *buffer, int buflen, uint64_t nano);
    const char *GetTimeSpan(uint64_t nanostart, uint64_t nanoend);

    void WriteHeader(std::shared_ptr<std::ofstream> out);
    void WriteSpecificStringRecord(std::shared_ptr<std::ofstream> out, uint64_t id, std::shared_ptr<std::string> message);
    void WriteValue(std::shared_ptr<std::ofstream> out, uint64_t value);
    void WriteString(std::shared_ptr<std::ofstream> out, std::shared_ptr<std::string> message);
};

#endif // PERFDLFILTERLIB_PERFETTO_H
