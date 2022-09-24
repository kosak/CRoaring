/*
A C++ header for 64-bit Roaring Bitmaps, implemented by way of a map of many
32-bit Roaring Bitmaps.
*/
#ifndef INCLUDE_ROARING_64_MAP_HH_
#define INCLUDE_ROARING_64_MAP_HH_

#include <algorithm>
#include <cstdarg>  // for va_list handling in bitmapOf()
#include <cstdio>  // for std::printf() in the printf() method
#include <cstring>  // for std::memcpy()
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "roaring.hh"

extern "C" {
void calcKey(const void *);
}

namespace roaring {

using roaring::Roaring;

class Roaring64MapSetBitForwardIterator;
class Roaring64MapSetBitBiDirectionalIterator;

class Roaring64Map {
    typedef api::roaring_bitmap_t roaring_bitmap_t;

public:
    /**
     * Creates an empty bitmap
     */
    Roaring64Map() = default;

    /**
     * Constructs a bitmap from a list of 32-bit integer values.
     */
    Roaring64Map(size_t n, const uint32_t *data) { addMany(n, data); }

    /**
     * Constructs a bitmap from a list of 64-bit integer values.
     */
    Roaring64Map(size_t n, const uint64_t *data) { addMany(n, data); }

    /**
     * Constructs a 64-bit map from a 32-bit one.
     */
    explicit Roaring64Map(const Roaring &r) { emplaceOrInsert(0, r); }

    /**
     * Construct a 64-bit map from a 32-bit rvalue.
     */
    explicit Roaring64Map(Roaring &&r) { emplaceOrInsert(0, std::move(r)); }

    /**
     * Construct a roaring object from the C struct.
     *
     * Passing a NULL pointer is not allowed. The pointer will be deallocated
     * by roaring_free after this call.
     */
    explicit Roaring64Map(roaring_bitmap_t *s) {
        emplaceOrInsert(0, Roaring(s));
    }

    Roaring64Map(const Roaring64Map& r) = default;

    Roaring64Map(Roaring64Map&& r) noexcept = default;

    /**
     * Copy assignment operator.
     */
    Roaring64Map &operator=(const Roaring64Map &r) = default;

    /**
     * Move assignment operator.
     */
    Roaring64Map &operator=(Roaring64Map &&r) noexcept = default;

    /**
     * Constructs a bitmap from a list of integer values.
     */
    static Roaring64Map bitmapOf(size_t n...) {
        Roaring64Map ans;
        va_list vl;
        va_start(vl, n);
        for (size_t i = 0; i < n; i++) {
            ans.add(va_arg(vl, uint64_t));
        }
        va_end(vl);
        return ans;
    }

    /**
     * Constructs a bitmap from an initializer_list.
     */
    static Roaring64Map bitmapOf(std::initializer_list<uint64_t> list) {
        Roaring64Map result;
        for (auto data : list) {
            result.add(data);
        }
        return result;
    }

    /**
     * Adds value x.
     */
    void add(uint32_t x) {
        auto &bitmap = roarings[0];
        bitmap.add(x);
        bitmap.setCopyOnWrite(copyOnWrite);
    }

    /**
     * Adds value x.
     */
    void add(uint64_t x) {
        auto &bitmap = roarings[highBytes(x)];
        bitmap.add(lowBytes(x));
        bitmap.setCopyOnWrite(copyOnWrite);
    }

    /**
     * Adds value x.
     * Returns true if a new value was added, false if the value was already
     * present.
     */
    bool addChecked(uint32_t x) {
        auto &bitmap = roarings[0];
        bool result = bitmap.addChecked(x);
        bitmap.setCopyOnWrite(copyOnWrite);
        return result;
    }

    /**
     * Adds value x.
     * Returns true if a new value was added, false if the value was already
     * present.
     */
    bool addChecked(uint64_t x) {
        auto &bitmap = roarings[highBytes(x)];
        bool result = bitmap.addChecked(lowBytes(x));
        bitmap.setCopyOnWrite(copyOnWrite);
        return result;
    }

    /**
     * Adds all values in the half-open interval [min, max).
     */
    void addRange(uint64_t min, uint64_t max) {
        if (min >= max) {
            return;
        }
        addRangeClosed(min, max - 1);
    }

    /**
     * Adds all values in the closed interval [min, max].
     */
    void addRangeClosed(uint32_t min, uint32_t max) {
        auto &bitmap = roarings[0];
        bitmap.addRangeClosed(min, max);
    }

    /**
     * Adds all values in the closed interval [min, max]
     */
    void addRangeClosed(uint64_t min, uint64_t max) {
        if (min > max) {
            return;
        }
        uint32_t start_high = highBytes(min);
        uint32_t start_low = lowBytes(min);
        uint32_t end_high = highBytes(max);
        uint32_t end_low = lowBytes(max);

        // We put std::numeric_limits<>::max in parentheses to avoid a
        // clash with the Windows.h header under Windows.
        const uint32_t maxUint32 = (std::numeric_limits<uint32_t>::max)();

        // If start and end land on the same inner bitmap, then we can do the
        // whole operation in one call.
        if (start_high == end_high) {
            auto &bitmap = roarings[start_high];
            bitmap.addRangeClosed(start_low, end_low);
            bitmap.setCopyOnWrite(copyOnWrite);
            return;
        }

        // Because start and end don't land on the same inner bitmap,
        // we need to do this in multiple steps:
        // 1. Partially fill the first bitmap with values from the closed
        //    interval [start_low, maxUint32]
        // 2. Fill intermediate bitmaps completely: [0, maxUint32]
        // 3. Partially fill the last bitmap with values from the closed
        //    interval [0, end_low]

        // Step 1: we do this inside a nested scope so that 'bitmap' doesn't
        // leak out and confuse us.
        {
            auto &bitmap = roarings[start_high++];
            bitmap.addRangeClosed(start_low, maxUint32);
            bitmap.setCopyOnWrite(copyOnWrite);
        }

        // Step 2: fill intermediate bitmaps completely.
        // TODO(kosak): How slow is this? Faster to do an empty bitmap, then
        // flip it? And/or once you've made one full bitmap, can you copy it
        // to reuse it?
        for (; start_high < end_high; ++start_high) {
            auto &bitmap = roarings[start_high];
            bitmap.addRangeClosed(0, maxUint32);
            bitmap.setCopyOnWrite(copyOnWrite);
        }

        // Step 3: Partially fill the last bitmap.
        auto &bitmap = roarings[end_high];
        bitmap.addRangeClosed(0, end_low);
        bitmap.setCopyOnWrite(copyOnWrite);
    }

    /**
     * Adds 'n_args' values from the contiguous memory range starting at 'vals'.
     */
    void addMany(size_t n_args, const uint32_t *vals) {
        auto &innerBitmap = roarings[0];
        innerBitmap.addMany(n_args, vals);
        innerBitmap.setCopyOnWrite(copyOnWrite);
    }

