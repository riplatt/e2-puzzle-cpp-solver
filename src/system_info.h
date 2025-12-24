#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

struct SystemInfo {
    std::string cpu_model;
    int cpu_cores;
    int cpu_threads;
    double cpu_frequency_ghz;

    // Cache sizes in KB
    int l1_icache_kb;
    int l1_dcache_kb;
    int l2_cache_kb;
    int l3_cache_kb;

    // Memory info
    long total_memory_gb;

    SystemInfo() {
        detect_cpu_info();
        detect_cache_info();
        detect_memory_info();
    }

private:
    void detect_cpu_info() {
        cpu_cores = 0;
        cpu_threads = std::thread::hardware_concurrency();
        cpu_frequency_ghz = 0.0;
        cpu_model = "Unknown";

        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;

        while (std::getline(cpuinfo, line)) {
            if (line.find("model name") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    cpu_model = line.substr(pos + 2);
                    // Extract frequency from model name if present
                    size_t freq_pos = cpu_model.find("@ ");
                    if (freq_pos != std::string::npos) {
                        std::string freq_str = cpu_model.substr(freq_pos + 2);
                        size_t ghz_pos = freq_str.find("GHz");
                        if (ghz_pos != std::string::npos) {
                            try {
                                cpu_frequency_ghz = std::stod(freq_str.substr(0, ghz_pos));
                            } catch (...) {}
                        }
                    }
                }
            } else if (line.find("cpu cores") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    try {
                        cpu_cores = std::stoi(line.substr(pos + 2));
                    } catch (...) {}
                }
            }
        }

        // Fallback: if cores not detected, assume threads/2 (common for hyperthreading)
        if (cpu_cores == 0 && cpu_threads > 0) {
            cpu_cores = cpu_threads / 2;
        }
    }

    void detect_cache_info() {
        l1_icache_kb = get_cache_size("LEVEL1_ICACHE_SIZE") / 1024;
        l1_dcache_kb = get_cache_size("LEVEL1_DCACHE_SIZE") / 1024;
        l2_cache_kb = get_cache_size("LEVEL2_CACHE_SIZE") / 1024;
        l3_cache_kb = get_cache_size("LEVEL3_CACHE_SIZE") / 1024;
    }

    void detect_memory_info() {
        total_memory_gb = 0;

        std::ifstream meminfo("/proc/meminfo");
        std::string line;

        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") != std::string::npos) {
                std::istringstream iss(line);
                std::string label, unit;
                long memory_kb;
                if (iss >> label >> memory_kb >> unit) {
                    total_memory_gb = memory_kb / (1024 * 1024);  // Convert KB to GB
                }
                break;
            }
        }
    }

    long get_cache_size(const std::string& cache_name) {
#if defined(__APPLE__)
        return 0;
#else
        long cache_size = sysconf(_SC_LEVEL1_ICACHE_SIZE);

        if (cache_name == "LEVEL1_ICACHE_SIZE") {
            cache_size = sysconf(_SC_LEVEL1_ICACHE_SIZE);
        } else if (cache_name == "LEVEL1_DCACHE_SIZE") {
            cache_size = sysconf(_SC_LEVEL1_DCACHE_SIZE);
        } else if (cache_name == "LEVEL2_CACHE_SIZE") {
            cache_size = sysconf(_SC_LEVEL2_CACHE_SIZE);
        } else if (cache_name == "LEVEL3_CACHE_SIZE") {
            cache_size = sysconf(_SC_LEVEL3_CACHE_SIZE);
        }

        return (cache_size > 0) ? cache_size : 0;
#endif
    }

public:
    std::string get_cpu_summary() const {
        std::ostringstream oss;

        // Simplify CPU model name
        std::string short_model = cpu_model;
        size_t intel_pos = short_model.find("Intel(R) Core(TM) ");
        if (intel_pos != std::string::npos) {
            short_model = short_model.substr(intel_pos + 18);
            size_t cpu_pos = short_model.find(" CPU");
            if (cpu_pos != std::string::npos) {
                short_model = short_model.substr(0, cpu_pos);
            }
        }

        oss << short_model;
        if (cpu_frequency_ghz > 0) {
            oss << " @ " << cpu_frequency_ghz << "GHz";
        }
        oss << " (" << cpu_cores << "C/" << cpu_threads << "T)";

        return oss.str();
    }

    std::string get_cache_summary() const {
        std::ostringstream oss;
        oss << "L1: " << l1_dcache_kb << "KB";
        if (l2_cache_kb > 0) oss << ", L2: " << l2_cache_kb << "KB";
        if (l3_cache_kb > 0) oss << ", L3: " << (l3_cache_kb/1024) << "MB";
        return oss.str();
    }

    std::string get_memory_summary() const {
        std::ostringstream oss;
        oss << "RAM: " << total_memory_gb << "GB";
        return oss.str();
    }

    void print_system_header() const {
        std::cout << "System: " << get_cpu_summary() << std::endl;
        std::cout << "Cache: " << get_cache_summary() << std::endl;
        std::cout << "Memory: " << get_memory_summary() << std::endl;
    }
};
