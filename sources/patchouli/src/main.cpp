#include "patchouli/patchouli.hpp"
#include "patchouli/json_io.hpp"
#include "patchouli/memory.hpp"
#include "patchouli/oracle.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>

int main() {
    try {
        const patchouli::InputData input = patchouli::read_input_json("input.json");
        const patchouli::Config config = patchouli::read_config_json("config.json");
        const auto start = std::chrono::steady_clock::now();
        const std::uint64_t effective_seed = config.seed.value_or(
            (static_cast<std::uint64_t>(std::random_device{}()) << 32U) ^ std::random_device{}());
        patchouli::ExternalOracle oracle(config.oracle_executable,
                                       config.max_total_oracle_calls);
        patchouli::AlgorithmMeasurements measurements;
        const std::string repaired =
            patchouli::patchouli(input, config, oracle, &measurements);
        const std::uint64_t peak_memory = patchouli::peak_memory_bytes();
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        if (elapsed < 0) {
            throw std::runtime_error("steady clock returned a negative duration");
        }

        const patchouli::ProgramResult result{repaired, effective_seed, peak_memory,
                                            static_cast<std::uint64_t>(elapsed),
                                            measurements};
        std::cout << patchouli::serialize_result(result) << '\n';
        if (!std::cout) {
            throw std::runtime_error("failed to write result JSON to stdout");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "patchouli: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "patchouli: unknown fatal error\n";
        return 1;
    }
}
