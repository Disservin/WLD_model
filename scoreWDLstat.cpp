#include "scoreWDLstat.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "external/chess.hpp"
#include "external/gzip/gzstream.h"
#include "external/parallel_hashmap/phmap.h"
#include "external/threadpool.hpp"

namespace fs = std::filesystem;
using json   = nlohmann::json;

using namespace chess;

std::atomic<std::size_t> total_chunks = 0;

namespace analysis {

std::pair<std::string, int> longest = {"", 0};

/// @brief Analyze a file with pgn games and update the position map, apply filter if present
class Analyze : public pgn::Visitor {
   public:
    Analyze(const std::string &file_name) { file_name_ = file_name; }

    virtual ~Analyze() {}

    void startPgn() override {}

    void startMoves() override { skipPgn(true); }

    void header(std::string_view key, std::string_view value) override {
        if (key == "PlyCount") {
            const int ply_count = std::stoi(value.data());

            if (ply_count > longest.second) {
                longest = {file_name_, ply_count};
            }
        }
    }

    void move(std::string_view, std::string_view) override {}

    void endPgn() override {}

   private:
    std::string file_name_;
};

void ana_files(const std::vector<std::string> &files) {
    for (const auto &file : files) {
        const auto pgn_iterator = [&](std::istream &iss) {
            auto vis = std::make_unique<Analyze>(file);

            pgn::StreamParser parser(iss);

            try {
                parser.readGames(*vis);
            } catch (const std::exception &e) {
                std::cout << "Error when parsing: " << file << std::endl;
                std::cerr << e.what() << '\n';
            }
        };

        if (file.size() >= 3 && file.substr(file.size() - 3) == ".gz") {
            igzstream input(file.c_str());
            pgn_iterator(input);
        } else {
            std::ifstream pgn_stream(file);
            pgn_iterator(pgn_stream);
            pgn_stream.close();
        }
    }
}

}  // namespace analysis

void process(const std::vector<std::string> &files_pgn, int concurrency) {
    // Create more chunks than threads to prevent threads from idling.
    int target_chunks = 4 * concurrency;

    auto files_chunked = split_chunks(files_pgn, target_chunks);

    std::cout << "Found " << files_pgn.size() << " .pgn(.gz) files, creating "
              << files_chunked.size() << " chunks for processing." << std::endl;

    // Mutex for progress success
    std::mutex progress_mutex;

    // Create a thread pool
    ThreadPool pool(concurrency);

    // Print progress
    std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size() << std::flush;

    for (const auto &files : files_chunked) {
        pool.enqueue([&files, &progress_mutex, &files_chunked]() {
            analysis::ana_files(files);

            total_chunks++;

            // Limit the scope of the lock
            {
                const std::lock_guard<std::mutex> lock(progress_mutex);

                // Print progress
                std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size()
                          << std::flush;
            }
        });
    }

    // Wait for all threads to finish
    pool.wait();
}

void print_usage(char const *program_name) {
    std::stringstream ss;

    // clang-format off
    ss << "Usage: " << program_name << " [options]" << "\n";
    ss << "Options:" << "\n";
    ss << "  --file <path>         Path to .pgn(.gz) file" << "\n";
    ss << "  --dir <path>          Path to directory containing .pgn(.gz) files (default: pgns)" << "\n";
    ss << "  -r                    Search for .pgn(.gz) files recursively in subdirectories" << "\n";
    ss << "  --help                Print this help message" << "\n";
    // clang-format on

    std::cout << ss.str();
}

/// @brief
/// @param argc
/// @param argv See print_usage() for possible arguments
/// @return
int main(int argc, char const *argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    std::vector<std::string> files_pgn;
    std::string regex_book, regex_rev, regex_engine, json_filename = "scoreWDLstat.json";

    std::vector<std::string>::const_iterator pos;

    int concurrency = std::max(1, int(std::thread::hardware_concurrency()));

    if (std::find(args.begin(), args.end(), "--help") != args.end()) {
        print_usage(argv[0]);
        return 0;
    }

    if (find_argument(args, pos, "--concurrency")) {
        concurrency = std::stoi(*std::next(pos));
    }

    if (find_argument(args, pos, "--file")) {
        files_pgn = {*std::next(pos)};
    } else {
        std::string path = "./pgns";

        if (find_argument(args, pos, "--dir")) {
            path = *std::next(pos);
        }

        bool recursive = find_argument(args, pos, "-r", true);
        std::cout << "Looking " << (recursive ? "(recursively) " : "") << "for pgn files in "
                  << path << std::endl;

        files_pgn = get_files(path, recursive);
    }

    // sort to easily check for "duplicate" files, i.e. "foo.pgn.gz" and "foo.pgn"
    std::sort(files_pgn.begin(), files_pgn.end());

    for (size_t i = 1; i < files_pgn.size(); ++i) {
        if (files_pgn[i].find(files_pgn[i - 1]) == 0) {
            std::cout << "Error: \"Duplicate\" files: " << files_pgn[i - 1] << " and "
                      << files_pgn[i] << std::endl;
            std::exit(1);
        }
    }

    std::cout << "Found " << files_pgn.size() << " .pgn(.gz) files in total." << std::endl;

    const auto t0 = std::chrono::high_resolution_clock::now();

    process(files_pgn, concurrency);

    const auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "\nTime taken: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0
              << "s" << std::endl;

    std::cout << "Longest game found in: " << analysis::longest.first << " ("
              << analysis::longest.second << " plies)" << std::endl;

    return 0;
}
