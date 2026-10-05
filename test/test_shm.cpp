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

#if defined(PTGRAPH_HAS_MPI)
    // Test MPI gathering if MPI was initialized
    int mpi_inited = 0;
    MPI_Initialized(&mpi_inited);
    if (!mpi_inited) {
        MPI_Init(nullptr, nullptr);
    }

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    std::string mpi_prefix = "ptgraph_mpi_test";
    PtGraphReader::UnlinkAll(mpi_prefix + (size > 1 ? ("_rank_" + std::to_string(rank)) : ""));

    {
        GraphDrawer mpi_drawer(nullptr, mpi_prefix, TEST_FRAME_CAP, TEST_STR_CAP, TEST_MAX_TH);
        // Each rank creates unique frames for its own TID
        uint32_t my_tid = 10000 + rank;
        for (int i = 0; i < 3; ++i) {
            FrameEle fe{};
            fe.timestamp = 5000 + i;
            fe.pid       = 500 + rank;
            fe.tid       = my_tid;
            fe.type      = FrameType::CALL;
            mpi_drawer.AppendFrameWithSymbols(fe, "mpi_func_" + std::to_string(rank), "caller", "lib.so");
        }
        mpi_drawer.Finish();

        auto all_gathered = mpi_drawer.GatherShmZonesMpi(MPI_COMM_WORLD);
        if (rank == 0) {
            std::cout << "[TEST MPI] Gathered " << all_gathered.size() << " zones across " << size << " ranks." << std::endl;
        }
        assert(all_gathered.size() >= static_cast<size_t>(size * 2) && "Expected master + string + thread zones from each rank");
    }

    PtGraphReader::UnlinkAll(mpi_prefix + (size > 1 ? ("_rank_" + std::to_string(rank)) : ""));

    int finalized = 0;
    MPI_Finalized(&finalized);
    if (!finalized) {
        MPI_Finalize();
    }
    if (rank == 0) {
        std::cout << "[TEST MPI] MPI test completed successfully!" << std::endl;
    }
#endif

    return 0;
}
