#include "replay_matcher.h"

#include <cassert>
#include <iostream>

int main()
{
    const std::vector<ReplayStackEntry> stack{{"C", "B"}, {"B", "A"}, {"A", "root"}};

    auto exact = ChooseReturnRecovery(stack, "C", "B", {}, 16);
    assert(!exact.keep_stack && exact.pop_count == 1);

    auto missing_returns = ChooseReturnRecovery(stack, "B", "A", {"A"}, 16);
    assert(!missing_returns.keep_stack && missing_returns.pop_count == 2);
    assert(missing_returns.pop_score > missing_returns.keep_score);

    auto missing_call = ChooseReturnRecovery(stack, "X", "C", {"C", "B", "A"}, 16);
    assert(missing_call.keep_stack && missing_call.pop_count == 0);

    auto future_supported_missing_call = ChooseReturnRecovery(stack, "X", "other", {"C", "B"}, 16);
    assert(future_supported_missing_call.keep_stack);

    auto unsupported_return = ChooseReturnRecovery(stack, "X", "other", {}, 16);
    assert(!unsupported_return.keep_stack && unsupported_return.fallback_pop);
    assert(unsupported_return.pop_count == 1);

    auto deeper_missing_call = ChooseReturnRecovery(stack, "B", "A", {"C", "B", "A"}, 16);
    assert(deeper_missing_call.keep_stack);
    assert(deeper_missing_call.keep_score > deeper_missing_call.pop_score);

    auto bounded = ChooseReturnRecovery(stack, "B", "A", {"C", "B", "A"}, 1);
    assert(bounded.keep_stack);

    std::cout << "Replay matcher tests passed" << std::endl;
    return 0;
}