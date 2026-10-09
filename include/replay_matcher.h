#ifndef __REPLAY_MATCHER_H__
#define __REPLAY_MATCHER_H__

#include <cstddef>
#include <string>
#include <vector>

struct ReplayStackEntry
{
    std::string sym;
    std::string caller;
};

struct ReplayMatchDecision
{
    bool   keep_stack{true};
    bool   fallback_pop{false};
    size_t pop_count{0};
    size_t keep_score{0};
    size_t pop_score{0};
};

// stack_top_first and future_returns are both ordered from nearest to farthest.
ReplayMatchDecision ChooseReturnRecovery(const std::vector<ReplayStackEntry> &stack_top_first,
                                         const std::string                   &return_sym,
                                         const std::string                   &return_caller,
                                         const std::vector<std::string>      &future_returns,
                                         size_t                               max_depth);

#endif // __REPLAY_MATCHER_H__