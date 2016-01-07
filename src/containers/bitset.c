/*
 * bitset.c
 *
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bitset.h"

//#define USEAVX  moved to makefile

/* Create a new bitset. Return NULL in case of failure. */
bitset_container_t *bitset_container_create() {
    bitset_container_t *bitset = NULL;

    if(posix_memalign((void *) &bitset, sizeof(__m256i), sizeof(bitset_container_t)))
        return NULL;

    memset(bitset, 0, sizeof(bitset_container_t));
    bitset->cardinality = 0;
    return bitset;
}

/* Free memory. */
void bitset_container_free(bitset_container_t *bitset) { free(bitset); }

/* Set the ith bit.  */
void bitset_container_set(bitset_container_t *bitset, uint16_t i) {
    const uint64_t oldw = bitset->array[i >> 6];
    const uint64_t neww = oldw | (UINT64_C(1) << (i & 63));
    bitset->cardinality += (oldw != neww);
    bitset->array[i >> 6] = neww;
}

/* Unset the ith bit.  */
void bitset_container_unset(bitset_container_t *bitset, uint16_t i) {
    const uint64_t oldw = bitset->array[i >> 6];
    const uint64_t neww = oldw & (~(UINT64_C(1) << (i & 63)));
    bitset->cardinality -= (oldw != neww);
    bitset->array[i >> 6] = neww;
}

/* Get the value of the ith bit.  */
bool bitset_container_get(const bitset_container_t *bitset, uint16_t i) {
    const uint64_t w = bitset->array[i >> 6];
    // getting rid of the mask can shave one cycle off...
    return (w >> (i & 63)) & 1;
}

/* Get the number of bits set (force computation) */
int bitset_container_compute_cardinality(const bitset_container_t *bitset) {
    const uint64_t *a = bitset->array;
    int32_t sum1 = 0;
    for (int k = 0; k < BITSET_CONTAINER_SIZE_IN_WORDS; k += 4) {
        sum1 += _mm_popcnt_u64(a[k]);
        sum1 += _mm_popcnt_u64(a[k + 1]);
        sum1 += _mm_popcnt_u64(a[k + 2]);
        sum1 += _mm_popcnt_u64(a[k + 3]);
    }
    return sum1;
}

/* computes a binary operation (eg union) on bitset1 and bitset2 and write the result to bitsetout
 */

#ifdef USEAVX

#define BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic) \
/* computes the union of bitset1 and bitset2 and write the result to bitsetout,\
 * does not compute the cardinality of the result */\
int bitset_container_ ## opname ## _nocard(const bitset_container_t *bitset1,\
                                           const bitset_container_t *bitset2,\
                                            bitset_container_t *bitsetout) {\
    const uint64_t *a1 = bitset1->array;\
    const uint64_t *a2 = bitset2->array;\
    /* not using the blocking optimization for some reason*/     \
    uint64_t *ao = bitsetout->array;\
    for (int k = 0; k < BITSET_CONTAINER_SIZE_IN_WORDS / 4; k++) {\
        __m256i A1 = _mm256_lddqu_si256((__m256i *)a1 + k);\
        __m256i A2 = _mm256_lddqu_si256((__m256i *)a2 + k);\
        __m256i AO = avx_intrinsic (A2,A1);/* swapped order to get andnot to work*/ \
        _mm256_storeu_si256((__m256i *)ao + k, AO);\
    }\
    bitsetout->cardinality = -1;\
    return bitsetout->cardinality;\
}\
/* next, a version that updates cardinality*/\
int bitset_container_ ## opname (const bitset_container_t *bitset1,\
                                 const bitset_container_t *bitset2,\
                                       bitset_container_t *bitsetout) {\
    const uint64_t *a1 = bitset1->array;\
    const uint64_t *a2 = bitset2->array;\
    uint64_t *ao = bitsetout->array;\
    int sum = 0;\
    const int repeat = 8; /*data is processed in blocks of "repeat" 32-byte registers */\
    const int wordsin256 = sizeof(__m256i) /  sizeof(uint64_t); /* how many 64-bit words are there in a 32-byte register? */\
    for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS / (wordsin256 * repeat); i++) {\
        for (int j = 0; j < repeat; ++j) {\
            __m256i A1 = _mm256_lddqu_si256((__m256i *)a1 + (i * repeat) + j);\
            __m256i A2 = _mm256_lddqu_si256((__m256i *)a2 + (i * repeat) + j);\
            __m256i AO = avx_intrinsic (A2, A1); /* swapped arg order to get andnot to work*/ \
            _mm256_storeu_si256((__m256i *)ao + (i * repeat) + j, AO);\
        }\
        for (int j = 0; j < repeat; ++j) {\
            sum += _mm_popcnt_u64(ao[(i * repeat * wordsin256) + j * 4]);\
            sum += _mm_popcnt_u64(ao[(i * repeat * wordsin256) + j * 4 + 1]);\
            sum += _mm_popcnt_u64(ao[(i * repeat * wordsin256) + j * 4 + 2]);\
            sum += _mm_popcnt_u64(ao[(i * repeat * wordsin256) + j * 4 + 3]);\
        }\
    }\
    bitsetout->cardinality = sum;\
    return bitsetout->cardinality;\
}

#else  /* not USEAVX  */

#define BITSET_CONTAINER_FN(opname, opsymbol, avxintrinsic) \
int bitset_container_ ## opname (const bitset_container_t *bitset1,\
                                 const bitset_container_t *bitset2,\
                                 bitset_container_t *bitsetout) {\
    const uint64_t *a1 = bitset1->array;\
    const uint64_t *a2 = bitset2->array;\
    uint64_t *ao = bitsetout->array;\
    int32_t cardinality = 0;\
    for (int k = 0; k < BITSET_CONTAINER_SIZE_IN_WORDS; k += 2) {\
      const uint64_t w1 = (a1[k]) opsymbol (a2[k]);              \
        ao[k] = w1;\
        cardinality += _mm_popcnt_u64(w1);\
        const uint64_t w2 = (a1[k + 1]) opsymbol (a2[k + 1]);   \
        ao[k + 1] = w2;\
        cardinality += _mm_popcnt_u64(w2);\
    }\
    bitsetout->cardinality = cardinality;\
    return bitsetout->cardinality;\
}\
int bitset_container_ ## opname ## _nocard(const bitset_container_t *bitset1,\
                                         const bitset_container_t *bitset2,\
                                         bitset_container_t *bitsetout) {\
    const uint64_t *a1 = bitset1->array;\
    const uint64_t *a2 = bitset2->array;\
    uint64_t *ao = bitsetout->array;\
    for (int k = 0; k < BITSET_CONTAINER_SIZE_IN_WORDS; k++) {\
      const uint64_t w = (a1[k]) opsymbol (a2[k]);\
        ao[k] = w;\
    }\
    bitsetout->cardinality = -1;\
    return bitsetout->cardinality;\
}
#endif


BITSET_CONTAINER_FN(xor, ^, _mm256_xor_si256)
BITSET_CONTAINER_FN(or, | , _mm256_or_si256)
BITSET_CONTAINER_FN(and, &, _mm256_and_si256)
BITSET_CONTAINER_FN(andnot, &~, _mm256_andnot_si256)