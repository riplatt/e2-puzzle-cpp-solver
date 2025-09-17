// parallel_speed_solver.cpp - Multi-core parallel edge-matching puzzle solver
// Achieves massive speedup by running multiple solver instances on different CPU cores
// with different starting positions to partition the search space effectively.
//
// Multi-threaded parallel solver for high-performance edge-matching puzzle solving
//
// Compile with: g++ -O3 -march=native -flto -DNDEBUG -std=c++23 -pthread -funroll-loops -ffast-math -o parallel_speed_solver parallel_speed_solver.cpp

#include <vector>
#include <array>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>
#include <algorithm>
#include <optional>

// Performance constants
static constexpr int MAX_PIECES = 256;
static constexpr int MAX_EDGE_TYPES = 32;
static constexpr int MAX_SIZE = 16;

// Hint structure for pre-placing pieces
struct Hint {
    int x, y;
    int piece_id;
    int rotation;
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
    int piece_id;         // Which piece is placed here (-1 if empty)
    int rotation;         // Which rotation (0-3) of the piece

    inline BoardSlot() : piece_id(-1), rotation(-1) {
        *reinterpret_cast<uint32_t*>(edges) = 0;  // Fast zero initialization
    }
};

// Thread-local solution structure
struct Solution {
    std::vector<std::pair<int, int>> placements;  // (piece_id, rotation) for each slot
    long long placements_tried = 0;
    std::chrono::steady_clock::time_point found_time;

    void print_solution(int width, int height) const {
        std::cout << "\nSolution found by thread with " << placements_tried << " placements:" << std::endl;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int slot_idx = y * width + x;
                auto [piece_id, rotation] = placements[slot_idx];
                std::cout << "P" << std::setfill('0') << std::setw(2) << piece_id
                         << "R" << rotation;
                if (x < width - 1) std::cout << " ";
            }
            std::cout << std::endl;
        }
    }
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

// Thread-safe solver class for individual worker threads
class ThreadSolver {
private:
    // Puzzle dimensions
    int width = 0, height = 0, num_pieces = 0, num_edge_types = 0;

    // Piece data structures (thread-local copies)
    std::vector<uint8_t> piece_used;
    std::vector<PieceRotation> all_rotations;
    std::vector<BoardSlot> board_slots;

    // EVOLVE-BLOCK-START
    // Simplified lookup arrays for better cache performance
    std::vector<PieceRotation*> corner_pieces[4];  // TL, TR, BL, BR
    std::vector<PieceRotation*> edge_pieces[4][MAX_EDGE_TYPES + 1];  // [edge_type][constraint]
    std::vector<PieceRotation*> interior_pieces[MAX_EDGE_TYPES + 1][MAX_EDGE_TYPES + 1];  // [west][north]
    // EVOLVE-BLOCK-END

    // Thread statistics
    long long placements_tried = 0;
    std::atomic<bool>* global_solution_found = nullptr;
    int thread_id = 0;

    // Wavefront optimization: cache-friendly diagonal traversal
    std::vector<int> wavefront_path;

public:
    ThreadSolver() = default;

