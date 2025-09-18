// solver.cpp - Main E2 puzzle solver
// Akos-inspired optimized solver with packed data structures
//
// Performance target: Approach 100M placements/second

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
#include "system_info.h"

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

// Packed piece format inspired by Akos assembly: C3:C2:DD:PP (4-byte version)
// Format: [N:8][E:8][S:8][W:8] + used flag separately
struct __attribute__((packed)) PackedPiece {
    union {
        struct {
            uint8_t n, e, s, w;
        } edges;
        uint32_t all_edges;
    };
    uint8_t piece_id;     // Original piece ID (0-based)
    uint8_t rotation;     // Rotation number (0-3)
    uint16_t padding;     // Align to 8 bytes

    inline PackedPiece(uint8_t n, uint8_t e, uint8_t s, uint8_t w, uint8_t pid, uint8_t rot)
        : piece_id(pid), rotation(rot), padding(0) {
        edges.n = n; edges.e = e; edges.s = s; edges.w = w;
    }

    inline PackedPiece() : all_edges(0), piece_id(0), rotation(0), padding(0) {}
} __attribute__((aligned(8)));

// Board slot with direct edge values - optimized for speed
struct BoardSlot {
    union {
        struct {
            uint8_t n, e, s, w;
        } edges;
        uint32_t all_edges;
    };
    int piece_idx;        // Index into all_rotations array (-1 if empty)

    inline BoardSlot() : all_edges(0), piece_idx(-1) {}
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
    uint8_t piece_used[MAX_PIECES];  // Fixed array instead of vector
    PackedPiece all_rotations[MAX_PIECES * 4];  // Fixed array
    int num_rotations;
    BoardSlot board_slots[MAX_PIECES];  // Fixed array

    // Performance statistics
    size_t placements_tried = 0;
    size_t solutions_found = 0;
    bool find_first_only = false;

    // Solution storage
    std::unique_ptr<SolutionWriter> solution_writer;

    // Wavefront placement path - diagonal traversal for cache efficiency
    int placement_path[MAX_PIECES];

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

    SlotInfo slot_info_table[MAX_PIECES];

    // Position-specific piece tables for O(1) constraint lookups
    uint16_t corner_tl[64];
    int corner_tl_count;
    uint16_t corner_tr[MAX_EDGE_TYPES][64];
    int corner_tr_count[MAX_EDGE_TYPES];
    uint16_t corner_bl[MAX_EDGE_TYPES][64];
    int corner_bl_count[MAX_EDGE_TYPES];
    uint16_t corner_br[MAX_EDGE_TYPES][MAX_EDGE_TYPES][64];
    int corner_br_count[MAX_EDGE_TYPES][MAX_EDGE_TYPES];
    uint16_t edge_top[MAX_EDGE_TYPES][256];
    int edge_top_count[MAX_EDGE_TYPES];
    uint16_t edge_bottom[MAX_EDGE_TYPES][MAX_EDGE_TYPES][256];
    int edge_bottom_count[MAX_EDGE_TYPES][MAX_EDGE_TYPES];
    uint16_t edge_left[MAX_EDGE_TYPES][256];
    int edge_left_count[MAX_EDGE_TYPES];
    uint16_t edge_right[MAX_EDGE_TYPES][MAX_EDGE_TYPES][256];
    int edge_right_count[MAX_EDGE_TYPES][MAX_EDGE_TYPES];
    uint16_t interior[MAX_EDGE_TYPES][MAX_EDGE_TYPES][256];
    int interior_count[MAX_EDGE_TYPES][MAX_EDGE_TYPES];

public:
    SpeedSolver() {
        memset(piece_used, 0, sizeof(piece_used));
        memset(corner_tr_count, 0, sizeof(corner_tr_count));
        memset(corner_bl_count, 0, sizeof(corner_bl_count));
        memset(corner_br_count, 0, sizeof(corner_br_count));
        memset(edge_top_count, 0, sizeof(edge_top_count));
        memset(edge_bottom_count, 0, sizeof(edge_bottom_count));
        memset(edge_left_count, 0, sizeof(edge_left_count));
        memset(edge_right_count, 0, sizeof(edge_right_count));
        memset(interior_count, 0, sizeof(interior_count));
        corner_tl_count = 0;
        num_rotations = 0;
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

        if (num_pieces > MAX_PIECES) {
            std::cerr << "Error: Too many pieces " << num_pieces << " > " << MAX_PIECES << std::endl;
            return false;
        }

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
        for (int i = 0; i < num_pieces; i++) {
            board_slots[i] = BoardSlot();
        }

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
                         << " is out of range [1, " << num_pieces << "]" << std::endl;
                return false;
            }

