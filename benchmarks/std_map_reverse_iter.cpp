#define _GNU_SOURCE
#include <roaring/roaring.h>
#include "roaring64map.hh"
#include <stdio.h>
#include "benchmark.h"
int quickfull() {
    printf("The naive approach works well when the bitmaps quickly become full\n");
    uint64_t cycles_start, cycles_final;
    size_t bitmapcount = 100;
    size_t size = 1000000;
    roaring_bitmap_t **bitmaps =
        (roaring_bitmap_t **)malloc(sizeof(roaring_bitmap_t *) * bitmapcount);
    for (size_t i = 0; i < bitmapcount; i++) {
        bitmaps[i] = roaring_bitmap_from_range(0, 1000000, 1);
        for (size_t j = 0; j < size / 20; j++)
            roaring_bitmap_remove(bitmaps[i], rand() % size);
        roaring_bitmap_run_optimize(bitmaps[i]);
    }

    RDTSC_START(cycles_start);
    roaring_bitmap_t *answer0 = roaring_bitmap_or_many_heap(bitmapcount, (const roaring_bitmap_t **)bitmaps);
    RDTSC_FINAL(cycles_final);
    printf("%f cycles per union (many heap) \n",
           (cycles_final - cycles_start) * 1.0 / bitmapcount);

    RDTSC_START(cycles_start);
    roaring_bitmap_t *answer1 = roaring_bitmap_or_many(bitmapcount, (const roaring_bitmap_t **)bitmaps);
    RDTSC_FINAL(cycles_final);
    printf("%f cycles per union (many) \n",
           (cycles_final - cycles_start) * 1.0 / bitmapcount);

    RDTSC_START(cycles_start);
    roaring_bitmap_t *answer2  = roaring_bitmap_copy(bitmaps[0]);
    for (size_t i = 1; i < bitmapcount; i++) {
        roaring_bitmap_or_inplace(answer2, bitmaps[i]);
    }
    RDTSC_FINAL(cycles_final);
    printf("%f cycles per union (naive) \n",
           (cycles_final - cycles_start) * 1.0 / bitmapcount);

    for (size_t i = 0; i < bitmapcount; i++) {
        roaring_bitmap_free(bitmaps[i]);
    }
    free(bitmaps);
    roaring_bitmap_free(answer0);
    roaring_bitmap_free(answer1);
    roaring_bitmap_free(answer2);
    return 0;
}

int notsofull() {
    printf("The naive approach works less well when the bitmaps do not quickly become full\n");
    uint64_t cycles_start, cycles_final;
    size_t bitmapcount = 100;
    size_t size = 1000000;
    roaring_bitmap_t **bitmaps =
        (roaring_bitmap_t **)malloc(sizeof(roaring_bitmap_t *) * bitmapcount);
    for (size_t i = 0; i < bitmapcount; i++) {
        bitmaps[i] = roaring_bitmap_from_range(0, 1000000, 100);
        for (size_t j = 0; j < size / 20; j++)
            roaring_bitmap_remove(bitmaps[i], rand() % size);
        roaring_bitmap_run_optimize(bitmaps[i]);
    }

    RDTSC_START(cycles_start);
    roaring_bitmap_t *answer0 = roaring_bitmap_or_many_heap(bitmapcount, (const roaring_bitmap_t **)bitmaps);
    RDTSC_FINAL(cycles_final);
    printf("%f cycles per union (many heap) \n",
           (cycles_final - cycles_start) * 1.0 / bitmapcount);

    RDTSC_START(cycles_start);
    roaring_bitmap_t *answer1 = roaring_bitmap_or_many(bitmapcount, (const roaring_bitmap_t **)bitmaps);
    RDTSC_FINAL(cycles_final);
    printf("%f cycles per union (many) \n",
           (cycles_final - cycles_start) * 1.0 / bitmapcount);

    RDTSC_START(cycles_start);
    roaring_bitmap_t *answer2  = roaring_bitmap_copy(bitmaps[0]);
    for (size_t i = 1; i < bitmapcount; i++) {
        roaring_bitmap_or_inplace(answer2, bitmaps[i]);
    }
    RDTSC_FINAL(cycles_final);
    printf("%f cycles per union (naive) \n",
           (cycles_final - cycles_start) * 1.0 / bitmapcount);

    for (size_t i = 0; i < bitmapcount; i++) {
        roaring_bitmap_free(bitmaps[i]);
    }
    free(bitmaps);
    roaring_bitmap_free(answer0);
    roaring_bitmap_free(answer1);
    roaring_bitmap_free(answer2);
    return 0;
}

void testReverseIteration() {
    // For fun, we space our elements "almost" 2^32 apart but not quite.
    const uint64_t four_billion = 4000000000;
    const size_t numValues = 50000000;  // 50 million

    // We want one remaining value at the very front of the bitmap. This is
    // because "maximum" has to scan backwards from the end, skipping
    // over all the empty bitmaps we've created (and we've created a lot of them)
    uint64_t soleRemainingValue = 12345;

    roaring::Roaring64Map r64;

    // This will create a lot of empty Roaring 32-bit bitmaps in the Roaring64Map
    r64.add(soleRemainingValue);
    for (size_t i = 0; i < numValues; ++i) {
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

    RDTSC_START(cycles_start);
    auto legacy_maximum = r64.maximum_legacy_impl();
    RDTSC_FINAL(cycles_final);
    auto legacy_cyclesPerElement = double(cycles_final - cycles_start) / numValues;

    if (maximum != legacy_maximum || maximum != soleRemainingValue) {
        std::cerr << "Programming error: maximum was not what was expected\n";
        std::exit(1);
    }

    std::cout << "


    roaring_bitmap_t *answer2  = roaring_bitmap_copy(bitmaps[0]);
    for (size_t i = 1; i < bitmapcount; i++) {
        roaring_bitmap_or_inplace(answer2, bitmaps[i]);
    }
    RDTSC_FINAL(cycles_final);




    {
        Timer timer("maximum");
        for (size_t i = 0; i < numRepetitions; ++i) {
            timer.start();
            auto m = r.maximum();
            if (m != soleRemainingValue) {
                std::cerr << "That was unexpected\n";
                exit(1);
            }
            timer.stop(i);
        }
        timer.showAverage();
    }

    {
        Timer timer("prev-maximum");
        for (size_t i = 0; i < numRepetitions; ++i) {
            timer.start();
            auto m = r.maximum_previous_impl();
            if (m != soleRemainingValue) {
                std::cerr << "That was unexpected\n";
                exit(1);
            }
            timer.stop(i);
        }
        timer.showAverage();
    }
}


int main() {

    printf("IT IS ZAMBONI TIME\n");
    quickfull();
    notsofull();
    return 0;
}