    /**
     * Adds 'n_args' values from the contiguous memory range starting at 'vals'.
     */
    void addMany(size_t n_args, const uint64_t *vals) {
        // Potentially reduce outer map lookups by optimistically
        // assuming that adjacent values will belong to the same inner bitmap.
        Roaring *lastInnerBitmap = nullptr;
        uint32_t last_value_high = 0;
        for (size_t lcv = 0; lcv < n_args; lcv++) {
            auto value = vals[lcv];
            auto value_high = highBytes(value);
            auto value_low = lowBytes(value);
            if (lastInnerBitmap == nullptr || value_high != last_value_high) {
                lastInnerBitmap = &roarings[value_high];
                last_value_high = value_high;
            }
            lastInnerBitmap->add(value_low);
            lastInnerBitmap->setCopyOnWrite(copyOnWrite);
        }
    }

    /**
     * Removes value x.
     */
    void remove(uint32_t x) {
        auto &bitmap = roarings[0];
        bitmap.remove(x);
    }

    /**
     * Removes value x.
     */
    void remove(uint64_t x) {
        auto iter = roarings.find(highBytes(x));
        if (iter != roarings.end()) {
            auto &bitmap = iter->second;
            bitmap.remove(lowBytes(x));
        }
    }

    /**
     * Removes value x
     * Returns true if a new value was removed, false if the value was not present.
     */
    bool removeChecked(uint32_t x) {
        auto &bitmap = roarings[0];
        return bitmap.removeChecked(x);
    }

    /**
     * Remove value x
     * Returns true if a new value was removed, false if the value was not existing.
     */
    bool removeChecked(uint64_t x) {
        auto iter = roarings.find(highBytes(x));
        if (iter != roarings.end()) {
            auto &bitmap = iter->second;
            return bitmap.removeChecked(lowBytes(x));
        }
        return false;
    }

    /**
     * Removes all values in the half-open interval [min, max).
     */
    void removeRange(uint64_t min, uint64_t max) {
        if (min >= max) {
            return;
        }
        return removeRangeClosed(min, max - 1);
    }

    /**
     * Removes all values in the closed interval [min, max].
     */
    void removeRangeClosed(uint32_t min, uint32_t max) {
        auto &bitmap = roarings[0];
        return bitmap.removeRangeClosed(min, max);
    }

    /**
     * Removes all values in the closed interval [min, max].
     */
    void removeRangeClosed(uint64_t min, uint64_t max) {
        if (min > max) {
            return;
        }
        uint32_t start_high = highBytes(min);
        uint32_t start_low = lowBytes(min);
        uint32_t end_high = highBytes(max);
        uint32_t end_low = lowBytes(max);

        // We put std::numeric_limits<>::max in parentheses to avoid a
        // clash with the Windows.h header under Windows.
        const uint32_t maxUint32 = (std::numeric_limits<uint32_t>::max)();

        // If the outer map is empty, end_high is less than the first key,
        // or start_high is greater than the last key, then exit now because
        // there is no work to do.
        if (roarings.empty() || end_high < roarings.cbegin()->first ||
            start_high > (roarings.crbegin())->first) {
            return;
        }

        // If we get here, start_iter points to the first entry in the outer map
        // with key >= start_high. Such an entry is known to exist (i.e. the
        // iterator will not be equal to end()) because start_high <= the last
        // key in the map (thanks to the above if statement).
        auto start_iter = roarings.lower_bound(start_high);
        // end_iter points to the first entry in the outer map with
        // key >= end_high, if such a key exists. Otherwise, it equals end().
        auto end_iter = roarings.lower_bound(end_high);

        // Preview of the remaining steps:
        // 1. If the start point falls on an existing entry (rather than
        //    before it), there are two subcases:
        //    a) if the end point falls on the same entry, remove the closed
        //       interval [start_low, end_low] from that entry and exit.
        //    b) Otherwise, remove the closed range [start_low, maxUint32] from
        //       that entry, advance start_iter, and fall through to step 2.
        // 2. Completely erase everything in the half-open interval
        //    [start_iter, end_iter)
        // 3. If the end point falls on an existing entry (rather then beyond
        //    it), remove the closed interval [0, end_high] from that entry.

        // Step 1. If the start point falls on an existing entry...
        if (start_iter->first == start_high) {
            auto &start_inner = start_iter->second;
            // Now, if the end point falls on the same inner bitmap as the
            // start, then we remove the closed interval [start_low, end_low]
            // from that inner bitmap and we are done.
            if (start_iter == end_iter) {
                start_inner.removeRangeClosed(start_low, end_low);
                return;
            }

            // Otherwise (if the end point does not fall on the same inner
            // bitmap), then remove the closed interval [start_low, maxUint32]
            // from the inner bitmap, and then fall through to later logic.
            start_inner.removeRangeClosed(start_low, maxUint32);
            ++start_iter;
        }

        // Step 2., Completely erase everything in the middle interval...
        roarings.erase(start_iter, end_iter);

        // Step 3. If end_iter happens to point to an entry with key == end_high
        // (rather than the other two possibilities, namely key > end_high or
        // end()), then remove the closed range [0, end_low] from that inner
        // bitmap.
        if (end_iter != roarings.end() && end_iter->first == end_high) {
            auto &end_inner = end_iter->second;
            end_inner.removeRangeClosed(0, end_low);
        }
    }

    /**
     * Clears the bitmap.
     */
    void clear() {
        roarings.clear();
    }

    /**
     * Return the largest value present in the bitmap. If the bitmap is empty,
     * return 0. If this method returns 0 and you need to distinguish the
     * empty bitmap from one containing the sole element 0, you can then call
     * isEmpty().
     */
    uint64_t maximum() const {
        auto iter = roarings.end();
        while (iter != roarings.begin()) {
            --iter;
            if (!iter->second.isEmpty()) {
                return uniteBytes(iter->first, iter->second.maximum());
            }
        }
        return 0;
    }

    // Leave this here for now so the two implementations can be benchmarked
    // side by side.

    /**
     * Return the largest value (if not empty).
     */
    uint64_t maximum_legacy_impl() const {
        for (auto roaring_iter = roarings.crbegin();
             roaring_iter != roarings.crend(); ++roaring_iter) {
            if (!roaring_iter->second.isEmpty()) {
                return uniteBytes(roaring_iter->first,
                                  roaring_iter->second.maximum());
            }
        }
        // we put std::numeric_limits<>::max/min in parentheses
        // to avoid a clash with the Windows.h header under Windows
        return (std::numeric_limits<uint64_t>::min)();
    }


    /**
     * Return the smallest value present in the bitmap. If the bitmap is empty,
     * return std::numeric_limits<uint64_t>::max(). If this method returns
     * std::numeric_limits<uint64_t>::max() and you need to distinguish the
     * empty bitmap from one containing the sole element
     * std::numeric_limits<uint64_t>::max(), you can then call isEmpty().
     */
    uint64_t minimum() const {
        for (const auto &entry : roarings) {
            auto innerKey = entry.first;
            const auto &innerBitmap = entry.second;
            if (!innerBitmap.isEmpty()) {
                return uniteBytes(innerKey, innerBitmap.minimum());
            }
        }
        // we put std::numeric_limits<>::max/min in parentheses
        // to avoid a clash with the Windows.h header under Windows
        return (std::numeric_limits<uint64_t>::max)();
    }

    /**
     * Returns true if x is contained in the bitmap. Otherwise, returns false.
     */
    bool contains(uint32_t x) const {
        auto iter = roarings.find(0);
        return iter != roarings.end() && iter->second.contains(x);
    }

