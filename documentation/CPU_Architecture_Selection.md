# CPU Architecture Selection

Jsonifier is a SIMD-heavy library, and getting the right SIMD backend selected for your target CPU is what separates near-peak performance from a slow fallback path. By default, everything is automatic — Jsonifier detects your CPU features at configure time and generates a binary specialized for exactly what your machine supports. When automatic detection isn't the right choice (cross-compilation, portable binaries, deployment mismatches), you can override the detection with a single CMake variable.

## How Auto-Detection Works

At configure time, Jsonifier's CMake build script builds and runs a small standalone helper program (`FeatureCheck/main.cpp`) on the host machine. The program calls `cpuid` (on x64) or reports NEON support (on ARM64), then prints a bitfield summarizing which instruction set extensions are available.

CMake captures the printed value, translates it into the appropriate compiler flags (`/arch:AVX2` on MSVC, `-mavx2 -mbmi -mpopcnt` and friends on GCC/Clang), and writes the final bitfield into `include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp` as `#define JSONIFIER_CPU_INSTRUCTIONS <value>`.

At compile time, Jsonifier's SIMD backend and bit-manipulation helpers select the fastest available implementation via `if constexpr` on the `JSONIFIER_CPU_INSTRUCTIONS` value. There is no runtime dispatch overhead — the correct code path is baked into the binary.

## The Feature Bits

| Feature | Bit | Value | Macro |
|---------|-----|-------|-------|
| POPCNT   | 0 | 1  | `JSONIFIER_POPCNT` |
| LZCNT    | 1 | 2  | `JSONIFIER_LZCNT` |
| BMI      | 2 | 4  | `JSONIFIER_BMI` |
| NEON     | 3 | 8  | `JSONIFIER_NEON` |
| AVX      | 4 | 16 | `JSONIFIER_AVX` |
| AVX2     | 5 | 32 | `JSONIFIER_AVX2` |
| AVX-512  | 6 | 64 | `JSONIFIER_AVX512` |

**Important detail:** the three AVX bits are mutually exclusive in the final bitfield. Auto-detection picks the highest supported tier (AVX-512 → AVX2 → AVX → none) and sets only that single bit. The compiler flags for lower tiers are still applied (an AVX2 build gets `-mavx -mavx2` under GCC), but the runtime bitfield only records the SIMD tier that was selected.

The bit-manipulation features (POPCNT, LZCNT, BMI) are independent and can co-exist with any SIMD tier. NEON is exclusive to ARM64.

## OS-Level Requirements

On x64, having AVX/AVX2/AVX-512 available in the CPU is not enough — the operating system also has to enable state-saving for those register sets. Jsonifier's detector checks this via `xgetbv`:

- **AVX/AVX2** require the OS to have XMM and YMM state enabled
- **AVX-512** additionally requires ZMM and OpMask state enabled

If a CPU supports AVX-512 but the OS hasn't enabled the state (common on some older Windows configurations, hypervisors, or containerized environments), the detector correctly reports it as unavailable and falls back to AVX2 or lower. This prevents illegal-instruction crashes at runtime.

## Overriding the Auto-Detected Value

When automatic detection isn't right for your use case, set `JSONIFIER_CPU_INSTRUCTIONS` explicitly at CMake configure time. Two forms are accepted.

**Pipe-separated flags (readable):**

```bash
cmake -B build -DJSONIFIER_CPU_INSTRUCTIONS="1|2|4|32"
```

**Or the numeric OR of the values:**

```bash
cmake -B build -DJSONIFIER_CPU_INSTRUCTIONS=39
```

Both produce identical results. The pipe form is self-documenting — pass it into your CI or build scripts and future-you will thank present-you.

Common override values:

| Target | Pipe form | Numeric |
|--------|-----------|---------|
| ARM64 with NEON | `8` | `8` |
| x64 with AVX-512 | `1\|2\|4\|64` | `71` |
| x64 with AVX2 | `1\|2\|4\|32` | `39` |
| x64 with AVX only | `1\|2\|4\|16` | `23` |
| x64 with bit-ops only (no SIMD) | `1\|2\|4` | `7` |
| Pure scalar fallback | `0` | `0` |

Remember that only one AVX tier bit is set at a time — a target of "AVX2" is just bit 5, not bits 4 and 5 together.

## The Pure-Scalar Fallback (`JSONIFIER_CPU_INSTRUCTIONS = 0`)

Setting the value to `0` produces a fully portable build with no SIMD and no hardware bit-manipulation intrinsics. Every operation falls back to `std::countl_zero`, `std::popcount`, and scalar C++20 stdlib equivalents.

This mode is slower than any SIMD-enabled build, but it is **fully supported** — every code path has an `#else` branch that reaches for the stdlib scalar version. Use it when:

- You're producing a maximally portable binary for unknown-CPU deployment
- You're building for a target where the intrinsics aren't available
- You want to verify Jsonifier's correctness independent of any SIMD-specific code

## When to Override Detection

**Cross-compiling.** You're building on machine A for machine B. Auto-detection would tell you about A's CPU; the binary needs to run on B. Set `JSONIFIER_CPU_INSTRUCTIONS` to B's feature set.

**Portable binaries.** You're building a binary that will be distributed to machines with different CPU generations. Pick the lowest common denominator across your target audience (often AVX2, sometimes just AVX for maximum compatibility) and override to that.

**Testing lower-tier code paths.** You want to benchmark or debug Jsonifier's AVX2 backend on a machine that has AVX-512. Override to `1|2|4|32` (AVX2 tier) instead of the auto-detected `1|2|4|64` to force the AVX2 code path.

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
#define JSONIFIER_CPU_INSTRUCTIONS 39
```

You can also verify the compiler flags Jsonifier is passing by looking at the CMake configure output — the detection script prints each `Instruction Set Found: <name>` line as it walks the bit table.

## The Check Macros

For internal code paths (and for anyone extending Jsonifier), the header exposes compile-time predicates:

```cpp
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT)
    // POPCNT is available
#endif
```

`JSONIFIER_CHECK_FOR_INSTRUCTION` is a bitwise AND against a specific feature bit.

Convenience masks for common groups:

- `JSONIFIER_ANY_AVX` — any AVX tier (AVX, AVX2, or AVX-512)
- `JSONIFIER_ANY_SIMD` — any SIMD backend (NEON, AVX, AVX2, or AVX-512)

## What's Next

- **[Installation](Installation.md)** — includes the ⚠️ warning about the `jsonifier_cpu_instructions.hpp` header when bypassing CMake
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the runtime API that benefits from correct SIMD selection

---