            // Validate rotation
            if (hint.rotation < 0 || hint.rotation > 3) {
                std::cerr << "Error: Hint rotation " << hint.rotation
                         << " is out of range [0, 3]" << std::endl;
                return false;
            }

            // Apply hint: find the corresponding rotation and place it
            int slot_idx = hint.y * width + hint.x;
            int piece_id_0based = hint.piece_id - 1;
            int rotation_idx = piece_id_0based * 4 + hint.rotation;

            if (rotation_idx >= num_rotations) {
                std::cerr << "Error: Invalid rotation index " << rotation_idx << std::endl;
                return false;
            }

            const PackedPiece& piece = all_rotations[rotation_idx];

            // Place the piece
            board_slots[slot_idx].all_edges = piece.all_edges;
            board_slots[slot_idx].piece_idx = rotation_idx;
            piece_used[piece_id_0based] = 1;

            std::cout << "Applied hint: piece " << hint.piece_id << " rotation " << hint.rotation
                      << " at (" << hint.x << ", " << hint.y << ")" << std::endl;
        }

        return true;
    }

    void enable_solution_storage(const std::string& output_file) {
        solution_writer = std::make_unique<SolutionWriter>(output_file);
    }

    void print_solution() const {
        std::cout << "\nSolution:" << std::endl;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int slot_idx = y * width + x;
                const BoardSlot& slot = board_slots[slot_idx];

                if (slot.piece_idx != -1) {
                    const PackedPiece& piece = all_rotations[slot.piece_idx];
                    std::cout << "P" << std::setfill('0') << std::setw(2) << static_cast<int>(piece.piece_id)
                             << "R" << static_cast<int>(piece.rotation);
                } else {
                    std::cout << "EMPTY";
                }

                if (x < width - 1) std::cout << " ";
            }
            std::cout << std::endl;
        }
    }

