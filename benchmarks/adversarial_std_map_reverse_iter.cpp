#define _GNU_SOURCE
#include <random>
#include <stdio.h>
#include <roaring/roaring.h>
#include "roaring64map.hh"
#include "benchmark.h"

namespace {
void checkMaximum(uint64_t expected, uint64_t actual) {
    if (expected != actual) {
        std::cerr << "Programming error: expected " << expected << ", actual " << actual << "\n";
        std::exit(1);
    }
}

void testIterationHypothesis() {
    std::cout << "Hypothesis: with std::map, it is better to use forward iterators\n"
                 "(moving in the reverse direction),\n"
                 "than it is to use reverse iterators.\n\n";

    std::cout << "However, this depends on the code. If the optimizer can\n"
                 "inline everything and prove that the iteration doesn't alter\n"
                 "the structure of the map, then it can make the two cases\n"
                 "equivalent in speed. But if it can't (for example, if the\n"
                 "iteration calls out to a function that the optimizer can't\n"
                 "inline or look through, then reverse iteration will be slower.\n\n";

    std::cout << "In our case, we do have such an external function, and so\n"
                 "we *would* expect reverse iteration to be slower.\n";

    std::cout << "\nFor Roaring64, currently the only case where we use reverse\n"
                 "iteration is in the implementation of maximum(), and even there\n"
                 "the difference will only be noticeable in situations where\n"
                 "there are a *lot* of empty bitmaps to skip over.\n\n";

    std::cout << "\nAlso, perhaps due to the vagaries of benchmarks, CPUs, cache,\n"
                 "phase of the moon, I don't see a speedup here 100% of the time.\n"
                 "Sometimes I see a 10% speedup, sometimes I see 0. Occasionally,\n"
                 "I see a slowdown.\n\n";

    // Repeat the test a few times to smooth out the measurements
    size_t numWarmupIterations = 3;
    size_t numTestIterations = 10;

    // For fun, we space our elements "almost" 2^32 apart but not quite.
    const uint64_t four_billion = 4000000000;
    const size_t numEmptyBitmaps = 10000000;  // 10 million

    roaring::Roaring64Map new_r64;
    roaring::Roaring64Map legacy_r64;

    // Seed RNG engine with fixed number for predictability
    std::mt19937 engine(12345);
    std::uniform_int_distribution<uint64_t> rng(0, numEmptyBitmaps + 1);

    // This will create a lot of empty Roaring 32-bit bitmaps in the Roaring64Map
    std::cout << "Creating " << numEmptyBitmaps << " empty bitmaps\n";
    for (size_t i = 0; i != numEmptyBitmaps; ++i) {
        auto value = rng(engine) * four_billion;
        new_r64.add(value);
        new_r64.remove(value);

        legacy_r64.add(value);
        legacy_r64.remove(value);
    }

    if (!new_r64.isEmpty() || !legacy_r64.isEmpty()) {
        std::cerr << "Programming error: r64s are not empty\n";
        std::exit(1);
    }


    // Warmups
    for (size_t warmupIter = 0; warmupIter < numWarmupIterations; ++warmupIter) {
        std::cout << "Running warmup iteration " << warmupIter << '\n';
        auto probe = rng(engine) * four_billion;
        new_r64.add(probe);
        legacy_r64.add(probe);
        auto new_maximum = new_r64.maximum();
        auto legacy_maximum = legacy_r64.maximum_legacy_impl();
        checkMaximum(probe, new_maximum);
        checkMaximum(probe, legacy_maximum);
        new_r64.remove(probe);
        legacy_r64.remove(probe);
    }

    // Real tests
    uint64_t new_cycles_total = 0;
    for (size_t testIter = 0; testIter < numTestIterations; ++testIter) {
        auto probe = rng(engine) * four_billion;
        std::cout << "Running 'new' iteration " << testIter
                  << ". Probe is " << probe << '\n';

        new_r64.add(probe);

        uint64_t cycles_start, cycles_final;
        RDTSC_START(cycles_start);
        auto maximum = new_r64.maximum();
        RDTSC_FINAL(cycles_final);

        new_cycles_total += cycles_final - cycles_start;

        checkMaximum(probe, maximum);
        new_r64.remove(probe);
    }

    uint64_t legacy_cycles_total = 0;
    for (size_t testIter = 0; testIter < numTestIterations; ++testIter) {
        auto probe = rng(engine) * four_billion;
        std::cout << "Running 'legacy' iteration " << testIter
                  << ". Probe is " << probe << '\n';

        legacy_r64.add(probe);

        uint64_t cycles_start, cycles_final;
        RDTSC_START(cycles_start);
        auto maximum = legacy_r64.maximum();
        RDTSC_FINAL(cycles_final);

        legacy_cycles_total += cycles_final - cycles_start;

        checkMaximum(probe, maximum);
        legacy_r64.remove(probe);
    }

    // Real tests
    uint64_t new_cycles_total2 = 0;
    for (size_t testIter = 0; testIter < numTestIterations; ++testIter) {
        auto probe = rng(engine) * four_billion;
        std::cout << "Running 'new' iteration 2 " << testIter
                  << ". Probe is " << probe << '\n';

        new_r64.add(probe);

        uint64_t cycles_start, cycles_final;
        RDTSC_START(cycles_start);
        auto maximum = new_r64.maximum();
        RDTSC_FINAL(cycles_final);

        new_cycles_total2 += cycles_final - cycles_start;

        checkMaximum(probe, maximum);
        new_r64.remove(probe);
    }

    uint64_t legacy_cycles_total2 = 0;
    for (size_t testIter = 0; testIter < numTestIterations; ++testIter) {
        auto probe = rng(engine) * four_billion;
        std::cout << "Running 'legacy' iteration 2 " << testIter
                  << ". Probe is " << probe << '\n';

        legacy_r64.add(probe);

        uint64_t cycles_start, cycles_final;
        RDTSC_START(cycles_start);
        auto maximum = legacy_r64.maximum();
        RDTSC_FINAL(cycles_final);

        legacy_cycles_total2 += cycles_final - cycles_start;

        checkMaximum(probe, maximum);
        legacy_r64.remove(probe);
    }

    auto totalElements = numEmptyBitmaps * numTestIterations;
    auto new_cyclesPerElement = double(new_cycles_total) / totalElements;
    auto new_cyclesPerElement2 = double(new_cycles_total2) / totalElements;
    auto legacy_cyclesPerElement = double(legacy_cycles_total) / totalElements;
    auto legacy_cyclesPerElement2 = double(legacy_cycles_total2) / totalElements;

    std::cout << "A = forward iterators moving backwards: "
              << new_cyclesPerElement << " cycles per element\n";

    std::cout << "B = reverse iterators: "
              << legacy_cyclesPerElement << " cycles per element\n";

    std::cout << "C = like A but again: "
              << new_cyclesPerElement2 << " cycles per element\n";

    std::cout << "D = like B but again: "
              << legacy_cyclesPerElement2 << " cycles per element\n";

    std::cout << "Ratio (A/B) = " << new_cyclesPerElement / legacy_cyclesPerElement
              << " (if materially < 1.0, then the hypothesis is confirmed)\n"
              << '\n';
}
}  // namespace

int main() {
    testIterationHypothesis();
}
