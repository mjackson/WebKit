/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/InlineMap.h>

#include "MoveOnly.h"
#include "RefLogger.h"
#include "Test.h"
#include <wtf/HashSet.h>
#include <wtf/PackedRefPtr.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>

namespace TestWebKitAPI {

TEST(WTF_InlineMap, Empty)
{
    // A freshly constructed map is empty and all queries return no results.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());
    EXPECT_EQ(map.size(), 0u);
    EXPECT_FALSE(map.contains(1));
    EXPECT_FALSE(map.contains(2));
    EXPECT_TRUE(map.find(1) == map.end());
    EXPECT_TRUE(map.begin() == map.end());
}

TEST(WTF_InlineMap, BasicAddAndFind)
{
    // Adding a single entry makes it findable; missing keys return end().
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    auto result = map.add(1, 100);
    EXPECT_TRUE(result.isNewEntry);
    EXPECT_EQ(result.iterator->key, 1u);
    EXPECT_EQ(result.iterator->value, 100u);

    EXPECT_FALSE(map.isEmpty());
    EXPECT_EQ(map.size(), 1u);
    EXPECT_TRUE(map.contains(1));
    EXPECT_FALSE(map.contains(2));

    auto it = map.find(1);
    EXPECT_FALSE(it == map.end());
    EXPECT_EQ(it->key, 1u);
    EXPECT_EQ(it->value, 100u);

    it = map.find(2);
    EXPECT_TRUE(it == map.end());
}

TEST(WTF_InlineMap, DuplicateAdd)
{
    // Adding a key that already exists preserves the original value.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    auto result1 = map.add(1, 100);
    EXPECT_TRUE(result1.isNewEntry);

    auto result2 = map.add(1, 200);
    EXPECT_FALSE(result2.isNewEntry);
    EXPECT_EQ(result2.iterator->value, 100u); // Original value preserved

    EXPECT_EQ(map.size(), 1u);
}

TEST(WTF_InlineMap, StorageModeTransitions)
{
    // Map transitions from inline to hashed storage when inline capacity is exceeded.
    // Explicitly specify InitialCapacity=3 and InitialHashedCapacity=8
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    // New map starts empty
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    // First add transitions to linear mode
    map.add(1, 10);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    // Stays linear through capacity (3 entries)
    map.add(2, 20);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    map.add(3, 30);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    // Fourth entry triggers transition to hashed mode
    map.add(4, 40);
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // Stays hashed as more entries are added
    map.add(5, 50);
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // All entries should be accessible
    EXPECT_EQ(map.size(), 5u);
    for (unsigned i = 1; i <= 5; ++i) {
        auto it = map.find(i);
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i * 10);
    }
}

TEST(WTF_InlineMap, LinearMode)
{
    // Entries within inline capacity are stored inline and are all findable.
    // Explicitly specify InitialCapacity=3 to test linear storage behavior
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    for (unsigned i = 1; i <= 3; ++i) {
        auto result = map.add(i, i * 10);
        EXPECT_TRUE(result.isNewEntry);
        EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    }

    EXPECT_EQ(map.size(), 3u);

    for (unsigned i = 1; i <= 3; ++i) {
        EXPECT_TRUE(map.contains(i));
        auto it = map.find(i);
        EXPECT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i * 10);
    }

    EXPECT_FALSE(map.contains(4));
}

TEST(WTF_InlineMap, GrowToHashedMode)
{
    // Growing well past inline capacity preserves all entries in hashed mode.
    // Explicitly specify InitialCapacity=3 and InitialHashedCapacity=8
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    // Fill linear capacity (3 entries)
    for (unsigned i = 1; i <= 3; ++i) {
        map.add(i, i * 10);
        EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    }

    // Adding one more should trigger transition to hashed mode
    map.add(4, 40);
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // Fill beyond initial capacity to trigger growth
    for (unsigned i = 5; i <= 100; ++i) {
        auto result = map.add(i, i * 10);
        EXPECT_TRUE(result.isNewEntry);
        EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    }

    EXPECT_EQ(map.size(), 100u);

    // Verify all entries are still accessible
    for (unsigned i = 1; i <= 100; ++i) {
        EXPECT_TRUE(map.contains(i));
        auto it = map.find(i);
        EXPECT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i * 10);
    }

    EXPECT_FALSE(map.contains(101));
}

TEST(WTF_InlineMap, Iteration)
{
    // Range-based for visits every entry exactly once.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    HashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> seenKeys;
    unsigned count = 0;

    for (auto& entry : map) {
        seenKeys.add(entry.key);
        EXPECT_EQ(entry.value, entry.key * 10);
        ++count;
    }

    EXPECT_EQ(count, 10u);
    EXPECT_EQ(seenKeys.size(), 10u);

    for (unsigned i = 1; i <= 10; ++i)
        EXPECT_TRUE(seenKeys.contains(i));
}

TEST(WTF_InlineMap, IterationAfterGrowth)
{
    // Iteration works correctly after the map has grown to hashed mode.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 100; ++i)
        map.add(i, i * 10);

    HashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> seenKeys;
    unsigned count = 0;

    for (auto& entry : map) {
        seenKeys.add(entry.key);
        EXPECT_EQ(entry.value, entry.key * 10);
        ++count;
    }

    EXPECT_EQ(count, 100u);
    EXPECT_EQ(seenKeys.size(), 100u);
}

