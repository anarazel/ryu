// Copyright 2018 Ulf Adams
//
// The contents of this file may be used under the terms of the Apache License,
// Version 2.0.
//
//    (See accompanying file LICENSE-Apache or copy at
//     http://www.apache.org/licenses/LICENSE-2.0)
//
// Alternatively, the contents of this file may be used under the terms of
// the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE-Boost or copy at
//     https://www.boost.org/LICENSE_1_0.txt)
//
// Unless required by applicable law or agreed to in writing, this software
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.

// Runtime compiler options:
// -DRYU_DEBUG Generate verbose debugging output to stdout.

#include "ryu/ryu.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef RYU_DEBUG
#include <stdio.h>
#endif

#include "ryu/common.h"
#include "ryu/digit_table.h"

#define FLOAT_MANTISSA_BITS 23
#define FLOAT_EXPONENT_BITS 8

// This table is generated by PrintFloatLookupTable.
#define FLOAT_POW5_INV_BITCOUNT 59
static const uint64_t FLOAT_POW5_INV_SPLIT[31] = {
  576460752303423489u, 461168601842738791u, 368934881474191033u, 295147905179352826u,
  472236648286964522u, 377789318629571618u, 302231454903657294u, 483570327845851670u,
  386856262276681336u, 309485009821345069u, 495176015714152110u, 396140812571321688u,
  316912650057057351u, 507060240091291761u, 405648192073033409u, 324518553658426727u,
  519229685853482763u, 415383748682786211u, 332306998946228969u, 531691198313966350u,
  425352958651173080u, 340282366920938464u, 544451787073501542u, 435561429658801234u,
  348449143727040987u, 557518629963265579u, 446014903970612463u, 356811923176489971u,
  570899077082383953u, 456719261665907162u, 365375409332725730u
};
#define FLOAT_POW5_BITCOUNT 61
static const uint64_t FLOAT_POW5_SPLIT[47] = {
 1152921504606846976u, 1441151880758558720u, 1801439850948198400u, 2251799813685248000u,
 1407374883553280000u, 1759218604441600000u, 2199023255552000000u, 1374389534720000000u,
 1717986918400000000u, 2147483648000000000u, 1342177280000000000u, 1677721600000000000u,
 2097152000000000000u, 1310720000000000000u, 1638400000000000000u, 2048000000000000000u,
 1280000000000000000u, 1600000000000000000u, 2000000000000000000u, 1250000000000000000u,
 1562500000000000000u, 1953125000000000000u, 1220703125000000000u, 1525878906250000000u,
 1907348632812500000u, 1192092895507812500u, 1490116119384765625u, 1862645149230957031u,
 1164153218269348144u, 1455191522836685180u, 1818989403545856475u, 2273736754432320594u,
 1421085471520200371u, 1776356839400250464u, 2220446049250313080u, 1387778780781445675u,
 1734723475976807094u, 2168404344971008868u, 1355252715606880542u, 1694065894508600678u,
 2117582368135750847u, 1323488980084844279u, 1654361225106055349u, 2067951531382569187u,
 1292469707114105741u, 1615587133892632177u, 2019483917365790221u
};

static inline uint32_t pow5Factor(uint32_t value) {
  for (uint32_t count = 0; value > 0; ++count) {
    if (value % 5 != 0) {
      return count;
    }
    value /= 5;
  }
  return 0;
}

// Returns true if value is divisible by 5^p.
static inline bool multipleOfPowerOf5(const uint32_t value, const uint32_t p) {
  return pow5Factor(value) >= p;
}

// Returns true if value is divisible by 2^p.
static inline bool multipleOfPowerOf2(const uint32_t value, const uint32_t p) {
  // return __builtin_ctz(value) >= p;
  return (value & ((1u << p) - 1)) == 0;
}

// It seems to be slightly faster to avoid uint128_t here, although the
// generated code for uint128_t looks slightly nicer.
static inline uint32_t mulShift(const uint32_t m, const uint64_t factor, const int32_t shift) {
  assert(shift > 32);

  // The casts here help MSVC to avoid calls to the __allmul library
  // function.
  const uint32_t factorLo = (uint32_t)(factor);
  const uint32_t factorHi = (uint32_t)(factor >> 32);
  const uint64_t bits0 = (uint64_t)m * factorLo;
  const uint64_t bits1 = (uint64_t)m * factorHi;

#if defined(_M_IX86) || defined(_M_ARM)
  // On 32-bit platforms we can avoid a 64-bit shift-right since we only
  // need the upper 32 bits of the result and the shift value is > 32.
  const uint32_t bits0Hi = (uint32_t)(bits0 >> 32);
  uint32_t bits1Lo = (uint32_t)(bits1);
  uint32_t bits1Hi = (uint32_t)(bits1 >> 32);
  bits1Lo += bits0Hi;
  bits1Hi += (bits1Lo < bits0Hi);
  const int32_t s = shift - 32;
  return (bits1Hi << (32 - s)) | (bits1Lo >> s);
#else
  const uint64_t sum = (bits0 >> 32) + bits1;
  const uint64_t shiftedSum = sum >> (shift - 32);
  assert(shiftedSum <= UINT32_MAX);
  return (uint32_t) shiftedSum;
#endif
}