private:
    // Generate all rotations for all pieces
    void precompute_rotations() {
        num_rotations = 0;

        for (int p = 0; p < num_pieces; p++) {
            const auto& piece = original_pieces[p];
            for (int r = 0; r < 4; r++) {
                uint8_t n = piece[(0 - r + 4) % 4];
                uint8_t e = piece[(1 - r + 4) % 4];
                uint8_t s = piece[(2 - r + 4) % 4];
                uint8_t w = piece[(3 - r + 4) % 4];
                all_rotations[num_rotations] = PackedPiece(n, e, s, w, p, r);
                num_rotations++;
            }
        }
    }

    // Build SlotInfo table for position metadata optimization
    void build_slot_info_table() {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int slot_idx = y * width + x;
                SlotInfo& info = slot_info_table[slot_idx];

                info.x = x;
                info.y = y;
                info.north_neighbor_idx = (y > 0) ? (y - 1) * width + x : -1;
                info.west_neighbor_idx = (x > 0) ? y * width + (x - 1) : -1;

                // Determine slot type
                if (y == 0 && x == 0) {
                    info.type = TOP_LEFT_CORNER;
                } else if (y == 0 && x == width - 1) {
                    info.type = TOP_RIGHT_CORNER;
                } else if (y == height - 1 && x == 0) {
                    info.type = BOTTOM_LEFT_CORNER;
                } else if (y == height - 1 && x == width - 1) {
                    info.type = BOTTOM_RIGHT_CORNER;
                } else if (y == 0) {
                    info.type = TOP_EDGE;
                } else if (y == height - 1) {
                    info.type = BOTTOM_EDGE;
                } else if (x == 0) {
                    info.type = LEFT_EDGE;
                } else if (x == width - 1) {
                    info.type = RIGHT_EDGE;
                } else {
                    info.type = INTERIOR;
                }
            }
        }
    }

    // Generate cache-optimized diagonal wavefront path
    void generate_diagonal_wavefront_path() {
        int path_idx = 0;

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
                placement_path[path_idx++] = slot_idx;
            }
        }

        // Wavefront path generated silently
    }

    // Build constraint-based lookup tables
    double build_position_tables() {
        auto start = std::chrono::high_resolution_clock::now();

        // Clear all tables
        corner_tl_count = 0;
        memset(corner_tr_count, 0, sizeof(corner_tr_count));
        memset(corner_bl_count, 0, sizeof(corner_bl_count));
        memset(corner_br_count, 0, sizeof(corner_br_count));
        memset(edge_top_count, 0, sizeof(edge_top_count));
        memset(edge_bottom_count, 0, sizeof(edge_bottom_count));
        memset(edge_left_count, 0, sizeof(edge_left_count));
        memset(edge_right_count, 0, sizeof(edge_right_count));
        memset(interior_count, 0, sizeof(interior_count));

        // Populate tables based on piece constraints
        for (uint16_t idx = 0; idx < num_rotations; idx++) {
            const PackedPiece& piece = all_rotations[idx];
            uint8_t n = piece.edges.n, e = piece.edges.e, s = piece.edges.s, w = piece.edges.w;

            // Top-left corner: needs border edges (n=0, w=0)
            if (n == 0 && w == 0) {
                corner_tl[corner_tl_count++] = idx;
            }

            // Top-right corner: needs border edges (n=0, e=0)
            if (n == 0 && e == 0) {
                corner_tr[w][corner_tr_count[w]++] = idx;
            }

            // Bottom-left corner: needs border edges (s=0, w=0)
            if (s == 0 && w == 0) {
                corner_bl[n][corner_bl_count[n]++] = idx;
            }

            // Bottom-right corner: needs border edges (s=0, e=0)
            if (s == 0 && e == 0) {
                corner_br[w][n][corner_br_count[w][n]++] = idx;
            }

            // Top edge: needs border north (n=0)
            if (n == 0) {
                edge_top[w][edge_top_count[w]++] = idx;
            }

            // Bottom edge: needs border south (s=0)
            if (s == 0) {
                edge_bottom[w][n][edge_bottom_count[w][n]++] = idx;
            }

            // Left edge: needs border west (w=0)
            if (w == 0) {
                edge_left[n][edge_left_count[n]++] = idx;
            }

            // Right edge: needs border east (e=0)
            if (e == 0) {
                edge_right[w][n][edge_right_count[w][n]++] = idx;
            }

            // Interior: no border constraints
            interior[w][n][interior_count[w][n]++] = idx;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

        // Tables built silently

        return elapsed;
    }

    void capture_solution() {
        if (!solution_writer) return;

        CompactSolution solution(width, height, solutions_found);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int slot_idx = y * width + x;
                const BoardSlot& slot = board_slots[slot_idx];

                if (slot.piece_idx != -1) {
                    const PackedPiece& piece = all_rotations[slot.piece_idx];
                    solution.pieces[slot_idx] = piece.piece_id + 1;  // Convert to 1-based
                    solution.rotations[slot_idx] = piece.rotation;
                }
            }
        }

        solution_writer->add_solution(solution);
    }

    // Wavefront solver: O(1) lookups + cache-friendly diagonal traversal
    __attribute__((hot, flatten))
    bool solve_recursive_wavefront(int step) {
        if (__builtin_expect(step >= num_pieces, 0)) {
            solutions_found++;
            capture_solution();
            return find_first_only;
        }

        // Get slot index from wavefront path - diagonal traversal for cache efficiency
        int slot_idx = placement_path[step];

        // Single table lookup instead of div/mod operations!
        const SlotInfo& info = slot_info_table[slot_idx];
        const uint16_t* candidates;
        int candidate_count;

        // SAME O(1) constraint-based table lookup as optimized solver
        switch (info.type) {
            case TOP_LEFT_CORNER:
                candidates = corner_tl;
                candidate_count = corner_tl_count;
                break;

            case TOP_RIGHT_CORNER: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges.e;
                candidates = corner_tr[west_constraint];
                candidate_count = corner_tr_count[west_constraint];
                break;
            }

            case BOTTOM_LEFT_CORNER: {
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges.s;
                candidates = corner_bl[north_constraint];
                candidate_count = corner_bl_count[north_constraint];
                break;
            }

            case BOTTOM_RIGHT_CORNER: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges.e;
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges.s;
                candidates = corner_br[west_constraint][north_constraint];
                candidate_count = corner_br_count[west_constraint][north_constraint];
                break;
            }

            case TOP_EDGE: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges.e;
                candidates = edge_top[west_constraint];
                candidate_count = edge_top_count[west_constraint];
                break;
            }

            case BOTTOM_EDGE: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges.e;
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges.s;
                candidates = edge_bottom[west_constraint][north_constraint];
                candidate_count = edge_bottom_count[west_constraint][north_constraint];
                break;
            }

            case LEFT_EDGE: {
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges.s;
                candidates = edge_left[north_constraint];
                candidate_count = edge_left_count[north_constraint];
                break;
            }

            case RIGHT_EDGE: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges.e;
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges.s;
                candidates = edge_right[west_constraint][north_constraint];
                candidate_count = edge_right_count[west_constraint][north_constraint];
                break;
            }

            case INTERIOR: {
                uint8_t west_constraint = board_slots[info.west_neighbor_idx].edges.e;
                uint8_t north_constraint = board_slots[info.north_neighbor_idx].edges.s;
                candidates = interior[west_constraint][north_constraint];
                candidate_count = interior_count[west_constraint][north_constraint];
                break;
            }
        }

        // Critical iteration loop - cache-optimized with indices
        BoardSlot& __restrict__ slot = board_slots[slot_idx];

        for (int i = 0; i < candidate_count; i++) {
            uint16_t piece_idx = candidates[i];
            const PackedPiece& __restrict__ piece = all_rotations[piece_idx];
            if (__builtin_expect(!piece_used[piece.piece_id], 1)) {
                ++placements_tried;

                // Ultra-fast 4-byte copy using uint32_t
                slot.all_edges = piece.all_edges;
                slot.piece_idx = piece_idx;
                piece_used[piece.piece_id] = 1;

                // Recurse to next step in wavefront path
                if (__builtin_expect(solve_recursive_wavefront(step + 1), 0)) return true;

                // Fast backtrack
                slot.all_edges = 0;
                slot.piece_idx = -1;
                piece_used[piece.piece_id] = 0;
            }
        }
        return false;
    }

    // Main solve entry point
    bool solve(bool first_only = false) {
        find_first_only = first_only;
        placements_tried = 0;
        solutions_found = 0;

        // Print system and puzzle information
        SystemInfo system_info;
        system_info.print_system_header();
        std::cout << "Puzzle: " << width << "x" << height << " (" << num_pieces << " positions)" << std::endl;
        std::cout << "Solver: High-performance (Akos-inspired)" << std::endl;

        double table_time = build_position_tables();

        auto solve_start = std::chrono::high_resolution_clock::now();
        bool result = solve_recursive_wavefront(0);
        auto solve_end = std::chrono::high_resolution_clock::now();

        double solve_time = std::chrono::duration<double, std::milli>(solve_end - solve_start).count();
        double total_time = table_time + solve_time;

        std::cout << "\n=== Performance Report ===" << std::endl;
        std::cout << "Solutions found: " << solutions_found << std::endl;
        std::cout << "Placements tried: " << placements_tried << std::endl;
        std::cout << "Table build time: " << std::fixed << std::setprecision(3) << table_time << " ms" << std::endl;
        std::cout << "Pure solve time: " << std::setprecision(3) << solve_time << " ms" << std::endl;
        std::cout << "Total time: " << std::setprecision(3) << total_time << " ms" << std::endl;

        if (solve_time > 0 && placements_tried > 0) {
            double placements_per_second = placements_tried / (solve_time / 1000.0);
            double overall_placements_per_second = placements_tried / (total_time / 1000.0);

            // Use SI units for better readability
            std::cout << "Pure solve performance: " << std::fixed << std::setprecision(1) << (placements_per_second / 1000000.0) << " M placements/s" << std::endl;
            std::cout << "Overall performance: " << std::setprecision(1) << (overall_placements_per_second / 1000000.0) << " M placements/s" << std::endl;
            std::cout << "Overhead: " << std::setprecision(3) << table_time << " ms (" << std::setprecision(2) << (table_time / total_time * 100) << "%)" << std::endl;
        }

        return result;
    }

    friend bool solve_puzzle(const std::string& puzzle_file, const std::string& hints_file, const std::string& output_file, bool find_first_only);
};