    /**
     * Returns true if x is contained in the bitmap. Otherwise, returns false.
     */
    bool contains(uint64_t x) const {
        auto iter = roarings.find(highBytes(x));
        return iter != roarings.end() && iter->second.contains(lowBytes(x));
    }

    /**
     * Compute the intersection of the current bitmap and the provided bitmap,
     * writing the result in the current bitmap. The provided bitmap is not
     * modified.
     */
    Roaring64Map &operator&=(const Roaring64Map &other) {
        if (this == &other) {
            // ANDing with ourself is a no-op.
            return *this;
        }

        auto self_next = roarings.begin();  // Placeholder value, replaced below
        for (auto self_iter = roarings.begin(); self_iter != roarings.end();
             self_iter = self_next) {
            // Do the 'next' operation early because we might invalidate
            // self_iter down below with the 'erase' operation.
            self_next = std::next(self_iter);

            auto self_key = self_iter->first;
            auto &self_bitmap = self_iter->second;

            auto other_iter = other.roarings.find(self_key);
            if (other_iter == other.roarings.end()) {
                // 'other' doesn't have self_key. This means that the result of
                // the intersection is empty and self should erase its whole
                // inner bitmap here.
                roarings.erase(self_iter);
                continue;
            }

            // Both sides have self_key so we need to compute the intersection.
            const auto &other_bitmap = other_iter->second;
            self_bitmap &= other_bitmap;
            if (self_bitmap.isEmpty()) {
                // The intersection operation has resulted in an empty bitmap.
                // So remove it from the map altogether.
                roarings.erase(self_iter);
            }
        }
        return *this;
    }

    /**
     * Compute the difference between the current bitmap and the provided
     * bitmap, writing the result in the current bitmap. The provided bitmap
     * is not modified.
     */
    Roaring64Map &operator-=(const Roaring64Map &other) {
        if (this == &other) {
            // Subtracting from ourself results in the empty map.
            roarings.clear();
            return *this;
        }

        auto self_next = roarings.begin();  // Placeholder value, replaced below
        for (auto self_iter = roarings.begin(); self_iter != roarings.end();
             self_iter = self_next) {
            // Do the 'next' operation early because we might invalidate
            // self_iter down below with the 'erase' operation.
            self_next = std::next(self_iter);

            auto self_key = self_iter->first;
            auto &self_bitmap = self_iter->second;

            auto other_iter = other.roarings.find(self_key);
            if (other_iter == other.roarings.end()) {
                // 'other' doesn't have self_key. This means that the
                // self_bitmap can be left alone (there is nothing to subtract
                // from it) and we can move on.
                continue;
            }

            // Both sides have self_key so we need to compute the difference.
            const auto &other_bitmap = other_iter->second;
            self_bitmap -= other_bitmap;

            // If the difference operation caused the inner bitmap to become
            // empty, remove it from the map.
            if (self_bitmap.isEmpty()) {
                roarings.erase(self_iter);
            }
        }
        return *this;
    }

    /**
     * Compute the union of the current bitmap and the provided bitmap,
     * writing the result in the current bitmap. The provided bitmap is not
     * modified.
     *
     * See also the fastunion function to aggregate many bitmaps more quickly.
     */
    Roaring64Map &operator|=(const Roaring64Map &other) {
        if (this == &other) {
            // ORing with ourself is a no-op.
            return *this;
        }

        for (const auto &other_entry : other.roarings) {
            const auto &other_bitmap = other_entry.second;

            // Try to insert other_bitmap into self at other_key. We take
            // advantage of the fact that insert will not overwrite an
            // existing key.
            auto insert_result = roarings.insert(other_entry);
            auto self_iter = insert_result.first;
            auto insert_happened = insert_result.second;
            auto &self_bitmap = self_iter->second;

            if (insert_happened) {
                // Key not present in self, so insert was performed, reflecting
                // the operation (empty | X) == X
                // The bitmap has been copied, so we just need to set the
                // copyOnWrite flag.
                self_bitmap.setCopyOnWrite(copyOnWrite);
                continue;
            }

            // Key was already present in self, so insert not performed.
            // So we have to union the other bitmap with self.
            self_bitmap |= other_bitmap;
        }
        return *this;
    }

    /**
     * Compute the XOR of the current bitmap and the provided bitmap, writing
     * the result in the current bitmap. The provided bitmap is not modified.
     */
    Roaring64Map &operator^=(const Roaring64Map &other) {
        if (this == &other) {
            // XORing with ourself results in the empty map.
            roarings.clear();
            return *this;
        }

        for (const auto &other_entry : other.roarings) {
            const auto &other_bitmap = other_entry.second;

            // Try to insert other_bitmap into self at other_key. We take
            // advantage of the fact that insert will not overwrite an
            // existing key.
            auto insert_result = roarings.insert(other_entry);
            auto self_iter = insert_result.first;
            auto insert_happened = insert_result.second;
            auto &self_bitmap = self_iter->second;

            if (insert_happened) {
                // Key not present in self, so insert was performed, reflecting
                // the operation (empty ^ X) == X
                // The bitmap has been copied, so we just need to set the
                // copyOnWrite flag.
                self_bitmap.setCopyOnWrite(copyOnWrite);
                continue;
            }

            // Key was already present in self, so insert not performed.
            // So we have to union the other bitmap with self.
            self_bitmap ^= other_bitmap;

            // The XOR operation might have caused the inner Roaring to become
            // empty (if self_bitmap == other_bitmap). If so, remove it from the
            // map.
            if (self_bitmap.isEmpty()) {
                roarings.erase(self_iter);
            }
        }
        return *this;
    }

    /**
     * Exchange the content of this bitmap with another.
     */
    void swap(Roaring64Map &r) { roarings.swap(r.roarings); }

    /**
     * Get the cardinality of the bitmap (number of elements).
     * Throws std::length_error or terminates in the special case where the
     * bitmap is completely full (cardinality() == 2^64). If this is a
     * possibility in your application, consider calling cardinality_nothrow()
     * instead.
     */
    uint64_t cardinality() const {
        auto result = cardinality_nothrow();
        if (!result.second) {
            return result.first;
        }

        const char *errorMessage = "bitmap is full, cardinality is 2^64, "
            "unable to represent in a 64-bit integer";

#if ROARING_EXCEPTIONS
        throw std::length_error(errorMessage);
#else
        ROARING_TERMINATE(errorMessage);
#endif
    }

    /**
     * Get the cardinality of the bitmap (number of elements).
     * Returns {0, true} if the bitmap is completely full
     * (cardinality == 2^64). Otherwise, returns {cardinality, false}.
     */
    std::pair<uint64_t, bool> cardinality_nothrow() const {
        // we put std::numeric_limits<>::max/min in parentheses
        // to avoid a clash with the Windows.h header under Windows
        const uint64_t maxCardinality =
            ((uint64_t)(std::numeric_limits<uint32_t>::max)()) + 1;

        uint64_t result = 0;
        auto allBitmapsAreMaxCardinality = true;
        for (const auto &entry : roarings) {
            const auto &bitmap = entry.second;
            auto bc = bitmap.cardinality();
            if (bc != maxCardinality) {
                allBitmapsAreMaxCardinality = false;
            }
            result += bc;
        }

        if (roarings.size() == maxCardinality && allBitmapsAreMaxCardinality) {
            return {0, true};
        }

        return {result, false};
    }