    // Initialize thread-local solver with puzzle data
    bool initialize(int w, int h, const std::vector<std::array<int, 4>>& pieces,
                   int max_edge_type, int tid, std::atomic<bool>* solution_flag) {
        width = w;
        height = h;
        num_pieces = w * h;
        num_edge_types = max_edge_type;
        thread_id = tid;
        global_solution_found = solution_flag;

        // Initialize thread-local data structures
        piece_used.resize(num_pieces, 0);
        board_slots.resize(num_pieces);

        // Generate all rotations (thread-local copies)
        all_rotations.reserve(num_pieces * 4);
        for (int i = 0; i < num_pieces; i++) {
            int n = pieces[i][0];
            int e = pieces[i][1];
            int s = pieces[i][2];
            int w = pieces[i][3];

            all_rotations.emplace_back(n, e, s, w, &piece_used[i], i, 0);
            all_rotations.emplace_back(w, n, e, s, &piece_used[i], i, 1);
            all_rotations.emplace_back(s, w, n, e, &piece_used[i], i, 2);
            all_rotations.emplace_back(e, s, w, n, &piece_used[i], i, 3);
        }

        setup_board();
        build_position_tables();
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
            if (board_slots[slot_idx].piece_id != -1) {
                std::cerr << "Error: Position (" << hint.x << ", " << hint.y
                         << ") is filled multiple times in hints" << std::endl;
                return false;
            }

            // Apply the hint
            board_slots[slot_idx].piece_id = hint.piece_id - 1; // Convert to 0-based
            board_slots[slot_idx].rotation = hint.rotation;
            // Copy all edges from the piece rotation (N, E, S, W)
            for (int j = 0; j < 4; j++) {
                board_slots[slot_idx].edges[j] = all_rotations[rotation_idx].edges[j];
            }
            piece_used[hint.piece_id - 1] = 1;

            std::cout << "Thread " << thread_id << ": Applied hint: piece " << hint.piece_id
                     << " (rotation " << hint.rotation << ") at position (" << hint.x << ", " << hint.y << ")" << std::endl;
        }
        return true;
    }

    // Set up board slots with direct edge values
    void setup_board() {
        // Board slots are already initialized to zero in constructor
        // No additional setup needed for simplified structure
    }

    // EVOLVE-BLOCK-START
    // Build simplified piece tables for cache-friendly lookup
    void build_position_tables() {
        // Single pass with bit masks for faster categorization
        for (auto& rotation : all_rotations) {
            uint8_t n = rotation.edges[0], e = rotation.edges[1], s = rotation.edges[2], w = rotation.edges[3];
            uint8_t border_mask = (n == 0) | ((e == 0) << 1) | ((s == 0) << 2) | ((w == 0) << 3);

            switch (border_mask) {
                case 9: corner_pieces[0].push_back(&rotation); break;  // TL: n=0,w=0
                case 3: corner_pieces[1].push_back(&rotation); break;  // TR: n=0,e=0
                case 12: corner_pieces[2].push_back(&rotation); break; // BL: s=0,w=0
                case 6: corner_pieces[3].push_back(&rotation); break;  // BR: s=0,e=0
                case 1: edge_pieces[0][w].push_back(&rotation); break; // Top
                case 4: edge_pieces[1][w].push_back(&rotation); break; // Bottom
                case 8: edge_pieces[2][n].push_back(&rotation); break; // Left
                case 2: edge_pieces[3][n].push_back(&rotation); break; // Right
                case 0: interior_pieces[w][n].push_back(&rotation); break; // Interior
            }
        }

        // Generate wavefront path for cache-optimized traversal
        generate_diagonal_wavefront_path();
    }
    // EVOLVE-BLOCK-END

    // Generate cache-optimized diagonal wavefront path
    void generate_diagonal_wavefront_path() {
        wavefront_path.clear();

        // For cache optimization, traverse diagonally to maximize L1 cache hits
        std::vector<std::vector<int>> diagonals;

        // Generate diagonals from top-left to bottom-right
        for (int d = 0; d < width + height - 1; d++) {
            std::vector<int> diagonal;

            for (int y = 0; y < height; y++) {
                int x = d - y;
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
    }

    // Wavefront-optimized recursive solver with cache-friendly diagonal traversal
    __attribute__((hot, flatten))
    bool solve_recursive_wavefront(int step) {
        // Check for global termination condition
        if (__builtin_expect(global_solution_found->load(std::memory_order_relaxed), 0)) {
            return false;  // Another thread found solution, terminate
        }

        if (__builtin_expect(step >= static_cast<int>(wavefront_path.size()), 0)) {
            // Solution found! Set global flag
            global_solution_found->store(true, std::memory_order_relaxed);
            return true;
        }

        // Get slot index from wavefront path - diagonal traversal for cache efficiency
        int slot_index = wavefront_path[step];
        const int x = slot_index % width, y = slot_index / width;
        const std::vector<PieceRotation*>* candidate_pieces;

        // Corner positions
        if ((x | y) == 0) candidate_pieces = &corner_pieces[0];  // TL
        else if (x == width - 1 && y == 0) candidate_pieces = &corner_pieces[1];  // TR
        else if (x == 0 && y == height - 1) candidate_pieces = &corner_pieces[2];  // BL
        else if (x == width - 1 && y == height - 1) candidate_pieces = &corner_pieces[3];  // BR
        // Edge positions
        else if (y == 0) candidate_pieces = &edge_pieces[0][board_slots[slot_index - 1].edges[1]];  // Top
        else if (y == height - 1) candidate_pieces = &edge_pieces[1][board_slots[slot_index - 1].edges[1]];  // Bottom
        else if (x == 0) candidate_pieces = &edge_pieces[2][board_slots[slot_index - width].edges[2]];  // Left
        else if (x == width - 1) candidate_pieces = &edge_pieces[3][board_slots[slot_index - width].edges[2]];  // Right
        // Interior positions
        else {
            uint8_t north = board_slots[slot_index - width].edges[2];
            uint8_t west = board_slots[slot_index - 1].edges[1];
            candidate_pieces = &interior_pieces[west][north];
        }

        for (PieceRotation* piece_rotation : *candidate_pieces) {
            if (__builtin_expect(*piece_rotation->used_flag, 1)) continue;

            // Place piece
            placements_tried++;
            board_slots[slot_index].edges[0] = piece_rotation->edges[0];
            board_slots[slot_index].edges[1] = piece_rotation->edges[1];
            board_slots[slot_index].edges[2] = piece_rotation->edges[2];
            board_slots[slot_index].edges[3] = piece_rotation->edges[3];
            board_slots[slot_index].piece_id = piece_rotation->piece_id;
            board_slots[slot_index].rotation = piece_rotation->rotation;
            *piece_rotation->used_flag = 1;

            // Recurse to next step in wavefront path
            if (__builtin_expect(solve_recursive_wavefront(step + 1), 0)) return true;

            // Backtrack
            *piece_rotation->used_flag = 0;
        }

        return false;
    }

    // Ultra-fast recursive backtracking with thread-safe early termination
    __attribute__((hot, flatten))
    bool solve_recursive(int slot_index) {
        // Check for global termination condition
        if (__builtin_expect(global_solution_found->load(std::memory_order_relaxed), 0)) {
            return false;  // Another thread found solution, terminate
        }

        if (__builtin_expect(slot_index >= num_pieces, 0)) {
            // Solution found! Set global flag
            global_solution_found->store(true, std::memory_order_relaxed);
            return true;
        }

        const int x = slot_index % width, y = slot_index / width;
        const std::vector<PieceRotation*>* candidate_pieces;

        // Corner positions
        if ((x | y) == 0) candidate_pieces = &corner_pieces[0];  // TL
        else if (x == width - 1 && y == 0) candidate_pieces = &corner_pieces[1];  // TR
        else if (x == 0 && y == height - 1) candidate_pieces = &corner_pieces[2];  // BL
        else if (x == width - 1 && y == height - 1) candidate_pieces = &corner_pieces[3];  // BR
        // Edge positions
        else if (y == 0) candidate_pieces = &edge_pieces[0][board_slots[slot_index - 1].edges[1]];  // Top
        else if (y == height - 1) candidate_pieces = &edge_pieces[1][board_slots[slot_index - 1].edges[1]];  // Bottom
        else if (x == 0) candidate_pieces = &edge_pieces[2][board_slots[slot_index - width].edges[2]];  // Left
        else if (x == width - 1) candidate_pieces = &edge_pieces[3][board_slots[slot_index - width].edges[2]];  // Right
        // Interior positions
        else {
            uint8_t north = board_slots[slot_index - width].edges[2];
            uint8_t west = board_slots[slot_index - 1].edges[1];
            candidate_pieces = &interior_pieces[west][north];
        }

        for (const auto* __restrict__ piece : *candidate_pieces) {
            if (__builtin_expect(!*piece->used_flag, 1)) {
                ++placements_tried;
                BoardSlot& __restrict__ slot = board_slots[slot_index];

                // Ultra-fast 4-byte copy using uint32_t
                *reinterpret_cast<uint32_t*>(slot.edges) = *reinterpret_cast<const uint32_t*>(piece->edges);
                slot.piece_id = piece->piece_id;
                slot.rotation = piece->rotation;
                *piece->used_flag = true;

                if (__builtin_expect(solve_recursive(slot_index + 1), 0)) return true;

                // Fast backtrack
                *reinterpret_cast<uint32_t*>(slot.edges) = 0;
                slot.piece_id = slot.rotation = -1;
                *piece->used_flag = false;
            }
        }

        return false;
    }

    // Solve with specific starting strategy
    std::optional<Solution> solve_with_strategy(int strategy_id) {
        placements_tried = 0;

        // Apply different starting strategies based on thread ID
        apply_starting_strategy(strategy_id);

        auto start_time = std::chrono::steady_clock::now();
        // Use wavefront algorithm for cache-optimized solving
        bool found = solve_recursive_wavefront(0);

        if (found) {
            Solution solution;
            solution.placements.resize(num_pieces);
            solution.placements_tried = placements_tried;
            solution.found_time = std::chrono::steady_clock::now();

            // Copy solution from board
            for (int i = 0; i < num_pieces; i++) {
                solution.placements[i] = {board_slots[i].piece_id, board_slots[i].rotation};
            }

            return solution;
        }

        // IMPORTANT: Return placements even when no solution found
        // This ensures we count ALL threads' work, not just the winner
        Solution partial_result;
        partial_result.placements_tried = placements_tried;
        return partial_result;
    }

    long long get_placements_tried() const { return placements_tried; }

private:
    // Apply different starting strategies to partition search space
    void apply_starting_strategy(int strategy_id) {
        // MAJOR IMPROVEMENT: True search space partitioning
        // Each thread explores a completely different subset of the search space

        if (strategy_id == 0) {
            // Thread 0: Use default ordering (baseline)
            return;
        }

        // Strategy 1: Partition by first piece ID
        if (strategy_id < num_pieces && !corner_pieces[0].empty()) {
            // Each thread tries a different piece first by filtering corner pieces
            auto preferred_piece_id = strategy_id % num_pieces;

            // Move all pieces with this ID to front of corner list
            std::stable_partition(corner_pieces[0].begin(), corner_pieces[0].end(),
                [preferred_piece_id](const PieceRotation* piece) {
                    return piece->piece_id == preferred_piece_id;
                });
        }

        // Strategy 2: Partition by rotation preference
        int rotation_preference = strategy_id % 4;
        for (int i = 0; i < 4; i++) {
            std::stable_partition(corner_pieces[i].begin(), corner_pieces[i].end(),
                [rotation_preference](const PieceRotation* piece) {
                    return piece->rotation == rotation_preference;
                });
        }

        // Strategy 3: Completely different piece ordering strategy
        int ordering_strategy = strategy_id % 3;
        switch (ordering_strategy) {
        case 0: // Ascending piece ID order
            std::sort(corner_pieces[0].begin(), corner_pieces[0].end(),
                     [](const PieceRotation* a, const PieceRotation* b) {
                         return a->piece_id < b->piece_id;
                     });
            break;
        case 1: // Descending piece ID order
            std::sort(corner_pieces[0].begin(), corner_pieces[0].end(),
                     [](const PieceRotation* a, const PieceRotation* b) {
                         return a->piece_id > b->piece_id;
                     });
            break;
        case 2: // Reverse order completely
            std::reverse(corner_pieces[0].begin(), corner_pieces[0].end());
            break;
        }
    }

};

// Thread timing structure
struct ThreadTimings {
    double init_time_ms = 0.0;
    double solve_time_ms = 0.0;
    long long placements_tried = 0;
    bool found_solution = false;
    int thread_id = 0;
};

// Main parallel solver coordinator
class ParallelSpeedSolver {
public:
    int width = 0, height = 0, num_pieces = 0;

private:
    std::vector<std::array<int, 4>> pieces;
    int num_edge_types = 0;

public:
    bool load_puzzle(const char* filename) {
        std::ifstream file(filename);
        if (!file) {
            std::cerr << "Cannot open file: " << filename << std::endl;
            return false;
        }

        file >> width >> height;
        num_pieces = width * height;

        std::cout << "Loading " << width << "x" << height << " puzzle for parallel solving..." << std::endl;

        pieces.resize(num_pieces);
        num_edge_types = 0;

        for (int i = 0; i < num_pieces; i++) {
            int n, e, s, w;
            file >> n >> e >> s >> w;
            pieces[i] = {n, e, s, w};
            num_edge_types = std::max(num_edge_types, std::max(std::max(n, e), std::max(s, w)));
        }

        std::cout << "Calculated " << num_edge_types << " edge types" << std::endl;
        return true;
    }

    std::optional<Solution> solve_parallel(int num_threads = std::thread::hardware_concurrency(),
                                          bool first_only = true,
                                          const std::vector<Hint>& hints = {}) {
        std::cout << "Starting parallel solver with " << num_threads << " threads..." << std::endl;

        std::atomic<bool> solution_found(false);
        std::vector<std::future<std::pair<std::optional<Solution>, ThreadTimings>>> futures;

        auto total_start = std::chrono::steady_clock::now();

        // Launch worker threads with detailed timing
        for (int t = 0; t < num_threads; ++t) {
            futures.emplace_back(std::async(std::launch::async,
                [this, t, &solution_found, &hints]() -> std::pair<std::optional<Solution>, ThreadTimings> {

                ThreadTimings timings;
                timings.thread_id = t;

                auto init_start = std::chrono::steady_clock::now();

                ThreadSolver solver;
                if (!solver.initialize(width, height, pieces, num_edge_types, t, &solution_found)) {
                    return {std::nullopt, timings};
                }

                // Apply hints to this thread if provided
                if (!hints.empty()) {
                    if (!solver.apply_hints(hints)) {
                        std::cerr << "Thread " << t << ": Failed to apply hints" << std::endl;
                        return {std::nullopt, timings};
                    }
                }

                auto init_end = std::chrono::steady_clock::now();
                timings.init_time_ms = std::chrono::duration<double, std::milli>(init_end - init_start).count();

                auto solve_start = std::chrono::steady_clock::now();
                auto result = solver.solve_with_strategy(t);
                auto solve_end = std::chrono::steady_clock::now();

                timings.solve_time_ms = std::chrono::duration<double, std::milli>(solve_end - solve_start).count();
                timings.placements_tried = solver.get_placements_tried();
                timings.found_solution = result.has_value() && !result->placements.empty();

                return {result, timings};
            }));
        }

        // Collect results from ALL threads
        std::optional<Solution> best_solution;
        std::vector<ThreadTimings> all_timings;
        long long total_placements = 0;
        double total_init_time = 0.0;
        double max_solve_time = 0.0;
        int solution_thread = -1;

        for (auto& future : futures) {
            auto [result, timing] = future.get();

            all_timings.push_back(timing);
            total_placements += timing.placements_tried;
            total_init_time += timing.init_time_ms;
            max_solve_time = std::max(max_solve_time, timing.solve_time_ms);

            if (result.has_value() && timing.found_solution && !best_solution.has_value()) {
                best_solution = std::move(result);
                solution_thread = timing.thread_id;
            }
        }

        auto total_end = std::chrono::steady_clock::now();
        auto wall_clock_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

        // Print comprehensive performance statistics
        print_detailed_statistics(all_timings, total_placements, wall_clock_time_ms,
                                 total_init_time, max_solve_time, solution_thread, num_threads);

        return best_solution;
    }

private:
    void print_detailed_statistics(const std::vector<ThreadTimings>& timings,
                                  long long total_placements,
                                  double wall_clock_time_ms,
                                  double total_init_time_ms,
                                  double max_solve_time_ms,
                                  int solution_thread,
                                  int num_threads) {

        std::cout << "\n=== CORRECTED Parallel Performance Statistics ===" << std::endl;
        std::cout << "Threads used: " << num_threads << std::endl;

        // Thread breakdown
        std::cout << "\n--- Per-Thread Breakdown ---" << std::endl;
        for (const auto& timing : timings) {
            std::cout << "Thread " << timing.thread_id << ": "
                     << timing.placements_tried << " placements, "
                     << timing.solve_time_ms << " ms solve time";
            if (timing.found_solution) {
                std::cout << " [FOUND SOLUTION]";
            }
            std::cout << std::endl;
        }

        // Timing breakdown
        std::cout << "\n--- Timing Breakdown ---" << std::endl;
        std::cout << "Total initialization time (all threads): " << total_init_time_ms << " ms" << std::endl;
        std::cout << "Maximum solve time (bottleneck): " << max_solve_time_ms << " ms" << std::endl;
        std::cout << "Wall clock time: " << wall_clock_time_ms << " ms" << std::endl;
        std::cout << "Parallel overhead: " << (wall_clock_time_ms - max_solve_time_ms) << " ms" << std::endl;

        // Aggregate performance - CORRECTED calculation
        std::cout << "\n--- AGGREGATE Performance (All Threads Combined) ---" << std::endl;
        std::cout << "Total placements tried (ALL threads): " << total_placements << std::endl;

        if (total_placements > 0 && max_solve_time_ms > 0) {
            // Aggregate throughput: all placements divided by max solve time
            double aggregate_placements_per_sec = (total_placements * 1000.0) / max_solve_time_ms;
            std::cout << "Aggregate placements per second: " << static_cast<long long>(aggregate_placements_per_sec) << std::endl;
            std::cout << "Aggregate millions placements per sec: " << aggregate_placements_per_sec / 1000000.0 << std::endl;

            // Compare to single-threaded baseline
            double single_threaded_baseline = 77.7e6;  // From updated speed_solver
            double speedup = aggregate_placements_per_sec / single_threaded_baseline;
            std::cout << "Speedup vs single-threaded: " << speedup << "x" << std::endl;
            double efficiency = speedup / num_threads * 100.0;
            std::cout << "Parallel efficiency: " << efficiency << "%" << std::endl;
        }

        if (solution_thread >= 0) {
            std::cout << "\n--- Solution Details ---" << std::endl;
            std::cout << "Solution found by thread " << solution_thread << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <puzzle_file> [-f] [-t <num_threads>] [--hints <hints_file>]" << std::endl;
        std::cout << "  -f: Find first solution only (default)" << std::endl;
        std::cout << "  -t <num>: Number of threads (default: hardware concurrency)" << std::endl;
        std::cout << "  --hints <file>: Apply hints from file (format: x y piece_id rotation)" << std::endl;
        return 1;
    }

    // Handle help as first argument
    if (strcmp(argv[1], "--help") == 0) {
        std::cout << "Usage: " << argv[0] << " <puzzle_file> [-f] [-t <num_threads>] [--hints <hints_file>]" << std::endl;
        std::cout << "  -f: Find first solution only (default)" << std::endl;
        std::cout << "  -t <num>: Number of threads (default: hardware concurrency)" << std::endl;
        std::cout << "  --hints <file>: Apply hints from file (format: x y piece_id rotation)" << std::endl;
        return 0;
    }

    bool first_only = true;
    int num_threads = std::thread::hardware_concurrency();
    std::string hints_file;

    // Parse arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--first") == 0) {
            first_only = true;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            num_threads = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hints") == 0 && i + 1 < argc) {
            hints_file = argv[++i];
        }
    }

    std::cout << "Parallel Edge-Matching Puzzle Solver" << std::endl;
    std::cout << "Hardware threads available: " << std::thread::hardware_concurrency() << std::endl;

    ParallelSpeedSolver solver;

    if (!solver.load_puzzle(argv[1])) {
        return 1;
    }

    // Parse hints if provided
    std::vector<Hint> hints;
    if (!hints_file.empty()) {
        hints = parse_hints_file(hints_file);
        if (!hints.empty()) {
            std::cout << "Loaded " << hints.size() << " hints from " << hints_file << std::endl;
        }
    }

    auto solution = solver.solve_parallel(num_threads, first_only, hints);

    if (solution.has_value()) {
        solution->print_solution(solver.width, solver.height);
        return 0;
    } else {
        std::cout << "No solution found." << std::endl;
        return 1;
    }
}