#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <thread>
#include <fstream>
#include <cstdint>

// Maximum puzzle size supported
static constexpr int MAX_PUZZLE_SIZE = 16;
static constexpr int MAX_PIECES = MAX_PUZZLE_SIZE * MAX_PUZZLE_SIZE;

// Compact solution representation - store piece IDs and rotations
struct CompactSolution {
    uint8_t pieces[MAX_PIECES];    // Piece IDs for each position
    uint8_t rotations[MAX_PIECES]; // Rotation for each position (0-3)
    uint16_t width;                // Puzzle width
    uint16_t height;               // Puzzle height
    uint32_t solution_id;          // Unique solution identifier

    CompactSolution() : width(0), height(0), solution_id(0) {
        std::fill(pieces, pieces + MAX_PIECES, 255);  // 255 = empty
        std::fill(rotations, rotations + MAX_PIECES, 255);  // 255 = empty
    }

    CompactSolution(uint16_t w, uint16_t h, uint32_t id)
        : width(w), height(h), solution_id(id) {
        std::fill(pieces, pieces + MAX_PIECES, 255);
        std::fill(rotations, rotations + MAX_PIECES, 255);
    }

    // Get piece ID at position (x, y)
    uint8_t get_piece(int x, int y) const {
        return pieces[y * width + x];
    }

    // Get rotation at position (x, y)
    uint8_t get_rotation(int x, int y) const {
        return rotations[y * width + x];
    }

    // Set piece ID at position (x, y)
    void set_piece(int x, int y, uint8_t piece_id) {
        pieces[y * width + x] = piece_id;
    }

    // Set rotation at position (x, y)
    void set_rotation(int x, int y, uint8_t rotation) {
        rotations[y * width + x] = rotation;
    }

    // Get total number of pieces in this puzzle
    int num_pieces() const {
        return width * height;
    }
};

// High-performance lock-free SPSC (Single Producer Single Consumer) queue
template<size_t CAPACITY>
class SolutionQueue {
private:
    std::array<CompactSolution, CAPACITY> buffer;
    alignas(64) std::atomic<size_t> head{0};    // Producer writes here
    alignas(64) std::atomic<size_t> tail{0};    // Consumer reads here

    static constexpr size_t MASK = CAPACITY - 1;
    static_assert((CAPACITY & MASK) == 0, "Capacity must be power of 2");

public:
    // Producer side: Add solution to queue (non-blocking)
    bool push(const CompactSolution& solution) {
        const size_t current_head = head.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & MASK;

        // Check if queue is full
        if (next_head == tail.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }

        // Copy solution to buffer
        buffer[current_head] = solution;

        // Make solution visible to consumer
        head.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer side: Remove solution from queue (non-blocking)
    bool pop(CompactSolution& solution) {
        const size_t current_tail = tail.load(std::memory_order_relaxed);

        // Check if queue is empty
        if (current_tail == head.load(std::memory_order_acquire)) {
            return false;  // Queue empty
        }

        // Copy solution from buffer
        solution = buffer[current_tail];

        // Advance tail pointer
        const size_t next_tail = (current_tail + 1) & MASK;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // Get approximate queue size (for monitoring)
    size_t size() const {
        const size_t current_head = head.load(std::memory_order_acquire);
        const size_t current_tail = tail.load(std::memory_order_acquire);
        return (current_head - current_tail) & MASK;
    }

    // Check if queue is empty
    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }

    // Get capacity
    constexpr size_t capacity() const {
        return CAPACITY;
    }
};

// Solution writer class for async disk I/O
class SolutionWriter {
private:
    static constexpr size_t QUEUE_SIZE = 4096;  // Must be power of 2
    static constexpr size_t BATCH_SIZE = 100;    // Write solutions in batches

    SolutionQueue<QUEUE_SIZE> queue;
    std::thread writer_thread;
    std::atomic<bool> running{true};
    std::string output_file;
    std::atomic<uint64_t> solutions_written{0};
    std::atomic<uint64_t> solutions_dropped{0};

    // Writer thread function
    void writer_loop() {
        std::ofstream file(output_file, std::ios::binary);
        if (!file) {
            return;  // Failed to open file
        }

        std::vector<CompactSolution> batch;
        batch.reserve(BATCH_SIZE);

        while (running.load(std::memory_order_acquire) || !queue.empty()) {
            CompactSolution solution;

            // Collect solutions into batch
            while (batch.size() < BATCH_SIZE && queue.pop(solution)) {
                batch.push_back(solution);
            }

            // Write batch to disk
            if (!batch.empty()) {
                write_batch(file, batch);
                solutions_written.fetch_add(batch.size(), std::memory_order_relaxed);
                batch.clear();
            }

            // Small sleep to avoid busy waiting
            if (queue.empty()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        file.close();
    }

    // Write a batch of solutions to file
    void write_batch(std::ofstream& file, const std::vector<CompactSolution>& batch) {
        for (const auto& solution : batch) {
            // Write solution header
            file.write(reinterpret_cast<const char*>(&solution.width), sizeof(solution.width));
            file.write(reinterpret_cast<const char*>(&solution.height), sizeof(solution.height));
            file.write(reinterpret_cast<const char*>(&solution.solution_id), sizeof(solution.solution_id));

            // Write piece data and rotation data
            int num_pieces = solution.num_pieces();
            file.write(reinterpret_cast<const char*>(solution.pieces), num_pieces);
            file.write(reinterpret_cast<const char*>(solution.rotations), num_pieces);
        }
        file.flush();  // Ensure data is written
    }

public:
    explicit SolutionWriter(const std::string& filename) : output_file(filename) {
        // Start writer thread
        writer_thread = std::thread(&SolutionWriter::writer_loop, this);
    }

    ~SolutionWriter() {
        stop();
    }

    // Add solution to write queue (called from solver thread)
    bool add_solution(const CompactSolution& solution) {
        if (queue.push(solution)) {
            return true;
        } else {
            // Queue full - drop solution
            solutions_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // Stop writer and wait for all pending writes
    void stop() {
        running.store(false, std::memory_order_release);
        if (writer_thread.joinable()) {
            writer_thread.join();
        }
    }

    // Get statistics
    uint64_t get_solutions_written() const {
        return solutions_written.load(std::memory_order_acquire);
    }

    uint64_t get_solutions_dropped() const {
        return solutions_dropped.load(std::memory_order_acquire);
    }

    size_t get_queue_size() const {
        return queue.size();
    }
};