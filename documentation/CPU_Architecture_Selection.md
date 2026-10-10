# CPU Architecture Selection

Jsonifier is a SIMD-heavy library, and getting the right SIMD backend selected for your target CPU is what separates near-peak performance from a slow fallback path. By default, everything is automatic — Jsonifier detects your CPU features at configure time and generates a binary specialized for exactly what your machine supports. On x64 the detected tier is the highest one compiled in, and the best tier the running CPU supports is picked at runtime. When automatic detection isn't the right choice (cross-compilation, portable binaries, deployment mismatches), you can override the detection with a single CMake variable.

## How Auto-Detection Works

At configure time, Jsonifier's CMake build fetches [voided-hw-detection](https://github.com/nihilai-collective/voided-hw-detection), which builds and runs a small standalone helper program on the host machine. The program calls `cpuid` (on x64) or queries NEON, SVE2, and PMULL/crypto (carry-less multiply) support via `getauxval`/`sysctlbyname` (on ARM64), then prints a bitfield summarizing which instruction set extensions are available.

CMake captures the printed value and writes the final bitfield into `include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp` as `#define JSONIFIER_CPU_INSTRUCTIONS <value>`, alongside `#define JSONIFIER_SVE2_VECTOR_BITS <value>`. The detected alignment (64 for AVX-512, 32 for AVX2, the SVE vector length in bytes for SVE2, otherwise 16) is written into the generated `jsonifier_cpu_properties.hpp`, and allocations use it as their alignment.

On ARM64, CMake translates the bitfield into the matching compiler flags and the configured backend is the only one compiled in. On x64, the configured value is the *highest* tier compiled into the binary, not the tier the whole build targets. CMake compiles every translation unit at the **AVX baseline**: `/arch:AVX` on MSVC, and `-mavx` plus the detected LZCNT/POPCNT/BMI/PCLMULQDQ flags on GCC/Clang, with `-mavx2`, `-mavx512*`, `-mfma` and `-mf16c` removed. The wider tiers are compiled as separate copies of the SIMD code (see below) and chosen at runtime, so one binary configured for AVX-512 runs on AVX, AVX2 and AVX-512 machines.

## Backends and the Core Collection

Every SIMD backend is a value of `jsonifier::jsonifier_backend` (`avx512`, `avx2`, `avx`, `neon`, `sve2`, `fallback`). Each one has a `jsonifier::backend_traits<backend>` specialization holding its register type (`simd_int_t`), its display `name`, and its per-compiler stage-1 tuning: `blocksPerStep` (64-byte blocks scanned per step) and `tapeStep` (burst size of the scalar tape drain). The derived helpers `backendBytesPerRegister<backend>`, `backendRegistersPerBlock<backend>` and `backendBytesPerStep<backend>` come from those traits.

### One generated copy per tier

On x64, the SIMD-dependent part of the library is compiled once per tier, up to the configured one:

| Copy | Namespace | Compiled with |
|------|-----------|---------------|
| AVX (baseline) | `jsonifier`, `jsonifier::internal` | the translation unit's baseline flags |
| AVX2 | `jsonifier::avx2`, `jsonifier::avx2::internal` | `#pragma GCC target` / `#pragma clang attribute` for AVX2 + BMI1/BMI2/LZCNT/POPCNT/PCLMULQDQ |
| AVX-512 | `jsonifier::avx512`, `jsonifier::avx512::internal` | the same, plus AVX-512 F/BW/VBMI2 |

The copies come from one source. `core/backend_passes.hpp` re-includes the per-tier headers (listed in `core/backend_pass_body.hpp`) once per tier. Before each pass it redefines `JSONIFIER_NAMESPACE`, `JSONIFIER_INTERNAL_NAMESPACE` and `JSONIFIER_CPU_INSTRUCTIONS`, and it resets the per-pass include guards (`JSONIFIER_PASS_GUARD_*`). Every existing `#if JSONIFIER_CHECK_FOR_INSTRUCTION(...)` therefore selects that tier's code inside its own copy, and every function in a copy is compiled for that copy's ISA with the library's normal inlining. MSVC compiles any intrinsic in any function, so it only needs the namespaces, not the pragmas.

