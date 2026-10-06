#include "graph.h"
#include "log.h"
#include "shm.h"

struct perf_dlfilter_fns perf_dlfilter_fns{};

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

//
// Binary structures of Linux perf.data file
//
#pragma pack(push, 8)
struct PerfFileSection
{
    uint64_t offset;
    uint64_t size;
};

struct PerfFileHeader
{
    uint64_t        magic;     // "PERFILE2" (0x32454c4946524550ULL)
    uint64_t        size;      // 104
    uint64_t        attr_size; // sizeof(perf_event_attr)
    PerfFileSection attrs;
    PerfFileSection data;
    PerfFileSection event_types;
    uint64_t        adds_features[4];
};
#pragma pack(pop)

constexpr uint32_t HEADER_SAMPLE_TIME = 21;

//
// Parse start and end timestamps directly from perf.data binary header
//
bool ParsePerfDataSampleTime(const std::string &path, uint64_t &out_begin_ns, uint64_t &out_end_ns)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOG_WARNING << "Cannot open " << path << ": " << strerror(errno) << std::endl;
        return false;
    }

    PerfFileHeader hdr{};
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd);
        return false;
    }

    if (std::memcmp(&hdr.magic, "PERFILE2", 8) != 0) {
        LOG_WARNING << path << " is not a valid PERFILE2 format." << std::endl;
        close(fd);
        return false;
    }

    // Check if bit 21 (HEADER_SAMPLE_TIME) is enabled
    if (!(hdr.adds_features[0] & (1ULL << HEADER_SAMPLE_TIME))) {
        LOG_WARNING << path << " does not contain HEADER_SAMPLE_TIME feature bit." << std::endl;
        close(fd);
        return false;
    }

    // Number of set bits before bit 21 determines the section header index
    uint64_t mask_before   = (1ULL << HEADER_SAMPLE_TIME) - 1;
    int      section_index = __builtin_popcountll(hdr.adds_features[0] & mask_before);

    off_t           sec_array_offset  = hdr.data.offset + hdr.data.size;
    off_t           target_sec_offset = sec_array_offset + section_index * sizeof(PerfFileSection);
    PerfFileSection time_sec{};
    if (pread(fd, &time_sec, sizeof(time_sec), target_sec_offset) != sizeof(time_sec)) {
        close(fd);
        return false;
    }

    uint64_t timestamps[2]{0, 0};
    if (pread(fd, timestamps, sizeof(timestamps), time_sec.offset) != sizeof(timestamps)) {
        close(fd);
        return false;
    }

    close(fd);

    out_begin_ns = timestamps[0];
    out_end_ns   = timestamps[1];
    return (out_begin_ns > 0 && out_end_ns > out_begin_ns);
}

void PrintUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -i <perf.data>    Input perf data file (default: perf.data)\n"
              << "  -n <num>          Number of parallel perf script worker processes (default: 4)\n"
              << "  -j <config.json>  Configuration JSON file\n"
              << "  -f <output.dot>   Output callgraph file (default: callgraph.dot)\n"
              << "  -s <dlfilter.so>  Path to libptgraph.so filter library\n"
              << "  -b <begin_ns>     Explicit global begin timestamp (nanoseconds)\n"
              << "  -e <end_ns>       Explicit global end timestamp (nanoseconds)\n"
              << "  -h                Show this help message\n";
}

