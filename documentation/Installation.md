# Installation

Jsonifier is a header-only C++20 library. You can install it via vcpkg, integrate it directly through CMake FetchContent, or drop the headers into your project by hand. Pick whichever fits your build setup.

## Requirements

- C++20 compliant compiler (MSVC 2022+, GCC 11+, Clang 14+)
- CMake 3.28 or later
- A supported CPU: x64 (with AVX/AVX2/AVX-512 optional) or ARM64 with NEON

Nothing else. No runtime dependencies, no linking against a shared library, no code generation step from your side.

## vcpkg

The fastest path. Jsonifier is published on the Microsoft vcpkg registry as `jsonifier`:

```
vcpkg install jsonifier
```

Then wire it up in your `CMakeLists.txt`:

```cmake
find_package(Jsonifier CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE Jsonifier::Jsonifier)
```

The vcpkg port is verified to build on Windows x64 (non-Xbox), Linux x64, and macOS x64. Windows ARM, Xbox, and 32-bit targets are not currently supported.

## CMake FetchContent

If you want to track a specific commit or branch, or avoid the vcpkg workflow, pull it in directly:

```cmake
include(FetchContent)

FetchContent_Declare(
  Jsonifier
  GIT_REPOSITORY https://github.com/nihilai-collective/Jsonifier.git
  GIT_TAG main
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Jsonifier)

target_link_libraries(your_target PRIVATE Jsonifier::Jsonifier)
```

Pin `GIT_TAG` to a release tag (e.g. `v1.0.0`) for reproducible builds. `main` gets you the latest but can move.

Note: Jsonifier's root `CMakeLists.txt` fetches its version string from the GitHub Releases API at configure time. If your build machine doesn't have network access at configure time, the version falls back to `0.0.0` — this is cosmetic only and doesn't affect functionality.

## From Source

Clone the repo and either install headers system-wide or point your build at the `include/` directory directly:

```bash
git clone https://github.com/nihilai-collective/Jsonifier.git
cd Jsonifier

cmake -B build -DCMAKE_INSTALL_PREFIX=/your/install/path
cmake --install build
```

Then in your project:

```cmake
find_package(Jsonifier CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE Jsonifier::Jsonifier)
```

## Including the Header

Whichever route you take, the include is a single line:

```cpp
#include <jsonifier>
```

That pulls in the entire library. There is no separate compilation unit to link against.

## ⚠️ Skipping the CMake Build? Read This.

Jsonifier ships with a CPU feature-detection step baked into its CMake flow. During configure, CMake builds and runs `cmake/main.cpp` on the host machine, queries the CPU via `cpuid` (or `getauxval` on ARM64 Linux), and **writes the results into `include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp`**. That file defines the `JSONIFIER_CPU_INSTRUCTIONS` preprocessor value that the entire SIMD backend keys off of.

**If you drop the headers into a project without running Jsonifier's CMake configure step, that file will be empty, stale, or wrong** — and you'll get one of the following:

- Compilation failures because the SIMD detection macros aren't defined
- A silently-selected fallback backend (no SIMD, much slower than expected)
- A binary that uses instructions your CPU doesn't support (crashes at runtime with `SIGILL`)

If you're bypassing CMake — pasting the headers into your project, using a hand-rolled Makefile, integrating into a non-CMake build system, or cross-compiling — you **must** manually edit `include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp` and set `JSONIFIER_CPU_INSTRUCTIONS` to the bitfield matching your target CPU:

| Feature   | Bit | Value |
|-----------|-----|-------|
| LZCNT     | 0   | `1 << 0` = 1 |
| POPCNT    | 1   | `1 << 1` = 2 |
| BMI       | 2   | `1 << 2` = 4 |
| PCLMULQDQ | 3   | `1 << 3` = 8 |
| NEON      | 4   | `1 << 4` = 16 |
| AVX       | 5   | `1 << 5` = 32 |
| AVX2      | 6   | `1 << 6` = 64 |
| AVX-512   | 7   | `1 << 7` = 128 |
| SVE2      | 8   | `1 << 8` = 256 |

OR them together for the features your target supports. A modern x64 CPU with AVX-512 would be `1 | 2 | 4 | 8 | 32 | 64 | 128 = 239`. An ARM64 CPU with NEON would be `16`. A conservative fallback with no SIMD would be `0`.

See the [CPU Architecture Selection](CPU_Architecture_Selection.md) page for the full breakdown and cross-compilation guidance.

## Running the Test Suite

If you want to verify the install against your compiler and platform, build the unit-tests target with `-DJSONIFIER_UNIT_TESTS=ON`. The test suite pulls in [rt-ut](https://github.com/nihilai-collective/rt-ut) automatically via FetchContent, so no extra setup is needed:

```bash
git clone https://github.com/nihilai-collective/Jsonifier.git
cd Jsonifier

cmake -B build -DJSONIFIER_UNIT_TESTS=ON
cmake --build build --target jsonifier-unit-tests
./build/unit-tests/jsonifier-unit-tests
```

Optional sanitizers (Clang/GCC only — no effect on MSVC for UBSan, and both are auto-disabled for GCC-on-macOS since they don't work in that combination):

```bash
cmake -B build -DJSONIFIER_UNIT_TESTS=ON -DJSONIFIER_ASAN=ON -DJSONIFIER_UBSAN=ON
```

## CPU Architecture Selection

By default, Jsonifier auto-detects the best available instruction set at configure time by building and running the feature-detection program described above. The result is baked into the library as a preprocessor definition — the correct SIMD backend is selected at compile time with zero runtime dispatch overhead.

If you need to override the auto-detected value even when using CMake — for example, when cross-compiling or building a portable binary for a different CPU baseline — set `JSONIFIER_CPU_INSTRUCTIONS` explicitly:

```cmake
cmake -B build -DJSONIFIER_CPU_INSTRUCTIONS=239
```

See the [CPU Architecture Selection](CPU_Architecture_Selection.md) page for the full details.