    /**
     * Returns true if the bitmap is empty (cardinality is zero).
     */
    bool isEmpty() const {
        for (const auto &entry : roarings) {
            if (!entry.second.isEmpty()) {
                return false;
            }
        }
        return true;
    }

    /**
     * Returns true if the bitmap is full (cardinality is max uint64_t + 1).
     */
    bool isFull() const {
        auto result = cardinality_nothrow();
        return result.second;
    }

    /**
     * Returns true if this Roaring64Map is a subset (strict or not) of the
     * other. Otherwise, returns false.
     */
    bool isSubset(const Roaring64Map &other) const {
        return isSubset(other, false);
    }

    /**
     * Returns true if this Roaring64Map is a strict subset of 'other'.
     * Otherwise, returns false.
     */
    bool isStrictSubset(const Roaring64Map &other) const {
        return isSubset(other, true);
    }

    /**
     * If requireStrict is true: returns true if this Roaring64Map is a
     * strict subset of 'other'.
     *
     * If requireStrict is false: returns true if this Roaring64Map is a
     * (strict or not) subset of 'other'.
     *
     * Otherwise, returns false.
     */
    bool isSubset(const Roaring64Map &other, bool requireStrict) const {
        // Once we know that this Roaring64Map is a subset of other, we *could*
        // determine whether it's a strict subset by comparing cardinalities.
        // However determining cardinality is a relatively expensive operation.
        // We can do better by just observing the properties of the inner
        // bitmaps as we process them. In particular:
        // The condition "this Roaring64Map is a strict subset of other" is true
        // if at least one of the below holds:
        // (a) At least one inner bitmap is a strict subset of the corresponding
        //     other bitmap.
        // (b) There is a non-empty bitmap in "other" that does not exist (or is
        //     empty) in "this".

        // We track condition (a) with this flag.
        auto someBitmapIsStrictSubset = false;

        for (const auto &self_entry : roarings) {
            auto self_key = self_entry.first;
            const auto &self_bitmap = self_entry.second;
            if (self_bitmap.isEmpty()) {
                continue;
            }

            // self_bitmap is a non-empty bitmap. In order for the isSubset
            // operation to succeed, there needs to be a corresponding set
            // in other that self_bitmap can be a subset of.
            auto other_iter = other.roarings.find(self_key);
            if (other_iter == other.roarings.end()) {
                // Other does not have self_key. Therefore, isSubset fails.
                return false;
            }

            const auto &other_bitmap = other_iter->second;

            // Both sides have self_key. Is self_bitmap a subset of other_bitmap?
            if (!self_bitmap.isSubset(other_bitmap)) {
                // self_bitmap is not a subset of other_bitmap. isSubset fails.
                return false;
            }

            // self_bitmap is a subset of other_bitmap. But is it a proper
            // subset? If we care, we only need to find one to satisfy ourselves.
            if (requireStrict && !someBitmapIsStrictSubset) {
                if (self_bitmap.cardinality() != other_bitmap.cardinality()) {
                    // self_bitmap is a subset of other_bitmap, but their
                    // cardinalities differ. So it must be a proper subset.
                    someBitmapIsStrictSubset = true;
                }
            }
        }

        // At this point, all inner bitmaps are confirmed to be subsets of
        // 'other'. If 'requireStrict' is false, then we can exit successfully.
        if (!requireStrict) {
            return true;
        }

        // 'requireStrict' is true. If at least one inner bitmap was a strict
        // subset, that is good enough to exit with success. See condition (a)
        // at the top of this method.
        if (someBitmapIsStrictSubset) {
            return true;
        }

        // 'requireStrict' is true but condition (a) failed. Test condition (b)
        for (const auto &other_entry : other.roarings) {
            auto other_key = other_entry.first;
            const auto &other_bitmap = other_entry.second;
            if (other_bitmap.isEmpty()) {
                // Empty bitmaps don't count
                continue;
            }

            auto self_iter = roarings.find(other_key);
            if (self_iter == roarings.end() || self_iter->second.isEmpty()) {
                // There is a non-empty bitmap in 'other'. The corresponding
                // bitmap either doesn't exist in 'self', or it does exist but
                // is empty. In either case, that proves strict subset.
                return true;
            }
        }

        // It's a subset, but it's not strict.
        return false;
    }

    /**
     * Converts the bitmap to an array. Writes the output to "ans".
     * The caller is responsible to ensure that there is enough memory
     * allocated (e.g., ans = new uint64_t[mybitmap.cardinality()];)
     */
    void toUint64Array(uint64_t *ans) const {
        for (const auto &entry : roarings) {
            auto key = entry.first;
            const auto &bitmap = entry.second;

            for (uint32_t low_bits : bitmap) {
                *ans++ = uniteBytes(key, low_bits);
            }
        }
    }

    /**
     * Return true if the two bitmaps contain the same elements. Otherwise,
     * return false.
     */
    bool operator==(const Roaring64Map &other) const {
        // We cannot simply use operator == on the outer map because either side
        // may contain empty Roaring Bitmaps.
        auto self_iter = roarings.begin();
        auto self_end = roarings.end();
        auto other_iter = other.roarings.begin();
        auto other_end = other.roarings.end();
        while (true) {
            // Advance self_iter past empty bitmaps.
            while (self_iter != self_end && self_iter->second.isEmpty()) {
                ++self_iter;
            }

            // Advance other_iter past empty bitmaps.
            while (other_iter != other_end && other_iter->second.isEmpty()) {
                ++other_iter;
            }

            // self_iter is either at end or at a non-empty bitmap.
            // The same holds for other_iter.

            // 1. If both are at end, then self and other are equal.
            // 2. If one is at end and one isn't, then self and other are unequal.
            // 3. If neither is at end, compare the keys and the bitmap. If they
            //    match, we need to keep going. If they don't match we have
            //    proved unequal so we are done.
            if (self_iter == self_end || other_iter == other_end) {
                // Success if both are at end. Failure if only one is at end.
                return self_iter == self_end && other_iter == other_end;
            }

            // self_iter and other_iter both point to two non-empty entries.
            auto self_key = self_iter->first;
            const auto &self_bitmap = self_iter->second;

            auto other_key = other_iter->first;
            const auto &other_bitmap = other_iter->second;

            if (self_key != other_key || !(self_bitmap == other_bitmap)) {
                // Either the keys differ, or the keys are the same but the
                // bitmaps differ.
                return false;
            }

            ++self_iter;
            ++other_iter;
        }
    }

    /**
     * Computes the negation of the roaring bitmap within the half-open interval
     * [range_start, range_end). Areas outside the interval are unchanged.
     */
    void flip(uint64_t range_start, uint64_t range_end) {
        if (range_start >= range_end) {
            return;
        }
        flipClosed(range_start, range_end - 1);
    }