TEST(WTF_InlineMap, MoveConstruction)
{
    // Move constructor transfers all entries and leaves the source empty.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;

    for (unsigned i = 1; i <= 10; ++i)
        map1.add(i, i * 10);

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2(WTF::move(map1));

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_EQ(map2.size(), 10u);

    for (unsigned i = 1; i <= 10; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, MoveAssignment)
{
    // Move assignment replaces the target's content and leaves the source empty.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2;

    for (unsigned i = 1; i <= 10; ++i)
        map1.add(i, i * 10);

    map2.add(100, 1000);

    map2 = WTF::move(map1);

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_EQ(map2.size(), 10u);
    EXPECT_FALSE(map2.contains(100));

    for (unsigned i = 1; i <= 10; ++i)
        EXPECT_TRUE(map2.contains(i));
}

TEST(WTF_InlineMap, CopyConstructionEmpty)
{
    // Copy-constructing from an empty map produces another empty map.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2(map1);

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_TRUE(map2.isEmpty());
    EXPECT_EQ(map1.size(), 0u);
    EXPECT_EQ(map2.size(), 0u);
}

TEST(WTF_InlineMap, CopyConstructionLinearMode)
{
    // Copy-constructing a map in inline mode produces an independent copy.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map1;

    map1.add(1, 10);
    map1.add(2, 20);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map1));

    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map2(map1);

    // Original should be unchanged
    EXPECT_EQ(map1.size(), 2u);
    EXPECT_TRUE(map1.contains(1));
    EXPECT_TRUE(map1.contains(2));
    EXPECT_EQ(map1.find(1)->value, 10u);
    EXPECT_EQ(map1.find(2)->value, 20u);

    // Copy should have same content
    EXPECT_EQ(map2.size(), 2u);
    EXPECT_TRUE(map2.contains(1));
    EXPECT_TRUE(map2.contains(2));
    EXPECT_EQ(map2.find(1)->value, 10u);
    EXPECT_EQ(map2.find(2)->value, 20u);

    // Modifying copy should not affect original
    map2.add(3, 30);
    EXPECT_EQ(map1.size(), 2u);
    EXPECT_EQ(map2.size(), 3u);
    EXPECT_FALSE(map1.contains(3));
    EXPECT_TRUE(map2.contains(3));
}

TEST(WTF_InlineMap, CopyConstructionHashedMode)
{
    // Copy-constructing a map in hashed mode produces an independent copy.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;

    for (unsigned i = 1; i <= 20; ++i)
        map1.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map1));

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2(map1);

    // Original should be unchanged
    EXPECT_EQ(map1.size(), 20u);
    for (unsigned i = 1; i <= 20; ++i) {
        EXPECT_TRUE(map1.contains(i));
        EXPECT_EQ(map1.find(i)->value, i * 10);
    }

    // Copy should have same content
    EXPECT_EQ(map2.size(), 20u);
    for (unsigned i = 1; i <= 20; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }

    // Modifying copy should not affect original
    map2.add(100, 1000);
    EXPECT_EQ(map1.size(), 20u);
    EXPECT_EQ(map2.size(), 21u);
    EXPECT_FALSE(map1.contains(100));
    EXPECT_TRUE(map2.contains(100));
}

TEST(WTF_InlineMap, CopyAssignment)
{
    // Copy assignment replaces the target's content with a copy of the source.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2;

    for (unsigned i = 1; i <= 10; ++i)
        map1.add(i, i * 10);

    map2.add(100, 1000);

    map2 = map1;

    // Original should be unchanged
    EXPECT_EQ(map1.size(), 10u);
    for (unsigned i = 1; i <= 10; ++i)
        EXPECT_TRUE(map1.contains(i));

    // Assigned map should have same content, old content replaced
    EXPECT_EQ(map2.size(), 10u);
    EXPECT_FALSE(map2.contains(100));
    for (unsigned i = 1; i <= 10; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, CopyAssignmentToSelf)
{
    // Self-assignment is a no-op and preserves the map's content.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    // Use a reference to avoid -Wself-assign-overloaded warning
    auto& ref = map;
    map = ref;

    // Map should be unchanged after self-assignment
    EXPECT_EQ(map.size(), 10u);
    for (unsigned i = 1; i <= 10; ++i) {
        EXPECT_TRUE(map.contains(i));
        EXPECT_EQ(map.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, CopyConstructionWithStrings)
{
    // Copy construction works correctly with String key/value types.
    InlineMap<String, String, 5, StringHash> map1;

    map1.add("key1"_s, "value1"_s);
    map1.add("key2"_s, "value2"_s);
    map1.add("key3"_s, "value3"_s);

    InlineMap<String, String, 5, StringHash> map2(map1);

    EXPECT_EQ(map1.size(), 3u);
    EXPECT_EQ(map2.size(), 3u);

    EXPECT_EQ(map1.find("key1"_s)->value, "value1"_s);
    EXPECT_EQ(map2.find("key1"_s)->value, "value1"_s);
    EXPECT_EQ(map1.find("key2"_s)->value, "value2"_s);
    EXPECT_EQ(map2.find("key2"_s)->value, "value2"_s);
    EXPECT_EQ(map1.find("key3"_s)->value, "value3"_s);
    EXPECT_EQ(map2.find("key3"_s)->value, "value3"_s);
}

TEST(WTF_InlineMap, RemoveFromEmpty)
{
    // Removing from an empty map returns false.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    EXPECT_FALSE(map.remove(1));
    EXPECT_TRUE(map.isEmpty());
}

TEST(WTF_InlineMap, RemoveLinearMode)
{
    // Removing entries in inline mode works and the map can be emptied completely.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    map.add(1, 10);
    map.add(2, 20);
    map.add(3, 30);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 3u);

    // Remove middle entry
    EXPECT_TRUE(map.remove(2));
    EXPECT_EQ(map.size(), 2u);
    EXPECT_TRUE(map.contains(1));
    EXPECT_FALSE(map.contains(2));
    EXPECT_TRUE(map.contains(3));
    EXPECT_EQ(map.find(1)->value, 10u);
    EXPECT_EQ(map.find(3)->value, 30u);

    // Remove first entry
    EXPECT_TRUE(map.remove(1));
    EXPECT_EQ(map.size(), 1u);
    EXPECT_FALSE(map.contains(1));
    EXPECT_TRUE(map.contains(3));

    // Remove last entry
    EXPECT_TRUE(map.remove(3));
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.isEmpty());
    EXPECT_FALSE(map.contains(3));

    // Remove from empty should return false
    EXPECT_FALSE(map.remove(1));
}

TEST(WTF_InlineMap, RemoveNonexistentLinearMode)
{
    // Removing a key not present in inline mode returns false and changes nothing.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    map.add(1, 10);
    map.add(2, 20);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    EXPECT_FALSE(map.remove(3));
    EXPECT_EQ(map.size(), 2u);
    EXPECT_TRUE(map.contains(1));
    EXPECT_TRUE(map.contains(2));
}

TEST(WTF_InlineMap, RemoveHashedMode)
{
    // Removing entries in hashed mode leaves the remaining entries intact.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 20; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 20u);

    // Remove several entries
    EXPECT_TRUE(map.remove(5));
    EXPECT_TRUE(map.remove(10));
    EXPECT_TRUE(map.remove(15));

    EXPECT_EQ(map.size(), 17u);
    EXPECT_FALSE(map.contains(5));
    EXPECT_FALSE(map.contains(10));
    EXPECT_FALSE(map.contains(15));

    // Other entries should still be accessible
    for (unsigned i = 1; i <= 20; ++i) {
        if (i == 5 || i == 10 || i == 15)
            continue;
        EXPECT_TRUE(map.contains(i));
        EXPECT_EQ(map.find(i)->value, i * 10);
    }

    // Remove nonexistent should return false
    EXPECT_FALSE(map.remove(5));
    EXPECT_FALSE(map.remove(100));
}

TEST(WTF_InlineMap, RemoveAndReaddHashedMode)
{
    // A removed key can be re-added with a different value.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // Remove and re-add with different value
    EXPECT_TRUE(map.remove(5));
    EXPECT_FALSE(map.contains(5));

    auto result = map.add(5, 500);
    EXPECT_TRUE(result.isNewEntry);
    EXPECT_TRUE(map.contains(5));
    EXPECT_EQ(map.find(5)->value, 500u);
    EXPECT_EQ(map.size(), 10u);
}

TEST(WTF_InlineMap, RemoveAllHashedMode)
{
    // Removing all entries one by one leaves the map empty.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // Remove all entries
    for (unsigned i = 1; i <= 10; ++i)
        EXPECT_TRUE(map.remove(i));

    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.isEmpty());

    // All entries should be gone
    for (unsigned i = 1; i <= 10; ++i)
        EXPECT_FALSE(map.contains(i));
}

