#include <map>
#include <iostream>
#include <mutex>
#include <cmath>
#include "paryfor.hpp"

int main(int argc, char** argv) {
    // just test that we can compile
    uint64_t todo_count = std::stoul(argv[1]);
    int thread_count = std::stoi(argv[2]);
    int chunk_size = std::stoi(argv[3]);
    std::vector<uint64_t> count(thread_count);
    std::mutex count_mutex;
    paryfor::parallel_for<uint64_t>(
        0, todo_count, thread_count, chunk_size,
        [&](uint64_t i, int tid) {
            // do some trivial work, so that we can see the effect of multithreading
            for (uint64_t j = 0; j < 100; ++j) {
                i *= exp(i) * exp(i+i) / log(i+i+i);;
            }
            ++count[tid];
        });
    uint64_t c = 0;
    for (int i = 0; i < thread_count; ++i) {
        std::cout << "thread " << i << " " << count[i] << std::endl;
        c += count[i];
    }
    if (c != todo_count) {
        std::cerr << "error: count does not match that requested" << std::endl;
        return 1;
    }
    return 0;
}