static inline uint32_t mulPow5InvDivPow2(const uint32_t m, const uint32_t q, const int32_t j) {
  return mulShift(m, FLOAT_POW5_INV_SPLIT[q], j);
}

static inline uint32_t mulPow5divPow2(const uint32_t m, const uint32_t i, const int32_t j) {
  return mulShift(m, FLOAT_POW5_SPLIT[i], j);
}

static inline uint32_t decimalLength(const uint32_t v) {
  // Function precondition: v is not a 10-digit number.
  // (9 digits are sufficient for round-tripping.)
  assert(v < 1000000000);
  if (v >= 100000000) { return 9; }
  if (v >= 10000000) { return 8; }
  if (v >= 1000000) { return 7; }
  if (v >= 100000) { return 6; }
  if (v >= 10000) { return 5; }
  if (v >= 1000) { return 4; }
  if (v >= 100) { return 3; }
  if (v >= 10) { return 2; }
  return 1;
}

// A floating decimal representing m * 10^e.
struct floating_decimal_32 {
  uint32_t mantissa;
  int32_t exponent;
};

static inline struct floating_decimal_32 f2d(const uint32_t ieeeMantissa, const uint32_t ieeeExponent) {
  const uint32_t bias = (1u << (FLOAT_EXPONENT_BITS - 1)) - 1;

  int32_t e2;
  uint32_t m2;
  if (ieeeExponent == 0) {
    // We subtract 2 so that the bounds computation has 2 additional bits.
    e2 = 1 - bias - FLOAT_MANTISSA_BITS - 2;
    m2 = ieeeMantissa;
  } else {
    e2 = ieeeExponent - bias - FLOAT_MANTISSA_BITS - 2;
    m2 = (1u << FLOAT_MANTISSA_BITS) | ieeeMantissa;
  }
  const bool even = (m2 & 1) == 0;
  const bool acceptBounds = even;

#ifdef RYU_DEBUG
  printf("-> %u * 2^%d\n", m2, e2 + 2);
#endif

  // Step 2: Determine the interval of legal decimal representations.
  const uint32_t mv = 4 * m2;
  const uint32_t mp = 4 * m2 + 2;
  // Implicit bool -> int conversion. True is 1, false is 0.
  const uint32_t mmShift = (ieeeMantissa != 0) || (ieeeExponent <= 1);
  const uint32_t mm = 4 * m2 - 1 - mmShift;

  // Step 3: Convert to a decimal power base using 64-bit arithmetic.
  uint32_t vr, vp, vm;
  int32_t e10;
  bool vmIsTrailingZeros = false;
  bool vrIsTrailingZeros = false;
  uint8_t lastRemovedDigit = 0;
  if (e2 >= 0) {
    const uint32_t q = log10Pow2(e2);
    e10 = q;
    const int32_t k = FLOAT_POW5_INV_BITCOUNT + pow5bits(q) - 1;
    const int32_t i = -e2 + q + k;
    vr = mulPow5InvDivPow2(mv, q, i);
    vp = mulPow5InvDivPow2(mp, q, i);
    vm = mulPow5InvDivPow2(mm, q, i);
#ifdef RYU_DEBUG
    printf("%u * 2^%d / 10^%u\n", mv, e2, q);
    printf("V+=%u\nV =%u\nV-=%u\n", vp, vr, vm);
#endif
    if (q != 0 && ((vp - 1) / 10 <= vm / 10)) {
      // We need to know one removed digit even if we are not going to loop below. We could use
      // q = X - 1 above, except that would require 33 bits for the result, and we've found that
      // 32-bit arithmetic is faster even on 64-bit machines.
      const int32_t l = FLOAT_POW5_INV_BITCOUNT + pow5bits(q - 1) - 1;
      lastRemovedDigit = (uint8_t) (mulPow5InvDivPow2(mv, q - 1, -e2 + q - 1 + l) % 10);
    }
    if (q <= 9) {
      // The largest power of 5 that fits in 24 bits is 5^10, but q<=9 seems to be safe as well.
      // Only one of mp, mv, and mm can be a multiple of 5, if any.
      if (mv % 5 == 0) {
        vrIsTrailingZeros = multipleOfPowerOf5(mv, q);
      } else if (acceptBounds) {
        vmIsTrailingZeros = multipleOfPowerOf5(mm, q);
      } else {
        vp -= multipleOfPowerOf5(mp, q);
      }
    }
  } else {
    const uint32_t q = log10Pow5(-e2);
    e10 = q + e2;
    const int32_t i = -e2 - q;
    const int32_t k = pow5bits(i) - FLOAT_POW5_BITCOUNT;
    int32_t j = q - k;
    vr = mulPow5divPow2(mv, i, j);
    vp = mulPow5divPow2(mp, i, j);
    vm = mulPow5divPow2(mm, i, j);
#ifdef RYU_DEBUG
    printf("%u * 5^%d / 10^%u\n", mv, -e2, q);
    printf("%u %d %d %d\n", q, i, k, j);
    printf("V+=%u\nV =%u\nV-=%u\n", vp, vr, vm);
#endif
    if (q != 0 && ((vp - 1) / 10 <= vm / 10)) {
      j = q - 1 - (pow5bits(i + 1) - FLOAT_POW5_BITCOUNT);
      lastRemovedDigit = (uint8_t) (mulPow5divPow2(mv, i + 1, j) % 10);
    }
    if (q <= 1) {
      // {vr,vp,vm} is trailing zeros if {mv,mp,mm} has at least q trailing 0 bits.
      // mv = 4 * m2, so it always has at least two trailing 0 bits.
      vrIsTrailingZeros = true;
      if (acceptBounds) {
        // mm = mv - 1 - mmShift, so it has 1 trailing 0 bit iff mmShift == 1.
        vmIsTrailingZeros = mmShift == 1;
      } else {
        // mp = mv + 2, so it always has at least one trailing 0 bit.
        --vp;
      }
    } else if (q < 31) { // TODO(ulfjack): Use a tighter bound here.
      vrIsTrailingZeros = multipleOfPowerOf2(mv, q - 1);
#ifdef RYU_DEBUG
      printf("vr is trailing zeros=%s\n", vrIsTrailingZeros ? "true" : "false");
#endif
    }
  }
#ifdef RYU_DEBUG
  printf("e10=%d\n", e10);
  printf("V+=%u\nV =%u\nV-=%u\n", vp, vr, vm);
  printf("vm is trailing zeros=%s\n", vmIsTrailingZeros ? "true" : "false");
  printf("vr is trailing zeros=%s\n", vrIsTrailingZeros ? "true" : "false");
#endif

  // Step 4: Find the shortest decimal representation in the interval of legal representations.
  uint32_t removed = 0;
  uint32_t output;
  if (vmIsTrailingZeros || vrIsTrailingZeros) {
    // General case, which happens rarely.
    while (vp / 10 > vm / 10) {
#ifdef __clang__ // https://bugs.llvm.org/show_bug.cgi?id=23106
      // The compiler does not realize that vm % 10 can be computed from vm / 10
      // as vm - (vm / 10) * 10.
      vmIsTrailingZeros &= vm - (vm / 10) * 10 == 0;
#else
      vmIsTrailingZeros &= vm % 10 == 0;
#endif
      vrIsTrailingZeros &= lastRemovedDigit == 0;
      lastRemovedDigit = (uint8_t) (vr % 10);
      vr /= 10;
      vp /= 10;
      vm /= 10;
      ++removed;
    }
#ifdef RYU_DEBUG
    printf("V+=%u\nV =%u\nV-=%u\n", vp, vr, vm);
    printf("d-10=%s\n", vmIsTrailingZeros ? "true" : "false");
#endif
    if (vmIsTrailingZeros) {
      while (vm % 10 == 0) {
        vrIsTrailingZeros &= lastRemovedDigit == 0;
        lastRemovedDigit = (uint8_t) (vr % 10);
        vr /= 10;
        vp /= 10;
        vm /= 10;
        ++removed;
      }
    }
#ifdef RYU_DEBUG
    printf("%u %d\n", vr, lastRemovedDigit);
    printf("vr is trailing zeros=%s\n", vrIsTrailingZeros ? "true" : "false");
#endif
    if (vrIsTrailingZeros && (lastRemovedDigit == 5) && (vr % 2 == 0)) {
      // Round even if the exact number is .....50..0.
      lastRemovedDigit = 4;
    }
    // We need to take vr+1 if vr is outside bounds or we need to round up.
    output = vr +
        ((vr == vm && (!acceptBounds || !vmIsTrailingZeros)) || (lastRemovedDigit >= 5));
  } else {
    // Common case.
    while (vp / 10 > vm / 10) {
      lastRemovedDigit = (uint8_t) (vr % 10);
      vr /= 10;
      vp /= 10;
      vm /= 10;
      ++removed;
    }
#ifdef RYU_DEBUG
    printf("%u %d\n", vr, lastRemovedDigit);
    printf("vr is trailing zeros=%s\n", vrIsTrailingZeros ? "true" : "false");
#endif
    // We need to take vr+1 if vr is outside bounds or we need to round up.
    output = vr + ((vr == vm) || (lastRemovedDigit >= 5));
  }
  const int32_t exp = e10 + removed;

#ifdef RYU_DEBUG
  printf("V+=%u\nV =%u\nV-=%u\n", vp, vr, vm);
  printf("O=%u\n", output);
  printf("EXP=%d\n", exp);
#endif

  struct floating_decimal_32 fd;
  fd.exponent = exp;
  fd.mantissa = output;
  return fd;
}