The per-tier headers are the SIMD layer, stage 1, UTF-8 validation, the comparators, the compile-time key hash maps, string scanning, the JSON cursor, and the parse, serialize, validate, minify and prettify implementations. Everything else is compiled once and shared by all copies: the public types (`jsonifier::string`, `raw_json_data`, the options structs, `error`), reflection, the containers, the scalar number parsers and formatters, and the printer. A higher-tier copy reaches shared code through `using namespace ::jsonifier::internal`. Scalar helpers that need no per-tier copy are defined only in the baseline copy (`#if JSONIFIER_BACKEND_PASS == 0`). Calls to per-tier helpers are written as `::JSONIFIER_INTERNAL_NAMESPACE::name(...)`, so argument-dependent lookup can never mix two copies.

Each copy then specializes `jsonifier::backend_types<backend>`. That struct maps the backend to that copy's parser, serializer, validator, minifier, prettifier and structural readers, plus the copy's `simdBlocksPerStep`, `simdTapeStep` and `default_backend` globals.

### The core collection

- **`jsonifier::jsonifier_core_internal<backend, initialBufferSize, sharedStorage>`** is one complete parser/serializer built from `backend_types<backend>`. It owns its own fused-path and two-stage structural readers, because their types and step sizes differ per tier.
- **`jsonifier::jsonifier_core_collection<cores...>`** inherits one `jsonifier_core_internal` per tier. It owns a single `jsonifier_core_storage`, the string buffer and error list that every core shares. Each top-level call (`parseJson`, `serializeJson`, `validateJson`, `minifyJson`, `prettifyJson`) is forwarded to exactly one core.

`jsonifier::jsonifier_core<initialBufferSize>` is an alias for the collection, so existing code that declares `jsonifier::jsonifier_core<> parser;` keeps working unchanged. Its cores depend on the configured tier:

| Configured tier | `jsonifier_core<>` holds |
|-----------------|--------------------------|
| AVX-512 | `avx512`, `avx2`, `avx` |
| AVX2 | `avx2`, `avx` |
| AVX, ARM64, scalar | the configured backend only |

### Runtime selection

The collection picks its core once, the first time `selectedBackend()` is called, and caches the result in a function-local static. It walks its cores in declaration order and takes the first one whose backend the running CPU supports, falling back to the last core. On x64 the check reads `cpuid` directly and confirms OS register-state support through `xgetbv`:

| Backend | Required features |
|---------|-------------------|
| `avx512` | AVX-512 F + BW + VBMI2, AVX2, BMI1, BMI2, LZCNT, POPCNT, PCLMULQDQ, ZMM/OpMask state enabled |
| `avx2` | AVX2, BMI1, BMI2, LZCNT, POPCNT, PCLMULQDQ, YMM state enabled |
| `avx` | always selected as the last resort |

Each forwarded call costs one comparison against a cached value. Root strings, bools and numbers are forwarded through `JSONIFIER_INLINE` overloads, and everything else through plain `inline` ones, matching the base parser and serializer.

### Limits

- **Baseline requirements:** the baseline copy requires AVX plus whichever of LZCNT/POPCNT/BMI/PCLMULQDQ were configured. A pre-AVX, SSE-only CPU cannot run the binary.
- **Shared code runs at the baseline tier:** The float formatting and the printer exist once and run at the baseline tier inside every core.
- **Generic parsing:** `jsonifier_core<>` exposes `iterate` and `iterateMany` through the per-tier `generic_iterator` CRTP base, so stage 1 for [generic parsing](Generic_Parsing.md) is selected at runtime like every other call, and it reuses the core's own structural readers. `jsonifier::generic::parser` is a thin wrapper around a `jsonifier_core<>`. The returned `document`/`value`/`object`/`array` types are called from user code, so they stay compiled once at the baseline tier. A `document` points into the core's tape, so a later `parseJson`, `validateJson` or `iterate` call on the same core invalidates it.
- **Compile time grows:** every user type parsed or serialized through `jsonifier_core<>` is instantiated once per compiled tier, including its compile-time key hash map.

## The Feature Bits

