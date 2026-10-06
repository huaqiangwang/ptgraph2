#include "graph.h"
#include <cassert>
#include <iostream>
#include <vector>

struct perf_dlfilter_fns perf_dlfilter_fns{};

int main()
{
    const std::string prefix = "ptgraph_test_" + std::to_string(getpid());

    // Clean up any stale SHM first
    PtGraphReader::UnlinkAll(prefix);

    constexpr size_t TEST_FRAME_CAP = 4; // small capacity to test zone rollover
    constexpr size_t TEST_STR_CAP   = 1024;
    constexpr size_t TEST_MAX_TH    = 64;

    std::cout << "[TEST] Initializing GraphDrawer with prefix: " << prefix << "..." << std::endl;
    {
        GraphDrawer drawer(nullptr, prefix, TEST_FRAME_CAP, TEST_STR_CAP, TEST_MAX_TH);

        // Append frames for TID 1001 (PID 100)
        for (int i = 0; i < 10; ++i) {
            FrameEle fe{};
            fe.timestamp    = 1000 + i;
            fe.pid          = 100;
            fe.tid          = 1001;
            fe.type         = FrameType::CALL;
            std::string sym = "func_1001_" + std::to_string(i);
            drawer.AppendFrameWithSymbols(fe, sym, "caller_common", "libc.so");
        }

        // Append frames for TID 2002 (PID 200)
        for (int i = 0; i < 5; ++i) {
            FrameEle fe{};
            fe.timestamp    = 2000 + i;
            fe.pid          = 200;
            fe.tid          = 2002;
            fe.type         = FrameType::RETURN;
            std::string sym = "func_2002_" + std::to_string(i);
            drawer.AppendFrameWithSymbols(fe, sym, "caller_common", "libm.so");
        }

        // Append frames for TID 3003 (PID 200)
        for (int i = 0; i < 2; ++i) {
            FrameEle fe{};
            fe.timestamp    = 3000 + i;
            fe.pid          = 200;
            fe.tid          = 3003;
            fe.type         = FrameType::SAMPLE;
            std::string sym = "func_3003_" + std::to_string(i);
            drawer.AppendFrameWithSymbols(fe, sym, "caller_common", "app.bin");
        }

        drawer.Finish();
    }

    std::cout << "[TEST] GraphDrawer finished and closed. Testing reader access..." << std::endl;

    // Now test reader access (as if another process)
    PtGraphReader reader(prefix);
    bool          opened = reader.OpenMaster();
    assert(opened && "Failed to open master catalog");
    assert(reader.IsFinished() && "Reader should detect finished state");

    auto entries = reader.GetThreadEntries();
    std::cout << "[TEST] Found " << entries.size() << " thread entries in master catalog." << std::endl;
    assert(entries.size() == 3 && "Expected 3 distinct threads");

    // Verify TID 1001
    ThreadCatalogEntry e1001{};
    assert(reader.GetThreadEntry(1001, e1001));
    assert(e1001.pid == 100);
    assert(e1001.total_frames == 10);
    assert(e1001.is_terminated == 1);
    // Capacity is 4, 10 frames -> zone 0 (4), zone 1 (4), zone 2 (2) -> active_zone_id = 2
    assert(e1001.active_zone_id == 2);

    auto frames1001 = reader.ReadAllFramesForThread(1001);
    assert(frames1001.size() == 10);
    std::cout << "[TEST] Sample FrameEle dump: ";
    frames1001[0].dump();
    for (size_t i = 0; i < frames1001.size(); ++i) {
        assert(frames1001[i].tid == 1001);
        assert(frames1001[i].pid == 100);
        std::string expected_sym = "func_1001_" + std::to_string(i);
        const char *resolved     = reader.ResolveString(frames1001[i].sym);
        assert(std::string(resolved) == expected_sym);
        assert(std::string(reader.ResolveString(frames1001[i].caller_sym)) == "caller_common");
        assert(std::string(reader.ResolveString(frames1001[i].dso)) == "libc.so");
    }

    // Verify TID 2002
    ThreadCatalogEntry e2002{};
    assert(reader.GetThreadEntry(2002, e2002));
    assert(e2002.pid == 200);
    assert(e2002.total_frames == 5);
    // Capacity 4, 5 frames -> zone 0 (4), zone 1 (1) -> active_zone_id = 1
    assert(e2002.active_zone_id == 1);

    auto frames2002 = reader.ReadAllFramesForThread(2002);
    assert(frames2002.size() == 5);
    for (size_t i = 0; i < frames2002.size(); ++i) {
        assert(frames2002[i].tid == 2002);
        assert(frames2002[i].pid == 200);
        std::string expected_sym = "func_2002_" + std::to_string(i);
        const char *resolved     = reader.ResolveString(frames2002[i].sym);
        assert(std::string(resolved) == expected_sym);
        assert(std::string(reader.ResolveString(frames2002[i].caller_sym)) == "caller_common");
        assert(std::string(reader.ResolveString(frames2002[i].dso)) == "libm.so");
    }

    // Verify TID 3003
    ThreadCatalogEntry e3003{};
    assert(reader.GetThreadEntry(3003, e3003));
    assert(e3003.pid == 200);
    assert(e3003.total_frames == 2);
    assert(e3003.active_zone_id == 0);

    auto frames3003 = reader.ReadAllFramesForThread(3003);
    assert(frames3003.size() == 2);
    for (size_t i = 0; i < frames3003.size(); ++i) {
        assert(frames3003[i].tid == 3003);
        assert(frames3003[i].pid == 200);
        std::string expected_sym = "func_3003_" + std::to_string(i);
        const char *resolved     = reader.ResolveString(frames3003[i].sym);
        assert(std::string(resolved) == expected_sym);
        assert(std::string(reader.ResolveString(frames3003[i].caller_sym)) == "caller_common");
        assert(std::string(reader.ResolveString(frames3003[i].dso)) == "app.bin");
    }

    // Clean up
    PtGraphReader::UnlinkAll(prefix);
    std::cout << "[TEST] All assertions passed successfully!" << std::endl;

    // Test Coordinator Shared Memory multi-process simulation
    std::cout << "[TEST] Testing Coordinator SHM multi-process aggregation..." << std::endl;
    std::string test_coord_id  = "test_coord_" + std::to_string(getpid());
    std::string coord_shm_name = "/ptgraph_coord_" + test_coord_id;

    int c_fd = shm_open(coord_shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    assert(c_fd >= 0);
    assert(ftruncate(c_fd, sizeof(ShmCoordinator)) == 0);
    void *c_addr = mmap(nullptr, sizeof(ShmCoordinator), PROT_READ | PROT_WRITE, MAP_SHARED, c_fd, 0);
    assert(c_addr != MAP_FAILED);
    close(c_fd);

    std::memset(c_addr, 0, sizeof(ShmCoordinator));
    auto *coord        = reinterpret_cast<ShmCoordinator *>(c_addr);
    coord->magic       = 0x50544752;
    coord->version     = 1;
    coord->total_ranks = 3;

    constexpr int NUM_TEST_PROCS = 3;
    std::string   multi_prefix   = "ptgraph_multi_test";

    // Simulate Worker 1 and Worker 2 publishing zones
    for (int r = 1; r < NUM_TEST_PROCS; ++r) {
        std::string worker_prefix = multi_prefix;
        GraphDrawer worker_drawer(nullptr, worker_prefix, TEST_FRAME_CAP, TEST_STR_CAP, TEST_MAX_TH);
        worker_drawer.SetProcRank(r);
        worker_drawer.SetProcSize(NUM_TEST_PROCS);
        worker_drawer.SetCoordId(test_coord_id);

        for (int i = 0; i < 4; ++i) {
            FrameEle fe{};
            fe.timestamp = 4000 + i;
            fe.pid       = 400 + r;
            fe.tid       = 4000 + r;
            fe.type      = FrameType::CALL;
            worker_drawer.AppendFrameWithSymbols(fe, "worker_sym_" + std::to_string(r), "caller", "lib.so");
        }
        worker_drawer.Finish();
        bool pub_ok = worker_drawer.PublishLocalZonesToCoordinator();
        assert(pub_ok && "Worker publish failed");
    }

    // Now simulate Main (Rank 0) collecting all zones
    {
        std::string main_prefix = multi_prefix;
        GraphDrawer main_drawer(nullptr, main_prefix, TEST_FRAME_CAP, TEST_STR_CAP, TEST_MAX_TH);
        main_drawer.SetProcRank(0);
        main_drawer.SetProcSize(NUM_TEST_PROCS);
        main_drawer.SetCoordId(test_coord_id);

        for (int i = 0; i < 2; ++i) {
            FrameEle fe{};
            fe.timestamp = 4000 + i;
            fe.pid       = 400;
            fe.tid       = 4000;
            fe.type      = FrameType::CALL;
            main_drawer.AppendFrameWithSymbols(fe, "main_sym", "caller", "lib.so");
        }
        main_drawer.Finish();

        auto all_collected = main_drawer.CollectAllZonesFromCoordinator(1000);
        std::cout << "[TEST] Main collected " << all_collected.size() << " SHM zones from coordinator." << std::endl;
        assert(all_collected.size() >= static_cast<size_t>(NUM_TEST_PROCS * 2) && "Expected master + string + thread zones from all processes");

        // Verify total frames
        uint64_t total_frames = 0;
        for (const auto &z : all_collected) {
            if (z.type == ShmZoneType::THREAD) {
                total_frames += z.element_count;
            }
        }
        std::cout << "[TEST] Total frames aggregated: " << total_frames << " (expected 10)" << std::endl;
        assert(total_frames == 10 && "Expected 2 + 4 + 4 = 10 frames");

        // Test Perfetto unified trace generation
        std::string test_ftf = "test_unified.ftf";
        main_drawer.GenerateGraph(test_ftf, all_collected);

        std::ifstream ftf_file(test_ftf, std::ios::binary);
        assert(ftf_file.is_open() && "Expected test_unified.ftf to exist");
        uint64_t magic = 0;
        ftf_file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
        assert(magic == 0x0016547846040010LL && "Expected valid Fuchsia trace header magic");
        ftf_file.close();
        std::remove(test_ftf.c_str());
        std::cout << "[TEST] Unified Perfetto trace test passed!" << std::endl;

        // Test Per-TID Perfetto trace generation
        main_drawer.SetPerTidOutput(true);
        main_drawer.GenerateGraph("out.ftf", all_collected);
        main_drawer.SetPerTidOutput(false);

        // Verify out_tid_4000.ftf, out_tid_4001.ftf, out_tid_4002.ftf
        for (uint32_t tid : {4000, 4001, 4002}) {
            std::string   tid_file = "out_tid_" + std::to_string(tid) + ".ftf";
            std::ifstream tf(tid_file, std::ios::binary);
            assert(tf.is_open() && "Expected per-tid file to exist");
            uint64_t tid_magic = 0;
            tf.read(reinterpret_cast<char *>(&tid_magic), sizeof(tid_magic));
            assert(tid_magic == 0x0016547846040010LL && "Expected valid Fuchsia trace header in per-tid file");
            tf.close();
            std::remove(tid_file.c_str());
        }
        std::cout << "[TEST] Per-TID Perfetto trace test passed!" << std::endl;
    }

    // Clean up
    munmap(c_addr, sizeof(ShmCoordinator));
    shm_unlink(coord_shm_name.c_str());
    for (int r = 0; r < NUM_TEST_PROCS; ++r) {
        PtGraphReader::UnlinkAll(multi_prefix + "_rank_" + std::to_string(r));
    }
    PtGraphReader::UnlinkAll(multi_prefix);

    std::cout << "[TEST] Multi-process Coordinator test passed successfully!" << std::endl;
    return 0;
}