bool solve_puzzle(const std::string& puzzle_file, const std::string& hints_file, const std::string& output_file, bool find_first_only) {
    SpeedSolver solver;

    // Load puzzle
    if (!solver.load_puzzle(puzzle_file)) {
        return false;
    }

    // Apply hints if provided
    if (!hints_file.empty()) {
        auto hints = parse_hints_file(hints_file);
        if (!hints.empty()) {
            if (!solver.apply_hints(hints)) {
                return false;
            }
        }
    }

    // Enable solution storage if output file provided
    if (!output_file.empty()) {
        solver.enable_solution_storage(output_file);
    }

    // Solve puzzle
    if (solver.solve(find_first_only)) {
        solver.print_solution();
        return true;
    } else {
        std::cout << "No solution found." << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <puzzle_file> [hints_file] [-o output_file] [-f]" << std::endl;
        std::cerr << "  -f: Find first solution only" << std::endl;
        return 1;
    }

    std::string puzzle_file = argv[1];
    std::string hints_file;
    std::string output_file;
    bool find_first_only = false;

    // Parse command line arguments
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "-f") {
            find_first_only = true;
        } else if (std::string(argv[i]) == "-o" && i + 1 < argc) {
            output_file = argv[i + 1];
            i++;
        } else if (hints_file.empty() && argv[i][0] != '-') {
            hints_file = argv[i];
        }
    }

    return solve_puzzle(puzzle_file, hints_file, output_file, find_first_only) ? 0 : 1;
}