| Feature  | Bit | Value | Macro |
|----------|-----|-------|-------|
| LZCNT    | 0 | 1   | `JSONIFIER_LZCNT` |
| POPCNT   | 1 | 2   | `JSONIFIER_POPCNT` |
| BMI      | 2 | 4   | `JSONIFIER_BMI` |
| PCLMULQDQ| 3 | 8   | `JSONIFIER_CLMUL` |
| NEON     | 4 | 16  | `JSONIFIER_NEON` |
| AVX      | 5 | 32  | `JSONIFIER_AVX` |
| AVX2     | 6 | 64  | `JSONIFIER_AVX2` |
| AVX-512  | 7 | 128 | `JSONIFIER_AVX512` |
| SVE2     | 8 | 256 | `JSONIFIER_SVE2` |

**Important detail:** on GCC and Clang, the AVX tier bits are mutually exclusive in the final bitfield — auto-detection picks the highest supported tier (AVX-512 → AVX2 → AVX → none) and records only that single bit, though the compiler flags for lower tiers are still applied (an AVX2 build gets `-mavx -mavx2` under GCC). **On MSVC, an AVX-512-capable target sets the AVX-512, AVX2, *and* AVX bits together** (and an AVX2-capable target sets AVX2 and AVX together), since `/arch:AVX512` doesn't imply the lower-tier intrinsics headers are unlocked the way `-mavx512...` does on GCC/Clang — code gated on `JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)` needs that bit set on MSVC even when AVX-512 is the selected tier.

NEON and SVE2 are mutually exclusive with each other (every SVE2-capable part also reports NEON, but only one backend bit is ever set) and are exclusive to ARM64. The bit-manipulation features (LZCNT, POPCNT, BMI) are independent and can co-exist with any SIMD tier. PCLMULQDQ (carry-less multiplication) is independent on x64; on ARM64 its presence upgrades the SVE2 build's `-march` flag to include `+aes`.

## OS-Level Requirements

On x64, having AVX/AVX2/AVX-512 available in the CPU is not enough — the operating system also has to enable state-saving for those register sets. Jsonifier's detector checks this via `xgetbv`:

- **AVX/AVX2** require the OS to have XMM and YMM state enabled
- **AVX-512** additionally requires ZMM and OpMask state enabled

If a CPU supports AVX-512 but the OS hasn't enabled the state (common on some older Windows configurations, hypervisors, or containerized environments), the detector correctly reports it as unavailable and falls back to AVX2 or lower. This prevents illegal-instruction crashes at runtime.

## Overriding the Auto-Detected Value

When automatic detection isn't right for your use case, set `JSONIFIER_CPU_INSTRUCTIONS` explicitly at CMake configure time. Two forms are accepted.

**Pipe-separated flags (readable):**

```bash
cmake -B build -DJSONIFIER_CPU_INSTRUCTIONS="1|2|4|64"
```

**Or the numeric OR of the values:**

```bash
cmake -B build -DJSONIFIER_CPU_INSTRUCTIONS=71
```

Both produce identical results. When targeting SVE2, also pass `-DJSONIFIER_SVE2_VECTOR_BITS=<bits>` with the target's SVE vector length — SVE2 is only enabled when that length is known, since the backend uses fixed-length SVE types (`-msve-vector-bits`). When cross-compiling without `JSONIFIER_CPU_INSTRUCTIONS` defined, configure warns and falls back to the scalar build (`0`).

The pipe form is self-documenting — pass it into your CI or build scripts and future-you will thank present-you.

Common override values:

| Target | Pipe form | Numeric |
|--------|-----------|---------|
| ARM64 with NEON | `16` | `16` |
| x64 with AVX-512 | `1\|2\|4\|128` | `135` |
| x64 with AVX2 | `1\|2\|4\|64` | `71` |
| x64 with AVX only | `1\|2\|4\|32` | `39` |
| x64 with bit-ops + PCLMULQDQ only (no SIMD) | `1\|2\|4\|8` | `15` |
| x64 with bit-ops only (no SIMD) | `1\|2\|4` | `7` |
| Pure scalar fallback | `0` | `0` |

Remember that on GCC/Clang only one AVX tier bit is set at a time — a target of "AVX2" is just bit 6 (value 64), not bits 5 and 6 together. On MSVC, request the tier you actually want and let the cascade fill in the lower bits (see the "Important detail" note above) rather than trying to compose them by hand.

