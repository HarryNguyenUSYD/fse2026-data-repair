#pragma once

#include "patchouli/partitioned_dfa.hpp"
#include "patchouli/state_merge.hpp"

namespace patchouli {

struct ReplayResult {
    PartitionedDfa partition;
    MergeHistory valid_history;
};

inline ReplayResult replay_merges(const Automaton& base,
                                  PartitionedDfa partition,
                                  const std::set<std::string>& negatives,
                                  const MergeHistory& history,
                                  AlgorithmMeasurements* measurements = nullptr) {
    MergeHistory valid_history;
    for (const auto& record : history) {
        StateId red{}, blue{};
        try {
            red = partition.find_exact_members(record.red_original_states);
            blue = partition.find_exact_members(record.blue_original_states);
        } catch (const std::runtime_error&) {
            break;
        }
        const auto checkpoint = partition.checkpoint();
        partition.merge_with_closure(red, blue);
        const auto validation_started = std::chrono::steady_clock::now();
        const bool consistent = rejects_all(base, partition, negatives);
        if (measurements)
            measurements->negative_validation_ns +=
                state_merge_elapsed_ns(validation_started);
        if (!consistent) {
            partition.rollback(checkpoint);
            break;
        }
        partition.commit(checkpoint);
        valid_history.push_back(record);
    }
    return {std::move(partition), std::move(valid_history)};
}

inline ReplayResult replay_merges(const Automaton& base,
                                  const std::set<std::string>& negatives,
                                  const MergeHistory& history,
                                  AlgorithmMeasurements* measurements = nullptr) {
    return replay_merges(base, PartitionedDfa(base), negatives, history,
                         measurements);
}

}  // namespace patchouli
