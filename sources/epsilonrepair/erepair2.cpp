#include "oracle_process.hpp"
#include <algorithm>
#include <iostream>
#include <queue>
#include <string>
#include <vector>
#include <functional>
#include <fstream>
#include <cctype>
#include <set>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>

#include <cstdio>

int interations = 0;
int success = 0;
int failure = 0;
int incomplete = 0;
std::uint64_t oracle_execution_time_ns = 0;
std::uint64_t oracle_total_calls = 0;
std::uint64_t oracle_candidates_submitted = 0;

//-------------------------------------
// 0. CharacterSet
//-------------------------------------
class CharacterSet {
private:
    std::set<char> valid_chars;

public:
    CharacterSet() {
        initializeDefault();
    }

    void initializeDefault() {
        valid_chars.clear();

        std::vector<char> chars = { ')', '}', ']' };

        for(auto c: chars){
            valid_chars.insert(c);
        }
                
        for (int i = 33; i <= 126; ++i) {
            char c = static_cast<char>(i);
            if (std::find(chars.begin(), chars.end(), c) 
                == chars.end()) 
            {
                valid_chars.insert(c);
            }
        }

        valid_chars.insert('\n');
        valid_chars.insert('\t');
    }

    std::set<char>::iterator begin() { return valid_chars.begin(); }
    std::set<char>::iterator end() { return valid_chars.end(); }
};

//-------------------------------------
// 1. ParseResult enum class
//-------------------------------------
enum class ParseResult { INCOMPLETE, CORRECT, INCORRECT };

std::function<ParseResult(const std::string&)> createParser(const std::string& parser_path) {
    return [parser_path](const std::string& input) -> ParseResult {
        oracle_process::Invocation measurement(oracle_total_calls, oracle_execution_time_ns,
                                               oracle_candidates_submitted, 1);
        oracle_process::TemporaryDirectory files;
        const auto candidate = files.file("candidate");
        oracle_process::write_file(candidate, input);
        const auto result = oracle_process::run({parser_path, candidate.string()});
        oracle_process::require_exit_code(result, {0, 1, 255});
        ++interations;
        if (result.exit_code == 0) { ++success; return ParseResult::CORRECT; }
        if (result.exit_code == 255) { ++incomplete; return ParseResult::INCOMPLETE; }
        ++failure;
        return ParseResult::INCORRECT;
    };
}
//-------------------------------------
// 3. BSearch function
//-------------------------------------
int BSearch(const std::string& s,
            const std::function<ParseResult(const std::string&)>& parser,
            int left = 0) {
    int right = static_cast<int>(s.size());
    // If the entire string is not INCORRECT, return directly
    if (parser(s.substr(0, right)) != ParseResult::INCORRECT) return right;

    // Binary search for boundary
    while (left < right - 1) {
        int middle = (left + right) / 2;
        if (parser(s.substr(0, middle)) != ParseResult::INCORRECT) {
            left = middle;
        } else {
            right = middle;
        }
    }

    return left;
}