## The Pure-Scalar Fallback (`JSONIFIER_CPU_INSTRUCTIONS = 0`)

Setting the value to `0` produces a fully portable build with no SIMD and no hardware bit-manipulation intrinsics. Every operation falls back to `std::countl_zero`, `std::popcount`, and scalar C++20 stdlib equivalents.

This mode is slower than any SIMD-enabled build, but it is **fully supported** — every code path has an `#else` branch that reaches for the stdlib scalar version. Use it when:

- You're producing a maximally portable binary for unknown-CPU deployment
- You're building for a target where the intrinsics aren't available
- You want to verify Jsonifier's correctness independent of any SIMD-specific code

## When to Override Detection

**Cross-compiling.** You're building on machine A for machine B. Auto-detection would tell you about A's CPU; the binary needs to run on B. Set `JSONIFIER_CPU_INSTRUCTIONS` to B's feature set.

**Portable binaries.** You're building a binary that will be distributed to machines with different CPU generations. Pick the lowest common denominator across your target audience (often AVX2, sometimes just AVX for maximum compatibility) and override to that.

**Testing lower-tier code paths.** You want to benchmark or debug Jsonifier's AVX2 backend on a machine that has AVX-512. Override to `1|2|4|64` (AVX2 tier) instead of the auto-detected AVX-512 value to force the AVX2 code path.

**Rare OS/CPU mismatches.** The host CPU has AVX-512 but the OS doesn't have ZMM state enabled — some older Windows configurations, older hypervisors. Auto-detection handles this correctly, but if you're overriding for another reason, remember to match reality.

## ⚠️ Skipping the CMake Build

If you drop Jsonifier's headers into a project without running its CMake configure step — hand-rolled Makefile, non-CMake build system, or copying headers into a monorepo — the `jsonifier_cpu_instructions.hpp` file will be empty, stale, or wrong, and you'll get one of:

- Compilation failures because the SIMD detection macros aren't defined
- A silently-selected fallback backend (much slower than expected)
- A binary that uses instructions your CPU doesn't support (crashes at runtime with `SIGILL`)

If you're bypassing CMake, **you must manually edit `include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp`** and set `JSONIFIER_CPU_INSTRUCTIONS` to a valid value. See [Installation](Installation.md) for the details on this footgun.

## Verifying What Got Selected

After configuring, check the generated header:

```bash
cat include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp
```

You'll see something like:

```cpp
#define JSONIFIER_CPU_INSTRUCTIONS 71

#define JSONIFIER_SVE2_VECTOR_BITS 128
```

(`71` = LZCNT + POPCNT + BMI + AVX2, i.e. `1|2|4|64` — a typical AVX2-tier x64 machine under GCC/Clang. The same machine under MSVC with PCLMULQDQ reports `111` = `1|2|4|8|32|64`, because of the MSVC AVX cascade described above. `JSONIFIER_SVE2_VECTOR_BITS` is ignored unless the SVE2 bit is set.)

You can also verify the compiler flags Jsonifier is passing by looking at the CMake configure output — the detection script prints each `Instruction Set Found: <name>` line as it walks the bit table.

## The Check Macros

For internal code paths (and for anyone extending Jsonifier), the header exposes compile-time predicates:

```cpp
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT)
    // POPCNT is available
#endif
```

`JSONIFIER_CHECK_FOR_INSTRUCTION` is a bitwise AND against a specific feature bit. The header also `#error`s if NEON and SVE2 are both set, or if SVE2 is set while `JSONIFIER_SVE2_VECTOR_BITS` is `0`.

Convenience masks for common groups:

- `JSONIFIER_ANY_AVX` — any AVX tier (AVX, AVX2, or AVX-512)
- `JSONIFIER_ANY_SIMD` — any SIMD backend (AVX, AVX2, AVX-512, NEON, or SVE2)

## What's Next

- **[Installation](Installation.md)** — includes the ⚠️ warning about the `jsonifier_cpu_instructions.hpp` header when bypassing CMake
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the runtime API that benefits from correct SIMD selection

---