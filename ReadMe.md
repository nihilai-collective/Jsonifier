<p align="center"><img src="./logo.png?raw=true" width="350"/></p>

![Commit Activity](https://img.shields.io/github/commit-activity/y/nihilai-collective/jsonifier?color=999EE0&label=Commits&style=plastic)
![License](https://img.shields.io/badge/License-MIT-blue.svg?style=plastic)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=plastic)

**Jsonifier is fully [RFC8259](https://datatracker.ietf.org/doc/html/rfc8259) compliant.**

A high-performance C++ library for validating, serializing, parsing, prettifying, and minifying JSON data — **very rapidly**.

It achieves this through the usage of [SIMD instructions](./include/jsonifier-incl/simd) as well as compile-time [hash maps](/include/jsonifier-incl/utilities/hash_map.hpp) for efficient key lookups during parsing.

- [Stage-1 Document](./Batched-Drain.md)
- [Full Arch Document](./Two-Stages.md)
- [Benchmarks](https://github.com/nihilai-collective/Json-Performance)
- [More Benchmarks](https://github.com/Loki-Astari/JsonBenchmark)

---

## Compiler Support

| Compiler | Status |
|----------|--------|
| ![MSVC](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=microsoft&logoColor=green&label=MSVC&labelColor=pewter&color=blue&branch=main) | Visual Studio (Latest) |
| ![GCC](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=linux&logoColor=green&label=GCC&labelColor=pewter&color=blue&branch=main) | GCC (Latest) |
| ![CLANG](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=apple&logoColor=green&label=CLANG&labelColor=pewter&color=blue&branch=main) | Clang (Latest) |

## Operating System Support

| OS | Status |
|----|--------|
| ![Windows](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=microsoft&logoColor=green&label=Windows&labelColor=pewter&color=blue&branch=main) | Windows (Latest) |
| ![Linux](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=linux&logoColor=green&label=Linux&labelColor=pewter&color=blue&branch=main) | Ubuntu (Latest) |
| ![Mac](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=apple&logoColor=green&label=MacOS&labelColor=pewter&color=blue&branch=main) | macOS (Latest) |

## CPU Architecture Support

Jsonifier automatically detects and optimizes for your CPU architecture:

- **x64 / AMD64** — 64-bit extension of x86 with enhanced memory addressing
- **AVX** — 128-bit vector registers for SIMD operations
- **AVX2** — 256-bit vector registers with additional integer operations
- **AVX-512** — 512-bit vector registers for maximum parallelism (requires F + BW + VBMI2 support detected together)
- **PCLMULQDQ** — carry-less multiplication support, detected and used where available
- **ARM-NEON** — SIMD instructions for ARM processors
- **ARM-SVE2** — scalable vector extensions for ARM processors ⚠️ **experimental** — see note below

Manual configuration is also available via `JSONIFIER_CPU_FLAGS` in CMake, and cross-compilation is supported by pre-defining `JSONIFIER_CPU_INSTRUCTIONS` to skip native feature detection.

> **A note on SVE2:** the SVE2 backend is new and still under active development. A handful of parsing cases are not yet handled correctly under SVE2. If you're building for ARM, **NEON remains the recommended path for production use** until SVE2 correctness is fully verified. Feedback and bug reports on SVE2-specific behavior are very welcome.

---

## Features

### Compile-Time Reflection
Structures are registered via template specialization using member pointers as non-type template parameters. No macros, no code generation, no runtime type registry — the entire schema is known to the compiler, which means key lookups collapse into compile-time hash-map dispatch and dead branches get eliminated before they exist. Custom JSON key names (kebab-case, digit-prefixed, reserved words) are handled at compile time via `makeJsonEntity<&member, "custom-name">()` with zero runtime overhead.

### SIMD-Accelerated Stage-1
Structural indexing runs through a batched-drain, fused-scan architecture: character classification, quote-scope tracking, and UTF-8 validation happen in the same pass over the input, with cross-SIMD-width validation state carried between blocks via a dedicated register-scoped validator. The structural index itself is stored as a compact array of offsets into the source buffer rather than raw pointers, keeping the index footprint small and cache-friendly. Everything the compiler can know statically about the target architecture — cache-line size, vector width, alignment — is baked into the binary as constexpr.

### UTF-8 Validation, By Default
Full UTF-8 validation runs as part of every parse by default (`parse_options::validateUtf8 = true`), built on a Lemire/simdjson-derived table-lookup validator with both a bulk-buffer path and a streaming, register-scoped path that carries state across chunk boundaries. Validation is fused directly into string scanning rather than requiring a separate pass. Correctness is backed by the full Markus Kuhn UTF-8 stress-test corpus, alignment-sweep fuzzing, and page-boundary overrun checks.

### Purpose-Built Hash Maps
Key lookups during parsing use compile-time-generated hash maps specialized for object size (1, 2, 3+ fields), each picking a different strategy based on what's fastest for that cardinality. No runtime hashing, no bucket walks — the lookup is generated for the specific set of keys your struct declares.

### Complete JSON Support
Full RFC8259 compliance. All types (objects, arrays, strings, numbers, booleans, null), full Unicode with proper surrogate-pair handling, all escape sequences, and `jsonifier::raw_json_data` for preserving arbitrary sub-trees verbatim.

### Flexible Parsing Modes
- **Ordered parsing** (default) — fastest, assumes JSON keys arrive in declaration order
- **Known-order parsing** — declaration order enforced, with a fused literal-match fast path for minified input
- **Partial reading** — parse unordered or partial JSON structures
- **Arbitrary data** — work with unknown JSON via `raw_json_data`

Every parsing-family test in the suite is run across all eight combinations of `partialRead`, `knownOrder`, and `nullTerminated`.

### Safety & Reliability
Comprehensive error reporting with source location tracking, backed by a compact, purpose-built status-code model — parsing, validation, minification, and prettification each propagate failure immediately on the first error rather than continuing to walk a broken buffer. Configurable maximum nesting depth (`parse_options::maxDepth`, default 1024) guards against malicious or runaway input.

Continuous integration runs AddressSanitizer and UndefinedBehaviorSanitizer on every push across every supported platform and compiler. The conformance test suite covers the full RFC8259 pass/fail battery — both the classic `jsonchecker` corpus and the full JSONTestSuite Y/N corpus — plus dedicated UTF-8 correctness tests including the full Markus Kuhn stress-test set, chunk/block-boundary crossings, unaligned-pointer sweeps, and an mmap page-boundary fault check.

---

## CI/CD with unit-tests

Jsonifier uses GitHub Actions to continuously test across multiple platforms and compilers with sanitizers enabled. The test suite is built on [rt-ut](https://github.com/realtimechris/rt-ut), fetched directly via CMake `FetchContent`, and runs on every push and pull request:

```yaml
name: unit-tests
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

This ensures memory safety and undefined behavior detection across all supported platforms. Note that sanitizers are automatically disabled for the GCC-on-macOS combination (unsupported), and UBSan has no effect under MSVC (no equivalent runtime) — Clang-cl or a Clang/GCC build is needed for UBSan coverage there.

The full suite passes on **Ubuntu (Clang/GCC), macOS (Clang/GCC), and Windows (MSVC)** on every push.

---

## Comprehensive Test Suite

Jsonifier includes an extensive test suite that runs on **every push** across **all supported platforms and compilers** with **ASAN and UBSAN** enabled where supported.

### Test Categories

| Test Category | Description |
|---------------|-------------|
| **Conformance Tests** | Full RFC8259 compliance testing against two corpora: the classic `jsonchecker` set (77 `fail*.json` + 27 `pass*.json`, each checked against a specific expected `parse_statuses` error) and the full JSONTestSuite Y/N corpus (189 `n_*.json` + 90 `y_*.json`). 383 documents total, each run across all eight `partialRead` / `knownOrder` / `nullTerminated` combinations |
| **Round-Trip Tests** | 27 serialize → parse → compare cases covering primitives, raw pointers, `unique_ptr`, and nested objects, ensuring data integrity across all types |
| **Float Validation** | 64 edge cases including denormals, subnormal boundaries, round-half-to-even cases, infinities, and extreme exponents |
| **Integer Validation** | Bounds testing for signed (24 pass / 11 fail) and unsigned (16 pass / 11 fail) integers, from zero through the full int64/uint64 range |
| **String Validation** | 35 pass cases (Unicode, escape sequences, control characters, multi-byte emoji, ZWJ sequences, surrogate pairs) and 26 fail cases (malformed escapes, invalid `\u` sequences, unterminated strings) |
| **UTF-8 Validation** | The full Markus Kuhn UTF-8 stress-test corpus, standalone 1–4 byte sequence and chunk/block-boundary tests, second-byte boundary tests per lead-byte class, fused string-parser validation (escape-aware, surrogate-pair-aware), an unaligned-pointer sweep, an unaligned invalid-sequence sweep, an mmap page-boundary fault check, and a width-transition sweep across body lengths 24–224 |
| **Bounds/Truncation** | Progressive truncation of full JSON payloads (Twitter, Discord, Canada, CitmCatalog, Apache Builds, GitHub Events, Google Maps, Instruments, Marine IK, Mesh, Random, Abc in/out-of-order) — 17 corpus files, minified and prettified, to validate graceful failure on malformed/truncated input |
| **Parsing Tests** | Parse/serialize/minify/prettify/validate correctness across the full real-world payload suite, both minified and prettified, across all eight `partialRead` / `knownOrder` / `nullTerminated` combinations |
| **Intrinsics Tests** | Direct correctness testing of the SIMD abstraction layer — comparison, bitmask, logical, saturating-subtract, shift, cross-register alignment, and load/store round-trips at every unaligned offset |
| **Error Tests** | Construction, equality, line-number reporting, and control-character escaping of the error-reporting model |
| **Type Coverage** | Primitives, containers (`vector`, `array`, `map`, `unordered_map`), tuples, `optional`, `shared_ptr`, enums, nested structs, renamed/escaped keys — 70+ dedicated unit tests |

### What Gets Tested

- **70+ dedicated unit tests** covering reflection, renamed fields, optionals, enums, `shared_ptr`, nested structs, containers, tuples, maps, and escaped keys
- **266 conformance fail cases + 117 pass cases** (77+27 from `jsonchecker`, 189+90 from JSONTestSuite), each asserting the exact expected `parse_statuses` value — 3,064 conformance assertions per platform across all eight configs
- **27 round-trip tests** including edge cases (null, empty, large numbers, raw pointers, `unique_ptr`, special floats)
- **64 float edge cases** and **24+16 int/uint pass cases** with **11+11 matching fail cases**
- **35 string pass cases + 26 fail cases**, including full Unicode/emoji/ZWJ/escape coverage
- **A full UTF-8 correctness gauntlet** — basic sequence tests, the complete Markus Kuhn stress corpus, second-byte boundary tests, fused string-parser tests, unaligned-pointer and unaligned-invalid-sequence sweeps, an mmap page-boundary fault check, and a width-transition sweep
- **Direct SIMD intrinsics correctness tests**, independent of parsing, covering every primitive operation across all supported backends
- **Memory safety** — No leaks, double-frees, or use-after-free (ASAN)
- **Undefined behavior** — No signed overflow, null pointer dereference, or invalid casts (UBSAN)
- **Bounds checking** — Proper, graceful handling of truncated/malformed input across the full real-world payload suite
- **Unicode and emoji** — Full UTF-8 support, including ZWJ sequences and surrogate pairs
- **Edge cases** — Infinity, NaN, denormal numbers, integer overflow boundaries

Every parsing-family category above runs across all **eight** combinations of `partialRead`, `knownOrder`, and `nullTerminated`, and the full suite passes on **Ubuntu (GCC/Clang), macOS (GCC/Clang), and Windows (MSVC)**.

### Running Tests Locally

```bash
git clone https://github.com/nihilai-collective/Jsonifier.git
cd Jsonifier

cmake -B build -DJSONIFIER_UNIT_TESTS=ON

cmake -B build -DJSONIFIER_UNIT_TESTS=ON -DJSONIFIER_ASAN=ON -DJSONIFIER_UBSAN=ON

cmake --build build --target jsonifier-unit-tests
./build/Tests/jsonifier-unit-tests
```

---

## Quick Example

```cpp
#include <jsonifier>

struct event {
    int64_t id{};
    std::string name{};
    std::optional<std::string> logo{};
    std::vector<int64_t> topicIds{};
};

struct catalog {
    std::unordered_map<std::string, event> events{};
    std::string schema_version{};
};

template<> struct jsonifier::core<event> {
    using value_type = event;
    static constexpr auto parseValue = createValue
        &value_type::id,
        &value_type::name,
        &value_type::logo,
        &value_type::topicIds>();
};

template<> struct jsonifier::core<catalog> {
    using value_type = catalog;
    static constexpr auto parseValue = createValue
        &value_type::events,
        makeJsonEntity<&value_type::schema_version, "schema-version">()>();
};

int main() {
    jsonifier::jsonifier_core<> parser;

    catalog data;
    std::string json = R"({"events":{"42":{"id":42,"name":"Concert","logo":null,"topicIds":[1,2,3]}},"schema-version":"1.0"})";
    parser.parseJson(data, json);

    std::string output;
    parser.serializeJson(data, output);

    return 0;
}
```

Note the `makeJsonEntity<&value_type::schema_version, "schema-version">()` — Jsonifier maps the C++-legal `schema_version` member to the kebab-case `"schema-version"` key in JSON entirely at compile time, with zero runtime cost.

The `jsonifier_core<>` type is now templated on an initial scratch-buffer size in bytes (default 1MB) — e.g. `jsonifier::jsonifier_core<4 * 1024 * 1024> parser;` for workloads that consistently deal with larger documents.

---

## Documentation

### Getting Started
- [Installation](documentation/Installation.md) — Install via vcpkg, CMake FetchContent, or source
- [Quick Start](documentation/QuickStart.md) — Five-minute onramp with a working example

### Core Usage
- [Reflection](documentation/Reflection.md) — Registering structures with compile-time reflection
- [Serializing & Parsing](documentation/Usage_Serializing_Parsing.md) — Complete serialization/parsing guide
- [Validating](documentation/Validating.md) — JSON validation against RFC standards

### Optimization
- [Known Order Parsing](documentation/Known_Order.md) — Guaranteed-order optimization for hot paths
- [Optimizing For Minified JSON](documentation/Optimizing_For_Minified_Json.md) — Boost performance for minified input
- [Partial Reading](documentation/PartialReading.md) — Parse unordered or partial JSON structures
- [CPU Architecture Selection](documentation/CPU_Architecture_Selection.md) — Manual CPU instruction set configuration, including SVE2 and cross-compilation overrides

### Output Formatting
- [Prettifying](documentation/Prettifying.md) — Pretty-print JSON with customizable indentation
- [Minifying](documentation/Minifying.md) — Minify JSON for compact output

### Advanced Topics
- [UTF-8 Validation](documentation/UTF8_Validation.md) — Cross-SIMD-width validation architecture
- [Error Handling](documentation/Errors.md) — Detailed error reporting and handling
- [Excluding Keys at Runtime](documentation/Excluding_Keys.md) — Dynamic key exclusion during serialization
- [Parsing Arbitrary Data](documentation/Parsing_Arbitrary_Data.md) — Working with unknown JSON structures

---

## Requirements

- CMake 3.28 or later
- C++20 compliant compiler (MSVC 2022+, GCC 11+, Clang 14+)
- Supported CPU (x64, ARM64 with NEON or SVE2*)

<sub>* SVE2 support is experimental — see the CPU Architecture Support note above.</sub>

---

## License

This library is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

---

## Acknowledgments

- SIMD parsing techniques inspired by [simdjson](https://github.com/simdjson/simdjson)
- Reflection interface inspired by [Glaze](https://github.com/stephenberry/glaze)
- Dragonbox algorithm for float conversion
- FastFloat for number parsing
- Unit test harness: [rt-ut](https://github.com/realtimechris/rt-ut)
- Raymond, because, Thanks.

---

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues on GitHub.

---

**Star this repository if you find it useful!** ⭐