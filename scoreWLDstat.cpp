#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "chess.hpp"
#include "json.hpp"
#include "threadpool.hpp"

using namespace chess;

namespace fs = std::filesystem;

struct ResultKey {
    std::string white;
    std::string black;
};

class PosAnalyzer {
   public:
    [[nodiscard]] std::unordered_map<std::string, int> pos_map(std::vector<std::string> files) {
        std::unordered_map<std::string, int> pos_map;
        pos_map.reserve(70000);

        for (auto file : files) {
            std::ifstream pgnFile(file);
            std::string line;

            while (true) {
                auto game = pgn::readGame(pgnFile);

                if (!game.has_value()) {
                    break;
                }

                if (game.value().headers().find("Result") == game.value().headers().end()) {
                    break;
                }

                const auto result = game.value().headers().at("Result");

                ResultKey key;

                if (result == "1-0") {
                    key.white = "W";
                    key.black = "L";
                } else if (result == "0-1") {
                    key.white = "L";
                    key.black = "W";
                } else if (result == "1/2-1/2") {
                    key.white = "D";
                    key.black = "D";
                } else {
                    continue;
                }

                int plies = 0;

                Board board = Board();

                if (game.value().headers().find("FEN") != game.value().headers().end()) {
                    board.setFen(game.value().headers().at("FEN"));
                }

                if (game.value().headers().find("Variant") != game.value().headers().end()) {
                    if (game.value().headers().at("Variant") == "fischerandom") {
                        board.set960(true);
                    }
                }

                for (const auto &move : game.value().moves()) {
                    plies++;

                    if (plies > 400) {
                        break;
                    }

                    const auto match_score = utils::splitString(move.comment, '/');

                    const int plieskey = (plies + 1) / 2;

                    int score_key = 0;

                    bool found_score = false;

                    if (match_score.size() >= 1 && move.comment != "book") {
                        found_score = true;

                        if (match_score[0][1] == 'M') {
                            if (match_score[0][1] == '+') {
                                score_key = 1001;
                            } else {
                                score_key = -1001;
                            }

                        } else {
                            const auto score = std::stof(match_score[0]);

                            int score_adjusted = score * 100;

                            if (score_adjusted > 1000) {
                                score_adjusted = 1000;
                            } else if (score_adjusted < -1000) {
                                score_adjusted = -1000;
                            }

                            score_key = int(std::floor(score_adjusted / 5.0)) * 5;
                        }
                    }

                    const auto knights = builtin::popcount(board.pieces(PieceType::KNIGHT));
                    const auto bishops = builtin::popcount(board.pieces(PieceType::BISHOP));
                    const auto rooks = builtin::popcount(board.pieces(PieceType::ROOK));
                    const auto queens = builtin::popcount(board.pieces(PieceType::QUEEN));
                    const auto pawns = builtin::popcount(board.pieces(PieceType::PAWN));

                    const int matcountkey =
                        9 * queens + 5 * rooks + 3 * bishops + 3 * knights + pawns;

                    if (found_score) {
                        const auto turn =
                            board.sideToMove() == Color::WHITE ? key.white : key.black;

                        const auto map_key = "('" + turn + "', " + std::to_string(plieskey) + ", " +
                                             std::to_string(matcountkey) + ", " +
                                             std::to_string(score_key) + ")";

                        pos_map[map_key]++;
                    }

                    board.makeMove(move.move);
                }
            }

            pgnFile.close();
        }

        return pos_map;
    }
};

/// @brief Get all files from a directory.
/// @param path
/// @return
[[nodiscard]] std::vector<std::string> getFiles(std::string_view path = "./pgns") {
    std::vector<std::string> files;

    for (const auto &entry : fs::directory_iterator(path)) {
        files.push_back(entry.path().string());
    }

    return files;
}

/// @brief Split into successive n-sized chunks from pgns.
/// @param pgns
/// @param targetchunks
/// @return
[[nodiscard]] std::vector<std::vector<std::string>> chunkPgns(const std::vector<std::string> &pgns,
                                                              int targetchunks) {
    std::vector<std::vector<std::string>> pgnschunked;

    int chunks_size = (pgns.size() + targetchunks - 1) / targetchunks;

    auto begin = pgns.begin();
    auto end = pgns.end();

    while (begin != end) {
        auto next =
            std::next(begin, std::min(chunks_size, static_cast<int>(std::distance(begin, end))));
        pgnschunked.push_back(std::vector<std::string>(begin, next));
        begin = next;
    }

    return pgnschunked;
}

/// @brief
/// @param argc
/// @param argv Possible ones are --dir and --file
/// @return
int main(int argc, char const *argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    std::vector<std::string> files_pgn;

    if (std::find(args.begin(), args.end(), "--dir") != args.end()) {
        const auto path = std::find(args.begin(), args.end(), "--dir") + 1;
        files_pgn = getFiles(*path);
    } else if (std::find(args.begin(), args.end(), "--file") != args.end()) {
        const auto path = std::find(args.begin(), args.end(), "--file") + 1;
        files_pgn = getFiles(*path);
    } else {
        files_pgn = getFiles();
    }

    int targetchunks = 100 * std::max(1, int(std::thread::hardware_concurrency()));

    std::vector<std::vector<std::string>> files_chunked = chunkPgns(files_pgn, targetchunks);

    std::cout << "Found " << files_pgn.size() << " pgn files, creating " << files_chunked.size()
              << " chunks for processing." << std::endl;

    // Create a thread pool
    ThreadPool pool(std::thread::hardware_concurrency());

    // Futures hold the results of each thread
    std::vector<std::future<std::unordered_map<std::string, int>>> fut;

    const auto t0 = std::chrono::high_resolution_clock::now();

    for (const auto &files : files_chunked) {
        fut.emplace_back(pool.enqueue([&files]() {
            PosAnalyzer analyzer = PosAnalyzer();

            return analyzer.pos_map(files);
        }));
    }

    std::unordered_map<std::string, int> pos_map;

    // Combine the results from all threads
    for (auto &f : fut) {
        auto local_map = f.get();

        for (const auto &pair : local_map) {
            pos_map[pair.first] += pair.second;
        }
    }

    const auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "Time taken: " << std::chrono::duration_cast<std::chrono::seconds>(t1 - t0).count()
              << "s" << std::endl;

    // save json
    std::uint64_t total = 0;

    std::ofstream outFile("scoreWLDstat.json");

    nlohmann::json j;

    for (const auto &pair : pos_map) {
        j[pair.first] = pair.second;
        total += pair.second;
    }

    outFile << j.dump(4);

    outFile.close();

    std::cout << "Retained " << total << " scored positions for analysis." << std::endl;

    return 0;
}
