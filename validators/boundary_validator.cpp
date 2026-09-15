#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

enum class Status { correct = 0, incorrect = 1, incomplete = 255 };

bool digit(char value) {
    return value >= '0' && value <= '9';
}

bool hex(char value) {
    return digit(value) || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

Status fixed_pattern(std::string_view value, std::string_view pattern) {
    if (value.size() > pattern.size()) return Status::incorrect;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (pattern[index] == 'D' ? !digit(value[index])
                                  : value[index] != pattern[index]) {
            return Status::incorrect;
        }
    }
    return value.size() == pattern.size() ? Status::correct : Status::incomplete;
}

Status isbn(std::string_view value) {
    // State: number of the first nine digits consumed, whether a separator
    // was just consumed, or digits == 10 for a complete ISBN.
    using State = std::pair<int, bool>;
    std::set<State> states{{0, false}};
    for (char character : value) {
        std::set<State> next;
        for (const auto [digits, after_separator] : states) {
            if (digits == 10) continue;
            if (after_separator) {
                if (digits < 9 && digit(character)) next.emplace(digits + 1, false);
                else if (digits == 9 && (digit(character) || character == 'X'))
                    next.emplace(10, false);
                continue;
            }
            if (digits >= 1 && digits <= 9 &&
                (character == '-' || character == ' ')) {
                next.emplace(digits, true);
            }
            if (digits < 9 && digit(character)) next.emplace(digits + 1, false);
            else if (digits == 9 && (digit(character) || character == 'X'))
                next.emplace(10, false);
        }
        states = std::move(next);
        if (states.empty()) return Status::incorrect;
    }
    if (states.contains({10, false})) return Status::correct;
    return states.empty() ? Status::incorrect : Status::incomplete;
}

Status separated_groups(std::string_view value, char separator, int groups,
                        int maximum_group_length, bool (*allowed)(char)) {
    int completed_groups = 0;
    int group_length = 0;
    for (char character : value) {
        if (character == separator) {
            if (group_length == 0 || completed_groups >= groups - 1)
                return Status::incorrect;
            ++completed_groups;
            group_length = 0;
        } else if (allowed(character) && group_length < maximum_group_length) {
            ++group_length;
        } else {
            return Status::incorrect;
        }
    }
    if (completed_groups == groups - 1 && group_length > 0) return Status::correct;
    return Status::incomplete;
}

bool url_a(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) ||
           std::string_view("-@:%._+~#=").find(value) != std::string_view::npos;
}

bool url_b(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '(' || value == ')';
}

bool url_c(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) ||
           std::string_view("-()@:%_+.~#?&/=").find(value) != std::string_view::npos;
}

bool word(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

enum class UrlPhase { optional_www, authority, suffix, tail };
struct UrlState {
    UrlPhase phase;
    int count;
    bool last_word;
    bool operator<(const UrlState& other) const {
        return std::tie(phase, count, last_word) <
               std::tie(other.phase, other.count, other.last_word);
    }
};

Status url_after_scheme(std::string_view value) {
    std::set<UrlState> states{
        {UrlPhase::authority, 0, false},
        {UrlPhase::optional_www, 0, false},
    };
    constexpr std::string_view www = "www.";
    for (char character : value) {
        std::set<UrlState> next;
        for (const UrlState& state : states) {
            if (state.phase == UrlPhase::optional_www) {
                if (state.count < static_cast<int>(www.size()) &&
                    character == www[static_cast<std::size_t>(state.count)]) {
                    const int position = state.count + 1;
                    next.insert(position == static_cast<int>(www.size())
                                    ? UrlState{UrlPhase::authority, 0, false}
                                    : UrlState{UrlPhase::optional_www, position, false});
                }
            } else if (state.phase == UrlPhase::authority) {
                if (url_a(character) && state.count < 256)
                    next.insert({UrlPhase::authority, state.count + 1, false});
                if (character == '.' && state.count >= 1)
                    next.insert({UrlPhase::suffix, 0, false});
            } else if (state.phase == UrlPhase::suffix) {
                if (url_b(character) && state.count < 6)
                    next.insert({UrlPhase::suffix, state.count + 1, word(character)});
                if (state.count >= 1 && state.last_word && url_c(character) && !word(character))
                    next.insert({UrlPhase::tail, 0, false});
            } else if (url_c(character)) {
                next.insert(state);
            }
        }
        states = std::move(next);
        if (states.empty()) return Status::incorrect;
    }
    const bool accepted = std::any_of(states.begin(), states.end(), [](const UrlState& state) {
        return state.phase == UrlPhase::tail ||
               (state.phase == UrlPhase::suffix && state.count >= 1 && state.last_word);
    });
    if (accepted) return Status::correct;
    const bool extendible = std::any_of(states.begin(), states.end(), [](const UrlState& state) {
        return state.phase != UrlPhase::suffix || state.count < 6 || state.last_word;
    });
    return extendible ? Status::incomplete : Status::incorrect;
}

Status url(std::string_view value) {
    constexpr std::string_view schemes[] = {"http://", "https://"};
    bool scheme_prefix = false;
    for (std::string_view scheme : schemes) {
        if (value.size() < scheme.size() && scheme.starts_with(value)) scheme_prefix = true;
        if (value.starts_with(scheme)) return url_after_scheme(value.substr(scheme.size()));
    }
    return scheme_prefix ? Status::incomplete : Status::incorrect;
}

Status validate(std::string_view format, std::string_view value) {
    if (format == "date") return fixed_pattern(value, "DDDD-DD-DD");
    if (format == "time") return fixed_pattern(value, "DD:DD:DD");
    if (format == "isbn") return isbn(value);
    if (format == "ipv4") return separated_groups(value, '.', 4, 3, digit);
    if (format == "ipv6") return separated_groups(value, ':', 8, 4, hex);
    if (format == "url") return url(value);
    return Status::incorrect;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream stream(argv[1], std::ios::binary);
    if (!stream) return 2;
    const std::string input{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad()) return 2;

    std::string name = std::filesystem::path(argv[0]).stem().string();
    constexpr std::string_view prefix = "boundary_validate_";
    if (!name.starts_with(prefix)) return 2;
    name.erase(0, prefix.size());
    if (name != "date" && name != "time" && name != "isbn" &&
        name != "ipv4" && name != "ipv6" && name != "url") return 2;
    return static_cast<int>(validate(name, input));
}