    /**
     * Computes the negation of the roaring bitmap within the closed interval
     * [range_start, range_end]. Areas outside the interval are unchanged.
     */
    void flipClosed(uint64_t range_start, uint64_t range_end) {
        if (range_start > range_end) {
          return;
        }
        uint32_t start_high = highBytes(range_start);
        uint32_t start_low = lowBytes(range_start);
        uint32_t end_high = highBytes(range_end);
        uint32_t end_low = lowBytes(range_end);

        // We put std::numeric_limits<>::max in parentheses to avoid a
        // clash with the Windows.h header under Windows.
        const uint32_t maxUint32 = (std::numeric_limits<uint32_t>::max)();

        // If start and end land on the same inner bitmap, then we can do the
        // whole operation in one call.
        if (start_high == end_high) {
            auto &bitmap = roarings[start_high];
            flipClosed(&bitmap, start_low, end_low);
            bitmap.setCopyOnWrite(copyOnWrite);
            return;
        }

        // Because start and end don't land on the same inner bitmap,
        // we need to do this in multiple steps:
        // 1. Partially flip the first bitmap at values from the closed
        //    interval [start_low, maxUint32]
        // 2. Flip intermediate bitmaps completely: [0, maxUint32]
        // 3. Partially flip the last bitmap with values from the closed
        //    interval [0, end_low]

        // Step 1. Partially flip the first bitmap.
        {
            auto &bitmap = roarings[start_high++];
            flipClosed(&bitmap, start_low, maxUint32);
            bitmap.setCopyOnWrite(copyOnWrite);
        }

        // Step 2. Flip intermediate bitmaps completely.
        for (; start_high < end_high; ++start_high) {
            auto &bitmap = roarings[start_high];
            flipClosed(&bitmap, 0, maxUint32);
            bitmap.setCopyOnWrite(copyOnWrite);
        }

        // Step 3. Partially flip the last bitmap.
        auto &bitmap = roarings[end_high];
        flipClosed(&bitmap, 0, end_low);
        bitmap.setCopyOnWrite(copyOnWrite);
    }

    // Because the Roaring bitmap does not have a method flipClosed (TODO)
    // we provide our own method here which adjusts the coordinates and calls
    // flip().
    static void flipClosed(Roaring *bitmap, uint32_t start, uint32_t end) {
        auto exclusive_end = uint64_t(end) + 1;
        bitmap->flip(start, exclusive_end);
    }

    /**
     * Remove run-length encoding even when it is more space efficient
     * return whether a change was applied
     */
    bool removeRunCompression() {
        return std::accumulate(
            roarings.begin(), roarings.end(), true,
            [](bool previous, std::pair<const uint32_t, Roaring> &map_entry) {
                return map_entry.second.removeRunCompression() && previous;
            });
    }

    /**
     * Convert array and bitmap containers to run containers when it is more
     * efficient; also convert from run containers when more space efficient.
     * Returns true if the result has at least one run container.
     * Additional savings might be possible by calling shrinkToFit().
     */
    bool runOptimize() {
        return std::accumulate(
            roarings.begin(), roarings.end(), true,
            [](bool previous, std::pair<const uint32_t, Roaring> &map_entry) {
                return map_entry.second.runOptimize() && previous;
            });
    }

    /**
     * If needed, reallocate memory to shrink the memory usage.
     * Returns the number of bytes saved.
     */
    size_t shrinkToFit() {
        size_t savedBytes = 0;
        auto iter = roarings.begin();
        while (iter != roarings.end()) {
            auto next_iter = std::next(iter);
            if (iter->second.isEmpty()) {
                // empty Roarings are 84 bytes
                savedBytes += 88;
                roarings.erase(iter);
            } else {
                savedBytes += iter->second.shrinkToFit();
            }
            iter = next_iter;
        }
        return savedBytes;
    }

    /**
     * Iterate over the bitmap elements in order(start from the smallest one)
     * and call iterator once for every element until the iterator function
     * returns false. To iterate over all values, the iterator function should
     * always return true.
     *
     * The roaring_iterator64 parameter is a pointer to a function that
     * returns bool (true means that the iteration should continue while false
     * means that it should stop), and takes (uint64_t element, void* ptr) as
     * inputs.
     */
    void iterate(api::roaring_iterator64 iterator, void *ptr) const {
        for (const auto &map_entry : roarings) {
            auto key = map_entry.first;
            const auto &bitmap = map_entry.second;

            auto highBits = uniteBytes(key, 0);

            bool should_continue = roaring_iterate64(&bitmap.roaring, iterator,
                                                     highBits, ptr);
            if (!should_continue) {
                break;
            }
        }
    }

    /**
     * If the size of the roaring bitmap is strictly greater than rank, then
     * this function returns true and set element to the element of given
     * rank.  Otherwise, it returns false and the contents of *element are
     * unspecified.
     */
    bool select(uint64_t rank, uint64_t *element) const {
        for (const auto &map_entry : roarings) {
            auto key = map_entry.first;
            const auto &bitmap = map_entry.second;

            auto sub_cardinality = bitmap.cardinality();
            if (rank < sub_cardinality) {
                uint32_t low_bytes;
                if (!bitmap.select((uint32_t)rank, &low_bytes)) {
                    return false;
                }
                *element = uniteBytes(key, low_bytes);
                return true;
            }
            rank -= sub_cardinality;
        }
        return false;
    }

    /**
     * Returns the number of integers that are smaller or equal to x.
     */
    uint64_t rank(uint64_t x) const {
        uint64_t result = 0;
        // dest_iter points to the first entry having
        // key >= highBytes(x), or it points to end()
        auto dest_iter = roarings.lower_bound(highBytes(x));

        // Add all the cardinalities of map entries with keys < highBytes(x)
        for (auto iter = roarings.begin(); iter != dest_iter; ++iter) {
            result += iter->second.cardinality();
        }

        // If dest_iter happens to point to a key == highBytes(x)
        // (rather than the other two possibilities, namely > highBytes(x) or
        // end()), then include the rank of lowBytes(x).
        if (dest_iter != roarings.end() && dest_iter->first == highBytes(x)) {
            result += dest_iter->second.rank(lowBytes(x));
        }
        return result;
    }

    /**
     * Write a bitmap to a char buffer. This is meant to be compatible with
     * the Java and Go versions. Returns how many bytes were written which
     * should be getSizeInBytes().
     *
     * Setting the portable flag to false enables a custom format that
     * can save space compared to the portable format (e.g., for very
     * sparse bitmaps).
     */
    size_t write(char *buf, bool portable = true) const {
        const char *orig = buf;
        // push map size
        uint64_t map_size = roarings.size();
        std::memcpy(buf, &map_size, sizeof(uint64_t));
        buf += sizeof(uint64_t);
        std::for_each(
            roarings.cbegin(), roarings.cend(),
            [&buf, portable](const std::pair<const uint32_t, Roaring> &map_entry) {
                // push map key
                std::memcpy(buf, &map_entry.first, sizeof(uint32_t));
                // ^-- Note: `*((uint32_t*)buf) = map_entry.first;` is undefined

                buf += sizeof(uint32_t);
                // push map value Roaring
                buf += map_entry.second.write(buf, portable);
            });
        return buf - orig;
    }