TEST(WTF_InlineMap, IterationAfterRemove)
{
    // Iteration skips removed entries and visits only live ones.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    // Remove some entries
    map.remove(2);
    map.remove(5);
    map.remove(8);

    // Iteration should only visit non-removed entries
    HashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> seenKeys;
    unsigned count = 0;

    for (auto& entry : map) {
        seenKeys.add(entry.key);
        EXPECT_EQ(entry.value, entry.key * 10);
        ++count;
    }

    EXPECT_EQ(count, 7u);
    EXPECT_EQ(seenKeys.size(), 7u);
    EXPECT_FALSE(seenKeys.contains(2));
    EXPECT_FALSE(seenKeys.contains(5));
    EXPECT_FALSE(seenKeys.contains(8));
}

TEST(WTF_InlineMap, RemoveWithStrings)
{
    // Remove works correctly with String keys.
    InlineMap<String, unsigned, 5, StringHash> map;

    map.add("one"_s, 1);
    map.add("two"_s, 2);
    map.add("three"_s, 3);

    EXPECT_TRUE(map.remove("two"_s));
    EXPECT_EQ(map.size(), 2u);
    EXPECT_TRUE(map.contains("one"_s));
    EXPECT_FALSE(map.contains("two"_s));
    EXPECT_TRUE(map.contains("three"_s));

    EXPECT_FALSE(map.remove("four"_s));
    EXPECT_FALSE(map.remove("two"_s));
}