static inline int to_chars(const struct floating_decimal_32 v, const bool sign, char* const result) {
  // Step 5: Print the decimal representation.
  int index = 0;
  if (sign) {
    result[index++] = '-';
  }

  uint32_t output = v.mantissa;
  const uint32_t olength = decimalLength(output);

#ifdef RYU_DEBUG
  printf("DIGITS=%u\n", v.mantissa);
  printf("OLEN=%u\n", olength);
  printf("EXP=%u\n", v.exponent + olength);
#endif

  // Print the decimal digits.
  // The following code is equivalent to:
  // for (uint32_t i = 0; i < olength - 1; ++i) {
  //   const uint32_t c = output % 10; output /= 10;
  //   result[index + olength - i] = (char) ('0' + c);
  // }
  // result[index] = '0' + output % 10;
  uint32_t i = 0;
  while (output >= 10000) {
#ifdef __clang__ // https://bugs.llvm.org/show_bug.cgi?id=38217
    const uint32_t c = output - 10000 * (output / 10000);
#else
    const uint32_t c = output % 10000;
#endif
    output /= 10000;
    const uint32_t c0 = (c % 100) << 1;
    const uint32_t c1 = (c / 100) << 1;
    memcpy(result + index + olength - i - 1, DIGIT_TABLE + c0, 2);
    memcpy(result + index + olength - i - 3, DIGIT_TABLE + c1, 2);
    i += 4;
  }
  if (output >= 100) {
    const uint32_t c = (output % 100) << 1;
    output /= 100;
    memcpy(result + index + olength - i - 1, DIGIT_TABLE + c, 2);
    i += 2;
  }
  if (output >= 10) {
    const uint32_t c = output << 1;
    // We can't use memcpy here: the decimal dot goes between these two digits.
    result[index + olength - i] = DIGIT_TABLE[c + 1];
    result[index] = DIGIT_TABLE[c];
  } else {
    result[index] = (char) ('0' + output);
  }

  // Print decimal point if needed.
  if (olength > 1) {
    result[index + 1] = '.';
    index += olength + 1;
  } else {
    ++index;
  }

  // Print the exponent.
  result[index++] = 'E';
  int32_t exp = v.exponent + olength - 1;
  if (exp < 0) {
    result[index++] = '-';
    exp = -exp;
  }

  if (exp >= 10) {
    memcpy(result + index, DIGIT_TABLE + (2 * exp), 2);
    index += 2;
  } else {
    result[index++] = (char) ('0' + exp);
  }

  return index;
}

