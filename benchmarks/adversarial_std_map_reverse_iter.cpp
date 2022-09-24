#define _GNU_SOURCE
#include <roaring/roaring.h>
#include "roaring64map.hh"
#include <stdio.h>
#include "benchmark.h"

namespace {
void testIteration() {
    // For fun, we space our elements "almost" 2^32 apart but not quite.
    const uint64_t four_billion = 4000000000;
    const size_t numValues = 20000000;  // 20 million

    // We want one remaining value at the very front of the bitmap. This is
    // because "maximum" has to scan backwards from the end, skipping
    // over all the empty bitmaps we've created (and we've created a lot of them)
    uint64_t soleRemainingValue = 12345;

    roaring::Roaring64Map r64;

    r64.add(soleRemainingValue);
    // This will create a lot of empty Roaring 32-bit bitmaps in the Roaring64Map
    for (size_t i = 1; i != numValues; ++i) {
        auto value = i * four_billion;
        r64.add(value);
        r64.remove(value);
    }

    if (r64.cardinality() != 1) {
        std::cerr << "Programming error: not 1 remaining element in set\n";
        std::exit(1);
    }

    uint64_t cycles_start, cycles_final;

    RDTSC_START(cycles_start);
    auto maximum = r64.maximum();
    RDTSC_FINAL(cycles_final);
    auto cyclesPerElement = double(cycles_final - cycles_start) / numValues;
    std::cout << "A = forward iterators moving backwards: " << cyclesPerElement
              << " cycles per element\n";

    RDTSC_START(cycles_start);
    auto legacy_maximum = r64.maximum_legacy_impl();
    RDTSC_FINAL(cycles_final);
    auto legacy_cyclesPerElement =
        double(cycles_final - cycles_start) / numValues;

    if (maximum != legacy_maximum || maximum != soleRemainingValue) {
        std::cerr << "Programming error: maximum was not what was expected\n";
        std::exit(1);
    }

    std::cout << "B = reverse iterators: " << legacy_cyclesPerElement
              << " cycles per element\n";

    std::cout << "Ratio (A/B) = " << cyclesPerElement / legacy_cyclesPerElement
              << '\n';

}
}  // namespace

int main() {
    std::cout << "With std::map, it is better to use forward iterators\n"
                 "(moving in the reverse direction),\n"
                 "than it is to use reverse iterators.\n";
    testIteration();
}
