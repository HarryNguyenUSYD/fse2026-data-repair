#pragma once

#include "patchouli/types.hpp"

#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace patchouli {

struct DfaState {
    bool accepting{false};
    std::map<Symbol, StateId> transitions;
};

class Automaton {
public:
    StateId add_state(bool accepting = false) {
        if (states_.size() > static_cast<std::size_t>(std::numeric_limits<StateId>::max())) {
            throw std::runtime_error("DFA state identifier overflow");
        }
        const auto id = static_cast<StateId>(states_.size());
        DfaState state;
        state.accepting = accepting;
        states_.push_back(std::move(state));
        return id;
    }

    StateId start_state() const noexcept { return start_; }
    void set_start_state(StateId state) { start_ = state; }

    std::size_t storage_size() const noexcept { return states_.size(); }

    StateId resolve(StateId state) const {
        if (state >= states_.size()) {
            throw std::runtime_error("invalid DFA state");
        }
        return state;
    }

    DfaState& state(StateId id) { return states_.at(resolve(id)); }
    const DfaState& state(StateId id) const { return states_.at(resolve(id)); }

    std::vector<StateId> active_states() const {
        std::vector<StateId> result;
        for (StateId id = 0; id < states_.size(); ++id) {
            result.push_back(id);
        }
        return result;
    }

    bool transition(StateId source, Symbol symbol, StateId& destination) const {
        const auto& transitions = state(source).transitions;
        const auto found = transitions.find(symbol);
        if (found == transitions.end()) {
            return false;
        }
        destination = resolve(found->second);
        return true;
    }

    bool accepts(std::string_view value) const {
        StateId current = resolve(start_);
        for (unsigned char byte : value) {
            StateId next{};
            if (!transition(current, byte, next)) {
                return false;
            }
            current = next;
        }
        return state(current).accepting;
    }

    std::vector<StateId> path(std::string_view value) const {
        std::vector<StateId> result;
        StateId current = resolve(start_);
        result.push_back(current);
        for (unsigned char byte : value) {
            StateId next{};
            if (!transition(current, byte, next)) {
                return {};
            }
            current = next;
            result.push_back(current);
        }
        return result;
    }

    std::string fingerprint() const {
        std::ostringstream out;
        out << "start=" << resolve(start_) << ';';
        for (StateId id : active_states()) {
            out << id << ':' << (state(id).accepting ? '1' : '0') << '[' << id << ",]{";
            for (const auto& [symbol, destination] : state(id).transitions) {
                out << static_cast<unsigned int>(symbol) << '>' << resolve(destination) << ',';
            }
            out << "};";
        }
        return out.str();
    }

private:
    std::vector<DfaState> states_;
    StateId start_{0};
};

}  // namespace patchouli