//-------------------------------------
// 4. DRepair function
//-------------------------------------
std::string DRepair(const std::string& input,
                    const std::function<ParseResult(const std::string&)>& parser) {
    struct State {
        std::string str;        // current string
        int boundary;           // current boundary
        int editingDistance;    // accumulated editing distance (lower is higher priority)

        bool operator>(const State& other) const {
            return editingDistance > other.editingDistance;
        }
    };

    // Min-heap, with smaller editingDistance having higher priority
    std::priority_queue<State, std::vector<State>, std::greater<State>> pq;

    // Initial boundary
    int boundary = BSearch(input, parser, 0);
    pq.push({input, boundary, 0});

    CharacterSet valid_chars;

    while (!pq.empty()) {
        State current = pq.top();
        pq.pop();
        std::cout << "Dealing with current string:\n" << current.str << "\n\n";
                // If the entire string is CORRECT, return directly
        if (parser(current.str) == ParseResult::CORRECT) {
            return current.str;
        }

        // 1) Try deleting the character at the boundary
        if (current.boundary < static_cast<int>(current.str.size())) {
            std::string new_str = current.str;
            new_str.erase(current.boundary, 1);
            if (parser(new_str)== ParseResult::CORRECT){
                return new_str;
            }
            int new_boundary = BSearch(new_str, parser);

            if(new_boundary - current.boundary > 0){
                // Believe this corruption has been healed, handling next corruption
                std::priority_queue<State, std::vector<State>, std::greater<State>> empty;
                pq.swap(empty);
                pq.push({new_str, new_boundary, current.editingDistance + 1});
                continue;
            }
            pq.push({new_str, new_boundary, current.editingDistance + 1});
        }

        // 2) Try inserting various valid characters at the boundary
        bool flag = false;
        bool all_accepted = true;
        for (char c : valid_chars) {
            std::string new_str = current.str;
            new_str.insert(current.boundary, 1, c);

            if(parser(new_str) == ParseResult::CORRECT){
                return new_str;
            }
            int new_boundary = BSearch(new_str, parser);
            if (new_boundary - current.boundary > 1) {
                // Believe this corruption has been healed, handling next corruption 
                std::priority_queue<State, std::vector<State>, std::greater<State>> empty;
                pq.swap(empty);
                pq.push({new_str, new_boundary, current.editingDistance + 1});
                break;
            } else if (new_boundary - current.boundary == 1) {
                pq.push({new_str, new_boundary, current.editingDistance + 1});
                if(c!='\n' && c!='\t'){
                    flag = true;
                }
            } else {
                all_accepted = false;
                continue;
            }
        }
        if (!flag) {
            if(parser(current.str.substr(0, current.boundary)) == ParseResult::CORRECT){
                return current.str.substr(0, current.boundary);
            }
        }
        // if (all_accepted && current.boundary == current.str.size()) {
        //     std::cout<<"All accepted"<<std::endl;
        //     std::string temp  = current.str;
        //     for(int i=33;i<=126;i++){
        //         char c = static_cast<char>(i);
        //         temp.push_back(c);
        //     }
        //     temp.push_back('a'); //watchman
        //     int temp_boundary = BSearch(temp, parser);
        //     if(temp_boundary!=temp.size()-1){

        //         char c = temp[temp_boundary-1];
        //         // std::cout<<"c: "<<c<<std::endl;
        //         // std::cout<<temp<<std::endl;
        //         current.str.push_back(c);
        //         int new_boundary = BSearch(current.str, parser);
        //         pq.push({current.str, new_boundary, current.editingDistance-1}); // priority is not increased
        //     } 
        // }
    }

    // No feasible solution found
    return "";
}

//-------------------------------------
// 5. Main function
//-------------------------------------
int main(int argc, char* argv[]) {
    try {
    oracle_process::persist_metrics(0, 0, 0, 0);
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <parser_path> <input_file> <output_file>\n";
        return 1;
    }

    std::string parser_path    = argv[1];
    std::string input_filename = argv[2];
    std::string output_filename= argv[3];

    std::ifstream input_file(input_filename);
    if (!input_file.is_open()) {
        std::cerr << "Error: Could not open file " << input_filename << std::endl;
        return 1;
    }

    // Read the file contents into a string
    std::string input((std::istreambuf_iterator<char>(input_file)), std::istreambuf_iterator<char>());
    input_file.close();

    // Create the parser and run DRepair
    auto parser = createParser(parser_path);
    std::string result = DRepair(input, parser);

    if (!result.empty()) {
        std::ofstream out_file(output_filename);
        if (!out_file.is_open()) {
            std::cerr << "Error: Could not open output file " << output_filename << std::endl;
            return 1;
        }
        out_file << result;
        out_file.close();

        std::cout << "After repair:\n" << result << "\n";
        std::cout << "Repaired string saved to: " << output_filename << std::endl;
    } else {
        std::cout << "No valid repair found." << std::endl;
    }
    printf("*** Number of required oracle runs: %d correct: %d incorrect: %d incomplete: %d ***\n", interations, success, failure, incomplete);
    std::cout << "ORACLE_METRICS total_calls=" << oracle_total_calls
              << " execution_time_ns=" << oracle_execution_time_ns << " candidates_submitted=" << oracle_candidates_submitted << "\n";
    return 0;
    } catch (const std::exception& error) {
        std::cerr << "erepair: " << error.what() << '\n';
        return 1;
    }
}
