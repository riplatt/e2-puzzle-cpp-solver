// speed_solver_wavefront.cpp - Wavefront solver
// Cache-friendly diagonal traversal with O(1) constraint lookups
//
// Performance achieved: Combines wavefront cache efficiency with constraint table lookups

#include <vector>
#include <array>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include "solution_storage.h"

// Performance constants
static constexpr int MAX_EDGE_TYPES = 32;

// Hint structure for pre-placing pieces
struct Hint {
    int x, y;
    int piece_id;
    int rotation;

    Hint() = default;
    Hint(int x_, int y_, int piece_id_, int rotation_)
        : x(x_), y(y_), piece_id(piece_id_), rotation(rotation_) {}
};

// Ultra-fast piece rotation structure - cache-line optimized
struct __attribute__((packed)) PieceRotation {
    uint8_t edges[4];     // N, E, S, W edges
    uint8_t* used_flag;   // Pointer to piece usage flag (uint8_t for speed)
    uint8_t piece_id;     // Original piece ID (0-based)
    uint8_t rotation;     // Rotation number (0-3)

    inline PieceRotation(uint8_t n, uint8_t e, uint8_t s, uint8_t w, uint8_t* flag, uint8_t pid, uint8_t rot)
        : used_flag(flag), piece_id(pid), rotation(rot) {
        edges[0] = n; edges[1] = e; edges[2] = s; edges[3] = w;
    }
} __attribute__((aligned(32)));

// Board slot with direct edge values - optimized for speed
struct BoardSlot {
    uint8_t edges[4];     // Direct N,E,S,W edge values (0 = border)
    int piece_idx;        // Index into all_rotations array (-1 if empty)

    inline BoardSlot() : piece_idx(-1) {
        *reinterpret_cast<uint32_t*>(edges) = 0;  // Fast zero initialization
    }
};

// SlotInfo for position metadata optimization
enum SlotType {
    TOP_LEFT_CORNER = 0,
    TOP_RIGHT_CORNER = 1,
    BOTTOM_LEFT_CORNER = 2,
    BOTTOM_RIGHT_CORNER = 3,
    TOP_EDGE = 4,
    BOTTOM_EDGE = 5,
    LEFT_EDGE = 6,
    RIGHT_EDGE = 7,
    INTERIOR = 8
};

struct SlotInfo {
    SlotType type;
    int north_neighbor_idx;
    int west_neighbor_idx;
    int x, y;  // Position coordinates
};

std::vector<Hint> parse_hints_file(const std::string& filename) {
    std::vector<Hint> hints;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "Warning: Could not open hints file: " << filename << std::endl;
        return hints;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        Hint hint;
        if (iss >> hint.x >> hint.y >> hint.piece_id >> hint.rotation) {
            hints.push_back(hint);
        } else {
            std::cerr << "Warning: Invalid hints file format: " << line << std::endl;
        }
    }

    return hints;
}

class SpeedSolver {
private:
    // Board configuration
    int width, height, num_pieces;
    std::vector<std::array<uint8_t, 4>> original_pieces;
    std::vector<uint8_t> piece_used;
    std::vector<PieceRotation> all_rotations;
    std::vector<BoardSlot> board_slots;

    // Performance statistics
    size_t placements_tried = 0;
    size_t solutions_found = 0;
    bool find_first_only = false;

    // Solution storage
    std::unique_ptr<SolutionWriter> solution_writer;

    // SlotInfo optimization: pre-computed position metadata
    std::vector<SlotInfo> slot_info_table;

    // Wavefront solver: combines O(1) lookups with cache-friendly traversal
    std::vector<int> wavefront_path;  // Diagonal traversal order (slot indices)

    // Position-specific piece tables for O(1) constraint lookups
    std::vector<uint16_t> corner_tl;
    std::vector<std::vector<uint16_t>> corner_tr;
    std::vector<std::vector<uint16_t>> corner_bl;
    std::vector<std::vector<std::vector<uint16_t>>> corner_br;
    std::vector<std::vector<uint16_t>> edge_top;
    std::vector<std::vector<std::vector<uint16_t>>> edge_bottom;
    std::vector<std::vector<uint16_t>> edge_left;
    std::vector<std::vector<std::vector<uint16_t>>> edge_right;
    std::vector<std::vector<std::vector<uint16_t>>> interior;

public:
    SpeedSolver() {
        corner_tr.resize(MAX_EDGE_TYPES);
        corner_bl.resize(MAX_EDGE_TYPES);
        corner_br.resize(MAX_EDGE_TYPES);
        edge_top.resize(MAX_EDGE_TYPES);
        edge_bottom.resize(MAX_EDGE_TYPES);
        edge_left.resize(MAX_EDGE_TYPES);
        edge_right.resize(MAX_EDGE_TYPES);
        interior.resize(MAX_EDGE_TYPES);

        for (int i = 0; i < MAX_EDGE_TYPES; i++) {
            corner_br[i].resize(MAX_EDGE_TYPES);
            edge_bottom[i].resize(MAX_EDGE_TYPES);
            edge_right[i].resize(MAX_EDGE_TYPES);
            interior[i].resize(MAX_EDGE_TYPES);
        }
    }