TEST(WTF_InlineMap, GrowAfterRemove)
{
    // The hash table can grow correctly after entries have been removed.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    // Fill to trigger hashed mode
    for (unsigned i = 1; i <= 4; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // Remove some entries
    map.remove(2);
    map.remove(3);
    EXPECT_EQ(map.size(), 2u);

    // Add more entries to trigger growth
    for (unsigned i = 5; i <= 20; ++i)
        map.add(i, i * 10);

    // Verify all expected entries are present
    EXPECT_TRUE(map.contains(1));
    EXPECT_FALSE(map.contains(2));
    EXPECT_FALSE(map.contains(3));
    EXPECT_TRUE(map.contains(4));
    for (unsigned i = 5; i <= 20; ++i)
        EXPECT_TRUE(map.contains(i));
}

TEST(WTF_InlineMap, PointerKeys)
{
    // Raw pointers work as map keys.
    InlineMap<int*, int, 5> map;

    constexpr unsigned arraySize = 50;
    int array[arraySize];

    for (unsigned i = 0; i < arraySize; ++i) {
        array[i] = i;
        int* ptr = &array[i];
        EXPECT_FALSE(map.contains(ptr));
        auto result = map.add(ptr, i * 10);
        EXPECT_TRUE(result.isNewEntry);
        EXPECT_TRUE(map.contains(ptr));
    }

    EXPECT_EQ(map.size(), arraySize);

    for (unsigned i = 0; i < arraySize; ++i) {
        int* ptr = &array[i];
        auto it = map.find(ptr);
        EXPECT_FALSE(it == map.end());
        EXPECT_EQ(it->value, static_cast<int>(i * 10));
    }
}

TEST(WTF_InlineMap, ConstIteration)
{
    // Iteration works through a const reference to the map.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    const auto& constMap = map;
    unsigned count = 0;

    for (const auto& entry : constMap) {
        EXPECT_EQ(entry.value, entry.key * 10);
        ++count;
    }

    EXPECT_EQ(count, 10u);
}

TEST(WTF_InlineMap, ConstFind)
{
    // find() works through a const reference to the map.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;
    map.add(1, 100);

    const auto& constMap = map;

    auto it = constMap.find(1);
    EXPECT_FALSE(it == constMap.end());
    EXPECT_EQ(it->value, 100u);

    it = constMap.find(2);
    EXPECT_TRUE(it == constMap.end());
}

TEST(WTF_InlineMap, MoveOnlyValues)
{
    // Move-only types work as map values.
    InlineMap<unsigned, MoveOnly, 5, IntHash<unsigned>> map;

    for (size_t i = 0; i < 100; ++i) {
        MoveOnly moveOnly(i + 1);
        auto result = map.add(static_cast<unsigned>(i + 1), WTF::move(moveOnly));
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_EQ(map.size(), 100u);

    for (size_t i = 0; i < 100; ++i) {
        auto it = map.find(static_cast<unsigned>(i + 1));
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value.value(), i + 1);
    }
}

TEST(WTF_InlineMap, MoveOnlyKeys)
{
    // Move-only types work as map keys, including duplicate detection.
    InlineMap<MoveOnly, unsigned, 5, DefaultHash<MoveOnly>> map;

    for (size_t i = 0; i < 100; ++i) {
        MoveOnly moveOnly(i + 1);
        auto result = map.add(WTF::move(moveOnly), static_cast<unsigned>(i + 1));
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_EQ(map.size(), 100u);

    for (size_t i = 0; i < 100; ++i) {
        auto it = map.find(MoveOnly(i + 1));
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, static_cast<unsigned>(i + 1));
    }

    // Verify duplicate add doesn't insert
    for (size_t i = 0; i < 100; ++i)
        EXPECT_FALSE(map.add(MoveOnly(i + 1), static_cast<unsigned>(i + 1)).isNewEntry);
}

namespace {

template<typename T> struct ZeroHash : public IntHash<T> {
    static unsigned hash(const T&) { return 0; }
};

} // anonymous namespace

TEST(WTF_InlineMap, HashCollisions)
{
    // All entries remain accessible even when every key hashes to the same bucket.
    // Use a hash that always returns 0 to force all entries into the same bucket
    InlineMap<unsigned, unsigned, 5, ZeroHash<unsigned>> map;

    // Add enough entries to trigger hashed mode
    for (unsigned i = 1; i <= 20; ++i) {
        auto result = map.add(i, i * 10);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_EQ(map.size(), 20u);

    // Verify all entries are still accessible despite hash collisions
    for (unsigned i = 1; i <= 20; ++i) {
        EXPECT_TRUE(map.contains(i));
        auto it = map.find(i);
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i * 10);
    }

    // Verify non-existent key lookup
    EXPECT_FALSE(map.contains(100));
}

TEST(WTF_InlineMap, IteratorComparison)
{
    // Iterator == and \!= operators work correctly, including const conversions.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;
    map.add(1, 100);

    ASSERT_TRUE(map.begin() != map.end());
    ASSERT_FALSE(map.begin() == map.end());

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>>::const_iterator begin = map.begin();
    ASSERT_TRUE(begin == map.begin());
    ASSERT_TRUE(map.begin() == begin);
    ASSERT_TRUE(begin != map.end());
    ASSERT_TRUE(map.end() != begin);
    ASSERT_FALSE(begin != map.begin());
    ASSERT_FALSE(map.begin() != begin);
    ASSERT_FALSE(begin == map.end());
    ASSERT_FALSE(map.end() == begin);
}

namespace {

class DestructorCounter {
public:
    static unsigned destructorCount;

    struct TestingScope {
        TestingScope() { destructorCount = 0; }
    };

    DestructorCounter() = default;
    DestructorCounter(unsigned value)
        : m_value(value)
    { }

    DestructorCounter(DestructorCounter&& other)
        : m_value(other.m_value)
    {
        other.m_value = 0;
    }

    DestructorCounter& operator=(DestructorCounter&& other)
    {
        m_value = other.m_value;
        other.m_value = 0;
        return *this;
    }

    ~DestructorCounter()
    {
        if (m_value != emptyValue())
            ++destructorCount;
    }

    unsigned value() const { return m_value; }

    static constexpr unsigned emptyValue() { return std::numeric_limits<unsigned>::max(); }

private:
    unsigned m_value { emptyValue() };
};

unsigned DestructorCounter::destructorCount = 0;

} // anonymous namespace

TEST(WTF_InlineMap, DestructorCalledOnClear)
{
    // Value destructors are called when the map is destroyed (inline mode).
    DestructorCounter::TestingScope scope;

    {
        InlineMap<unsigned, DestructorCounter, 5, IntHash<unsigned>> map;

        for (unsigned i = 1; i <= 3; ++i)
            map.add(i, DestructorCounter(i));

        EXPECT_EQ(map.size(), 3u);
        EXPECT_EQ(DestructorCounter::destructorCount, 3u); // Moved-from temporaries
    }

    // Destructor should be called for all 3 entries when map is destroyed
    EXPECT_EQ(DestructorCounter::destructorCount, 6u);
}

TEST(WTF_InlineMap, DestructorCalledOnClearAfterGrowth)
{
    // Value destructors are called for all live entries after growth (hashed mode).
    DestructorCounter::TestingScope scope;
    unsigned countBeforeDestruction = 0;

    {
        InlineMap<unsigned, DestructorCounter, 5, IntHash<unsigned>> map;

        for (unsigned i = 1; i <= 100; ++i)
            map.add(i, DestructorCounter(i));

        EXPECT_EQ(map.size(), 100u);

        // Record the count before map destruction. This includes temporaries from add()
        // calls as well as internal temporaries created during hash table growth.
        countBeforeDestruction = DestructorCounter::destructorCount;
    }

    // Map destruction should destroy exactly 100 live entries
    EXPECT_EQ(DestructorCounter::destructorCount, countBeforeDestruction + 100u);
}

TEST(WTF_InlineMap, StringKeys)
{
    // String objects work as map keys.
    InlineMap<String, unsigned, 5, StringHash> map;

    map.add("one"_s, 1);
    map.add("two"_s, 2);
    map.add("three"_s, 3);

    EXPECT_EQ(map.size(), 3u);
    EXPECT_TRUE(map.contains("one"_s));
    EXPECT_TRUE(map.contains("two"_s));
    EXPECT_TRUE(map.contains("three"_s));
    EXPECT_FALSE(map.contains("four"_s));

    EXPECT_EQ(map.find("one"_s)->value, 1u);
    EXPECT_EQ(map.find("two"_s)->value, 2u);
    EXPECT_EQ(map.find("three"_s)->value, 3u);
}

TEST(WTF_InlineMap, StringValues)
{
    // String objects work as map values.
    InlineMap<unsigned, String, 5, IntHash<unsigned>> map;

    map.add(1, "one"_s);
    map.add(2, "two"_s);
    map.add(3, "three"_s);

    EXPECT_EQ(map.size(), 3u);
    EXPECT_EQ(map.find(1)->value, "one"_s);
    EXPECT_EQ(map.find(2)->value, "two"_s);
    EXPECT_EQ(map.find(3)->value, "three"_s);
}

TEST(WTF_InlineMap, StringKeysAndValues)
{
    // String-to-String map works, and duplicate add preserves the original value.
    InlineMap<String, String, 5, StringHash> map;

    map.add("key1"_s, "value1"_s);
    map.add("key2"_s, "value2"_s);
    map.add("key3"_s, "value3"_s);

    EXPECT_EQ(map.size(), 3u);
    EXPECT_EQ(map.find("key1"_s)->value, "value1"_s);
    EXPECT_EQ(map.find("key2"_s)->value, "value2"_s);
    EXPECT_EQ(map.find("key3"_s)->value, "value3"_s);

    // Duplicate add should not overwrite
    auto result = map.add("key1"_s, "newvalue"_s);
    EXPECT_FALSE(result.isNewEntry);
    EXPECT_EQ(map.find("key1"_s)->value, "value1"_s);
}

TEST(WTF_InlineMap, StringKeysGrowth)
{
    // String keys survive growth from inline to hashed mode.
    InlineMap<String, unsigned, 5, StringHash> map;

    // Add enough entries to trigger growth to hashed mode
    for (unsigned i = 1; i <= 100; ++i)
        map.add(makeString("key"_s, i), i);

    EXPECT_EQ(map.size(), 100u);

    // Verify all entries are still accessible
    for (unsigned i = 1; i <= 100; ++i) {
        auto key = makeString("key"_s, i);
        EXPECT_TRUE(map.contains(key));
        auto it = map.find(key);
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i);
    }

    EXPECT_FALSE(map.contains("key101"_s));
}

