# Unit Tests

Jsonifier ships a large test suite that runs on **every push and pull request**, on **every supported platform and compiler**, with **AddressSanitizer and UndefinedBehaviorSanitizer** turned on where the toolchain supports them. This page explains how the suite is put together and lists everything it tests.

- [Running Tests Locally](#running-tests-locally)
- [Continuous Integration](#continuous-integration)
- [How the Suite Is Organized](#how-the-suite-is-organized)
- [Test Fixtures](#test-fixtures)
- [End-to-End Parsing and Serialization](#end-to-end-parsing-and-serialization)
- [Conformance and Validation](#conformance-and-validation)
- [Number and String Primitives](#number-and-string-primitives)
- [Writers and Formatters](#writers-and-formatters)
- [Core Infrastructure](#core-infrastructure)
- [SIMD and Memory Primitives](#simd-and-memory-primitives)
- [Internal Containers](#internal-containers)
- [Sanitizer Guarantees](#sanitizer-guarantees)

---

## Running Tests Locally

```bash
git clone https://github.com/nihilai-collective/Jsonifier.git
cd Jsonifier

cmake -B build -DJSONIFIER_UNIT_TESTS=ON

cmake -B build -DJSONIFIER_UNIT_TESTS=ON -DJSONIFIER_ASAN=ON -DJSONIFIER_UBSAN=ON

cmake --build build --target jsonifier-unit-tests
./build/unit-tests/jsonifier-unit-tests
```

The test binary first prints the active CPU backend (`jsonifier::activeBackendName()`), then runs every suite in order. Any failure makes the process exit with code `1`.

---

## Continuous Integration

The suite is built on [rt-ut](https://github.com/nihilai-collective/rt-ut), which CMake fetches with `FetchContent`. GitHub Actions runs it on this matrix:

```yaml
name: Unit Tests
on:
  push:
    branches: [ "**" ]
  pull_request:
    branches: [ "**" ]
  workflow_dispatch:

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
            compiler: clang
            name: "Ubuntu Clang"
          - os: ubuntu-latest
            compiler: gcc
            name: "Ubuntu GCC"
          - os: macos-latest
            compiler: clang
            name: "macOS Clang"
          - os: macos-latest
            compiler: gcc
            name: "macOS GCC"
          - os: windows-latest
            compiler: msvc
            name: "Windows MSVC"
    runs-on: ${{ matrix.os }}
```

Sanitizers are turned off automatically for GCC on macOS, which does not support them. UBSan does nothing under MSVC because MSVC has no UBSan runtime, so on Windows you need a Clang-cl, Clang or GCC build for UBSan coverage.

A second workflow, `Fallback Unit Tests` (`.github/workflows/unit-tests-fallback.yml`), runs the same matrix with `-DJSONIFIER_CPU_INSTRUCTIONS=0`, so the pure-scalar fallback backend is exercised on every push as well.

The full suite passes on **Ubuntu (Clang/GCC), macOS (Clang/GCC), and Windows (MSVC)** on every push.

---

## How the Suite Is Organized

`unit-tests/main.cpp` calls `tests::testFunction()` in `unit-tests/tests.hpp`, which runs 34 suites in order. Each suite lives in its own header and exposes a `runTests()` entry point.

### The Eight-Configuration Matrix

Every parsing-family suite runs once for each combination of three parser options, so 8 times in total:

| Option | Effect |
|---|---|
| `partialRead` | Tolerates missing, extra, or reordered keys |
| `knownOrder` | Turns key-order prediction on or off |
| `nullTerminated` | Treats the input buffer as null terminated or not |

These suites use the matrix: parsing, bounds/truncation, conformance (`jsonchecker` and JSONTestSuite), round trip, float, signed integer, unsigned integer, and string.

---

## Test Fixtures

| Location | Contents |
|---|---|
| `unit-tests/json/` | 11 real-world corpora, each minified and prettified (Apache Builds, Canada, CitmCatalog, Discord, Github Events, Google Maps Response, Instruments, Marine IK, Random, Twitter, Twitter Partial), plus `Minify Test`, `Prettify Test`, and `Validate Test` |
| `unit-tests/jsonchecker/` | The classic `jsonchecker` corpus: 27 `pass*.json` and 79 `fail*` files |
| `unit-tests/JSONTestSuite/test_parsing/` | The JSONTestSuite corpus: 95 `y_*` (must accept), 188 `n_*` (must reject), and 35 `i_*` (implementation-defined, deliberately unused) |
| `unit-tests/round_trip/` | 27 `roundtripNN.json` documents and the `semanticscholar` schema |

The C++ structs for each corpus are defined in `apache_builds.hpp`, `canada.hpp`, `citm_catalog.hpp`, `discord.hpp`, `github_events.hpp`, `google_maps_response.hpp`, `instruments.hpp`, `marine_ik.hpp`, `random.hpp`, and `twitter.hpp`. Shared helpers (the pass/fail runners, the file loader, and the configuration labels) live in `common.hpp`.

---

## End-to-End Parsing and Serialization

### Parsing Tests (`parsing_tests.hpp`)
- Parses all 22 corpus files into their typed structs, serializes them back, and parses the output again. Runs in all eight configurations.
- Runs the minify, prettify, and validate checks against `Minify Test`, `Prettify Test`, and `Validate Test`.

### Bounds and Truncation (`bounds_test.hpp`)
- Cuts each of the same 22 corpus files at random lengths, using a seeded generator, and checks that the parser fails cleanly without reading past the end of the input.
- That is 176 truncation runs across the eight configurations. A failure prints its seed so the run can be replayed.

### Round Trip (`round_trip.hpp`)
- 27 documents go through serialize, parse, and compare. They cover primitives, null, empty values, large numbers, special floats, raw pointers, `unique_ptr`, and nested objects.
- Runs in all eight configurations.

### Feature Unit Tests (`unit_tests.hpp`)
About 75 named tests for the user-facing API:
- **Scalars:** bool, int, and double parse/write, large numbers, float precision, integers with a fraction that gets truncated.
- **Strings:** escaped characters, special characters, Unicode, a skipped string containing an escaped quote, empty `char` strings, escaped member keys.
- **Containers:** arrays and vectors, vectors of vectors, vectors of structs, `map`, `unordered_map`, nested maps, empty containers.
- **Enums:** enum parse and serialize, enums written as integers, enum arrays, enums as map keys.
- **Optionals and pointers:** `optional` with and without a value, optional fields, `shared_ptr`.
- **Structs:** basic reflection, nested structs, complex structs, renamed fields set through `jsonifier::core`, tuple round trips.
- **Partial reads (19 tests):** nested objects, maps, renamed fields, optional fields present and absent, Unicode, special characters, large payloads, buffer-length edge cases, vectors of structs, round trip, and partial reads with validation.
- **Validation:** valid and invalid documents, strict scalars, truncated literals.

### Generic Parser (`generic_parser.hpp`)
75 tests for the schema-free, On Demand-style parser:
- **Streaming and NDJSON:** documents separated by newlines, whitespace, or commas; rules for commas (not allowed, trailing, doubled); small batch sizes; truncated documents and strings; capacity overflow; empty input; stray closing brackets; partial reads.
- **Navigation:** root type detection, nested access, accessing the same value twice, iterating arrays, `count_elements` and `count_fields`, field keys returned in order, raw JSON containers.
- **Value extraction:** doubles, bools, nulls, escaped and plain strings into `std::string` and `jsonifier::string`, zero-copy non-ASCII strings, escaped string views that stay valid.
- **JSON Pointer:** nested paths, escaped keys, array indices, invalid pointers, index overflow.
- **Errors:** missing field, wrong type, index out of bounds, unclosed or mismatched containers, trailing content, missing comma or colon, trailing comma, bad literals, empty input.
- **Key-order prediction:** a miss on an unordered object keeps the parse position, large or prettified objects read in reverse order, long keys, the same keys in different objects, the field index resetting between documents.

### raw_json_data (`raw_json_data.hpp`)
51 tests for the dynamic `raw_json_data` type:
- `getType` for every JSON type and for a default-constructed value.
- Round trips for uint, int, and double, including negative, scientific, leading-zero, and negative-exponent values.
- Index and key access, const and non-const, including `operator[]` creating an object from null.
- `contains` and `size` for every type.
- Equality, deeply nested access, serialization (minified, prettified, root scalars, default-constructed as `null`), rejection of invalid values.

---

## Conformance and Validation

### jsonchecker (`conformance.hpp`)
- Runs all 27 `pass*.json` files and 77 of the 79 `fail*` files. Each failure case is checked against the exact `parse_statuses` error it should produce.
- Runs in all eight configurations.

### JSONTestSuite (`JSONTestSuite.hpp`)
- Runs all 95 `y_*` (must accept) and all 188 `n_*` (must reject) cases. They cover malformed arrays, numbers, objects, strings, and structure, plus invalid UTF-8.
- Runs in all eight configurations.

### UTF-8 Validation (`utf8_validation.hpp`)
About 200 tests that check both `jsonifier::validateUtf8` and validation inside string parsing:
- ASCII runs at every SIMD width, and valid 2-, 3-, and 4-byte sequences.
- The full Markus Kuhn UTF-8 stress-test corpus.
- The edges of the allowed second-byte range for each lead-byte class (`E0`, `ED`, `F0`, `F4`), which is where overlong encodings, surrogates, and values above U+10FFFF are caught.
- Invalid `0xFE`/`0xFF` bytes, bad continuation bytes, and lead bytes left unfinished at the end of a chunk, block, or step.
- Sequences that cross chunk and block boundaries.
- Sweeps: width transitions (accept and reject), unaligned pointers, unaligned invalid sequences, and an mmap check that nothing reads past a page boundary.

---

## Number and String Primitives

### Float Parsing and Serialization (`float.hpp`)
- **Parsing:** 64 edge cases, including signed zero, every exponent form, `DBL_MAX`, the smallest normal and denormal values, underflow to zero, overflow, the classic `2.2250738585072011e-308` case, round-half-to-even cases just above and below the halfway point near 2^56, 2^63, 2^103, and 2^152, and inputs with 50 or more digits. Runs in all eight configurations.
- **Serialization (zmij):** zero and negative zero, simple fractions, where the output switches between fixed and scientific notation, three-digit exponents, the largest, smallest, and denormal doubles, whole numbers written without a trailing point, and checks that values survive a round trip unchanged.

### Integer Parsing and Serialization (`integer.hpp`)
- **Signed:** 28 pass cases (the int64 limits, plus decimal and exponent forms that truncate to an integer) and 14 fail cases (overflow, a lone `-`, `1.2.3`, an exponent with no digits, non-numeric values, huge exponents).
- **Unsigned:** 18 pass cases up to `UINT64_MAX` and 13 fail cases.
- **`i_to_str`:** `toChars` and `jsonifier::to_string` must match `std::to_string` at every digit-count boundary for every integer type.

### String Parsing (`string.hpp`)
- **35 pass cases:** empty strings, every simple escape, `\uXXXX` in the 1-, 2-, and 3-byte ranges, surrogate pairs, emoji, ZWJ sequences, CJK, Cyrillic, and Arabic text, embedded NULs, long runs of backslashes, and long plain strings.
- **30 fail cases:** short or invalid `\u` escapes, illegal escapes (`\x`, `\a`, `\v`, octal), lone or mismatched surrogates, unterminated strings, and high bytes inside `\u` digits.
- Runs in all eight configurations.

---

## Writers and Formatters

- **Minifier (`minifier.hpp`):** strips whitespace while keeping string contents, escapes, and number text exactly as written. Handles bare top-level scalars and deep nesting. Reuses buffers correctly whether they need to grow, shrink, or be overwritten. Does not overrun an exactly sized buffer. Reports errors on empty, whitespace-only, or unclosed input, and clears them after a later successful call.
- **Prettifier (`prettifier.hpp`):** the same buffer and error checks, plus custom indent size and character, and exact output at nesting depths of 10, 50, and 650.
- **Printer (`printer.hpp`):** `printJson` debug output for bools, chars, enums, strings, raw and shared pointers, optionals, variants, tuples, maps, vectors, nested objects, skipped types, and the header line.
- **fastio (`fastio.hpp`):** the stdout and stderr singletons, write ordering through a buffered stream, oversized writes that skip the buffer, integer and float output matching `std::to_string`/`std::to_chars`, and digit counting.

---

## Core Infrastructure

- **`jsonifier_core` (`jsonifier_core.hpp`):** records errors, and stays usable with its errors intact after move construction, move assignment, and self-move-assignment.
- **Errors (`error.hpp`):** error offsets set by `constructError`, line numbers and escaped control characters in `reportError`, equality, and the stream operator.
- **Reflection (`reflection.hpp`):** member names read at compile time, covering short, long, and underscored names, numeric suffixes (so `member10` is not cut to `member1`), structs in nested namespaces, and no mixing up of member names.
- **Enum names (`get_enum_name.hpp`):** enum-to-string conversion, sparse enums with gaps, out-of-range values reported as unknown, checks that a name is a valid identifier, and the stream operator.
- **Hash (`hash.hpp`):** matches reference outputs for several seeds, output changes when the seed changes, zero-length input, and `updateSeed` cycling through and wrapping around the seed table.
- **Hash maps (`hash_map.hpp`):** compile-time key lookup for 1-, 2-, and 5-field structs, first-byte buckets, wide binary keys, unknown and almost-matching keys that leave real fields untouched, and empty structs.

---

## SIMD and Memory Primitives

- **Intrinsics (`intrinsics.hpp`):** tests the SIMD layer directly, without going through the parser: `opCmpEq`, bitmask sweeps, and/or/xor, saturating subtraction, shifts, `alignr`, `isAscii`, and aligned and unaligned store round trips.
- **Tape emission (`add_tape_values.hpp`):** turning stage-1 bitmasks into tape indices, from an empty mask through a single bit, every bit set, crossing a tape step, and spanning several blocks with order preserved.
- **Compare (`compare.hpp`):** `compare` and `memchar` at every length around the 16-, 32-, and 64-byte SSE, AVX2, and AVX-512 widths, plus lengths that need several vectors, and zero length.
- **String-literal comparator (`string_literal_comparator.hpp`):** compile-time literal matching at lengths from 0 to 200, at every point where the comparison splits into chunks, with binary content, near a page boundary (lengths 15 to 64), in an exhaustive mode, and through the driver's accept and reject paths.
- **memcpy (`memcpy.hpp`):** `jsonifier::memcpy` and `memcpyUpTo` with zero length and null pointers, every byte value, every length from 0 to 256 at every offset, lengths from 257 to 1100, powers of two up to 1 MB, large odd sizes, guard bytes to catch overruns, back-to-back appends into one buffer, and results identical to `std::memcpy`.
- **Allocator (`allocator.hpp`):** SIMD-aligned allocations, zero-size and null handling, rejection right at the `max_size` limit, rebinding and equality, over-aligned types, and `roundUp`/`roundDown` for power-of-two and other sizes, including negative values and zero.

---

## Internal Containers

- **`jsonifier::string` (`string_class.hpp`):** 80 tests: construction from every source, copy and move, append and growth, insert and erase, every `find` variant, `at` throwing on out-of-range indices, a null terminator after every change, conversion to `std::string`/`std::string_view`, and iteration.
- **`jsonifier::internal::array` (`array.hpp`):** element access, `at` throwing, `fill` and `swap`, equality, the zero-size specialization, and compile-time tag subscripts.
- **Tuple (`tuple.hpp`):** `get` by index and by type, `tuple_cat`, equality, relational, and three-way comparison, structured bindings, and the empty tuple.
- **Iterator (`iterator.hpp`):** everything a contiguous iterator must support (arithmetic, comparisons, subscripting, use with `std::sort` and `std::distance`), the zero-size variant, and `string_view` iteration.

---

## Sanitizer Guarantees

With ASan and UBSan enabled, the suite checks for:
- **Memory safety:** no leaks, double frees, or use-after-free.
- **Undefined behavior:** no signed overflow, null-pointer dereferences, or invalid casts.
- **Bounds:** truncated or malformed input from any real-world corpus fails cleanly.
- **Unicode:** full UTF-8 support, including ZWJ sequences and surrogate pairs.
- **Numeric edge cases:** infinity, NaN, denormal numbers, and the limits where integers overflow.
