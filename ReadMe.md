<p align="center"><img src="./Logo.png?raw=true" width="450"/></p>

![Lines of Code](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/nihilai-collective/jsonifier/badges/loc.json&style=plastic)
[![License](https://img.shields.io/badge/license-MIT-blue.svg?style=plastic)](License.md)
[![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg?style=plastic)](https://isocpp.org/)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg?style=plastic)](include/)

---
## Compiler Support
![MSVC](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=microsoft&logoColor=green&label=MSVC&labelColor=pewter&color=blue&branch=main)
![GCC](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=linux&logoColor=green&label=GCC&labelColor=pewter&color=blue&branch=main)
![CLANG](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=apple&logoColor=green&label=CLANG&labelColor=pewter&color=blue&branch=main)

## Operating System Support
![Windows](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=microsoft&logoColor=green&label=Windows&labelColor=pewter&color=blue&branch=main)
![Linux](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=linux&logoColor=green&label=Linux&labelColor=pewter&color=blue&branch=main)
![Mac](https://img.shields.io/github/actions/workflow/status/nihilai-collective/jsonifier/unit-tests.yml?style=plastic&logo=apple&logoColor=green&label=MacOS&labelColor=pewter&color=blue&branch=main)

---

Parsing at least slightly more gigabytes of JSON per second, most of the time.
===============================================

Jsonifier uses SIMD structural indexing and compile-time perfect hash maps to read and write JSON **1.8x to 2.3x faster than Glaze** and **1.6x to 3.1x faster than simdjson**, across every major compiler and both x86-64 and ARM64. ([Oct 21, 2026 results](https://github.com/nihilai-collective/Json-Performance))

* **Schema-aware (reflected structs):** 913 wins, 12 ties, and 40 losses in 965 head-to-head tests against Glaze and simdjson on Linux, macOS, and Windows.
* **Faster reads:** geometric-mean parse throughput is 1.33x to 1.48x that of Glaze and 1.42x to 2.37x that of simdjson On Demand.
* **Faster writes:** geometric-mean serialization throughput is 2.25x to 3.72x that of Glaze and up to 6.07x that of simdjson's C++26 reflection writer.
* **Generic (schemaless) parsing:** 1366 wins, 74 ties, and 180 losses against simdjson On Demand, running 1.20x to 1.30x faster on GCC and Clang and 2.17x faster on MSVC.
* **Portable:** CI-verified on GCC, Clang, and MSVC, with AVX2 and NEON backends.
* **Strict:** fully [RFC8259](https://datatracker.ietf.org/doc/html/rfc8259) compliant.

| Platform | vs Glaze (schema-aware) | vs simdjson (schema-aware) | vs simdjson On Demand (generic) |
| :--- | :---: | :---: | :---: |
| Linux GCC 16.2.1 | 1.79x | 2.72x | 1.22x |
| Linux Clang 24.0.0 | 1.79x | 1.59x | 1.22x |
| macOS GCC 16.2.0 | 2.20x | 3.13x | 1.30x |
| macOS Clang 23.1.3 | 2.27x | 1.78x | 1.20x |
| Windows MSVC 19.51 / 19.44 | 1.83x | 2.47x | 2.17x |

<sub>Geometric mean of per-test throughput ratios: Jsonifier's best variant against each competitor's best variant. Glaze has no schemaless entry in the generic suite.</sub>

**Jsonifier is fully [RFC8259](https://datatracker.ietf.org/doc/html/rfc8259) compliant.**

A high-performance C++ library for validating, serializing, parsing, prettifying, and minifying JSON data, **very rapidly**.

It achieves this through the usage of [SIMD instructions](./include/jsonifier-incl/simd) as well as compile-time [hash maps](./include/jsonifier-incl/utilities/hash_map.hpp) for efficient key lookups during parsing.

- [Stage-1 Document](./Batched-Drain.md)
- [Full Arch Document](./Two-Stages.md)
- [Benchmarks](https://github.com/nihilai-collective/Json-Performance)
- [More Benchmarks](https://github.com/Loki-Astari/JsonBenchmark)

---

> **Android:** `JSONIFIER_PLATFORM_ANDROID` is detected and handled distinctly from `JSONIFIER_PLATFORM_LINUX` (separate `mmap`/`unistd.h` wiring, its own entry in the supported-platform check), for NDK cross-compilation. It is not part of the CI matrix above, so treat it as best-effort rather than continuously verified.

## CPU Architecture Support

Jsonifier automatically detects and optimizes for your CPU architecture:

- **x64 / AMD64**: 64-bit extension of x86 with enhanced memory addressing
- **AVX**: 128-bit vector registers for SIMD operations
- **AVX2**: 256-bit vector registers with additional integer operations
- **AVX-512**: 512-bit vector registers for maximum parallelism (requires F + BW + VBMI2 support detected together)
- **PCLMULQDQ**: carry-less multiplication support, detected and used where available
- **ARM-NEON**: SIMD instructions for ARM processors
- **ARM-SVE2**: scalable vector extensions for ARM processors ⚠️ **experimental**: the SVE2 backend is new and still under active development; a handful of parsing cases are not yet handled correctly. NEON remains the recommended path for production ARM builds until SVE2 correctness is fully verified. Feedback and bug reports on SVE2-specific behavior are very welcome.

Exactly one SIMD backend is compiled into each build, chosen at configure time; nothing is selected at run time. A binary configured for AVX-512 requires AVX-512 on every machine that runs it, so configure for the lowest tier you need to support when distributing to mixed hardware. Pass `-DJSONIFIER_CPU_INSTRUCTIONS` at configure time to build a lower tier than the one detected. See [One tier per build](documentation/CPU_Architecture_Selection.md#one-tier-per-build).

Manual configuration and cross-compilation are supported by pre-defining `JSONIFIER_CPU_INSTRUCTIONS` (plus `JSONIFIER_SVE2_VECTOR_BITS` for SVE2 targets) at CMake configure time to skip native feature detection.

---

## Features

### Compile-Time Reflection
Structures are registered via template specialization using member pointers as non-type template parameters. No macros, no code generation, no runtime type registry; the entire schema is known to the compiler, which means key lookups collapse into compile-time hash-map dispatch and dead branches get eliminated before they exist. Custom JSON key names (kebab-case, digit-prefixed, reserved words) are handled at compile time via `makeJsonEntity<&member, "custom-name">()` with zero runtime overhead.

### SIMD-Accelerated Stage-1
Structural indexing runs through a batched-drain, fused-scan architecture: character classification, quote-scope tracking, and UTF-8 validation happen in the same pass over the input, with cross-SIMD-width validation state carried between blocks via a dedicated register-scoped validator. The structural index itself is stored as a compact array of offsets into the source buffer rather than raw pointers, keeping the index footprint small and cache-friendly. Everything the compiler can know statically about the target architecture (cache-line size, vector width, alignment) is baked into the binary as constexpr.

### UTF-8 Validation, By Default
Full UTF-8 validation runs as part of every parse, always, built on a Lemire/simdjson-derived table-lookup validator with both a bulk-buffer path and a streaming, register-scoped path that carries state across chunk boundaries. Validation is fused directly into string scanning rather than requiring a separate pass. Correctness is backed by the full Markus Kuhn UTF-8 stress-test corpus, alignment-sweep fuzzing, and page-boundary overrun checks.

### Purpose-Built Hash Maps
Key lookups during parsing use compile-time-generated hash maps specialized for object size (1, 2, 3+ fields), each picking a different strategy based on what's fastest for that cardinality. No runtime hashing, no bucket walks; the lookup is generated for the specific set of keys your struct declares.

### Complete JSON Support
Full RFC8259 compliance. All types (objects, arrays, strings, numbers, booleans, null), full Unicode with proper surrogate-pair handling, all escape sequences, and `jsonifier::raw_json_data` for preserving arbitrary sub-trees verbatim.

### Flexible Parsing Modes
- **Default parsing**: single pass over the raw buffer, compile-time hash-map key dispatch, keys accepted in any order
- **Known-order parsing**: tries the declaration-order key first, then a self-tuning per-position memo, then the hash map; adds a fused literal-match fast path for minified input
- **Partial reading**: parse unordered or partial JSON structures
- **Arbitrary data**: work with unknown JSON via `raw_json_data`
- **Generic (schema-free) parsing**: lazy, On Demand-style `jsonifier_core::iterate` with re-readable values, out-of-order field access, JSON Pointers and `iterateMany` for NDJSON streams

Every parsing-family test in the suite is run across all eight combinations of `partialRead`, `knownOrder`, and `nullTerminated`.

### Safety & Reliability
Comprehensive error reporting with source location tracking, backed by a compact, purpose-built status-code model: parsing, validation, minification, and prettification each propagate failure immediately on the first error rather than continuing to walk a broken buffer. Configurable maximum nesting depth (`parse_options::maxDepth`, default 1024) guards against malicious or runaway input.

Continuous integration runs AddressSanitizer and UndefinedBehaviorSanitizer on every push across every supported platform and compiler. The conformance test suite covers the full RFC8259 pass/fail battery (both the classic `jsonchecker` corpus and the full JSONTestSuite Y/N corpus), plus dedicated UTF-8 correctness tests including the full Markus Kuhn stress-test set, chunk/block-boundary crossings, unaligned-pointer sweeps, and an mmap page-boundary fault check.

---

## Testing

Every push runs the full unit-test suite on Ubuntu (Clang/GCC), macOS (Clang/GCC), and Windows (MSVC), with ASan and UBSan enabled where supported. See [Unit Tests](documentation/Unit_Tests.md) for the CI setup, how to run the tests locally, and a breakdown of every test suite.

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
    static constexpr auto parseValue = createValue<
        &value_type::id,
        &value_type::name,
        &value_type::logo,
        &value_type::topicIds>();
};

template<> struct jsonifier::core<catalog> {
    using value_type = catalog;
    static constexpr auto parseValue = createValue<
        &value_type::events,
        makeJsonEntity<&value_type::schema_version, "schema-version">()>();
};

int32_t main() {
    jsonifier::jsonifier_core<> parser;

    catalog data;
    std::string json = R"({"events":{"42":{"id":42,"name":"Concert","logo":null,"topicIds":[1,2,3]}},"schema-version":"1.0"})";
    parser.parseJson(data, json);

    std::string output;
    parser.serializeJson(data, output);

    return 0;
}
```

Note the `makeJsonEntity<&value_type::schema_version, "schema-version">()`: Jsonifier maps the C++-legal `schema_version` member to the kebab-case `"schema-version"` key in JSON entirely at compile time, with zero runtime cost.

Warning: Include only <jsonifier>. Direct inclusion of internal headers may cause unrelated code in the including translation unit to become uncompilable.

The `jsonifier_core<>` type is templated on an initial scratch-buffer size in bytes (default 1MB), e.g. `jsonifier::jsonifier_core<4 * 1024 * 1024> parser;` for workloads that consistently deal with larger documents. It is compiled for the single SIMD tier configured at build time, so a binary configured for AVX-512 requires AVX-512 on every machine that runs it. See [Backends and the Core](documentation/CPU_Architecture_Selection.md#backends-and-the-core).

---

## Documentation

### Getting Started
- [Installation](documentation/Installation.md): Install via vcpkg, CMake FetchContent, or source
- [Quick Start](documentation/QuickStart.md): Five-minute onramp with a working example

### Core Usage
- [Reflection](documentation/Reflection.md): Registering structures with compile-time reflection
- [Serializing & Parsing](documentation/Usage_Serializing_Parsing.md): Complete serialization/parsing guide
- [Validating](documentation/Validating.md): JSON validation against RFC standards

### Optimization
- [Known Order Parsing](documentation/Known_Order.md): Guaranteed-order optimization for hot paths
- [Optimizing For Minified JSON](documentation/Optimizing_For_Minified_Json.md): Boost performance for minified input
- [Partial Reading](documentation/PartialReading.md): Parse unordered or partial JSON structures
- [CPU Architecture Selection](documentation/CPU_Architecture_Selection.md): Configure-time backend selection, stage-1 step tuning, and manual CPU instruction set configuration, including SVE2 and cross-compilation overrides

### Output Formatting
- [Prettifying](documentation/Prettifying.md): Pretty-print JSON with customizable indentation
- [Minifying](documentation/Minifying.md): Minify JSON for compact output

### Advanced Topics
- [UTF-8 Validation](documentation/UTF8_Validation.md): Cross-SIMD-width validation architecture
- [Error Handling](documentation/Errors.md): Detailed error reporting and handling
- [Excluding Keys at Runtime](documentation/Excluding_Keys.md): Dynamic key exclusion during serialization
- [Parsing Arbitrary Data](documentation/Parsing_Arbitrary_Data.md): Working with unknown JSON structures
- [Generic Parsing](documentation/Generic_Parsing.md): Schema-free, On Demand-style parsing and NDJSON streams
- [Unit Tests](documentation/Unit_Tests.md): CI matrix, running tests locally, and everything the suite covers

---

## Requirements

- CMake 3.28 or later
- C++23 compliant compiler (MSVC 2022 v19.40+, GCC 14+, Clang 18+)
- Supported CPU (x64, ARM64 with NEON or SVE2*)

<sub>* SVE2 support is experimental; see the CPU Architecture Support section above.</sub>

---

## License

This library is licensed under the MIT License. See the [LICENSE](License.md) file for details.

---

## Acknowledgments

- SIMD parsing techniques inspired by [simdjson](https://github.com/simdjson/simdjson)
- Reflection interface inspired by [Glaze](https://github.com/stephenberry/glaze)
- Dragonbox algorithm for float conversion
- FastFloat for number parsing
- Unit test harness: [rt-ut](https://github.com/nihilai-collective/rt-ut)
- Raymond, because, Thanks.
- Paul Mandarino. "Back yourself up, back your words up."

---

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues on GitHub.

---

**Star this repository if you find it useful!** ⭐
