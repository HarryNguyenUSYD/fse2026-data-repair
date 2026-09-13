#include "patchouli/oracle.hpp"
#include <iostream>
#include <iterator>

int main(int argc,char** argv) {
    try {
        const std::string input{std::istreambuf_iterator<char>(std::cin),{}};
        const auto name=std::filesystem::path(argv[0]).stem().string();
        if (name.starts_with("fixture_")) {
            if (name=="fixture_bad_json") std::cout << "bad";
            else if (name=="fixture_wrong_count") std::cout << "[]";
            else if (name=="fixture_wrong_type") std::cout << "[1]";
            else if (name=="fixture_nonarray") std::cout << "true";
            else if (name=="fixture_failure") { std::cout << "[true]"; return 1; }
            return 0;
        }
        if (argc!=2) return 2;
        patchouli::ExternalOracle oracle(argv[1]);
        const auto values=nlohmann::json::parse(input).get<std::vector<std::string>>();
        std::cout << nlohmann::json(oracle.accepts_batch(values)).dump();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what();
        return 2;
    }
}
