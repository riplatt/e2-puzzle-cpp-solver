# Eternity 2 Puzzle Solver

High-performance C++23 solver for Eternity 2 and similar edge-matching puzzles with parallel processing support

A comprehensive collection of optimized solvers featuring wavefront algorithms, template metaprogramming, and parallel processing.

## 🚀 Quick Start

### Single-Thread Solver:
```bash
# Build and run wavefront solver (89.9M placements/sec)
mkdir build && cd build
cmake .. && make
./solver ../data/e2pieces_hard_7x7.txt -f
```

### Multi-Thread Solver:
```bash
# Run parallel wavefront solver (529.7M placements/sec)
./parallel_solver ../data/e2pieces_hard_7x7.txt -f -t 16
```

## 📁 Project Structure

- **`src/`** - Source code
  - `solver.cpp` - Single-threaded solver (89.9M/sec) + hints support
  - `parallel_solver.cpp` - Multi-threaded solver (529.7M/sec) + hints support
  - `solution_storage.h` - Supporting header for solution output

- **`data/`** - **Test puzzles** (4x4 to 8x8)
- **`build/`** - **Compiled executables**
- **`archive/`** - **Development history and experiments**
  - CSP research framework (16 different algorithms)
  - Previous solver iterations and experimental optimizations

## 🔧 Build Instructions

```bash
# Standard CMake build (recommended)
mkdir build && cd build
cmake ..
make

# All executables built in build/ directory:
# - solver (production single-thread)
# - parallel_solver (production multi-thread)
```

### Build Requirements
- **C++23 compiler** (GCC 11+ or Clang 14+)
- **CMake 3.20+**
- **Multi-core CPU** recommended for parallel solver

## 📊 Performance Results (7x7 puzzle)

*Benchmarked on Intel i9-9900KF @ 3.60GHz (single core)*

| **Solver** | **Performance** | **Speedup** | **Use Case** |
|-----------|----------------|------------|-------------|
| **parallel_solver** | **529.7M/sec** | **5.8x** | Multi-thread |
| **solver** | **89.9M/sec** | **1.8x** | Single-thread |

## 🧠 Technical Achievements

### **Micro-Optimization Breakthroughs:**
- **Cache Analysis**: Only 27.5% L1 utilization for 16×16 puzzles - cache not the bottleneck
- **Wavefront Algorithm**: Diagonal traversal provides 87% performance improvement
- **Template Metaprogramming**: Static recursion achieves 83% improvement
- **Parallel Scaling**: 41.6% efficiency with 16 threads vs 10% in basic implementation

### **Algorithm Insights:**
- **Access pattern > Data compression** at practical puzzle sizes
- **Simple algorithms often outperform complex ones** - overhead matters
- **L1 cache pressure**: Would only occur at 64×64+ puzzle sizes
- **Bit-packing unnecessary**: 23-bit representation adds complexity without benefit

## 🎯 Production Recommendations

### **For Single Solutions:**
- **Use `solver`** - 89.9M placements/sec, universal compatibility

### **For Exhaustive Search:**
- **Use `parallel_solver`** - 529.7M placements/sec aggregate performance

### **For Research:**
- **Archive contains experimental optimizations** and CSP algorithms for academic comparison

## 🎮 Usage Examples

```bash
# Single-thread solver (production)
./solver ../data/e2pieces_hard_6x6.txt -f

# Multi-thread parallel solver (maximum performance)
./parallel_solver ../data/e2pieces_hard_7x7.txt -f -t 16

# Using hints for search space partitioning
./solver ../data/e2pieces_hard_4x4.txt --hints ../data/hints_4x4.txt -f
./parallel_solver ../data/e2pieces_hard_7x7.txt --hints ../data/hints_7x7.txt -f -t 16
```

### Command Line Options:

**solver (single-thread):**
- `-f, --first` - Find first solution only (default: find all solutions)
- `-o, --output FILE` - Save solutions to file
- `--hints FILE` - Apply hints from file for search space partitioning
- `--help` - Show help message

**parallel_solver (multi-thread):**
- `-f` - Find first solution only (default behavior)
- `-t N` - Use N threads (default: hardware concurrency)
- `--hints FILE` - Apply hints from file for search space partitioning
- `--help` - Show help message

## 🎯 Hints System for Search Space Partitioning

The solvers support a hints file system that allows pre-placing pieces to partition the search space effectively across multiple threads.

### Hints File Format
```
# Comments start with #
# Format: x y piece_id rotation
0 0 1 3
1 0 7 0
```

- **x, y**: Board coordinates (0-based)
- **piece_id**: Piece number (1-based, matching puzzle file order)
- **rotation**: Rotation (0-3, where 0=no rotation, 1=90°, 2=180°, 3=270°)

### Use Cases
- **Parallel Efficiency**: Give different starting positions to threads
- **Known Solutions**: Test solver with partial known placements
- **Search Guidance**: Constrain search space for difficult puzzles
- **Performance Testing**: Benchmark different search space partitions

### Example Hints Files
```bash
# Create hints for 4x4 puzzle
echo "0 0 1 3" > data/hints_4x4.txt

# Create multiple hints for parallel threads
echo -e "0 0 1 3\n1 0 7 0" > data/hints_multi.txt
```

### Validation
- Coordinates must be within puzzle bounds
- Piece IDs must be valid (1 to puzzle_size²)
- Rotations must be 0-3
- No duplicate pieces or positions allowed
- Automatic constraint checking ensures valid placements

## 📚 Documentation

- **CLAUDE.md** - Comprehensive technical analysis and micro-optimization insights
- **Source code** - Fully documented implementations with performance annotations

## License

See individual directories for license information.