TEST(WTF_InlineMap, RefPtrKeys)
{
    // RefPtr objects work as map keys, looked up by raw pointer.
    DerivedRefLogger a("a");
    DerivedRefLogger b("b");
    DerivedRefLogger c("c");

    InlineMap<RefPtr<RefLogger>, int, 5> map;

    map.add(RefPtr<RefLogger>(&a), 1);
    map.add(RefPtr<RefLogger>(&b), 2);
    map.add(RefPtr<RefLogger>(&c), 3);

    EXPECT_EQ(map.size(), 3u);
    EXPECT_TRUE(map.contains(&a));
    EXPECT_TRUE(map.contains(&b));
    EXPECT_TRUE(map.contains(&c));

    EXPECT_EQ(map.find(&a)->value, 1);
    EXPECT_EQ(map.find(&b)->value, 2);
    EXPECT_EQ(map.find(&c)->value, 3);
}

TEST(WTF_InlineMap, RefPtrValues)
{
    // RefPtr objects work as map values.
    DerivedRefLogger a("a");
    DerivedRefLogger b("b");

    InlineMap<unsigned, RefPtr<RefLogger>, 5, IntHash<unsigned>> map;

    map.add(1, RefPtr<RefLogger>(&a));
    map.add(2, RefPtr<RefLogger>(&b));

    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(map.find(1)->value.get(), &a);
    EXPECT_EQ(map.find(2)->value.get(), &b);
}

TEST(WTF_InlineMap, RefKeys)
{
    // Ref objects work as map keys and are properly deref'd on destruction.
    RefLogger a("a");

    {
        InlineMap<Ref<RefLogger>, int, 5> map;

        Ref<RefLogger> ref(a);
        map.add(WTF::move(ref), 1);

        EXPECT_EQ(map.size(), 1u);

        // Verify through iteration since we don't have translator support
        bool found = false;
        for (auto& entry : map) {
            if (entry.key.ptr() == &a) {
                EXPECT_EQ(entry.value, 1);
                found = true;
            }
        }
        EXPECT_TRUE(found);
    }

    EXPECT_STREQ("ref(a) deref(a) ", takeLogStr().c_str());
}

TEST(WTF_InlineMap, RefValues)
{
    // Ref objects work as map values and are properly deref'd on destruction.
    RefLogger a("a");

    {
        InlineMap<unsigned, Ref<RefLogger>, 5, IntHash<unsigned>> map;

        Ref<RefLogger> ref(a);
        map.add(1, WTF::move(ref));

        EXPECT_EQ(map.size(), 1u);
        EXPECT_EQ(map.find(1)->value.ptr(), &a);
    }

    EXPECT_STREQ("ref(a) deref(a) ", takeLogStr().c_str());
}

TEST(WTF_InlineMap, RefKeysGrowth)
{
    // Ref keys work correctly through growth transitions.
    // Test that Ref keys work correctly through growth transitions
    Vector<Ref<RefLogger>> loggers;
    for (int i = 0; i < 50; ++i)
        loggers.append(adoptRef(*new RefLogger("a")));

    {
        InlineMap<Ref<RefLogger>, int, 5> map;

        for (int i = 0; i < 50; ++i) {
            Ref<RefLogger> ref = loggers[i].copyRef();
            map.add(WTF::move(ref), i + 1);
        }

        EXPECT_EQ(map.size(), 50u);

        // Verify all entries through iteration
        unsigned count = 0;
        for (auto& entry : map) {
            ++count;
            // Just verify values are in expected range
            EXPECT_GE(entry.value, 1);
            EXPECT_LE(entry.value, 50);
        }
        EXPECT_EQ(count, 50u);
    }
}

TEST(WTF_InlineMap, ClearEmpty)
{
    // Clearing an already-empty map is a no-op.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    map.clear();

    EXPECT_TRUE(map.isEmpty());
    EXPECT_EQ(map.size(), 0u);
}

TEST(WTF_InlineMap, ClearLinearMode)
{
    // Clearing in inline mode removes entries but preserves inline storage.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    map.add(1, 10);
    map.add(2, 20);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 2u);

    map.clear();

    // Storage is preserved, just cleared
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());
    EXPECT_EQ(map.size(), 0u);

    // Should be able to add entries again after clear
    map.add(3, 30);
    EXPECT_EQ(map.size(), 1u);
    EXPECT_TRUE(map.contains(3));
    EXPECT_FALSE(map.contains(1));
    EXPECT_FALSE(map.contains(2));
}

TEST(WTF_InlineMap, ClearHashedMode)
{
    // Clearing in hashed mode removes entries but preserves hashed storage.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 20; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 20u);

    map.clear();

    // Storage is preserved, just cleared
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());
    EXPECT_EQ(map.size(), 0u);

    // Should be able to add entries again after clear
    for (unsigned i = 100; i <= 105; ++i)
        map.add(i, i);

    EXPECT_EQ(map.size(), 6u);
    for (unsigned i = 100; i <= 105; ++i)
        EXPECT_TRUE(map.contains(i));
    EXPECT_FALSE(map.contains(1));
}

TEST(WTF_InlineMap, ClearWithStrings)
{
    // Clearing works correctly with String key/value types.
    InlineMap<String, String, 5, StringHash> map;

    map.add("key1"_s, "value1"_s);
    map.add("key2"_s, "value2"_s);
    map.add("key3"_s, "value3"_s);

    EXPECT_EQ(map.size(), 3u);

    map.clear();

    EXPECT_TRUE(map.isEmpty());
    EXPECT_FALSE(map.contains("key1"_s));
}

TEST(WTF_InlineMap, ReserveInitialCapacityZero)
{
    // Reserving zero capacity keeps the map in inline mode.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    map.reserveInitialCapacity(0);

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());
}

