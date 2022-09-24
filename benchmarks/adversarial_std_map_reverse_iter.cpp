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

// Meta-executive summary: I cannot reliably make this benchmark prove my point.
// Sometimes it's like 10% faster. When I change a few parameters, it's 5% slower.
// It is not the slam-dunk I expected. I'm leaving this file here for now in
// case there's interest. But I will probably abandon this approach.
//
// Executive summary: it is faster to use std::map::iterator,
// explicitly traveling in the reverse direction, than it is to use
// std::map::reverse_iterator. The reason has to do with the standard
// library's implementation of reverse_iterator.
//
// Rationale:
//
// The reverse_iterator provided by map's rbegin(), rend(), crbegin(),
// and crend() is just an "adaptor" wrapped around its normal,
// forward-moving iterator. That adaptor is the template
// std::reverse_iterator<Iter>. This is the case for all the standard
// library collections: their reverse_iterator is a wrapper around their
// regular iterator.
//
// Typically, regular iterators point directly at their current target.
// However, the standard library's implementation of
// std::reverse_iterator<Iter> always keeps its underlying iterator
// pointing one element past its current target. Upon dereference
// (operator*() or operator->()) the reverse_iterator will create a copy
// of its underlying iterator (typically cheap), decrement that iterator
// (foreshadowing: potentially not cheap), and then return the
// dereference of that temporary (again, cheap).
//
// Consider this code in stl_iterator.h for my version of gcc
// which does exactly the above:
//
// _GLIBCXX17_CONSTEXPR reference
// operator*() const
// {
//   _Iterator __tmp = current;
//   return *--__tmp;
// }
//
// In a collection like std::vector, the overhead of doing things this
// way is negligible. An extra pointer subtraction here or there is
// unlikely to be measurable. However, traversal in an ordered binary
// tree (the data structure underlying std::map) is significantly more
// expensive than pointer arithmetic. Finding the next (or previous)
// node in an ordered binary tree is a standard coding interview problem
// and has to do with walking down or up the tree, potentially taking
// multiple steps, until the suitable next/previous node is found.
//
// This could have a surprising impact on performance. Consider the
// following code:
//
// auto rit = map.rbegin();
// while (rit != map.rend()) {
//   const auto &entry = *rit;  // (1)
//   process(entry.key, entry.value);
//   ++rit;  // (2)
// }
//
// Note: it would be more natural to write the above as a 'for' loop,
// but the above code is equivalent and easier to compare to the
// alternative code we write below.
//
// In the above, lines marked (1) and (2) both end up doing the same,
// redundant, tree operation. In line (1), the implementation of
// operator*() makes a temporary underlying iterator, does a tree
// operation to find the previous node, dereferences it to get the
// address, and then throws away the temporary. In line (2) the
// implementation of operator++ again does the redundant tree operation
// to move to the previous tree node (note: ++ goes to "previous node"
// because reverse iterators go backwards). The only difference is that
// this time the iterator keeps the result of its work rather than
// discarding it. In aggregate, the above code effectively traverses the
// underlying tree twice. Code that happens to use multiple operator*
// or operator->, which would appear harmless, would actually traverse
// the tree even more. It is more efficient to traverse it only once.
//
// ON THE OTHER HAND, the compiler seems to be well-aware of this, and it
// is able to elide the second tree operation if it can inline or look through
// the function being called ("process" in the above). Put another way,
// if the compiler can prove that "process" can't change the map, it will
// elide the second tree operation.
//
// Knowing the above, we can do better by mimicking the same
// operations on the underlying iterator that std::reverse_iterator
// would do, but simply avoid wasting work. The code we write looks
// like this, which can be compared directly to the above:
//
// auto it = map.end();  // compare to rit = map.rbegin()
// while (it != map.begin()) {  // compare to rit != map.rend()
//   --it;  // Do the underlying tree operation once, not twice.
//   const auto &entry = *it;  // Normal iterator deref is very cheap.
//   process(entry.key, entry.value);
// }
//
// Here we can see that we do the underlying tree operations once,
// rather than twice.
//
// See https://stackoverflow.com/questions/889262/iterator-vs-reverse-iterator
// and https://en.cppreference.com/w/cpp/iterator/reverse_iterator

