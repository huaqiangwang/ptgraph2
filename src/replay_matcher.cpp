#include "replay_matcher.h"

#include <algorithm>

namespace {

    size_t MatchDepth(const std::vector<ReplayStackEntry> &stack,
                      size_t                               begin,
                      const std::vector<std::string>      &returns,
                      size_t                               max_depth)
    {
        size_t depth = std::min(stack.size() - begin, max_depth);
        size_t return_count = std::min(returns.size(), max_depth);
        std::vector<size_t> scores(return_count + 1, 0);

        for (size_t stack_offset = 0; stack_offset < depth; ++stack_offset) {
            size_t previous = 0;
            for (size_t return_idx = 0; return_idx < return_count; ++return_idx) {
                size_t saved = scores[return_idx + 1];
                if (stack[begin + stack_offset].sym == returns[return_idx]) {
                    scores[return_idx + 1] = previous + 1;
                } else {
                    scores[return_idx + 1] = std::max(scores[return_idx + 1], scores[return_idx]);
                }
                previous = saved;
            }
        }
        return scores.back();
    }

} // namespace

ReplayMatchDecision ChooseReturnRecovery(const std::vector<ReplayStackEntry> &stack,
                                         const std::string                   &return_sym,
                                         const std::string                   &return_caller,
                                         const std::vector<std::string>      &future_returns,
                                         size_t                               max_depth)
{
    ReplayMatchDecision decision;
    if (stack.empty() || max_depth == 0)
        return decision;

    size_t match_offset = stack.size();
    for (size_t i = 0; i < stack.size() && i < max_depth; ++i) {
        if (stack[i].sym != return_sym)
            continue;
        if (match_offset == stack.size())
            match_offset = i;
        if (stack[i].caller == return_caller) {
            match_offset = i;
            break;
        }
    }

    if (match_offset == 0) {
        decision.keep_stack = false;
        decision.pop_count  = 1;
        decision.pop_score  = 1;
        return decision;
    }

    decision.keep_score = MatchDepth(stack, 0, future_returns, max_depth);
    if (match_offset == stack.size()) {
        bool caller_supports_missing_call = stack.front().sym == return_caller;
        bool next_return_matches_top = !future_returns.empty() &&
                                       future_returns.front() == stack.front().sym;
        bool deep_future_support = decision.keep_score >= 2;
        decision.keep_stack = caller_supports_missing_call || next_return_matches_top || deep_future_support;
        if (!decision.keep_stack) {
            decision.fallback_pop = true;
            decision.pop_count    = 1;
        }
        return decision;
    }

    if (match_offset < stack.size()) {
        decision.pop_score = 1 + MatchDepth(stack, match_offset + 1, future_returns, max_depth);
    }

    bool caller_supports_missing_call = stack.front().sym == return_caller;
    decision.keep_stack = decision.keep_score > decision.pop_score ||
                          (decision.keep_score == decision.pop_score && caller_supports_missing_call);
    if (!decision.keep_stack)
        decision.pop_count = match_offset + 1;
    return decision;
}