int f2s_buffered_n(float f, char* result) {
  // Step 1: Decode the floating-point number, and unify normalized and subnormal cases.
  uint32_t bits = 0;
  // This only works on little-endian architectures.
  memcpy(&bits, &f, sizeof(float));

#ifdef RYU_DEBUG
  printf("IN=");
  for (int32_t bit = 31; bit >= 0; --bit) {
    printf("%u", (bits >> bit) & 1);
  }
  printf("\n");
#endif

  // Decode bits into sign, mantissa, and exponent.
  const bool ieeeSign = ((bits >> (FLOAT_MANTISSA_BITS + FLOAT_EXPONENT_BITS)) & 1) != 0;
  const uint32_t ieeeMantissa = bits & ((1u << FLOAT_MANTISSA_BITS) - 1);
  const uint32_t ieeeExponent = (bits >> FLOAT_MANTISSA_BITS) & ((1u << FLOAT_EXPONENT_BITS) - 1);

  // Case distinction; exit early for the easy cases.
  if (ieeeExponent == ((1u << FLOAT_EXPONENT_BITS) - 1u) || (ieeeExponent == 0 && ieeeMantissa == 0)) {
    return copy_special_str(result, ieeeSign, ieeeExponent, ieeeMantissa);
  }

  const struct floating_decimal_32 v = f2d(ieeeMantissa, ieeeExponent);
  return to_chars(v, ieeeSign, result);
}

void f2s_buffered(float f, char* result) {
  const int index = f2s_buffered_n(f, result);

  // Terminate the string.
  result[index] = '\0';
}

char* f2s(float f) {
  char* const result = (char*) malloc(16);
  f2s_buffered(f, result);
  return result;
}
