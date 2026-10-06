#include "graph.h"
#include "log.h"

extern "C" {
#include "perf_dlfilter.h"

// Table of callback functions provided by perf
struct perf_dlfilter_fns perf_dlfilter_fns;

#if defined(PTGRAPH_HAS_MPI)
static bool g_mpi_initialized_by_filter = false;
#endif

int start(void **data, void *ctx)
{
    // Initialize GraphDrawer and manage ctx
    auto drawer = new GraphDrawer(ctx);
    *data       = static_cast<void *>(drawer);

#if defined(PTGRAPH_HAS_MPI)
    int mpi_inited = 0;
    MPI_Initialized(&mpi_inited);
    if (!mpi_inited) {
        int    argc = 0;
        char **argv = nullptr;
        if (MPI_Init(&argc, &argv) == MPI_SUCCESS) {
            g_mpi_initialized_by_filter = true;
            int rank;
            int size;
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            MPI_Comm_size(MPI_COMM_WORLD, &size);
            drawer->SetMpiRank(rank);
            drawer->SetMpiSize(size);
        }
    }
#endif
    LOG_INFO << "[ptgraph " << getpid() << "] Starting filter" << std::endl;

    return 0;
}

int stop(void *raw_state, [[maybe_unused]] void *ctx)
{
    auto drawer = static_cast<GraphDrawer *>(raw_state);

    if (drawer) {

#if defined(PTGRAPH_HAS_MPI)
        int mpi_inited = 0;
        MPI_Initialized(&mpi_inited);
        if (mpi_inited) {
            // Gather all SHM zone descriptors (Frames and symbols zones) from all MPI processes
            auto gathered_zones = drawer->GatherShmZonesMpi();

            // At rank 0 or for diagnostic output / downstream handoff:
            if (drawer->GetMpiRank() == 0) {
                std::fprintf(stderr,
                             "[ptgraph dlfilter] MPI gathered %zu SHM zones across %u ranks\n",
                             gathered_zones.size(),
                             drawer->GetMpiSize());
            }

            MPI_Barrier(MPI_COMM_WORLD);

            if (g_mpi_initialized_by_filter) {
                int finalized = 0;
                MPI_Finalized(&finalized);
                if (!finalized) {
                    MPI_Finalize();
                }
                g_mpi_initialized_by_filter = false;
            }
        }
#endif
    }

    drawer->Finish();
    delete drawer;
    return 0;
}

int filter_event_early(void *raw_state, const struct perf_dlfilter_sample *sample, [[maybe_unused]] void *ctx)
{
    auto drawer = static_cast<GraphDrawer *>(raw_state);
    if (!drawer || !sample) {
        return 0;
    }

    auto ret = drawer->FilterByTimestamp(sample->time);
    if (ret != 0) {
        return ret;
    }

    const auto &para = drawer->GetJsonPara();

    // Function zone: if enough accumulated functions captured across threads, stop early
    if (para.funcZone && para.funcZone->IsEnoughAccumulatedFunctions()) {
        return -38;
    }

    return 0;
}

int filter_event(void *raw_state, const struct perf_dlfilter_sample *sample, void *ctx)
{
    auto drawer = static_cast<GraphDrawer *>(raw_state);
    if (!drawer || !sample) {
        return 0;
    }

    // Add perf_dlfilter_sample into Frame / SHM.
    // If the active SHM zone capacity (frame queue or string pool) is full,
    // GraphDrawer will automatically append another SHM zone.
    drawer->AddSample(sample, ctx);

    return 1;
}
}