TEST(WTF_InlineMap, ReserveInitialCapacityLinear)
{
    // Reserving within inline capacity keeps inline storage.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    map.reserveInitialCapacity(2);

    // Should allocate linear storage
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());

    // Should be able to add entries
    map.add(1, 10);
    map.add(2, 20);
    EXPECT_EQ(map.size(), 2u);
    EXPECT_TRUE(map.contains(1));
    EXPECT_TRUE(map.contains(2));
}

TEST(WTF_InlineMap, ReserveInitialCapacityHashed)
{
    // Reserving beyond inline capacity switches directly to hashed storage.
    InlineMap<unsigned, unsigned, 3, IntHash<unsigned>> map;

    map.reserveInitialCapacity(10);

    // Should allocate hashed storage directly
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());

    // Should be able to add entries without triggering growth
    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    EXPECT_EQ(map.size(), 10u);
    for (unsigned i = 1; i <= 10; ++i) {
        EXPECT_TRUE(map.contains(i));
        EXPECT_EQ(map.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, ReserveInitialCapacityLarge)
{
    // Reserving a large capacity pre-allocates hashed storage for many entries.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    map.reserveInitialCapacity(100);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_TRUE(map.isEmpty());

    // Add all 100 entries
    for (unsigned i = 1; i <= 100; ++i)
        map.add(i, i * 10);

    EXPECT_EQ(map.size(), 100u);
    for (unsigned i = 1; i <= 100; ++i)
        EXPECT_TRUE(map.contains(i));
}

TEST(WTF_InlineMap, ValuesIteration)
{
    // The values() range visits every value exactly once.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 10; ++i)
        map.add(i, i * 10);

    unsigned sum = 0;
    unsigned count = 0;
    for (auto& value : map.values()) {
        sum += value;
        ++count;
    }

    EXPECT_EQ(count, 10u);
    // Sum of 10 + 20 + ... + 100 = 550
    EXPECT_EQ(sum, 550u);
}

TEST(WTF_InlineMap, ValuesIterationModify)
{
    // Values can be modified in-place through the values() iterator.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 5; ++i)
        map.add(i, i);

    // Modify values through the values iterator
    for (auto& value : map.values())
        value *= 10;

    // Verify modifications
    for (unsigned i = 1; i <= 5; ++i)
        EXPECT_EQ(map.find(i)->value, i * 10);
}

TEST(WTF_InlineMap, ValuesIterationEmpty)
{
    // The values() range on an empty map produces zero iterations.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    unsigned count = 0;
    for (auto& value : map.values()) {
        UNUSED_PARAM(value);
        ++count;
    }

    EXPECT_EQ(count, 0u);
}

TEST(WTF_InlineMap, ValuesIterationConst)
{
    // The values() range works through a const reference.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 5; ++i)
        map.add(i, i * 10);

    const auto& constMap = map;
    unsigned sum = 0;
    for (const auto& value : constMap.values())
        sum += value;

    EXPECT_EQ(sum, 150u); // 10 + 20 + 30 + 40 + 50
}

TEST(WTF_InlineMap, SwapBothEmpty)
{
    // Swapping two empty maps is a no-op.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2;

    map1.swap(map2);

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_TRUE(map2.isEmpty());
}

TEST(WTF_InlineMap, SwapOneEmpty)
{
    // Swapping a populated map with an empty one exchanges their contents.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2;

    for (unsigned i = 1; i <= 5; ++i)
        map1.add(i, i * 10);

    EXPECT_EQ(map1.size(), 5u);
    EXPECT_TRUE(map2.isEmpty());

    map1.swap(map2);

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_EQ(map2.size(), 5u);
    for (unsigned i = 1; i <= 5; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, SwapBothPopulated)
{
    // Swapping two populated maps exchanges their contents.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2;

    for (unsigned i = 1; i <= 5; ++i)
        map1.add(i, i * 10);

    for (unsigned i = 100; i <= 103; ++i)
        map2.add(i, i);

    EXPECT_EQ(map1.size(), 5u);
    EXPECT_EQ(map2.size(), 4u);

    map1.swap(map2);

    EXPECT_EQ(map1.size(), 4u);
    EXPECT_EQ(map2.size(), 5u);

    // Verify map1 now has map2's original content
    for (unsigned i = 100; i <= 103; ++i) {
        EXPECT_TRUE(map1.contains(i));
        EXPECT_EQ(map1.find(i)->value, i);
    }
    EXPECT_FALSE(map1.contains(1));

    // Verify map2 now has map1's original content
    for (unsigned i = 1; i <= 5; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
    EXPECT_FALSE(map2.contains(100));
}

TEST(WTF_InlineMap, SwapWithStrings)
{
    // Swap works correctly with String key/value types.
    InlineMap<String, String, 5, StringHash> map1;
    InlineMap<String, String, 5, StringHash> map2;

    map1.add("key1"_s, "value1"_s);
    map1.add("key2"_s, "value2"_s);

    map2.add("other"_s, "data"_s);

    map1.swap(map2);

    EXPECT_EQ(map1.size(), 1u);
    EXPECT_EQ(map2.size(), 2u);

    EXPECT_TRUE(map1.contains("other"_s));
    EXPECT_EQ(map1.find("other"_s)->value, "data"_s);

    EXPECT_TRUE(map2.contains("key1"_s));
    EXPECT_TRUE(map2.contains("key2"_s));
}

// --- PackedRefPtr key tests (matching production use in VariableEnvironment) ---

TEST(WTF_InlineMap, PackedRefPtrKeysBasic)
{
    // PackedRefPtr<StringImpl> keys work for basic add/find/contains.
    Vector<String> strings;
    strings.append("alpha"_s);
    strings.append("beta"_s);
    strings.append("gamma"_s);

    InlineMap<PackedRefPtr<StringImpl>, unsigned, 5> map;

    for (unsigned i = 0; i < strings.size(); ++i) {
        auto result = map.add(strings[i].impl(), i + 1);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 3u);

    for (unsigned i = 0; i < strings.size(); ++i) {
        EXPECT_TRUE(map.contains(strings[i].impl()));
        auto it = map.find(strings[i].impl());
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i + 1);
    }

    EXPECT_FALSE(map.contains(String("delta"_s).impl()));
}

TEST(WTF_InlineMap, PackedRefPtrKeysGrowth)
{
    // PackedRefPtr keys survive growth from inline to hashed mode.
    constexpr unsigned count = 50;
    Vector<String> strings;
    for (unsigned i = 0; i < count; ++i)
        strings.append(makeString("key_"_s, i));

    InlineMap<PackedRefPtr<StringImpl>, unsigned, 5> map;

    for (unsigned i = 0; i < count; ++i) {
        auto result = map.add(strings[i].impl(), i * 10);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), count);

    for (unsigned i = 0; i < count; ++i) {
        EXPECT_TRUE(map.contains(strings[i].impl()));
        auto it = map.find(strings[i].impl());
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i * 10);
    }

    EXPECT_FALSE(map.contains(String("nonexistent"_s).impl()));
}

