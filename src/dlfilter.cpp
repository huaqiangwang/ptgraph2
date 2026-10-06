#include "graph.h"
#include "log.h"

extern "C" {
#include "perf_dlfilter.h"

// Table of callback functions provided by perf
struct perf_dlfilter_fns perf_dlfilter_fns;

int start(void **data, void *ctx)
{
    // Initialize GraphDrawer and manage ctx
    auto drawer = new GraphDrawer(ctx);
    *data       = static_cast<void *>(drawer);

    LOG_INFO << "[ptgraph pid=" << getpid() << " rank=" << drawer->GetProcRank()
             << "/" << drawer->GetProcSize() << "] Starting filter" << std::endl;

    return 0;
}

int stop(void *raw_state, [[maybe_unused]] void *ctx)
{
    auto drawer = static_cast<GraphDrawer *>(raw_state);

    if (drawer) {
        LOG_INFO << "[ptgraph rank " << drawer->GetProcRank() << "] Stats: "
                 << "early_samples=" << drawer->GetEarlySamples()
                 << ", filtered_samples=" << drawer->GetFilteredSamples()
                 << ", inserted_frames=" << drawer->GetInsertedFrames()
                 << std::endl;

        drawer->Finish();

        if (drawer->IsCoordinated()) {
            if (drawer->IsMainProc()) {
                // Rank 0 (Main process): Wait for all workers to finish and collect all zones
                auto all_zones = drawer->CollectAllZonesFromCoordinator(60000);

                uint64_t total_gathered_frames = 0;
                for (const auto &desc : all_zones) {
                    if (desc.type == ShmZoneType::THREAD) {
                        total_gathered_frames += desc.element_count;
                    }
                }

                LOG_INFO << "[ptgraph main] Gathered " << all_zones.size()
                         << " SHM zones across " << drawer->GetProcSize()
                         << " processes, total frames: " << total_gathered_frames << std::endl;

                // Let Main process generate the final graph
                drawer->GenerateGraph("", all_zones);

                // Record Rank 0 completion in coordinator
                drawer->PublishLocalZonesToCoordinator();
            } else {
                // Worker process (Rank > 0): Publish local zones to coordinator and exit
                drawer->PublishLocalZonesToCoordinator();
            }
        } else {
            // Standalone mode: Directly generate the graph locally
            drawer->GenerateGraph();
        }
    }

    delete drawer;
    return 0;
}

int filter_event_early(void *raw_state, const struct perf_dlfilter_sample *sample, [[maybe_unused]] void *ctx)
{
    auto drawer = static_cast<GraphDrawer *>(raw_state);
    if (!drawer || !sample) {
        return 0;
    }

    drawer->IncEarlySamples();

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

    drawer->IncFilteredSamples();

    // Add perf_dlfilter_sample into Frame / SHM.
    // If the active SHM zone capacity (frame queue or string pool) is full,
    // GraphDrawer will automatically append another SHM zone.
    drawer->AddSample(sample, ctx);

    return 1;
}
}
