#pragma once
#include "oracle_process.hpp"
#include <nlohmann/json.hpp>

namespace patchouli {
class Oracle {
public:
    virtual ~Oracle() = default;
    virtual std::vector<bool> accepts_batch(const std::vector<std::string>& values) = 0;
};

class ExternalOracle final : public Oracle {
public:
    explicit ExternalOracle(std::filesystem::path executable)
        : executable_(std::move(executable)) {}

    std::vector<bool> accepts_batch(const std::vector<std::string>& values) override {
        if (values.empty()) return {};
        const auto executable = executable_.u8string();
        const auto response = oracle_process::run(
            {std::string(reinterpret_cast<const char*>(executable.data()), executable.size())},
            nlohmann::json(values).dump());
        oracle_process::require_exit_code(response, {0});
        const auto result = nlohmann::json::parse(response.output);
        if (!result.is_array() || result.empty() || result.size() > values.size())
            throw std::runtime_error("oracle result count mismatch: expected a non-empty boolean prefix");
        std::vector<bool> accepted;
        for (const auto& item : result) {
            if (!item.is_boolean()) throw std::runtime_error("oracle result is not boolean");
            accepted.push_back(item.get<bool>());
        }
        if (accepted.size() < values.size() && !accepted.back())
            throw std::runtime_error("oracle stopped before accepting a candidate");
        return accepted;
    }
private:
    std::filesystem::path executable_;
};
} // namespace patchouli