int main(int argc, char **argv)
{
    std::string perf_data = "perf.data";
    int         num_procs = 4;
    std::string json_conf = "";
    std::string out_file  = "callgraph.dot";
    std::string dlfilter  = "build/lib/libptgraph.so";
    uint64_t    begin_ns  = 0;
    uint64_t    end_ns    = 0;

    int opt;
    while ((opt = getopt(argc, argv, "i:n:j:f:s:b:e:h")) != -1) {
        switch (opt) {
        case 'i':
            perf_data = optarg;
            break;
        case 'n':
            num_procs = std::max(1, std::atoi(optarg));
            break;
        case 'j':
            json_conf = optarg;
            break;
        case 'f':
            out_file = optarg;
            break;
        case 's':
            dlfilter = optarg;
            break;
        case 'b':
            begin_ns = std::stoull(optarg);
            break;
        case 'e':
            end_ns = std::stoull(optarg);
            break;
        case 'h':
        default:
            PrintUsage(argv[0]);
            return 0;
        }
    }

    // If dlfilter does not exist in build/lib, check lib/ or current directory
    struct stat st{};
    if (stat(dlfilter.c_str(), &st) != 0) {
        if (stat("lib/libptgraph.so", &st) == 0) {
            dlfilter = "lib/libptgraph.so";
        } else if (stat("./libptgraph.so", &st) == 0) {
            dlfilter = "./libptgraph.so";
        }
    }

    LOG_INFO << "[mperf] Target perf.data: " << perf_data << ", workers: " << num_procs << std::endl;

    // Check timestamps
    if (begin_ns == 0 || end_ns <= begin_ns) {
        LOG_INFO << "[mperf] Parsing timestamps directly from " << perf_data << " binary header..." << std::endl;
        if (ParsePerfDataSampleTime(perf_data, begin_ns, end_ns)) {
            LOG_INFO << "[mperf] Detected time range: [" << begin_ns << ", " << end_ns
                     << "] (span: " << (end_ns - begin_ns) / 1000000.0 << " ms)" << std::endl;
        } else {
            LOG_WARNING << "[mperf] Could not parse timestamps from header. Workers will run in default time range." << std::endl;
            begin_ns = 0;
            end_ns   = 0;
        }
    } else {
        LOG_INFO << "[mperf] Using specified time range: [" << begin_ns << ", " << end_ns << "]" << std::endl;
    }

    // Generate unique coordination ID
    auto        now_ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    std::string coord_id   = std::to_string(now_ms) + "_" + std::to_string(getpid());
    std::string coord_name = "/ptgraph_coord_" + coord_id;

    // Create and initialize Coordinator Shared Memory
    int coord_fd = shm_open(coord_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (coord_fd < 0) {
        LOG_ERROR << "[mperf] Failed to create coordinator SHM: " << strerror(errno) << std::endl;
        return 1;
    }
    if (ftruncate(coord_fd, sizeof(ShmCoordinator)) != 0) {
        LOG_ERROR << "[mperf] Failed to ftruncate coordinator SHM: " << strerror(errno) << std::endl;
        close(coord_fd);
        shm_unlink(coord_name.c_str());
        return 1;
    }

    void *caddr = mmap(nullptr, sizeof(ShmCoordinator), PROT_READ | PROT_WRITE, MAP_SHARED, coord_fd, 0);
    close(coord_fd);
    if (caddr == MAP_FAILED) {
        LOG_ERROR << "[mperf] Failed to mmap coordinator SHM" << std::endl;
        shm_unlink(coord_name.c_str());
        return 1;
    }

    std::memset(caddr, 0, sizeof(ShmCoordinator));
    auto *coord        = reinterpret_cast<ShmCoordinator *>(caddr);
    coord->magic       = 0x50544752;
    coord->version     = 1;
    coord->total_ranks = num_procs;
    coord->job_id      = now_ms;

    // Clean up any stale SHM files from previous runs
    for (int r = 0; r < num_procs; ++r) {
        PtGraphReader::UnlinkAll("ptgraph_rank_" + std::to_string(r));
    }
    PtGraphReader::UnlinkAll("ptgraph");

    uint64_t total_span = (end_ns > begin_ns) ? (end_ns - begin_ns) : 0;

    // Spawn N worker perf script processes
    std::vector<pid_t> pids(num_procs, 0);
    for (int r = 0; r < num_procs; ++r) {
        uint64_t r_begin = 0;
        uint64_t r_end   = 0;
        if (total_span > 0) {
            r_begin = begin_ns + total_span * r / num_procs;
            r_end   = (r == num_procs - 1) ? (end_ns + 1) : (begin_ns + total_span * (r + 1) / num_procs);
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Child process: execute perf script
            std::string dlarg_str = "-r " + std::to_string(r) +
                                    " -n " + std::to_string(num_procs) +
                                    " -c " + coord_id;
            if (!json_conf.empty()) {
                dlarg_str += " -j " + json_conf;
            }
            if (!out_file.empty()) {
                dlarg_str += " -f " + out_file;
            }
            if (r_begin > 0 && r_end > r_begin) {
                dlarg_str += " -b " + std::to_string(r_begin) + " -e " + std::to_string(r_end);
            }

            std::vector<std::string> args = {
                "perf",
                "--no-pager",
                "script",
                "-i",
                perf_data,
                "--itrace=bcr",
                "--dlfilter",
                dlfilter,
                "--dlarg",
                dlarg_str};

            std::vector<char *> c_args;
            for (auto &a : args) {
                c_args.push_back(const_cast<char *>(a.c_str()));
            }
            c_args.push_back(nullptr);

            execvp("perf", c_args.data());
            std::cerr << "[mperf worker " << r << "] execvp perf failed: " << strerror(errno) << std::endl;
            _exit(127);
        } else if (pid > 0) {
            pids[r] = pid;
            LOG_INFO << "[mperf] Spawned worker rank " << r << " (PID " << pid << ")"
                     << (total_span > 0 ? (" slice: [" + std::to_string(r_begin) + ", " + std::to_string(r_end) + "]") : "")
                     << std::endl;
        } else {
            LOG_ERROR << "[mperf] fork failed for rank " << r << ": " << strerror(errno) << std::endl;
            coord->is_aborted = 1;
            break;
        }
    }

    // Wait for all child processes
    int failed_count = 0;
    for (int r = 0; r < num_procs; ++r) {
        if (pids[r] <= 0)
            continue;
        int   status = 0;
        pid_t w      = waitpid(pids[r], &status, 0);
        if (w > 0) {
            bool completed = false;
            if (r < static_cast<int>(MAX_RANKS)) {
                completed = (coord->slots[r].status == static_cast<uint32_t>(WorkerState::COMPLETED));
            }
            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                // Exit code 0, or completed successfully via -ENOSYS early stop
                if (code == 0 || completed) {
                    // Succeeded
                } else {
                    failed_count++;
                    LOG_WARNING << "[mperf] Worker PID " << pids[r] << " (rank " << r << ") exited with code "
                                << code << std::endl;
                }
            } else if (WIFSIGNALED(status)) {
                failed_count++;
                LOG_WARNING << "[mperf] Worker PID " << pids[r] << " (rank " << r << ") killed by signal "
                            << WTERMSIG(status) << " (" << strsignal(WTERMSIG(status)) << ")" << std::endl;
            } else if (!completed) {
                failed_count++;
                LOG_WARNING << "[mperf] Worker PID " << pids[r] << " (rank " << r << ") failed." << std::endl;
            }
        }
    }

    // Log per-rank summary
    LOG_INFO << "[mperf] Per-rank processing summary:" << std::endl;
    uint64_t sum_early = 0, sum_filtered = 0, sum_frames = 0;
    for (int r = 0; r < num_procs; ++r) {
        const auto &slot = coord->slots[r];
        sum_early += slot.early_samples;
        sum_filtered += slot.filtered_samples;
        sum_frames += slot.total_frames;
        LOG_INFO << "  Rank " << r << " (PID " << pids[r] << "): "
                 << "early_samples=" << slot.early_samples
                 << ", filtered_samples=" << slot.filtered_samples
                 << ", inserted_frames=" << slot.total_frames
                 << ", status=" << (slot.status == static_cast<uint32_t>(WorkerState::COMPLETED) ? "COMPLETED" : "INCOMPLETE")
                 << std::endl;
    }
    LOG_INFO << "  TOTAL across all ranks: early_samples=" << sum_early
             << ", filtered_samples=" << sum_filtered
             << ", inserted_frames=" << sum_frames << std::endl;

    // Clean up coordinator SHM
    munmap(caddr, sizeof(ShmCoordinator));
    shm_unlink(coord_name.c_str());

    for (int r = 0; r < num_procs; ++r) {
        PtGraphReader::UnlinkAll("ptgraph_rank_" + std::to_string(r));
    }

    LOG_INFO << "[mperf] All worker processes finished. (failures: " << failed_count << ")" << std::endl;
    return failed_count > 0 ? 1 : 0;
}
