#define _GNU_SOURCE
#include <roaring/roaring.h>
#include "roaring64map.hh"
#include <stdio.h>
#include "benchmark.h"

namespace {
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

    std::cout << "\nAlso, perhaps due to cache effects, this difference is only\n"
                 "noticeable on large data sets. On my CPU I can reliably\n"
                 "see an improvement only when I have 50 million elements\n"
                 "to skip over.n\n";

    // Repeat the test a few times to smooth out the measurements
    size_t numWarmupIterations = 3;
    size_t numTestIterations = 10;

    // For fun, we space our elements "almost" 2^32 apart but not quite.
    const uint64_t four_billion = 4000000000;
    const size_t numEmptyBitmaps = 10000000;  // 10 million

    // We want one remaining value at the very front of the bitmap. This is
    // because "maximum" has to scan backwards from the end, skipping
    // over all the empty bitmaps we've created (and we've created a lot of them)
    uint64_t soleRemainingValue = 12345;

    roaring::Roaring64Map r64;

    // This will create a lot of empty Roaring 32-bit bitmaps in the Roaring64Map
    std::cout << "Creating " << numEmptyBitmaps << " empty bitmaps\n";
    r64.add(soleRemainingValue);
    for (size_t i = 1; i != numEmptyBitmaps; ++i) {
        auto value = i * four_billion;
        r64.add(value);
        r64.remove(value);
    }

    if (r64.cardinality() != 1) {
        std::cerr << "Programming error: not 1 remaining element in set\n";
        std::exit(1);
    }

    // Warmups
    for (size_t warmupIter = 0; warmupIter < numWarmupIterations; ++warmupIter) {
        std::cout << "Running warmup iteration " << warmupIter << '\n';
        auto maximum = r64.maximum();
        auto legacy_maximum = r64.maximum_legacy_impl();
        if (maximum != legacy_maximum || maximum != soleRemainingValue) {
            std::cerr
                << "Programming error: maximum was not what was expected\n";
            std::exit(1);
        }
    }

    // Real tests
    size_t new_totalCycles = 0;
    size_t legacy_totalCycles = 0;
    size_t totalElements = 0;

    for (size_t testIter = 0; testIter < numTestIterations; ++testIter) {
        std::cout << "Running iteration " << testIter << '\n';
        uint64_t cycles_start, cycles_final;

        RDTSC_START(cycles_start);
        auto maximum = r64.maximum();
        RDTSC_FINAL(cycles_final);
        new_totalCycles += cycles_final - cycles_start;

        RDTSC_START(cycles_start);
        auto legacy_maximum = r64.maximum_legacy_impl();
        RDTSC_FINAL(cycles_final);
        legacy_totalCycles += cycles_final - cycles_start;

        totalElements += numEmptyBitmaps;

        if (maximum != legacy_maximum || maximum != soleRemainingValue) {
            std::cerr
                << "Programming error: maximum was not what was expected\n";
            std::exit(1);
        }
    }

    auto new_cyclesPerElement = double(new_totalCycles) / totalElements;
    auto legacy_cyclesPerElement = double(legacy_totalCycles) / totalElements;

    std::cout << "A = forward iterators moving backwards: "
              << new_cyclesPerElement << " cycles per element\n";

    std::cout << "B = reverse iterators: " << legacy_cyclesPerElement
              << " cycles per element\n";

    std::cout << "Ratio (A/B) = " << new_cyclesPerElement / legacy_cyclesPerElement
              << " (if materially < 1.0, then the hypothesis is confirmed)\n"
              << '\n';
}
}  // namespace

int main() {
    testIterationHypothesis();
}
