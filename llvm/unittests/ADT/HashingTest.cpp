//===- llvm/unittest/ADT/HashingTest.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hashing.h unit tests.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Hashing.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/HashBuilder.h"
#include "gtest/gtest.h"
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <vector>

namespace llvm {

// Helper for test code to print hash codes.
void PrintTo(const hash_code &code, std::ostream *os) {
  *os << static_cast<size_t>(code);
}

// Fake an object that is recognized as hashable data to test super large
// objects.
struct LargeTestInteger { uint64_t arr[8]; };

struct NonPOD {
  uint64_t x, y;
  NonPOD(uint64_t x, uint64_t y) : x(x), y(y) {}
  friend hash_code hash_value(const NonPOD &obj) {
    return hash_combine(obj.x, obj.y);
  }
};

namespace hashing {
namespace detail {
template <> struct is_hashable_data<LargeTestInteger> : std::true_type {};
} // namespace detail
} // namespace hashing

} // namespace llvm

using namespace llvm;

namespace {

enum TestEnumeration {
  TE_Foo = 42,
  TE_Bar = 43
};

TEST(HashingTest, HashValueBasicTest) {
  int x = 42, y = 43, c = 'x';
  void *p = nullptr;
  uint64_t i = 71;
  const unsigned ci = 71;
  volatile int vi = 71;
  const volatile int cvi = 71;
  uintptr_t addr = reinterpret_cast<uintptr_t>(&y);
  EXPECT_EQ(hash_value(42), hash_value(x));
  EXPECT_EQ(hash_value(42), hash_value(TE_Foo));
  EXPECT_NE(hash_value(42), hash_value(y));
  EXPECT_NE(hash_value(42), hash_value(TE_Bar));
  EXPECT_NE(hash_value(42), hash_value(p));
  EXPECT_EQ(hash_value(71), hash_value(i));
  EXPECT_EQ(hash_value(71), hash_value(ci));
  EXPECT_EQ(hash_value(71), hash_value(vi));
  EXPECT_EQ(hash_value(71), hash_value(cvi));
  EXPECT_EQ(hash_value(c), hash_value('x'));
  EXPECT_EQ(hash_value('4'), hash_value('0' + 4));
  EXPECT_EQ(hash_value(addr), hash_value(&y));
}

TEST(HashingTest, HashValueStdPair) {
  EXPECT_EQ(hash_combine(42, 43), hash_value(std::make_pair(42, 43)));
  EXPECT_NE(hash_combine(43, 42), hash_value(std::make_pair(42, 43)));
  EXPECT_NE(hash_combine(42, 43), hash_value(std::make_pair(42ull, 43ull)));
  EXPECT_NE(hash_combine(42, 43), hash_value(std::make_pair(42, 43ull)));
  EXPECT_NE(hash_combine(42, 43), hash_value(std::make_pair(42ull, 43)));

  // Note that pairs are implicitly flattened to a direct sequence of data and
  // hashed efficiently as a consequence.
  EXPECT_EQ(hash_combine(42, 43, 44),
            hash_value(std::make_pair(42, std::make_pair(43, 44))));
  EXPECT_EQ(hash_value(std::make_pair(42, std::make_pair(43, 44))),
            hash_value(std::make_pair(std::make_pair(42, 43), 44)));

  // Ensure that pairs which have padding bytes *inside* them don't get treated
  // this way.
  EXPECT_EQ(hash_combine('0', hash_combine(1ull, '2')),
            hash_value(std::make_pair('0', std::make_pair(1ull, '2'))));

  // Ensure that non-POD pairs don't explode the traits used.
  NonPOD obj1(1, 2), obj2(3, 4), obj3(5, 6);
  EXPECT_EQ(hash_combine(obj1, hash_combine(obj2, obj3)),
            hash_value(std::make_pair(obj1, std::make_pair(obj2, obj3))));
}

TEST(HashingTest, HashValueStdTuple) {
  EXPECT_EQ(hash_combine(), hash_value(std::make_tuple()));
  EXPECT_EQ(hash_combine(42), hash_value(std::make_tuple(42)));
  EXPECT_EQ(hash_combine(42, 'c'), hash_value(std::make_tuple(42, 'c')));

  EXPECT_NE(hash_combine(43, 42), hash_value(std::make_tuple(42, 43)));
  EXPECT_NE(hash_combine(42, 43), hash_value(std::make_tuple(42ull, 43ull)));
  EXPECT_NE(hash_combine(42, 43), hash_value(std::make_tuple(42, 43ull)));
  EXPECT_NE(hash_combine(42, 43), hash_value(std::make_tuple(42ull, 43)));
}

TEST(HashingTest, HashValueStdString) {
  std::string s = "Hello World!";
  EXPECT_EQ(hash_combine_range(s.c_str(), s.c_str() + s.size()), hash_value(s));
  EXPECT_EQ(hash_combine_range(s.c_str(), s.c_str() + s.size() - 1),
            hash_value(s.substr(0, s.size() - 1)));
  EXPECT_EQ(hash_combine_range(s.c_str() + 1, s.c_str() + s.size() - 1),
            hash_value(s.substr(1, s.size() - 2)));

  std::wstring ws = L"Hello Wide World!";
  EXPECT_EQ(hash_combine_range(ws.c_str(), ws.c_str() + ws.size()),
            hash_value(ws));
  EXPECT_EQ(hash_combine_range(ws.c_str(), ws.c_str() + ws.size() - 1),
            hash_value(ws.substr(0, ws.size() - 1)));
  EXPECT_EQ(hash_combine_range(ws.c_str() + 1, ws.c_str() + ws.size() - 1),
            hash_value(ws.substr(1, ws.size() - 2)));
}

TEST(HashingTest, HashValueStdOptional) {
  // Check that std::nullopt, false, and true all hash differently.
  std::optional<bool> B, B0 = false, B1 = true;
  EXPECT_NE(hash_value(B0), hash_value(B));
  EXPECT_NE(hash_value(B1), hash_value(B));
  EXPECT_NE(hash_value(B1), hash_value(B0));

  // Check that std::nullopt, 0, and 1 all hash differently.
  std::optional<int> I, I0 = 0, I1 = 1;
  EXPECT_NE(hash_value(I0), hash_value(I));
  EXPECT_NE(hash_value(I1), hash_value(I));
  EXPECT_NE(hash_value(I1), hash_value(I0));

  // Check std::nullopt hash the same way regardless of type.
  EXPECT_EQ(hash_value(B), hash_value(I));
}

template <typename T, size_t N> T *begin(T (&arr)[N]) { return arr; }
template <typename T, size_t N> T *end(T (&arr)[N]) { return arr + N; }

// Provide a dummy, hashable type designed for easy verification: its hash is
// the same as its value.
struct HashableDummy { size_t value; };
hash_code hash_value(HashableDummy dummy) { return dummy.value; }

TEST(HashingTest, HashCombineRangeBasicTest) {
  // Leave this uninitialized in the hope that valgrind will catch bad reads.
  int dummy;
  hash_code dummy_hash = hash_combine_range(&dummy, &dummy);
  EXPECT_NE(hash_code(0), dummy_hash);

  const int arr1[] = { 1, 2, 3 };
  hash_code arr1_hash = hash_combine_range(begin(arr1), end(arr1));
  EXPECT_NE(dummy_hash, arr1_hash);
  EXPECT_EQ(arr1_hash, hash_combine_range(begin(arr1), end(arr1)));
  EXPECT_EQ(arr1_hash, hash_combine_range(arr1));

  const std::vector<int> vec(begin(arr1), end(arr1));
  EXPECT_EQ(arr1_hash, hash_combine_range(vec.begin(), vec.end()));
  EXPECT_EQ(arr1_hash, hash_combine_range(vec));

  const std::list<int> list(begin(arr1), end(arr1));
  EXPECT_EQ(arr1_hash, hash_combine_range(list.begin(), list.end()));
  EXPECT_EQ(arr1_hash, hash_combine_range(list));

  const std::deque<int> deque(begin(arr1), end(arr1));
  EXPECT_EQ(arr1_hash, hash_combine_range(deque.begin(), deque.end()));
  EXPECT_EQ(arr1_hash, hash_combine_range(deque));

  const int arr2[] = { 3, 2, 1 };
  hash_code arr2_hash = hash_combine_range(begin(arr2), end(arr2));
  EXPECT_NE(dummy_hash, arr2_hash);
  EXPECT_NE(arr1_hash, arr2_hash);

  const int arr3[] = { 1, 1, 2, 3 };
  hash_code arr3_hash = hash_combine_range(begin(arr3), end(arr3));
  EXPECT_NE(dummy_hash, arr3_hash);
  EXPECT_NE(arr1_hash, arr3_hash);

  const int arr4[] = { 1, 2, 3, 3 };
  hash_code arr4_hash = hash_combine_range(begin(arr4), end(arr4));
  EXPECT_NE(dummy_hash, arr4_hash);
  EXPECT_NE(arr1_hash, arr4_hash);

  const size_t arr5[] = { 1, 2, 3 };
  const HashableDummy d_arr5[] = { {1}, {2}, {3} };
  hash_code arr5_hash = hash_combine_range(begin(arr5), end(arr5));
  hash_code d_arr5_hash = hash_combine_range(begin(d_arr5), end(d_arr5));
  EXPECT_EQ(arr5_hash, d_arr5_hash);
}

TEST(HashingTest, HashCombineRangeLengthDiff) {
  // Test that as only the length varies, we compute different hash codes for
  // sequences.
  std::map<size_t, size_t> code_to_size;
  std::vector<char> all_one_c(256, '\xff');
  for (unsigned Idx = 1, Size = all_one_c.size(); Idx < Size; ++Idx) {
    hash_code code = hash_combine_range(&all_one_c[0], &all_one_c[0] + Idx);
    std::map<size_t, size_t>::iterator
      I = code_to_size.insert(std::make_pair(code, Idx)).first;
    EXPECT_EQ(Idx, I->second);
  }
  code_to_size.clear();
  std::vector<char> all_zero_c(256, '\0');
  for (unsigned Idx = 1, Size = all_zero_c.size(); Idx < Size; ++Idx) {
    hash_code code = hash_combine_range(&all_zero_c[0], &all_zero_c[0] + Idx);
    std::map<size_t, size_t>::iterator
      I = code_to_size.insert(std::make_pair(code, Idx)).first;
    EXPECT_EQ(Idx, I->second);
  }
  code_to_size.clear();
  std::vector<unsigned> all_one_int(512, -1);
  for (unsigned Idx = 1, Size = all_one_int.size(); Idx < Size; ++Idx) {
    hash_code code = hash_combine_range(&all_one_int[0], &all_one_int[0] + Idx);
    std::map<size_t, size_t>::iterator
      I = code_to_size.insert(std::make_pair(code, Idx)).first;
    EXPECT_EQ(Idx, I->second);
  }
  code_to_size.clear();
  std::vector<unsigned> all_zero_int(512, 0);
  for (unsigned Idx = 1, Size = all_zero_int.size(); Idx < Size; ++Idx) {
    hash_code code = hash_combine_range(&all_zero_int[0], &all_zero_int[0] + Idx);
    std::map<size_t, size_t>::iterator
      I = code_to_size.insert(std::make_pair(code, Idx)).first;
    EXPECT_EQ(Idx, I->second);
  }
}

TEST(HashingTest, HashCombineBasicTest) {
  // Hashing a sequence of homogenous types matches range hashing.
  const int i1 = 42, i2 = 43, i3 = 123, i4 = 999, i5 = 0, i6 = 79;
  const int arr1[] = { i1, i2, i3, i4, i5, i6 };
  EXPECT_EQ(hash_combine_range(arr1, arr1 + 1), hash_combine(i1));
  EXPECT_EQ(hash_combine_range(arr1, arr1 + 2), hash_combine(i1, i2));
  EXPECT_EQ(hash_combine_range(arr1, arr1 + 3), hash_combine(i1, i2, i3));
  EXPECT_EQ(hash_combine_range(arr1, arr1 + 4), hash_combine(i1, i2, i3, i4));
  EXPECT_EQ(hash_combine_range(arr1, arr1 + 5),
            hash_combine(i1, i2, i3, i4, i5));
  EXPECT_EQ(hash_combine_range(arr1, arr1 + 6),
            hash_combine(i1, i2, i3, i4, i5, i6));

  // Hashing a sequence of heterogeneous types which *happen* to all produce the
  // same data for hashing produces the same as a range-based hash of the
  // fundamental values.
  const size_t s1 = 1024, s2 = 8888, s3 = 9000000;
  const HashableDummy d1 = { 1024 }, d2 = { 8888 }, d3 = { 9000000 };
  const size_t arr2[] = { s1, s2, s3 };
  EXPECT_EQ(hash_combine_range(begin(arr2), end(arr2)),
            hash_combine(s1, s2, s3));
  EXPECT_EQ(hash_combine(s1, s2, s3), hash_combine(s1, s2, d3));
  EXPECT_EQ(hash_combine(s1, s2, s3), hash_combine(s1, d2, s3));
  EXPECT_EQ(hash_combine(s1, s2, s3), hash_combine(d1, s2, s3));
  EXPECT_EQ(hash_combine(s1, s2, s3), hash_combine(d1, d2, s3));
  EXPECT_EQ(hash_combine(s1, s2, s3), hash_combine(d1, d2, d3));

  // Permuting values causes hashes to change.
  EXPECT_NE(hash_combine(i1, i1, i1), hash_combine(i1, i1, i2));
  EXPECT_NE(hash_combine(i1, i1, i1), hash_combine(i1, i2, i1));
  EXPECT_NE(hash_combine(i1, i1, i1), hash_combine(i2, i1, i1));
  EXPECT_NE(hash_combine(i1, i1, i1), hash_combine(i2, i2, i1));
  EXPECT_NE(hash_combine(i1, i1, i1), hash_combine(i2, i2, i2));
  EXPECT_NE(hash_combine(i2, i1, i1), hash_combine(i1, i1, i2));
  EXPECT_NE(hash_combine(i1, i1, i2), hash_combine(i1, i2, i1));
  EXPECT_NE(hash_combine(i1, i2, i1), hash_combine(i2, i1, i1));

  // Changing type w/o changing value causes hashes to change.
  EXPECT_NE(hash_combine(i1, i2, i3), hash_combine((char)i1, i2, i3));
  EXPECT_NE(hash_combine(i1, i2, i3), hash_combine(i1, (char)i2, i3));
  EXPECT_NE(hash_combine(i1, i2, i3), hash_combine(i1, i2, (char)i3));

  // This is array of uint64, but it should have the exact same byte pattern as
  // an array of LargeTestIntegers.
  const uint64_t bigarr[] = {
    0xaaaaaaaaababababULL, 0xacacacacbcbcbcbcULL, 0xccddeeffeeddccbbULL,
    0xdeadbeafdeadbeefULL, 0xfefefefededededeULL, 0xafafafafededededULL,
    0xffffeeeeddddccccULL, 0xaaaacbcbffffababULL,
    0xaaaaaaaaababababULL, 0xacacacacbcbcbcbcULL, 0xccddeeffeeddccbbULL,
    0xdeadbeafdeadbeefULL, 0xfefefefededededeULL, 0xafafafafededededULL,
    0xffffeeeeddddccccULL, 0xaaaacbcbffffababULL,
    0xaaaaaaaaababababULL, 0xacacacacbcbcbcbcULL, 0xccddeeffeeddccbbULL,
    0xdeadbeafdeadbeefULL, 0xfefefefededededeULL, 0xafafafafededededULL,
    0xffffeeeeddddccccULL, 0xaaaacbcbffffababULL
  };
  // Hash a preposterously large integer, both aligned with the buffer and
  // misaligned.
  const LargeTestInteger li = { {
    0xaaaaaaaaababababULL, 0xacacacacbcbcbcbcULL, 0xccddeeffeeddccbbULL,
    0xdeadbeafdeadbeefULL, 0xfefefefededededeULL, 0xafafafafededededULL,
    0xffffeeeeddddccccULL, 0xaaaacbcbffffababULL
  } };
  // Rotate the storage from 'li'.
  const LargeTestInteger l2 = { {
    0xacacacacbcbcbcbcULL, 0xccddeeffeeddccbbULL, 0xdeadbeafdeadbeefULL,
    0xfefefefededededeULL, 0xafafafafededededULL, 0xffffeeeeddddccccULL,
    0xaaaacbcbffffababULL, 0xaaaaaaaaababababULL
  } };
  const LargeTestInteger l3 = { {
    0xccddeeffeeddccbbULL, 0xdeadbeafdeadbeefULL, 0xfefefefededededeULL,
    0xafafafafededededULL, 0xffffeeeeddddccccULL, 0xaaaacbcbffffababULL,
    0xaaaaaaaaababababULL, 0xacacacacbcbcbcbcULL
  } };
  EXPECT_EQ(hash_combine_range(begin(bigarr), end(bigarr)),
            hash_combine(li, li, li));
  EXPECT_EQ(hash_combine_range(bigarr, bigarr + 9),
            hash_combine(bigarr[0], l2));
  EXPECT_EQ(hash_combine_range(bigarr, bigarr + 10),
            hash_combine(bigarr[0], bigarr[1], l3));
  EXPECT_EQ(hash_combine_range(bigarr, bigarr + 17),
            hash_combine(li, bigarr[0], l2));
  EXPECT_EQ(hash_combine_range(bigarr, bigarr + 18),
            hash_combine(li, bigarr[0], bigarr[1], l3));
  EXPECT_EQ(hash_combine_range(bigarr, bigarr + 18),
            hash_combine(bigarr[0], l2, bigarr[9], l3));
  EXPECT_EQ(hash_combine_range(bigarr, bigarr + 20),
            hash_combine(bigarr[0], l2, bigarr[9], l3, bigarr[18], bigarr[19]));
}

TEST(HashingTest, HashCombineArgs18) {
  // This tests that we can pass in up to 18 args.
#define CHECK_SAME(...)                                                        \
  EXPECT_EQ(hash_combine(__VA_ARGS__), hash_combine(__VA_ARGS__))
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8, 9);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7, 8);
  CHECK_SAME(1, 2, 3, 4, 5, 6, 7);
  CHECK_SAME(1, 2, 3, 4, 5, 6);
  CHECK_SAME(1, 2, 3, 4, 5);
  CHECK_SAME(1, 2, 3, 4);
  CHECK_SAME(1, 2, 3);
  CHECK_SAME(1, 2);
  CHECK_SAME(1);