    /**
     * Read a bitmap from a serialized version. This is meant to be compatible
     * with the Java and Go versions.
     *
     * Setting the portable flag to false enable a custom format that
     * can save space compared to the portable format (e.g., for very
     * sparse bitmaps).
     *
     * This function is unsafe in the sense that if you provide bad data, many
     * bytes could be read, possibly causing a buffer overflow. See also
     * readSafe.
     */
    static Roaring64Map read(const char *buf, bool portable = true) {
        Roaring64Map result;
        // get map size
        uint64_t map_size;
        std::memcpy(&map_size, buf, sizeof(uint64_t));
        buf += sizeof(uint64_t);
        for (uint64_t lcv = 0; lcv < map_size; lcv++) {
            // get map key
            uint32_t key;
            std::memcpy(&key, buf, sizeof(uint32_t));
            // ^-- Note: `uint32_t key = *((uint32_t*)buf);` is undefined

            buf += sizeof(uint32_t);
            // read map value Roaring
            Roaring read_var = Roaring::read(buf, portable);
            // forward buffer past the last Roaring Bitmap
            buf += read_var.getSizeInBytes(portable);
            result.emplaceOrInsert(key, std::move(read_var));
        }
        return result;
    }

    /**
     * Read a bitmap from a serialized version, reading no more than maxbytes
     * bytes.  This is meant to be compatible with the Java and Go versions.
     *
     * Setting the portable flag to false enable a custom format that can save
     * space compared to the portable format (e.g., for very sparse bitmaps).
     */
    static Roaring64Map readSafe(const char *buf, size_t maxbytes) {
        if (maxbytes < sizeof(uint64_t)) {
            ROARING_TERMINATE("ran out of bytes");
        }
        Roaring64Map result;
        uint64_t map_size;
        std::memcpy(&map_size, buf, sizeof(uint64_t));
        buf += sizeof(uint64_t);
        maxbytes -= sizeof(uint64_t);
        for (uint64_t lcv = 0; lcv < map_size; lcv++) {
            if(maxbytes < sizeof(uint32_t)) {
                ROARING_TERMINATE("ran out of bytes");
            }
            uint32_t key;
            std::memcpy(&key, buf, sizeof(uint32_t));
            // ^-- Note: `uint32_t key = *((uint32_t*)buf);` is undefined

            buf += sizeof(uint32_t);
            maxbytes -= sizeof(uint32_t);
            // read map value Roaring
            Roaring read_var = Roaring::readSafe(buf, maxbytes);
            // forward buffer past the last Roaring Bitmap
            size_t tz = read_var.getSizeInBytes(true);
            buf += tz;
            maxbytes -= tz;
            result.emplaceOrInsert(key, std::move(read_var));
        }
        return result;
    }

    /**
     * Return the number of bytes required to serialize this bitmap (meant to
     * be compatible with Java and Go versions)
     *
     * Setting the portable flag to false enable a custom format that can save
     * space compared to the portable format (e.g., for very sparse bitmaps).
     */
    size_t getSizeInBytes(bool portable = true) const {
        // start with, respectively, map size and size of keys for each map
        // entry
        return std::accumulate(
            roarings.cbegin(), roarings.cend(),
            sizeof(uint64_t) + roarings.size() * sizeof(uint32_t),
            [=](size_t previous,
                const std::pair<const uint32_t, Roaring> &map_entry) {
                // add in bytes used by each Roaring
                return previous + map_entry.second.getSizeInBytes(portable);
            });
    }

    static const Roaring64Map frozenView(const char *buf) {
        // size of bitmap buffer and key
        const size_t metadata_size = sizeof(size_t) + sizeof(uint32_t);

        Roaring64Map result;

        // get map size
        uint64_t map_size;
        memcpy(&map_size, buf, sizeof(uint64_t));
        buf += sizeof(uint64_t);

        for (uint64_t lcv = 0; lcv < map_size; lcv++) {
            // pad to 32 bytes minus the metadata size
            while (((uintptr_t)buf + metadata_size) % 32 != 0) buf++;

            // get bitmap size
            size_t len;
            memcpy(&len, buf, sizeof(size_t));
            buf += sizeof(size_t);

            // get map key
            uint32_t key;
            memcpy(&key, buf, sizeof(uint32_t));
            buf += sizeof(uint32_t);

            // read map value Roaring
            const Roaring read = Roaring::frozenView(buf, len);
            result.emplaceOrInsert(key, read);

            // forward buffer past the last Roaring Bitmap
            buf += len;
        }
        return result;
    }

    // As with serialized 64-bit bitmaps, 64-bit frozen bitmaps are serialized
    // by concatenating one or more Roaring::write output buffers with the
    // preceeding map key. Unlike standard bitmap serialization, frozen bitmaps
    // must be 32-byte aligned and requires a buffer length to parse. As a
    // result, each concatenated output of Roaring::writeFrozen is preceeded by
    // padding, the buffer size (size_t), and the map key (uint32_t). The
    // padding is used to ensure 32-byte alignment, but since it is followed by
    // the buffer size and map key, it actually pads to `(x - sizeof(size_t) +
    // sizeof(uint32_t)) mod 32` to leave room for the metadata.
    void writeFrozen(char *buf) const {
        // size of bitmap buffer and key
        const size_t metadata_size = sizeof(size_t) + sizeof(uint32_t);

        // push map size
        uint64_t map_size = roarings.size();
        memcpy(buf, &map_size, sizeof(uint64_t));
        buf += sizeof(uint64_t);

        for (auto &map_entry : roarings) {
            size_t frozenSizeInBytes = map_entry.second.getFrozenSizeInBytes();

            // pad to 32 bytes minus the metadata size
            while (((uintptr_t)buf + metadata_size) % 32 != 0) buf++;

            // push bitmap size
            memcpy(buf, &frozenSizeInBytes, sizeof(size_t));
            buf += sizeof(size_t);

            // push map key
            memcpy(buf, &map_entry.first, sizeof(uint32_t));
            buf += sizeof(uint32_t);

            // push map value Roaring
            map_entry.second.writeFrozen(buf);
            buf += map_entry.second.getFrozenSizeInBytes();
        }
    }

    size_t getFrozenSizeInBytes() const {
        // size of bitmap size and map key
        const size_t metadata_size = sizeof(size_t) + sizeof(uint32_t);
        size_t ret = 0;

        // map size
        ret += sizeof(uint64_t);

        for (auto &map_entry : roarings) {
            // pad to 32 bytes minus the metadata size
            while ((ret + metadata_size) % 32 != 0) ret++;
            ret += metadata_size;

            // frozen bitmaps must be 32-byte aligned
            ret += map_entry.second.getFrozenSizeInBytes();
        }
        return ret;
    }

    /**
     * Computes the intersection between two bitmaps and returns new bitmap.
     * The current bitmap and the provided bitmap are unchanged.
     */
    Roaring64Map operator&(const Roaring64Map &o) const {
        return Roaring64Map(*this) &= o;
    }

    /**
     * Computes the difference between two bitmaps and returns new bitmap.
     * The current bitmap and the provided bitmap are unchanged.
     */
    Roaring64Map operator-(const Roaring64Map &o) const {
        return Roaring64Map(*this) -= o;
    }

    /**
     * Computes the union between two bitmaps and returns new bitmap.
     * The current bitmap and the provided bitmap are unchanged.
     */
    Roaring64Map operator|(const Roaring64Map &o) const {
        return Roaring64Map(*this) |= o;
    }