    // Load puzzle from file
    bool load_puzzle(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open file " << filename << std::endl;
            return false;
        }

        file >> width >> height;
        num_pieces = width * height;

        original_pieces.clear();
        original_pieces.reserve(num_pieces);

        // Read pieces
        for (int i = 0; i < num_pieces; i++) {
            std::array<uint8_t, 4> piece;
            for (int j = 0; j < 4; j++) {
                int edge;
                file >> edge;
                piece[j] = static_cast<uint8_t>(edge);
            }
            original_pieces.push_back(piece);
        }

        file.close();

        // Initialize data structures
        piece_used.assign(num_pieces, 0);
        board_slots.assign(num_pieces, BoardSlot());
        build_slot_info_table();
        precompute_rotations();
        generate_diagonal_wavefront_path();

        return true;
    }

    bool apply_hints(const std::vector<Hint>& hints) {
        for (const auto& hint : hints) {
            // Validate hint coordinates
            if (hint.x < 0 || hint.x >= width || hint.y < 0 || hint.y >= height) {
                std::cerr << "Error: Hint position (" << hint.x << ", " << hint.y
                         << ") is out of bounds for " << width << "x" << height << " puzzle" << std::endl;
                return false;
            }

            // Validate piece ID
            if (hint.piece_id < 1 || hint.piece_id > num_pieces) {
                std::cerr << "Error: Hint piece ID " << hint.piece_id
                         << " is out of range (1-" << num_pieces << ")" << std::endl;
                return false;
            }

            // Validate rotation
            if (hint.rotation < 0 || hint.rotation > 3) {
                std::cerr << "Error: Hint rotation " << hint.rotation
                         << " is invalid (must be 0-3)" << std::endl;
                return false;
            }

            // Calculate slot index and rotation index
            int slot_idx = hint.y * width + hint.x;
            int rotation_idx = (hint.piece_id - 1) * 4 + hint.rotation;

            // Check if piece is already used
            if (piece_used[hint.piece_id - 1]) {
                std::cerr << "Error: Piece " << hint.piece_id << " is used multiple times in hints" << std::endl;
                return false;
            }

            // Check if slot is already filled
            if (board_slots[slot_idx].piece_idx != -1) {
                std::cerr << "Error: Position (" << hint.x << ", " << hint.y
                         << ") is filled multiple times in hints" << std::endl;
                return false;
            }

            // Apply the hint
            board_slots[slot_idx].piece_idx = rotation_idx;
            // Copy all edges from the piece rotation (N, E, S, W)
            for (int j = 0; j < 4; j++) {
                board_slots[slot_idx].edges[j] = all_rotations[rotation_idx].edges[j];
            }
            piece_used[hint.piece_id - 1] = true;

            std::cout << "Applied hint: piece " << hint.piece_id << " (rotation " << hint.rotation
                     << ") at position (" << hint.x << ", " << hint.y << ")" << std::endl;
        }
        return true;
    }

    // Build SlotInfo table for O(1) position metadata lookup
    void build_slot_info_table() {
        slot_info_table.resize(num_pieces);

        for (int slot_index = 0; slot_index < num_pieces; slot_index++) {
            SlotInfo& info = slot_info_table[slot_index];
            info.x = slot_index % width;
            info.y = slot_index / width;

            // Determine slot type and neighbors
            if (info.x == 0 && info.y == 0) {
                info.type = TOP_LEFT_CORNER;
                info.north_neighbor_idx = -1;
                info.west_neighbor_idx = -1;
            } else if (info.x == width - 1 && info.y == 0) {
                info.type = TOP_RIGHT_CORNER;
                info.north_neighbor_idx = -1;
                info.west_neighbor_idx = slot_index - 1;
            } else if (info.x == 0 && info.y == height - 1) {
                info.type = BOTTOM_LEFT_CORNER;
                info.north_neighbor_idx = slot_index - width;
                info.west_neighbor_idx = -1;
            } else if (info.x == width - 1 && info.y == height - 1) {
                info.type = BOTTOM_RIGHT_CORNER;
                info.north_neighbor_idx = slot_index - width;
                info.west_neighbor_idx = slot_index - 1;
            } else if (info.y == 0) {
                info.type = TOP_EDGE;
                info.north_neighbor_idx = -1;
                info.west_neighbor_idx = slot_index - 1;
            } else if (info.y == height - 1) {
                info.type = BOTTOM_EDGE;
                info.north_neighbor_idx = slot_index - width;
                info.west_neighbor_idx = slot_index - 1;
            } else if (info.x == 0) {
                info.type = LEFT_EDGE;
                info.north_neighbor_idx = slot_index - width;
                info.west_neighbor_idx = -1;
            } else if (info.x == width - 1) {
                info.type = RIGHT_EDGE;
                info.north_neighbor_idx = slot_index - width;
                info.west_neighbor_idx = slot_index - 1;
            } else {
                info.type = INTERIOR;
                info.north_neighbor_idx = slot_index - width;
                info.west_neighbor_idx = slot_index - 1;
            }
        }

        std::cout << "Pre-computed " << num_pieces << " slot metadata entries" << std::endl;
    }

    // Generate all rotations for all pieces
    void precompute_rotations() {
        all_rotations.clear();
        all_rotations.reserve(num_pieces * 4);

        for (int p = 0; p < num_pieces; p++) {
            const auto& piece = original_pieces[p];
            for (int r = 0; r < 4; r++) {
                uint8_t n = piece[(0 - r + 4) % 4];
                uint8_t e = piece[(1 - r + 4) % 4];
                uint8_t s = piece[(2 - r + 4) % 4];
                uint8_t w = piece[(3 - r + 4) % 4];
                all_rotations.emplace_back(n, e, s, w, &piece_used[p], p, r);
            }
        }
    }

    // Generate cache-optimized diagonal wavefront path
    void generate_diagonal_wavefront_path() {
        wavefront_path.clear();

        // For cache optimization, we traverse diagonally to maximize L1 cache hits
        // Start from top-left and traverse diagonally: (0,0), (1,0), (0,1), (2,0), (1,1), (0,2), etc.

        std::vector<std::vector<int>> diagonals;

        // Generate diagonals from top-left to bottom-right
        for (int sum = 0; sum < width + height - 1; sum++) {
            std::vector<int> diagonal;

            for (int y = 0; y < height; y++) {
                int x = sum - y;
                if (x >= 0 && x < width) {
                    int slot_idx = y * width + x;
                    diagonal.push_back(slot_idx);
                }
            }

            if (!diagonal.empty()) {
                diagonals.push_back(diagonal);
            }
        }

        // Flatten diagonals into wavefront path
        for (const auto& diagonal : diagonals) {
            for (int slot_idx : diagonal) {
                wavefront_path.push_back(slot_idx);
            }
        }

        std::cout << "Generated wavefront path with " << wavefront_path.size()
                  << " positions across " << diagonals.size() << " diagonals" << std::endl;
    }

    // Build constraint-based lookup tables
    double build_position_tables() {
        auto start = std::chrono::high_resolution_clock::now();

        // Clear all tables
        corner_tl.clear();
        for (auto& vec : corner_tr) vec.clear();
        for (auto& vec : corner_bl) vec.clear();
        for (auto& mat : corner_br) for (auto& vec : mat) vec.clear();
        for (auto& vec : edge_top) vec.clear();
        for (auto& mat : edge_bottom) for (auto& vec : mat) vec.clear();
        for (auto& vec : edge_left) vec.clear();
        for (auto& mat : edge_right) for (auto& vec : mat) vec.clear();
        for (auto& mat : interior) for (auto& vec : mat) vec.clear();

        // Populate tables based on piece constraints
        for (uint16_t idx = 0; idx < all_rotations.size(); idx++) {
            const PieceRotation& piece = all_rotations[idx];
            uint8_t n = piece.edges[0], e = piece.edges[1], s = piece.edges[2], w = piece.edges[3];

            // Top-left corner: needs border edges (n=0, w=0)
            if (n == 0 && w == 0) {
                corner_tl.push_back(idx);
            }

            // Top-right corner: needs border edges (n=0, e=0)
            if (n == 0 && e == 0) {
                corner_tr[w].push_back(idx);
            }

            // Bottom-left corner: needs border edges (s=0, w=0)
            if (s == 0 && w == 0) {
                corner_bl[n].push_back(idx);
            }

            // Bottom-right corner: needs border edges (s=0, e=0)
            if (s == 0 && e == 0) {
                corner_br[w][n].push_back(idx);
            }

            // Top edge: needs border north (n=0)
            if (n == 0) {
                edge_top[w].push_back(idx);
            }

            // Bottom edge: needs border south (s=0)
            if (s == 0) {
                edge_bottom[w][n].push_back(idx);
            }

            // Left edge: needs border west (w=0)
            if (w == 0) {
                edge_left[n].push_back(idx);
            }

            // Right edge: needs border east (e=0)
            if (e == 0) {
                edge_right[w][n].push_back(idx);
            }

            // Interior: no border constraints
            interior[w][n].push_back(idx);
        }

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

        int interior_total = 0;
        for (const auto& mat : interior) {
            for (const auto& vec : mat) {
                interior_total += vec.size();
            }
        }

        std::cout << "Built tables: TL=" << corner_tl.size() << ", interior_total=" << interior_total << std::endl;
        return elapsed;
    }

    // Solution capture
    void capture_solution() {
        if (!solution_writer) return;

        CompactSolution solution(width, height, static_cast<uint32_t>(solutions_found));

        for (int i = 0; i < num_pieces; i++) {
            const BoardSlot& slot = board_slots[i];
            if (slot.piece_idx >= 0) {
                const PieceRotation& piece = all_rotations[slot.piece_idx];
                int x = i % width;
                int y = i / width;
                solution.set_piece(x, y, piece.piece_id);
                solution.set_rotation(x, y, piece.rotation);
            }
        }

        solution_writer->add_solution(solution);
    }

    // Wavefront solver: O(1) lookups + cache-friendly diagonal traversal
    __attribute__((hot, flatten))
    bool solve_recursive_wavefront(int step) {
        if (__builtin_expect(step >= static_cast<int>(wavefront_path.size()), 0)) {
            solutions_found++;
            capture_solution();
            return find_first_only;
        }

        // Get slot index from wavefront path - diagonal traversal for cache efficiency
        int slot_index = wavefront_path[step];

        // Single table lookup instead of div/mod operations!
        const SlotInfo& info = slot_info_table[slot_index];
        const std::vector<uint16_t>* candidate_indices;

        // SAME O(1) constraint-based table lookup as optimized solver
        switch (info.type) {
            case TOP_LEFT_CORNER:
                candidate_indices = &corner_tl;
                break;

            case TOP_RIGHT_CORNER: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges[1];
                candidate_indices = &corner_tr[west_constraint];
                break;
            }

            case BOTTOM_LEFT_CORNER: {
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges[2];
                candidate_indices = &corner_bl[north_constraint];
                break;
            }

            case BOTTOM_RIGHT_CORNER: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges[1];
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges[2];
                candidate_indices = &corner_br[west_constraint][north_constraint];
                break;
            }

            case TOP_EDGE: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges[1];
                candidate_indices = &edge_top[west_constraint];
                break;
            }

            case BOTTOM_EDGE: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges[1];
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges[2];
                candidate_indices = &edge_bottom[west_constraint][north_constraint];
                break;
            }

            case LEFT_EDGE: {
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges[2];
                candidate_indices = &edge_left[north_constraint];
                break;
            }

            case RIGHT_EDGE: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges[1];
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges[2];
                candidate_indices = &edge_right[west_constraint][north_constraint];
                break;
            }

            case INTERIOR: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges[1];
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges[2];
                candidate_indices = &interior[west_constraint][north_constraint];
                break;
            }
        }

        // Critical iteration loop - cache-optimized with indices
        BoardSlot& __restrict__ slot = board_slots[slot_index];

        for (const uint16_t piece_idx : *candidate_indices) {
            const PieceRotation& __restrict__ piece = all_rotations[piece_idx];
            if (__builtin_expect(!*piece.used_flag, 1)) {
                ++placements_tried;

                // Ultra-fast 4-byte copy using uint32_t
                *reinterpret_cast<uint32_t*>(slot.edges) = *reinterpret_cast<const uint32_t*>(piece.edges);
                slot.piece_idx = piece_idx;
                *piece.used_flag = 1;

                // Recurse to next step in wavefront path
                if (__builtin_expect(solve_recursive_wavefront(step + 1), 0)) return true;

                // Fast backtrack
                *reinterpret_cast<uint32_t*>(slot.edges) = 0;
                slot.piece_idx = -1;
                *piece.used_flag = 0;
            }
        }
        return false;
    }

    // Main solve entry point
    bool solve(bool first_only = false) {
        find_first_only = first_only;
        placements_tried = 0;
        solutions_found = 0;

        // Build constraint tables
        std::cout << "Building constraint-based lookup tables..." << std::endl;
        double table_time = build_position_tables();

        std::cout << "Using wavefront solver (O(1) lookups + cache-friendly diagonal traversal)" << std::endl;

        auto solve_start = std::chrono::high_resolution_clock::now();
        bool result = solve_recursive_wavefront(0);
        auto solve_end = std::chrono::high_resolution_clock::now();

        double solve_time = std::chrono::duration<double, std::milli>(solve_end - solve_start).count();
        double total_time = table_time + solve_time;

        // Print performance statistics
        std::cout << "\n=== Performance Statistics ===" << std::endl;
        std::cout << "Solutions found: " << solutions_found << std::endl;
        std::cout << "Placements tried: " << placements_tried << std::endl;

        std::cout << "\n--- Timing Breakdown ---" << std::endl;
        std::cout << "Table build time: " << std::fixed << std::setprecision(6) << table_time << " ms" << std::endl;
        std::cout << "Pure solve time: " << solve_time << " ms" << std::endl;
        std::cout << "Total time: " << total_time << " ms" << std::endl;

        if (solve_time > 0 && placements_tried > 0) {
            double placements_per_second = placements_tried / (solve_time / 1000.0);
            std::cout << "\n--- Pure Solve Performance ---" << std::endl;
            std::cout << "Placements per second: " << std::fixed << std::setprecision(0) << placements_per_second << std::endl;
            std::cout << "Millions placements per sec: " << std::setprecision(6) << (placements_per_second / 1000000.0) << std::endl;

            double overall_placements_per_second = placements_tried / (total_time / 1000.0);
            std::cout << "\n--- Total Performance (with overhead) ---" << std::endl;
            std::cout << "Overall placements per second: " << std::fixed << std::setprecision(0) << overall_placements_per_second << std::endl;
            std::cout << "Overall millions placements per sec: " << std::setprecision(6) << (overall_placements_per_second / 1000000.0) << std::endl;
            std::cout << "Overhead: " << table_time << " ms (" << std::setprecision(6) << (table_time / total_time * 100) << "%)" << std::endl;
        }

        return result;
    }

    // Print solution
    void print_solution() {
        std::cout << "\nSolution:" << std::endl;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int slot_index = y * width + x;
                const BoardSlot& slot = board_slots[slot_index];
                if (slot.piece_idx >= 0) {
                    const PieceRotation& piece = all_rotations[slot.piece_idx];
                    std::cout << "P" << std::setfill('0') << std::setw(2) << static_cast<int>(piece.piece_id)
                              << "R" << static_cast<int>(piece.rotation);
                    if (x < width - 1) std::cout << " ";
                }
            }
            std::cout << std::endl;
        }
    }

    // Enable solution storage
    void enable_solution_storage(const std::string& output_file) {
        solution_writer = std::make_unique<SolutionWriter>(output_file);
    }

    // Get solution count
    size_t get_solutions_found() const { return solutions_found; }
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <puzzle_file> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -f, --first         Find first solution only (default: find all)" << std::endl;
    std::cout << "  -o, --output FILE   Save solutions to file" << std::endl;
    std::cout << "  --hints FILE        Apply hints from file (format: x y piece_id rotation)" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program_name << " puzzle.txt -f" << std::endl;
    std::cout << "  " << program_name << " puzzle.txt -o solutions.txt" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Handle help as first argument
    if (strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    std::string puzzle_file = argv[1];
    bool find_first_only = false;
    std::string output_file;
    std::string hints_file;

    // Parse command line arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--first") == 0) {
            find_first_only = true;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--hints") == 0 && i + 1 < argc) {
            hints_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cout << "Error: Unknown option '" << argv[i] << "'" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    SpeedSolver solver;

    // Load puzzle
    if (!solver.load_puzzle(puzzle_file)) {
        return 1;
    }

    // Apply hints if provided
    if (!hints_file.empty()) {
        std::vector<Hint> hints = parse_hints_file(hints_file);
        if (!hints.empty()) {
            if (!solver.apply_hints(hints)) {
                std::cerr << "Error: Failed to apply hints" << std::endl;
                return 1;
            }
        }
    }

    // Enable solution storage if requested
    if (!output_file.empty()) {
        solver.enable_solution_storage(output_file);
    }

    // Solve puzzle
    if (solver.solve(find_first_only)) {
        solver.print_solution();
    } else {
        std::cout << "No solution found." << std::endl;
    }

    return 0;
}