#undef CHECK_SAME
}

struct StructWithHashBuilderSupport {
  char C;
  int I;
  template <typename HasherT, llvm::endianness Endianness>
  friend void addHash(llvm::HashBuilder<HasherT, Endianness> &HBuilder,
                      const StructWithHashBuilderSupport &Value) {
    HBuilder.add(Value.C, Value.I);
  }
};

TEST(HashingTest, HashWithHashBuilder) {
  StructWithHashBuilderSupport S{'c', 1};
  EXPECT_NE(static_cast<size_t>(llvm::hash_value(S)), static_cast<size_t>(0));
}

struct StructWithHashBuilderAndHashValueSupport {
  char C;
  int I;
  template <typename HasherT, llvm::endianness Endianness>
  friend void addHash(llvm::HashBuilder<HasherT, Endianness> &HBuilder,
                      const StructWithHashBuilderAndHashValueSupport &Value) {}
  friend hash_code
  hash_value(const StructWithHashBuilderAndHashValueSupport &Value) {
    return 0xbeef;
  }
};

TEST(HashingTest, HashBuilderAndHashValue) {
  StructWithHashBuilderAndHashValueSupport S{'c', 1};
  EXPECT_EQ(static_cast<size_t>(hash_value(S)), static_cast<size_t>(0xbeef));
}

} // namespace
