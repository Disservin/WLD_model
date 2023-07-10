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

using namespace chess;

namespace fs = std::filesystem;

struct ResultKey {
    std::string white;
    std::string black;
};

class PosAnalyzer {
   public:
    std::unordered_map<std::string, int> pos_map(std::vector<std::string> files) {
        std::unordered_map<std::string, int> pos_map;

        for (auto file : files) {
            std::ifstream pgnFile(file);
            std::string line;

            while (true) {
                auto game = pgn::readGame(pgnFile);

                if (!game.has_value()) {
                    break;
                }

                if (game.value().headers().count("Result") == 0) {
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

                if (game.value().headers().count("FEN")) {
                    board.setFen(game.value().headers().at("FEN"));
                }

                for (const auto& move : game.value().moves()) {
                    plies++;

                    if (plies > 400) {
                        break;
                    }

                    const int plieskey = (plies + 1) / 2;

                    const auto turn = board.sideToMove();

                    const auto match = utils::regex(move.comment, score_regex_);

                    int score_key = 0;

                    bool found_score = false;

                    if (!match.str(1).empty()) {
                        found_score = true;

                        const auto match_mate = utils::regex(move.comment, mate_regex);

                        if (!match_mate.str(1).empty()) {
                            const auto mate = match_mate.str(1);

                            if (mate == "+") {
                                score_key = 1001;
                            } else {
                                score_key = -1001;
                            }

                        } else {
                            const auto score = std::stof(match.str(1));

                            int score_adjusted = score * 100;

                            if (score_adjusted > 1000) {
                                score_adjusted = 1000;
                            } else if (score_adjusted < -1000) {
                                score_adjusted = -1000;
                            }

                            score_key = (score_adjusted / 5) * 5;
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

   private:
    const std::string score_regex_ = "([+-]*M*[0-9.]*)/([0-9]*)";
    const std::string mate_regex = "([+-])M[0-9]*";
};

std::vector<std::string> getFiles() {
    std::string path = "./pgns";
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(path)) {
        files.push_back(entry.path());
    }

    return files;
}

std::vector<std::vector<std::string>> chunkPgns(const std::vector<std::string>& pgns,
                                                int targetchunks) {
    int chunks_size = (pgns.size() + targetchunks - 1) / targetchunks;
    std::vector<std::vector<std::string>> pgnschunked;

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

/// @brief https://github.com/Disservin/fast-chess/blob/master/src/matchmaking/threadpool.hpp
class ThreadPool {
   public:
    ThreadPool(std::size_t num_threads) : stop_(false) {
        for (std::size_t i = 0; i < num_threads; ++i) workers_.emplace_back([this] { work(); });
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) throw std::runtime_error("Warning: enqueue on stopped ThreadPool");
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return res;
    }

    void kill() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
            tasks_ = {};
        }

        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        workers_.clear();
    }

    ~ThreadPool() { kill(); }

    std::size_t queueSize() {
        std::unique_lock<std::mutex> lock(this->queue_mutex_);
        return tasks_.size();
    }

    bool getStop() { return stop_; }

   private:
    void work() {
        while (!this->stop_) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(this->queue_mutex_);
                this->condition_.wait(lock,
                                      [this] { return this->stop_ || !this->tasks_.empty(); });
                if (this->stop_ && this->tasks_.empty()) return;
                task = std::move(this->tasks_.front());
                this->tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;

    std::atomic_bool stop_;
};

int main(int argc, char const* argv[]) {
    const auto files_pgn = getFiles();

    int targetchunks = 100 * std::max(1, int(std::thread::hardware_concurrency()));
    std::vector<std::vector<std::string>> files_chunked = chunkPgns(files_pgn, targetchunks);

    std::cout << "Found " << files_pgn.size() << " pgn files, creating " << files_chunked.size()
              << " chunks for processing." << std::endl;

    std::vector<std::future<std::unordered_map<std::string, int>>> fut;

    ThreadPool pool(std::thread::hardware_concurrency());

    const auto t0 = std::chrono::high_resolution_clock::now();

    for (const auto& files : files_chunked) {
        fut.emplace_back(pool.enqueue([&files]() {
            PosAnalyzer analyzer = PosAnalyzer();

            return analyzer.pos_map(files);
        }));
    }

    // Combine the results from all threads
    std::unordered_map<std::string, int> pos_map;

    for (auto& f : fut) {
        auto local_map = f.get();

        for (const auto& pair : local_map) {
            pos_map[pair.first] += pair.second;
        }
    }

    const auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "Time taken: " << std::chrono::duration_cast<std::chrono::seconds>(t1 - t0).count()
              << "s" << std::endl;

    std::cout << "Retained " << pos_map.size() << " scored positions for analysis." << std::endl;

    // save json

    std::ofstream outFile("scoreWLDstat.json");

    nlohmann::json j;

    for (const auto& pair : pos_map) {
        j[pair.first] = pair.second;
    }

    outFile << j.dump(4);

    outFile.close();

    return 0;
}