TEST(WTF_InlineMap, PackedRefPtrKeysRemoveAndReinsert)
{
    // PackedRefPtr keys can be removed and re-inserted with new values.
    constexpr unsigned count = 20;
    Vector<String> strings;
    for (unsigned i = 0; i < count; ++i)
        strings.append(makeString("var_"_s, i));

    InlineMap<PackedRefPtr<StringImpl>, unsigned, 5> map;

    for (unsigned i = 0; i < count; ++i)
        map.add(strings[i].impl(), i);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));

    // Remove even-indexed entries
    for (unsigned i = 0; i < count; i += 2)
        EXPECT_TRUE(map.remove(strings[i].impl()));

    EXPECT_EQ(map.size(), count / 2);

    // Verify odd-indexed entries are still present
    for (unsigned i = 1; i < count; i += 2) {
        EXPECT_TRUE(map.contains(strings[i].impl()));
        EXPECT_EQ(map.find(strings[i].impl())->value, i);
    }

    // Re-add even-indexed entries with new values
    for (unsigned i = 0; i < count; i += 2) {
        auto result = map.add(strings[i].impl(), i + 1000);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_EQ(map.size(), count);

    // Verify all entries
    for (unsigned i = 0; i < count; ++i) {
        EXPECT_TRUE(map.contains(strings[i].impl()));
        unsigned expectedValue = (i % 2) ? i : i + 1000;
        EXPECT_EQ(map.find(strings[i].impl())->value, expectedValue);
    }
}

TEST(WTF_InlineMap, PackedRefPtrKeysCopy)
{
    // Copying a map with PackedRefPtr keys produces an independent copy.
    constexpr unsigned count = 20;
    Vector<String> strings;
    for (unsigned i = 0; i < count; ++i)
        strings.append(makeString("name_"_s, i));

    InlineMap<PackedRefPtr<StringImpl>, unsigned, 5> map1;

    for (unsigned i = 0; i < count; ++i)
        map1.add(strings[i].impl(), i);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map1));

    InlineMap<PackedRefPtr<StringImpl>, unsigned, 5> map2(map1);

    EXPECT_EQ(map1.size(), count);
    EXPECT_EQ(map2.size(), count);

    for (unsigned i = 0; i < count; ++i) {
        EXPECT_TRUE(map1.contains(strings[i].impl()));
        EXPECT_TRUE(map2.contains(strings[i].impl()));
        EXPECT_EQ(map1.find(strings[i].impl())->value, i);
        EXPECT_EQ(map2.find(strings[i].impl())->value, i);
    }

    // Modifying copy should not affect original
    map2.add(String("extra"_s).impl(), 999);
    EXPECT_EQ(map1.size(), count);
    EXPECT_EQ(map2.size(), count + 1);
}

// --- Stress tests ---

TEST(WTF_InlineMap, StressInsertions)
{
    // Inserting 1000 entries works and all are retrievable.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 1000; ++i) {
        auto result = map.add(i, i * 10);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 1000u);

    for (unsigned i = 1; i <= 1000; ++i) {
        EXPECT_TRUE(map.contains(i));
        auto it = map.find(i);
        ASSERT_FALSE(it == map.end());
        EXPECT_EQ(it->value, i * 10);
    }

    EXPECT_FALSE(map.contains(1001));
}

TEST(WTF_InlineMap, StressInsertRemoveReinsert)
{
    // Bulk remove of odd keys and re-insert with new values preserves integrity.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    // Add 200 entries
    for (unsigned i = 1; i <= 200; ++i)
        map.add(i, i);

    EXPECT_EQ(map.size(), 200u);

    // Remove odd-keyed entries
    for (unsigned i = 1; i <= 200; i += 2)
        EXPECT_TRUE(map.remove(i));

    EXPECT_EQ(map.size(), 100u);

    // Verify even entries remain, odd entries gone
    for (unsigned i = 1; i <= 200; ++i) {
        if (!(i % 2)) {
            EXPECT_TRUE(map.contains(i));
            EXPECT_EQ(map.find(i)->value, i);
        } else
            EXPECT_FALSE(map.contains(i));
    }

    // Re-add odd entries with new values
    for (unsigned i = 1; i <= 200; i += 2) {
        auto result = map.add(i, i + 1000);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_EQ(map.size(), 200u);

    // Verify all entries
    for (unsigned i = 1; i <= 200; ++i) {
        EXPECT_TRUE(map.contains(i));
        unsigned expectedValue = (i % 2) ? i + 1000 : i;
        EXPECT_EQ(map.find(i)->value, expectedValue);
    }
}

TEST(WTF_InlineMap, StressRemoveAll)
{
    // Removing all 500 entries then re-adding them works correctly.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    // Add 500 entries
    for (unsigned i = 1; i <= 500; ++i)
        map.add(i, i * 10);

    EXPECT_EQ(map.size(), 500u);

    // Remove all one by one
    for (unsigned i = 1; i <= 500; ++i)
        EXPECT_TRUE(map.remove(i));

    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.isEmpty());

    for (unsigned i = 1; i <= 500; ++i)
        EXPECT_FALSE(map.contains(i));

    // Re-add 500 entries with different values
    for (unsigned i = 1; i <= 500; ++i) {
        auto result = map.add(i, i + 5000);
        EXPECT_TRUE(result.isNewEntry);
    }

    EXPECT_EQ(map.size(), 500u);

    for (unsigned i = 1; i <= 500; ++i) {
        EXPECT_TRUE(map.contains(i));
        EXPECT_EQ(map.find(i)->value, i + 5000);
    }
}