    /**
     * Computes the symmetric union between two bitmaps and returns new bitmap.
     * The current bitmap and the provided bitmap are unchanged.
     */
    Roaring64Map operator^(const Roaring64Map &o) const {
        return Roaring64Map(*this) ^= o;
    }

    /**
     * Whether or not we apply copy and write.
     */
    void setCopyOnWrite(bool val) {
        if (copyOnWrite == val) return;
        copyOnWrite = val;
        std::for_each(roarings.begin(), roarings.end(),
                      [=](std::pair<const uint32_t, Roaring> &map_entry) {
                          map_entry.second.setCopyOnWrite(val);
                      });
    }


    /**
     * Print the content of the bitmap
     */
    void printf() const {
        std::cout << *this << '\n';
    }

    /**
     * Print the content of the bitmap into a string
     */
    std::string toString() const {
        // A more efficient alternative to ostringstream that allows you to grab the internal
        // buffer if you want it, saving a copy over std::stringstream.
        struct MyOstringStream final : private std::basic_streambuf<char>, public std::ostream {
            using Buf = std::basic_streambuf<char>;

            MyOstringStream() : std::ostream(this) {}

            Buf::int_type overflow(int c) final {
                if (!Buf::traits_type::eq_int_type(c, Buf::traits_type::eof())) {
                    internalBuffer_.push_back(c);
                }
                return c;
            }
            std::streamsize xsputn(const char *s, std::streamsize n) final {
                internalBuffer_.append(s, n);
                return n;
            }

            std::string internalBuffer_;
        };

        MyOstringStream ostr;
        ostr << *this;
        return std::move(ostr.internalBuffer_);
    }

    /**
     * Stream the contents of the bitmap.
     */
     friend std::ostream &operator<<(std::ostream &s, const Roaring64Map &o) {
         struct context_t {
             std::ostream &s;
             uint32_t high_bits;
             const char *separator;
         };
         context_t context{s, 0, ""};

         auto streamElement = [](uint32_t low_bits, void *arg) {
             auto *ctx = static_cast<context_t*>(arg);
             ctx->s << ctx->separator << uniteBytes(ctx->high_bits, low_bits);
             ctx->separator = ",";
             return true;
         };

         s << '{';
         for (const auto &entry : o.roarings) {
             context.high_bits = entry.first;
             const auto &bitmap = entry.second;
             bitmap.iterate(streamElement, &context);
         }
         s << '}';
         return s;
     }

    /**
     * Whether or not copy and write is active.
     */
    bool getCopyOnWrite() const { return copyOnWrite; }

    /**
     * Computes the logical or (union) between "n" bitmaps (referenced by a
     * pointer).
     */
    static Roaring64Map fastunion(size_t n, const Roaring64Map **inputs) {
        Roaring64Map result;

        // iters[i] holds the current iterator for bitmap i.
        // ends[i] holds the 'end' iterator for bitmap i.
        // (This is for the sake of convenience)
        std::vector<roarings_t::const_iterator> iters(n);
        std::vector<roarings_t::const_iterator> ends(n);
        for (size_t i = 0; i < n; ++i) {
            iters[i] = inputs[i]->roarings.begin();
            ends[i] = inputs[i]->roarings.end();
        }

        // A comparison function for the priority queue, which looks up
        // the current iterators (by index), extracts their keys, and then
        // compares them (in the opposite direction, because we want a priority
        // queue that orders from smallest to largest).
        auto pqComp = [&iters](size_t left_index, size_t right_index) {
            auto left_key = iters[left_index]->first;
            auto right_key = iters[right_index]->first;
            // We prefer the priority queue to prioritize the lowest-value item,
            // so we invert the usual less-than comparison.
            return left_key > right_key;
        };

        // A priority queue that holds the index of the input. It is ordered by
        // the key of the underlying map.
        std::priority_queue<size_t, std::vector<size_t>, decltype(pqComp)> pq(pqComp);

        // Populate the pq with (the index of) all non-empty sets.
        for (size_t i = 0; i < n; ++i) {
            if (iters[i] != ends[i]) {
                pq.push(i);
            }
        }

        // A reusable vector that holds the storage for the pointers to the
        // inner bitmaps that we are going to union together in this round.
        std::vector<const roaring_bitmap_t*> bitmapsToProcess;

        while (!pq.empty()) {
            // Find the next key in the priority queue
            auto target_key = iters[pq.top()]->first;

            // The purpose of the inner loop is to gather all the inner bitmaps
            // that share "target_key" into "bitmapsToProcess" so that we can
            // feed them to roaring_bitmap_or_many. While we are doing this, we
            // advance those iterators to their next value and reinsert them
            // into the priority queue (unless they reach their end).
            bitmapsToProcess.clear();
            while (!pq.empty()) {
                // index of the top element of the priority queue.
                auto next_index = pq.top();
                // The corresponding iterator. Take a reference because we are
                // going to increment next_iter below and we want it to be
                // reflected in iters[]
                auto &next_iter = iters[next_index];
                auto next_key = next_iter->first;
                const auto &next_bitmap = next_iter->second;
                if (next_key != target_key) {
                    // This means that the remaining items in the PQ are all
                    // greater than 'target_key'.
                    break;
                }

                bitmapsToProcess.push_back(&next_bitmap.roaring);
                pq.pop();
                if (++next_iter != ends[next_index]) {
                    pq.push(next_index);
                }
            }

            // Use the fast inner union to combine these.
            auto innerResult = roaring_bitmap_or_many(bitmapsToProcess.size(),
                                                      bitmapsToProcess.data());
            result.roarings[target_key] = Roaring(innerResult);
        }
        return result;
    }

    friend class Roaring64MapSetBitForwardIterator;
    friend class Roaring64MapSetBitBiDirectionalIterator;
    typedef Roaring64MapSetBitForwardIterator const_iterator;
    typedef Roaring64MapSetBitBiDirectionalIterator const_bidirectional_iterator;

    /**
     * Returns an iterator that can be used to access the position of the set
     * bits. The running time complexity of a full scan is proportional to the
     * number of set bits: be aware that if you have long strings of 1s, this
     * can be very inefficient.
     *
     * It can be much faster to use the toArray method if you want to
     * retrieve the set bits.
     */
    const_iterator begin() const;

    /**
     * A bogus iterator that can be used together with begin()
     * for constructions such as: for (auto i = b.begin(); * i!=b.end(); ++i) {}
     */
    const_iterator end() const;

private:
    typedef std::map<uint32_t, Roaring> roarings_t;

    roarings_t roarings{}; // The empty constructor silences warnings from pedantic static analyzers.
    bool copyOnWrite{false};
    static uint32_t highBytes(const uint64_t in) { return uint32_t(in >> 32); }
    static uint32_t lowBytes(const uint64_t in) { return uint32_t(in); }
    static uint64_t uniteBytes(const uint32_t highBytes,
                               const uint32_t lowBytes) {
        return (uint64_t(highBytes) << 32) | uint64_t(lowBytes);
    }
    // this is needed to tolerate gcc's C++11 libstdc++ lacking emplace
    // prior to version 4.8
    void emplaceOrInsert(const uint32_t key, const Roaring &value) {
#if defined(__GLIBCXX__) && __GLIBCXX__ < 20130322
        roarings.insert(std::make_pair(key, value));
#else
        roarings.emplace(std::make_pair(key, value));
#endif
    }