void testIterationHypothesis() {
    std::cout << R"(Hypothesis: with std::map, it is better to use forward iterators
moving in the reverse direction), than it is to use reverse iterators.

However, whether this matters depends on the code. If the compiler can inline
everything and prove that the iteration doesn't alter the structure of the map,
then it can optimize out the redundant tree operation and make the two cases
equivalent. But if it can't (for example, if the iteration calls out to a
function that the optimizer can't inline or look through), then reverse
iteration will be slower.

In our case, we do have such an external call (namely, Roaring64Map::maximum()
calls Roaring::isEmpty(), which is inlined, but it in turn
calls api::roaring_bitmap_is_empty(), which is not inlined. In this case we
*would* expect reverse iteration to be slower.

For Roaring64, currently the only case where we use reverse iteration is in the
implementation of maximum(), and even there the difference will only be
noticeable in situations where there are a *lot* of empty bitmaps to skip over.

Also, perhaps due to the vagaries of benchmarks, CPUs, cache, phase of the moon, I don't see a speedup here 100% of the time.
Sometimes I see a 10% speedup, sometimes I see 0. Occasionally,
I see a slowdown.

)";

    // Repeat the test a few times to smooth out the measurements
    size_t numWarmupIterations = 5;
    size_t numTestIterations = 25;

    // We want to space our elements 2^32 apart so the end up in different
    // map slots in the "outer" Roaring64Map. For fun I space them "almost"
    // 2^32 apart but not quite.
    const uint64_t four_billion = 4000000000;
    const size_t numEmptyBitmaps = 10000000;  // 10 million

    // Construct two Roaring64Maps in the same way for our side-by-side tests.
    roaring::Roaring64Map new_r64;
    roaring::Roaring64Map legacy_r64;

    // Seed RNG engine with fixed number for predictability
    std::mt19937 engine(12345);
    // closed interval
    std::uniform_int_distribution<uint64_t> rng(0, numEmptyBitmaps - 1);

    // Our calls to add, then remove, will end up creating lots of "outer"
    // entries in the Roaring64Map that point to empty Roaring (32-bit) maps.
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
        checkMaximum(probe, new_maximum);

        auto legacy_maximum = legacy_r64.maximum_legacy_impl();
        checkMaximum(probe, legacy_maximum);

        new_r64.remove(probe);
        legacy_r64.remove(probe);
    }

    // Give both maps the same sequence of random probes.
    std::vector<uint64_t> probes;
    for (size_t i = 0; i < numTestIterations; ++i) {
        auto probe = rng(engine) * four_billion;
        std::cout << "Probe " << i << " is " << probe << '\n';
        probes.push_back(probe);
    }

    // Real tests
    uint64_t new_cycles_total = 0;
    for (size_t testIter = 0; testIter < numTestIterations; ++testIter) {
        auto probe = probes[testIter];
        std::cout << "Running 'new' iteration " << testIter << '\n';

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
        auto probe = probes[testIter];
        std::cout << "Running 'legacy' iteration " << testIter << '\n';

        legacy_r64.add(probe);

        uint64_t cycles_start, cycles_final;
        RDTSC_START(cycles_start);
        auto maximum = legacy_r64.maximum();
        RDTSC_FINAL(cycles_final);

        legacy_cycles_total += cycles_final - cycles_start;

        checkMaximum(probe, maximum);
        legacy_r64.remove(probe);
    }

    auto totalElements = numEmptyBitmaps * numTestIterations;
    auto new_cyclesPerElement = double(new_cycles_total) / totalElements;
    auto legacy_cyclesPerElement = double(legacy_cycles_total) / totalElements;

    std::cout << "A = forward iterators moving backwards: "
              << new_cyclesPerElement << " cycles per element\n";

    std::cout << "B = reverse iterators: "
              << legacy_cyclesPerElement << " cycles per element\n";

    std::cout << "Ratio (A/B) = " << new_cyclesPerElement / legacy_cyclesPerElement
              << " (if materially < 1.0, then the hypothesis is confirmed)\n"
              << '\n';
}
}  // namespace

int main() {
    testIterationHypothesis();
}