TEST(WTF_InlineMap, DuplicateAddHashedMode)
{
    // Duplicate adds in hashed mode preserve original values.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 20; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 20u);

    // Attempt duplicate adds
    for (unsigned i = 1; i <= 20; ++i) {
        auto result = map.add(i, i * 100);
        EXPECT_FALSE(result.isNewEntry);
        EXPECT_EQ(result.iterator->value, i * 10); // Original value preserved
    }

    EXPECT_EQ(map.size(), 20u);
}

TEST(WTF_InlineMap, IterationInlineMode)
{
    // Iteration visits all entries while still in inline mode.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 3; ++i)
        map.add(i, i * 10);

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));

    HashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> seenKeys;
    unsigned count = 0;

    for (auto& entry : map) {
        seenKeys.add(entry.key);
        EXPECT_EQ(entry.value, entry.key * 10);
        ++count;
    }

    EXPECT_EQ(count, 3u);
    EXPECT_EQ(seenKeys.size(), 3u);
    for (unsigned i = 1; i <= 3; ++i)
        EXPECT_TRUE(seenKeys.contains(i));
}

TEST(WTF_InlineMap, MoveConstructionInlineMode)
{
    // Move construction in inline mode transfers entries and preserves inline storage.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;

    for (unsigned i = 1; i <= 3; ++i)
        map1.add(i, i * 10);

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map1));

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2(WTF::move(map1));

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map1));
    EXPECT_EQ(map2.size(), 3u);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map2));

    for (unsigned i = 1; i <= 3; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, MoveConstructionHashedMode)
{
    // Move construction in hashed mode steals the heap pointer.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;

    for (unsigned i = 1; i <= 20; ++i)
        map1.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map1));

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2(WTF::move(map1));

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map1)); // Reset to inline after move
    EXPECT_EQ(map2.size(), 20u);
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map2)); // Stole heap pointer

    for (unsigned i = 1; i <= 20; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, MoveAssignmentInlineToInline)
{
    // Move assignment between two inline maps works correctly.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2;

    for (unsigned i = 1; i <= 3; ++i)
        map1.add(i, i * 10);

    map2.add(100, 1000);

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map1));
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map2));

    map2 = WTF::move(map1);

    EXPECT_TRUE(map1.isEmpty());
    EXPECT_EQ(map2.size(), 3u);
    EXPECT_FALSE(map2.contains(100));

    for (unsigned i = 1; i <= 3; ++i) {
        EXPECT_TRUE(map2.contains(i));
        EXPECT_EQ(map2.find(i)->value, i * 10);
    }
}

TEST(WTF_InlineMap, SwapInlineAndHashed)
{
    // Swapping an inline map with a hashed map exchanges both content and storage mode.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> inlineMap;
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> hashedMap;

    for (unsigned i = 1; i <= 3; ++i)
        inlineMap.add(i, i * 10);

    for (unsigned i = 100; i <= 120; ++i)
        hashedMap.add(i, i);

    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(inlineMap));
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(hashedMap));

    inlineMap.swap(hashedMap);

    // inlineMap now has hashed content
    EXPECT_EQ(inlineMap.size(), 21u);
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(inlineMap));
    for (unsigned i = 100; i <= 120; ++i) {
        EXPECT_TRUE(inlineMap.contains(i));
        EXPECT_EQ(inlineMap.find(i)->value, i);
    }
    EXPECT_FALSE(inlineMap.contains(1));

    // hashedMap now has inline content
    EXPECT_EQ(hashedMap.size(), 3u);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(hashedMap));
    for (unsigned i = 1; i <= 3; ++i) {
        EXPECT_TRUE(hashedMap.contains(i));
        EXPECT_EQ(hashedMap.find(i)->value, i * 10);
    }
    EXPECT_FALSE(hashedMap.contains(100));
}

TEST(WTF_InlineMap, ClearThenGrow)
{
    // A cleared map can grow beyond its original capacity.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    for (unsigned i = 1; i <= 20; ++i)
        map.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map));
    unsigned capacityAfterFirstGrowth = WTF::InlineMapAccessForTesting::capacity(map);

    map.clear();

    EXPECT_TRUE(map.isEmpty());
    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map)); // Storage preserved

    // Add enough entries to trigger growth beyond the cleared capacity
    for (unsigned i = 1; i <= 100; ++i)
        map.add(i, i + 100);

    EXPECT_EQ(map.size(), 100u);
    EXPECT_GT(WTF::InlineMapAccessForTesting::capacity(map), capacityAfterFirstGrowth);

    for (unsigned i = 1; i <= 100; ++i) {
        EXPECT_TRUE(map.contains(i));
        EXPECT_EQ(map.find(i)->value, i + 100);
    }
}

TEST(WTF_InlineMap, CopyWithDeletedEntries)
{
    // Copying a map with deleted tombstones produces a clean copy without them.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map1;

    for (unsigned i = 1; i <= 20; ++i)
        map1.add(i, i * 10);

    EXPECT_FALSE(WTF::InlineMapAccessForTesting::isInline(map1));

    // Remove some entries to create deleted tombstones
    map1.remove(3);
    map1.remove(7);
    map1.remove(11);
    map1.remove(15);
    map1.remove(19);

    EXPECT_EQ(map1.size(), 15u);

    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map2(map1);

    EXPECT_EQ(map2.size(), 15u);

    // Verify copy has exactly the right entries
    for (unsigned i = 1; i <= 20; ++i) {
        if (i == 3 || i == 7 || i == 11 || i == 15 || i == 19)
            EXPECT_FALSE(map2.contains(i));
        else {
            EXPECT_TRUE(map2.contains(i));
            EXPECT_EQ(map2.find(i)->value, i * 10);
        }
    }

    // Adding to copy should not affect original
    map2.add(3, 999);
    EXPECT_TRUE(map2.contains(3));
    EXPECT_FALSE(map1.contains(3));
}

TEST(WTF_InlineMap, RemoveLastInlineEntry)
{
    // Removing the sole inline entry leaves the map empty and still usable.
    InlineMap<unsigned, unsigned, 5, IntHash<unsigned>> map;

    map.add(42, 420);
    EXPECT_TRUE(WTF::InlineMapAccessForTesting::isInline(map));
    EXPECT_EQ(map.size(), 1u);

    EXPECT_TRUE(map.remove(42));
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.isEmpty());
    EXPECT_FALSE(map.contains(42));

    // Map should still be usable
    map.add(99, 990);
    EXPECT_EQ(map.size(), 1u);
    EXPECT_TRUE(map.contains(99));
    EXPECT_EQ(map.find(99)->value, 990u);
}

} // namespace TestWebKitAPI
