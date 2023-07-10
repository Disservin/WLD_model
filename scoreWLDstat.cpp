#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
    std::unordered_map<std::string, int> posMap(std::vector<std::string> files) {
        std::unordered_map<std::string, int> posMap;

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

                for (const auto &move : game.value().moves()) {
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

                        posMap[map_key]++;
                    }

                    board.makeMove(move.move);
                }
            }

            pgnFile.close();
        }

        return posMap;
    }

   private:
    const std::string score_regex_ = "([+-]*M*[0-9.]*)/([0-9]*)";
    const std::string mate_regex = "([+-])M[0-9]*";
};

std::vector<std::string> getFiles() {
    std::string path = "./pgns";
    std::vector<std::string> files;
    for (const auto &entry : fs::directory_iterator(path)) {
        files.push_back(entry.path());
        std::cout << entry.path() << std::endl;
    }

    return files;
}

int main(int argc, char const *argv[]) {
    PosAnalyzer analyzer = PosAnalyzer();

    std::vector<std::string> files = getFiles();

    std::unordered_map<std::string, int> posMap = analyzer.posMap(files);

    std::ofstream outFile("posMap.json");

    // save json

    nlohmann::json j;

    for (const auto &pair : posMap) {
        j[pair.first] = pair.second;
    }

    outFile << j.dump(4);

    outFile.close();

    return 0;
}
