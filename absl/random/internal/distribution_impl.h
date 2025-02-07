// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_RANDOM_INTERNAL_DISTRIBUTION_IMPL_H_
#define ABSL_RANDOM_INTERNAL_DISTRIBUTION_IMPL_H_

// This file contains some implementation details which are used by one or more
// of the absl random number distributions.

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#if (defined(_WIN32) || defined(_WIN64)) && defined(_M_IA64)
#include <intrin.h>  // NOLINT(build/include_order)
#pragma intrinsic(_umul128)
#define ABSL_INTERNAL_USE_UMUL128 1
#endif

#include "absl/base/config.h"
#include "absl/base/internal/bits.h"
#include "absl/numeric/int128.h"
#include "absl/random/internal/fastmath.h"
#include "absl/random/internal/traits.h"

namespace absl {
namespace random_internal {

// Creates a double from `bits`, with the template fields controlling the
// output.
//
// RandU64To is both more efficient and generates more unique values in the
// result interval than known implementations of std::generate_canonical().
//
// The `Signed` parameter controls whether positive, negative, or both are
// returned (thus affecting the output interval).
//   When Signed == SignedValueT, range is U(-1, 1)
//   When Signed == NegativeValueT, range is U(-1, 0)
//   When Signed == PositiveValueT, range is U(0, 1)
//
// When the `IncludeZero` parameter is true, the function may return 0 for some
// inputs, otherwise it never returns 0.
//
// The `ExponentBias` parameter determines the scale of the output range by
// adjusting the exponent.
//
// When a value in U(0,1) is required, use:
//   RandU64ToDouble<PositiveValueT, true, 0>();
//
// When a value in U(-1,1) is required, use:
//   RandU64ToDouble<SignedValueT, false, 0>() => U(-1, 1)
// This generates more distinct values than the mathematically equivalent
// expression `U(0, 1) * 2.0 - 1.0`, and is preferable.
//
// Scaling the result by powers of 2 (and avoiding a multiply) is also possible:
//   RandU64ToDouble<PositiveValueT, false, 1>();  => U(0, 2)
//   RandU64ToDouble<PositiveValueT, false, -1>();  => U(0, 0.5)
//

// Tristate types controlling the output.
struct PositiveValueT {};
struct NegativeValueT {};
struct SignedValueT {};

// RandU64ToDouble is the double-result variant of RandU64To, described above.
template <typename Signed, bool IncludeZero, int ExponentBias = 0>
inline double RandU64ToDouble(uint64_t bits) {
  static_assert(std::is_same<Signed, PositiveValueT>::value ||
                    std::is_same<Signed, NegativeValueT>::value ||
                    std::is_same<Signed, SignedValueT>::value,
                "");

  // Maybe use the left-most bit for a sign bit.
  uint64_t sign = std::is_same<Signed, NegativeValueT>::value
                      ? 0x8000000000000000ull
                      : 0;  // Sign bits.

  if (std::is_same<Signed, SignedValueT>::value) {
    sign = bits & 0x8000000000000000ull;
    bits = bits & 0x7FFFFFFFFFFFFFFFull;
  }
  if (IncludeZero) {
    if (bits == 0u) return 0;
  }

  // Number of leading zeros is mapped to the exponent: 2^-clz
  int clz = base_internal::CountLeadingZeros64(bits);
  // Shift number left to erase leading zeros.
  bits <<= IncludeZero ? clz : (clz & 63);

  // Shift number right to remove bits that overflow double mantissa.  The
  // direction of the shift depends on `clz`.
  bits >>= (64 - DBL_MANT_DIG);

  // Compute IEEE 754 double exponent.
  // In the Signed case, bits is a 63-bit number with a 0 msb.  Adjust the
  // exponent to account for that.
  const uint64_t exp =
      (std::is_same<Signed, SignedValueT>::value ? 1023U : 1022U) +
      static_cast<uint64_t>(ExponentBias - clz);
  constexpr int kExp = DBL_MANT_DIG - 1;
  // Construct IEEE 754 double from exponent and mantissa.
  const uint64_t val = sign | (exp << kExp) | (bits & ((1ULL << kExp) - 1U));

  double res;
  static_assert(sizeof(res) == sizeof(val), "double is not 64 bit");
  // Memcpy value from "val" to "res" to avoid aliasing problems.  Assumes that
  // endian-ness is same for double and uint64_t.
  std::memcpy(&res, &val, sizeof(res));

  return res;
}

// RandU64ToFloat is the float-result variant of RandU64To, described above.
template <typename Signed, bool IncludeZero, int ExponentBias = 0>
inline float RandU64ToFloat(uint64_t bits) {
  static_assert(std::is_same<Signed, PositiveValueT>::value ||
                    std::is_same<Signed, NegativeValueT>::value ||
                    std::is_same<Signed, SignedValueT>::value,
                "");

  // Maybe use the left-most bit for a sign bit.
  uint64_t sign = std::is_same<Signed, NegativeValueT>::value
                      ? 0x80000000ul
                      : 0;  // Sign bits.

  if (std::is_same<Signed, SignedValueT>::value) {
    uint64_t a = bits & 0x8000000000000000ull;
    sign = static_cast<uint32_t>(a >> 32);
    bits = bits & 0x7FFFFFFFFFFFFFFFull;
  }
  if (IncludeZero) {
    if (bits == 0u) return 0;
  }

  // Number of leading zeros is mapped to the exponent: 2^-clz
  int clz = base_internal::CountLeadingZeros64(bits);
  // Shift number left to erase leading zeros.
  bits <<= IncludeZero ? clz : (clz & 63);
  // Shift number right to remove bits that overflow double mantissa.  The
  // direction of the shift depends on `clz`.
  bits >>= (64 - FLT_MANT_DIG);

  // Construct IEEE 754 float exponent.
  // In the Signed case, bits is a 63-bit number with a 0 msb.  Adjust the
  // exponent to account for that.
  const uint32_t exp =
      (std::is_same<Signed, SignedValueT>::value ? 127U : 126U) +
      static_cast<uint32_t>(ExponentBias - clz);
  constexpr int kExp = FLT_MANT_DIG - 1;
  const uint32_t val = sign | (exp << kExp) | (bits & ((1U << kExp) - 1U));

  float res;
  static_assert(sizeof(res) == sizeof(val), "float is not 32 bit");
  // Assumes that endian-ness is same for float and uint32_t.
  std::memcpy(&res, &val, sizeof(res));

  return res;
}

template <typename Result>
struct RandU64ToReal {
  template <typename Signed, bool IncludeZero, int ExponentBias = 0>
  static inline Result Value(uint64_t bits) {
    return RandU64ToDouble<Signed, IncludeZero, ExponentBias>(bits);
  }
};

template <>
struct RandU64ToReal<float> {
  template <typename Signed, bool IncludeZero, int ExponentBias = 0>
  static inline float Value(uint64_t bits) {
    return RandU64ToFloat<Signed, IncludeZero, ExponentBias>(bits);
  }
};

}  // namespace random_internal
}  // namespace absl

#endif  // ABSL_RANDOM_INTERNAL_DISTRIBUTION_IMPL_H_