    void emplaceOrInsert(const uint32_t key, Roaring &&value) {
#if defined(__GLIBCXX__) && __GLIBCXX__ < 20130322
        roarings.insert(std::make_pair(key, std::move(value)));
#else
        roarings.emplace(key, std::move(value));
#endif
    }
};

/**
 * Used to go through the set bits. Not optimally fast, but convenient.
 */
class Roaring64MapSetBitForwardIterator {
public:
    typedef std::forward_iterator_tag iterator_category;
    typedef uint64_t *pointer;
    typedef uint64_t &reference_type;
    typedef uint64_t value_type;
    typedef int64_t difference_type;
    typedef Roaring64MapSetBitForwardIterator type_of_iterator;

    /**
     * Provides the location of the set bit.
     */
    value_type operator*() const {
        return Roaring64Map::uniteBytes(map_iter->first, i.current_value);
    }

    bool operator<(const type_of_iterator &o) const {
        if (map_iter == map_end) return false;
        if (o.map_iter == o.map_end) return true;
        return **this < *o;
    }

    bool operator<=(const type_of_iterator &o) const {
        if (o.map_iter == o.map_end) return true;
        if (map_iter == map_end) return false;
        return **this <= *o;
    }

    bool operator>(const type_of_iterator &o) const {
        if (o.map_iter == o.map_end) return false;
        if (map_iter == map_end) return true;
        return **this > *o;
    }

    bool operator>=(const type_of_iterator &o) const {
        if (map_iter == map_end) return true;
        if (o.map_iter == o.map_end) return false;
        return **this >= *o;
    }

    type_of_iterator &operator++() {  // ++i, must returned inc. value
        if (i.has_value == true) roaring_advance_uint32_iterator(&i);
        while (!i.has_value) {
            map_iter++;
            if (map_iter == map_end) return *this;
            roaring_init_iterator(&map_iter->second.roaring, &i);
        }
        return *this;
    }

    type_of_iterator operator++(int) {  // i++, must return orig. value
        Roaring64MapSetBitForwardIterator orig(*this);
        roaring_advance_uint32_iterator(&i);
        while (!i.has_value) {
            map_iter++;
            if (map_iter == map_end) return orig;
            roaring_init_iterator(&map_iter->second.roaring, &i);
        }
        return orig;
    }

    bool move(const value_type& x) {
        map_iter = p.lower_bound(Roaring64Map::highBytes(x));
        if (map_iter != p.cend()) {
            roaring_init_iterator(&map_iter->second.roaring, &i);
            if (map_iter->first == Roaring64Map::highBytes(x)) {
                if (roaring_move_uint32_iterator_equalorlarger(&i, Roaring64Map::lowBytes(x)))
                    return true;
                map_iter++;
                if (map_iter == map_end) return false;
                roaring_init_iterator(&map_iter->second.roaring, &i);
            }
            return true;
        }
        return false;
    }

    bool operator==(const Roaring64MapSetBitForwardIterator &o) const {
        if (map_iter == map_end && o.map_iter == o.map_end) return true;
        if (o.map_iter == o.map_end) return false;
        return **this == *o;
    }

    bool operator!=(const Roaring64MapSetBitForwardIterator &o) const {
        if (map_iter == map_end && o.map_iter == o.map_end) return false;
        if (o.map_iter == o.map_end) return true;
        return **this != *o;
    }

    Roaring64MapSetBitForwardIterator &operator=(const Roaring64MapSetBitForwardIterator& r) {
        map_iter = r.map_iter;
        map_end = r.map_end;
        i = r.i;
        return *this;
    }

    Roaring64MapSetBitForwardIterator(const Roaring64MapSetBitForwardIterator& r)
        : p(r.p),
          map_iter(r.map_iter),
          map_end(r.map_end),
          i(r.i)
    {}

    Roaring64MapSetBitForwardIterator(const Roaring64Map &parent,
                                      bool exhausted = false)
        : p(parent.roarings), map_end(parent.roarings.cend()) {
        if (exhausted || parent.roarings.empty()) {
            map_iter = parent.roarings.cend();
        } else {
            map_iter = parent.roarings.cbegin();
            roaring_init_iterator(&map_iter->second.roaring, &i);
            while (!i.has_value) {
                map_iter++;
                if (map_iter == map_end) return;
                roaring_init_iterator(&map_iter->second.roaring, &i);
            }
        }
    }

protected:
    const std::map<uint32_t, Roaring>& p;
    std::map<uint32_t, Roaring>::const_iterator map_iter{}; // The empty constructor silences warnings from pedantic static analyzers.
    std::map<uint32_t, Roaring>::const_iterator map_end{}; // The empty constructor silences warnings from pedantic static analyzers.
    api::roaring_uint32_iterator_t i{}; // The empty constructor silences warnings from pedantic static analyzers.
};

class Roaring64MapSetBitBiDirectionalIterator final :public Roaring64MapSetBitForwardIterator {
public:
    explicit Roaring64MapSetBitBiDirectionalIterator(const Roaring64Map &parent,
                                                     bool exhausted = false)
        : Roaring64MapSetBitForwardIterator(parent, exhausted), map_begin(parent.roarings.cbegin())
    {}

    Roaring64MapSetBitBiDirectionalIterator &operator=(const Roaring64MapSetBitForwardIterator& r) {
        *(Roaring64MapSetBitForwardIterator*)this = r;
        return *this;
    }

    Roaring64MapSetBitBiDirectionalIterator& operator--() { //  --i, must return dec.value
        if (map_iter == map_end) {
            --map_iter;
            roaring_init_iterator_last(&map_iter->second.roaring, &i);
            if (i.has_value) return *this;
        }

        roaring_previous_uint32_iterator(&i);
        while (!i.has_value) {
            if (map_iter == map_begin) return *this;
            map_iter--;
            roaring_init_iterator_last(&map_iter->second.roaring, &i);
        }
        return *this;
    }

    Roaring64MapSetBitBiDirectionalIterator operator--(int) {  // i--, must return orig. value
        Roaring64MapSetBitBiDirectionalIterator orig(*this);
        if (map_iter == map_end) {
            --map_iter;
            roaring_init_iterator_last(&map_iter->second.roaring, &i);
            return orig;
        }

        roaring_previous_uint32_iterator(&i);
        while (!i.has_value) {
            if (map_iter == map_begin) return orig;
            map_iter--;
            roaring_init_iterator_last(&map_iter->second.roaring, &i);
        }
        return orig;
    }

protected:
    std::map<uint32_t, Roaring>::const_iterator map_begin;
};

inline Roaring64MapSetBitForwardIterator Roaring64Map::begin() const {
    return Roaring64MapSetBitForwardIterator(*this);
}

inline Roaring64MapSetBitForwardIterator Roaring64Map::end() const {
    return Roaring64MapSetBitForwardIterator(*this, true);
}

}  // namespace roaring

#endif /* INCLUDE_ROARING_64_MAP_HH_ */
