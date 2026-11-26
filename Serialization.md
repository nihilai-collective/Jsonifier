# Jsonifier Serialization: Design and Performance

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Jsonifier serializes a reflection-registered C++ object in two passes: an exact-or-over size estimate, one buffer resize, then a single branch-light write pass that never checks capacity. Every byte the schema already knows (key names, quotes, colons, commas, indentation) is folded at compile time into packed integer constants and emitted with fixed-width stores, so the runtime work left is the data itself: strings, numbers and container lengths.

This paper describes that design as it stands in `include/jsonifier-incl/serializing/` (`serializer.hpp`, `serialize_impl.hpp`) and its helpers for strings (`string_utils.hpp`), integers (`i_to_str.hpp`) and floats (`d_to_str.hpp`).

## Architecture

The whole pipeline is resolved at compile time from `jsonifier::core<T>` and `serialize_options`: one sizing walk, one resize, one write walk.

```mermaid
flowchart LR
    A["serializeJson<br/>entry point"] --> B["Size pass<br/>staticSize shortcuts"]
    B --> C["One resize<br/>bound + 64 bytes slack"]
    C --> D["Write pass<br/>fixed-width overstores"]
    D --> E["Member headers<br/>one packed store each"]
    D --> F["Strings<br/>SIMD, SWAR, scalar"]
    D --> G["Integers<br/>4 digits per store"]
    D --> H["Floats<br/>zmij shortest round-trip"]
```

The `serialize<options>` dispatcher is a force-inlined (`JSONIFIER_INLINE`) forwarder for every value type: it strips cv and reference qualifiers and calls that type's `serialize_impl` specialization directly. Each registered type gets its own `serialize_impl` specialization: objects, maps, vectors, fixed arrays, tuples, strings, chars, enums, numbers, bools, pointers and optionals, raw JSON, null, and variants (via `visit`). Every `impl` is force-inlined; the only outlined (`JSONIFIER_NOINLINE`) calls are the object and array cases of raw JSON.

The serializer is compiled once, for the single instruction-set tier chosen at CMake configure time (detected on the build machine, or named by `JSONIFIER_CPU_INSTRUCTIONS`, and capped with `JSONIFIER_SIMD_TIER`). `serializeJson` calls `serialize<options>::impl` directly, with no CPUID read, function pointer, virtual call or per-call check.

## Key techniques

### Size first, then write without bounds checks

`get_size_impl` computes an upper bound for the output before a byte is written. Fixed-width types report a `staticSize` (numbers and enums 32, `bool` 5, `char` 8, null 4), so a `vector<double>` is sized as `n × 32` plus separators with no loop. Strings are sized at `6n + 2` by default, the worst case where every byte becomes a `\u00XX` escape. With `prescanStrings` set, each string is scanned instead and sized at its exact escaped length plus 2 for the quotes (see *Pre-scanning strings*). Object keys contribute `key.size() + 3` (or `+ 4` when prettified) as a compile-time constant.

`serializeJson` adds 64 bytes of slack and calls `resize_and_overwrite` when the buffer has it, so no zero-fill happens. The write pass then trusts the bound completely: no growth checks, no reallocation, and every writer may overstore (write 8 bytes, advance 4) into the slack.

### Writing through `resize_and_overwrite`

When the output buffer has C++23's `resize_and_overwrite` (detected by the `resize_and_overwriteable_types` concept), the whole write pass runs inside it. `std::string::resize_and_overwrite(n, op)` grows the buffer to at least `n` without initializing it, calls `op(data, n)`, and sets the final size to whatever `op` returns. One call replaces the usual three steps: zero-filling resize, write, shrinking resize.

The `op` Jsonifier passes is `serialize_writer_ro`, a small functor that holds a reference to the object:

```cpp
template<serialize_options options, typename value_type> struct serialize_writer_ro {
	JSONIFIER_INLINE uint64_t operator()(char* ptrNew, uint64_t) noexcept {
		const write_buffer_ptr start = std::bit_cast<write_buffer_ptr>(ptrNew);
		return static_cast<uint64_t>(serialize<options>::impl(object, start, 0) - start);
	}

	JSONIFIER_INLINE serialize_writer_ro(value_type& __restrict objectNew) noexcept : object{ objectNew } {
	}

	serialize_writer_ro& operator=(const serialize_writer_ro&) noexcept = delete;
	serialize_writer_ro& operator=(serialize_writer_ro&&) noexcept		= delete;
	serialize_writer_ro(const serialize_writer_ro&) noexcept			= delete;
	serialize_writer_ro(serialize_writer_ro&&) noexcept					= delete;

	value_type& __restrict object;
};
```

- **`operator()` is force-inlined.** `JSONIFIER_INLINE` makes the compiler expand the serializer into the standard library's call site, instead of leaving an out-of-line function that the library calls. The write pass then runs as part of `resize_and_overwrite`'s own body, with the buffer pointer in a register and no extra call frame. Whether `resize_and_overwrite` itself inlines into `serializeJson` is still the compiler's choice.
- **The functor is a reference and a function.** It holds only `value_type& object`, and its copy and move operations are deleted. `resize_and_overwrite` takes `op` by value, which compiles only because `serializeJson` passes a prvalue: C++17's guaranteed copy elision constructs the functor directly in the parameter, so passing it costs nothing and the object is never copied.
- **The length comes straight from the pointer.** `operator()` returns the end pointer minus the start, so the final size is set from the write itself, with no second pass and no separate size variable.

Buffers without `resize_and_overwrite` take a fallback: `resize` to the bound if the buffer is smaller, write, then `resize` down to the written length. That path zero-fills on growth.

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD register, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

### Schema text as packed integers

`packed_blitter` turns `"key":` into one constant at compile time. Literals up to 8 bytes become a single 2, 4 or 8-byte integer; longer ones become an array of `uint64_t` rounded up to 16 or a multiple of 32. Writing a member header is one `pow2MemcpyWrapper` store and a pointer bump by the true length.

### Member iteration: CABERIHT

Object members are walked with CABERIHT, Constexpresh Aggregated Bases for Efficient Runtime Iteration of Heterogeneous Types. It is CAFBERIHT without the filter; the full pattern, with compile-time filtering, is described in [its own paper](https://nihilai-collective.net/cafberiht) [1]. Each member becomes a base class, and one fold expression over those bases writes the whole object in declaration order.

1. **One base per member.** `get_serialize_base` expands an index sequence over the `core<T>::parseValue` tuple and wraps each entry in `json_entity_serialize<options, entity>`. Each base carries its member's `name`, `memberPtr` and `isItLast` as compile-time constants.
2. **Aggregate the bases.** `serialize_map<bases...>` inherits from every one of them, so a type with 12 members gets one type with 12 bases.
3. **Fold over them.** `iterateValues` is a single fold, `((bufferPtr = bases::processIndex(value, bufferPtr, indent)), ...)`. Each `processIndex` is a static function of its own base, so every call is direct and resolved at compile time.

Inside each `processIndex`, nothing about the member is decided at runtime:

- the header `"name":` is that base's own `packed_blitter` constant;
- the value type is known, so `serialize<options>::impl` picks its writer with no dispatch;
- the trailing comma comes from `if constexpr (!isItLast)`, not an index check.

The sizing pass uses the same pattern: `size_getter_map` aggregates `json_entity_size` bases and folds `processIndex` over them, so both passes walk members the same way.

The result is no virtual calls, no `std::tuple` `get<I>` recursion, no runtime member index and no loop over members. A whole object becomes straight-line code: a run of fixed stores for headers and commas, with the value writers in between. The CAFBERIHT paper measures this same fold-over-bases dispatch as zero overhead against a hand-written baseline by assembly diff.

Runtime key exclusion (`jsonifierExcludedKeys`) is a separate mechanism, a set lookup inside each `processIndex` (see Limitations).

### Copies have a compile-time power-of-2 size

Nearly every copy on the write path has a power-of-2 size fixed at compile time: 2, 4 or 8 bytes. `pow2MemcpyWrapper<size>` takes the size as a template argument and `static_assert`s `std::has_single_bit(size)`, so a copy of any other size will not compile. A `memcpy` of 2, 4 or 8 bytes known at compile time lowers to a single unaligned load and store, with no call, no length branch and no tail loop.

What this does to the generated code:

| Copy | Typical x86-64 | Typical ARM64 |
| --- | --- | --- |
| `std::memcpy`, runtime length | `call memcpy` (or an inline length dispatch with branches and a tail loop) | `bl memcpy` |
| `jsonifierMemcpy`, runtime length under 256 bytes | at most four compare-and-branch steps on the length (no jump table), then two overlapping 1 to 128-byte copies, each split into register-width load–store pairs | the same, with `ldr`/`str` or `ldr q`/`str q` pairs |
| `jsonifierMemcpy`, runtime length of 256 bytes or more | `call memcpy` | `bl memcpy` |
| Compile-time 7 bytes | two overlapping 4-byte moves, or 4 + 2 + 1 | several loads and stores |
| Compile-time 2 / 4 / 8 bytes | one `mov` load, one `mov` store | one `ldrh`/`ldr` and one `strh`/`str` |
| Compile-time 2 / 4 / 8 bytes of constant data | 2 or 4 bytes: one immediate store, no load (`mov dword ptr [rdi], imm32`); 8 bytes: one `movabs` of the constant into a register, then one `mov` store (a single `mov qword ptr [rdi], imm32` only when the value fits a sign-extended 32-bit immediate) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers across every member write of the object.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Runtime lengths stay short.** Copies whose length is only known at run time, such as the runs that Minify and Prettify carry over, go through `jsonifierMemcpy` (`utility.hpp`). It sorts the length into a power-of-2 bucket `b`, where `b ≤ length < 2b`, and copies the first `b` bytes and the last `b` bytes. The two copies overlap, so any length from 2 to 255 bytes takes one fixed-size copy pair (a single byte takes one store), with no loop and no tail. Buckets wider than the vector register are split into register-width moves: the 128-byte bucket is 8 moves on AVX2, 4 on AVX-512 and 16 on SSE or NEON. Only lengths of 256 bytes or more reach `std::memcpy`. `jsonifierMemcpyUpTo<maxBytes>` takes a compile-time upper bound and leaves out every bucket above it, so a caller whose length is bounded never pays for the cases it cannot hit.
- **The bucket is chosen by a compare tree, not a jump table.** `dispatchBuckets` is a balanced binary tree of `length < 2^k` tests generated at compile time, so the bucket is found in at most four conditional branches on the length itself, with no `lzcnt` and no indirect jump. A switch on `std::bit_width(length)` compiled to a jump table on GCC and lost to glibc's `memcpy`; the tree compiles to the same branch-only shape on GCC and Clang, and in the [memcpy-decomp](https://github.com/nihilai-collective/memcpy-decomp) [2] harness it beats `std::memcpy` on every range tested, from 1–15 to 1–129 bytes, on all five platforms.
- **Inlining and scheduling.** Each copy is one or two instructions, so a whole object's serializer inlines into straight-line code. The compiler can then interleave stores from neighbouring members with the value work between them.

**What actually reaches `jsonifierMemcpy`.** We counted the length of every call to `jsonifierMemcpy` and `jsonifierMemcpyUpTo`, separately, over one instrumented run of the unit tests in `unit-tests/parsing_tests.hpp`. The counters were a temporary patch and are not in the library.

| Length | `jsonifierMemcpy` calls | Share | `jsonifierMemcpyUpTo` calls | Share |
| --- | --- | --- | --- | --- |
| 0 | 13,040 | 1.16% | 0 | 0.00% |
| 1 | 4,012 | 0.36% | 938 | 0.22% |
| 2–3 | 9,592 | 0.85% | 1,891 | 0.44% |
| 4–7 | 22,641 | 2.01% | 8,303 | 1.93% |
| 8–15 | 185,573 | 16.47% | 411,279 | 95.82% |
| 16–31 | 735,745 | 65.29% | 6,797 | 1.58% |
| 32–63 | 149,125 | 13.23% | 0 | 0.00% |
| 64–127 | 6,552 | 0.58% | 0 | 0.00% |
| 128–255 | 312 | 0.03% | 0 | 0.00% |
| 256 or more (`std::memcpy`) | 351 | 0.03% | 1 | 0.00% |
| **Total** | **1,126,943** | | **429,209** | |

Two lengths dominate `jsonifierMemcpy`: exactly 16 bytes (659,358 calls, 58.5%) and exactly 32 bytes (146,188 calls, 13.0%), with a smaller spike at exactly 64 (3,688 calls). That matches the member-header blit, whose length `lengthToCopy` is the key literal rounded up to 16, 32 or 64 bytes. Those calls carry a template-constant length, so they fold to fixed-size vector stores. Of the 1,126,943 calls, 99.69% are 64 bytes or less, and only 351 (0.03%) are 256 bytes or more and reach `std::memcpy`.

`jsonifierMemcpyUpTo` is narrower still. 95.8% of its 429,209 calls fall in the 8–15 bucket, led by 10 bytes (31.8%) and 9 bytes (22.4%), and every call but one is 31 bytes or less. The one exception was a single 339-byte copy, which took the `std::memcpy` fallback.

That is why the writers round up and overstore instead of copying exact lengths. A 5-byte `false` is one 8-byte store. A 3-digit group is a 4-byte load followed by a 2-byte and a 1-byte store. A 7-byte key literal is one 8-byte store. The pointer then advances by the true length, and the size pass's slack absorbs the extra bytes.

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through `jsonifierMemcpy` rather than `pow2MemcpyWrapper`. That is why they appear in the counts above, but `lengthToCopy` is still a compile-time constant, so the force-inlined call folds its length dispatch away. When serializing an object, the copies whose length is known only at runtime are off the common path: the clean runs and escape sequences of a string block that holds an escapable byte (see *Strings*), raw JSON passthrough, and the memset for indents deeper than 8 levels. The Minify and Prettify reformatters are the exception: they copy each run of input between structural characters at its runtime length, through `jsonifierMemcpy`, as their common path.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test this writer leads Glaze by 7.79–13.61× freshly allocated on all five builds.

### Strings: picked by length

`string_serializer::impl` sorts every string into one of three length classes before it touches a byte. In all three the escapable bytes are the same: `"`, `\` and anything below 0x20.

1. **Under 16 bytes: one page-safe vector.** If the source does not sit within 16 bytes of the end of a 4 KiB page, `pageSafeShortImpl` loads a full 16-byte vector, masks its escape bits to the string's true length and, when nothing is set, stores all 16 bytes and advances by the length. A short clean key or value is one load, one test and one store, with no branch on its length. Reading past the string's end cannot fault, because the page check guarantees the 16 bytes are mapped; under ASan the load moves into a `JSONIFIER_NO_SANITIZE_ADDRESS` helper so the sanitizer does not flag it. Near a page end the string falls back to two overlapping 4- or 2-byte loads (`smallImpl`) below 8 bytes, or SWAR on 64-bit words (`swarFinish`) from 8 to 15.
2. **16 to 64 bytes: head and tail.** `mediumImpl` loads one vector from the start of the string and one ending at its last byte, tests both and, when both are clean, stores both. The two overlap in the middle, so any length in the range is covered with no loop and no remainder: 16-byte vectors up to 32 bytes, then 32-byte vectors on AVX2, or two 16-byte head-and-tail pairs over 32-byte halves where the widest register is 16 bytes. A string with any escapable byte returns `false` and takes the general path.
3. **Everything else: check first, then store.** The general path walks the widest available SIMD width down to the narrowest, testing each block before storing it. A clean block is stored and advances the full width; a dirty one is split into 16-byte lanes (`escapeLane`), which copy the clean runs at their exact lengths and write each escape from a 256-entry table. The last partial block is one 16-byte load ending at the string's last byte (`overlappedFinish`), with its escape mask shifted down to the bytes not yet written, and clean bytes are copied at their exact length.

### Integers: 4 digits per store, split at 10^8

Integers go through `to_chars` in `i_to_str.hpp`, which has one specialization per width (8, 16, 32 and 64-bit unsigned) plus one for every signed type.

1. **Sign.** A signed value stores `'-'` unconditionally and advances the pointer by one only when the value is negative, then takes its magnitude in the unsigned type of the same width (`0 - v` when negative), so `INT64_MIN` works too. The magnitude goes to the unsigned writer of that width.
2. **Below 10^8.** `writeBelow1e8` covers every value under 100,000,000 with at most two compares. Below 100 it copies 2 bytes from a 100-entry table of one- or two-digit groups and advances by 1 or 2. Below 10,000 it copies 4 bytes from the 10,000-entry four-digit table, starting past the leading zeros, so the store is always 4 bytes and the pointer advances by the true length. Otherwise it splits the value into two 4-digit halves, writes the high half the same way and the low half as a full 4-byte group.
3. **10^8 and above.** A 64-bit value splits once at 10^8 into a high part and an 8-digit low part, and splits the high part again when it is itself 10^8 or more, so a 20-digit value is a 1-to-4-digit head plus two 8-digit blocks. A 32-bit value needs only one split, into a 1- or 2-digit head and one 8-digit block. Every 8-digit block is two 4-byte stores from the four-digit table.

Inside an 8-digit block the split at 10^4 is a multiply by 109951163 and a shift right by 40. The 10^8 splits are plain divisions by a constant, which the compiler lowers to a multiply by a reciprocal.

So a 20-digit `uint64_t` is two 10^8 splits, one 10^4 split per 8-digit block and five 4-byte stores, with no loop and no per-digit work. Each table copy writes a full 2 or 4 bytes while the pointer advances only past the digits that count, so a short head can write a few bytes beyond its digits; the next store overwrites them, and the 32 bytes the size estimate reserves per number cover the last one.

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on four of the five builds, by 1.04× to 2.48×; on macOS / GCC the reflection writer wins Int64 (0.85×) and ties Uint64 (1.00×).

### Floats

Doubles go through `jsonifier::internal::write`, a shortest-round-trip formatter vendored in `d_to_str.hpp`.

## Minified and prettified output

`serialize_options` carries `prettify`, `indentSize` (default 3), `indentChar` (default space) and `prescanStrings` (default false) as template parameters, so the two modes compile to separate code with no runtime mode check.

| Piece | Minified | Prettified |
| --- | --- | --- |
| Member header | `"key":` packed store | `"key": ` packed store |
| Separator | one `,` byte | `indent_table<",\n">` blit |
| Open / close | one `{` / `}` byte | `"{\n"` + indent, `"\n"` + indent, then `}` |
| Size estimate per member | `key + 3` | `key + 4`, plus `2 + indent` per separator |

`indent_table` prebuilds, at compile time, the prefix (`,\n`, `{\n` or `\n`) followed by 8 levels of indent characters as a packed `uint64_t` array. A newline plus indent is a short run of 8-byte stores from that table, rounded up into the slack. Nesting deeper than 8 levels falls back to `memset` for the remainder, marked `[[unlikely]]`.

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3].

## Performance results

Jsonifier was fastest in 244 of the 255 write tests on five builds, tied 1 and lost 10. With the output string reused across iterations it was again fastest in 244 of 255, tied 1 and lost 10. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on the three x86 builds in both runs and loses it to simdjson On Demand on both macOS builds, and wins Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. Two are to simdjson On Demand: Minify on macOS / GCC and macOS / Clang. The other six are against simdjson's reflection writer, which exists only on GCC: Uint64, minified CitmCatalog, minified Marine IK Reverse Small and minified Marine IK Small on Linux / GCC, and Double and minified Marine IK Small on macOS / GCC. The one freshly allocated tie is also against the reflection writer: minified Github Events Small on macOS / GCC. Reused, the two Minify losses on macOS remain, and the other eight losses are against the reflection writer: minified CitmCatalog, Marine IK Reverse Small and Marine IK Small on Linux / GCC, and Double, Int64 and minified CitmCatalog, Marine IK Reverse and CitmCatalog Small on macOS / GCC. The one reused tie is Uint64 on macOS / GCC. Linux / Clang wins all 51 tests in both runs, and Windows / MSVC wins all 51 reused. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.37–8.48× across the five builds and the reflection writer by 14–58× on the two GCC builds; Small documents lead them by 1.40–8.71× and 12–60×. Three AVX-512 builds are reported separately, in *AVX-512 builds* below: there Jsonifier was fastest in 149 of 153 write tests freshly allocated and 150 of 153 reused.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) [4] sweep published November 27, 2026 (the two macOS builds finished on November 28), with Jsonifier [a1328b5](https://github.com/nihilai-collective/jsonifier/commit/a1328b5) [5], Glaze [283c9664](https://github.com/stephenberry/glaze/commit/283c9664) [6], simdjson [4ba1cb3ae](https://github.com/simdjson/simdjson/commit/4ba1cb3ae) [7] and BenchmarkSuite [ff0f871](https://github.com/nihilai-collective/benchmarksuite/commit/ff0f871) [8] on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3]. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-43 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-43 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.52 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size. Ratios are computed from unrounded throughput, so one recomputed from the rounded values shown can differ in the last digit.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M2 Pro. Non-converged results are included in the rankings and in the win, tie and loss counts. Each one is marked n/c in the tables below, and a test with a non-converged result should be read with reduced statistical confidence.

**Ranking.** Libraries are ordered by throughput. Variance is Bessel-corrected, and two neighbouring libraries tie when Welch's t-test (two-sided, p < 0.05) on their per-call times cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. Every tie in this sweep is at the top. Because the verdict is a significance test and not a ratio threshold, a lead of a few percent with tight samples is decisive: on Linux / GCC, Bool reused (3,305 vs the reflection writer's 3,190 MB/s) is a win of 1.04×, while a 10% lead with RSEs of 4.4% and 3.0% can still be a tie (see *Losses and ties*). Because the order follows MB/s, which credits each library its own output size, a library that is faster per call can still rank below one that writes more bytes. No single-value verdict in this sweep turns on that: on Double and Int64, Jsonifier and the reflection writer write the same bytes, so the MB/s order is also the per-call order.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 47 | 0 | 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 47 | 1 | 3 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| **Total** | **255** | **244** | **1** | **10** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,148 | 101 | 11.42× |
| Double | 494 | 153 | 3.24× |
| Int64 | 609 | 135 | 4.50× |
| String | 582 | 458 | 1.27× |
| Uint64 | 593 (n/c) | 137 | 4.32× |
| Apache Builds (minified) | 11,260 | 3,824 | 2.94× |
| Canada (minified) | 1,327 | 776 | 1.71× |
| Apache Builds (prettified) | 12,736 | 4,449 | 2.86× |
| Canada (prettified) | 2,694 (n/c) | 1,319 | 2.04× |
| CitmCatalog (minified) | 9,589 | 3,847 | 2.49× |
| CitmCatalog (prettified) | 3,932 | 1,750 | 2.25× |
| Discord (minified) | 10,645 | 4,112 | 2.59× |
| Discord (prettified) | 10,664 | 3,396 | 3.14× |
| Github Events (minified) | 10,543 | 5,178 | 2.04× |
| Google Maps Response (minified) | 9,711 | 3,221 | 3.02× |
| Github Events (prettified) | 11,088 | 2,988 | 3.71× |
| Google Maps Response (prettified) | 14,321 | 5,180 | 2.76× |
| Instruments (minified) | 14,949 | 3,776 | 3.96× |
| Instruments (prettified) | 13,864 | 4,182 | 3.32× |
| Marine IK Reverse (minified) | 991 | 665 | 1.49× |
| Marine IK Reverse (prettified) | 2,997 | 1,670 | 1.79× |
| Marine IK (minified) | 953 | 575 | 1.66× |
| Marine IK (prettified) | 2,973 (n/c) | 1,205 | 2.47× |
| Random (minified) | 3,627 | 2,862 | 1.27× |
| Random (prettified) | 3,821 | 2,011 | 1.90× |
| Twitter (minified) | 4,212 | 5,330 | 0.79× |
| Twitter (prettified) | 4,222 | 1,786 | 2.36× |
| Apache Builds Small (minified) | 9,112 | 3,643 | 2.50× |
| Canada Small (minified) | 1,822 | 1,157 | 1.57× |
| Apache Builds Small (prettified) | 10,026 | 3,894 | 2.57× |
| Canada Small (prettified) | 4,734 | 2,852 | 1.66× |
| CitmCatalog Small (minified) | 7,318 | 3,326 | 2.20× |
| CitmCatalog Small (prettified) | 8,305 | 4,031 | 2.06× |
| Discord Small (minified) | 9,387 | 3,474 | 2.70× |
| Discord Small (prettified) | 8,443 | 3,282 | 2.57× |
| Github Events Small (minified) | 10,182 | 4,351 | 2.34× |
| Google Maps Response Small (minified) | 9,317 | 3,413 | 2.73× |
| Github Events Small (prettified) | 8,039 | 3,632 | 2.21× |
| Google Maps Response Small (prettified) | 12,975 | 4,743 | 2.74× |
| Instruments Small (minified) | 14,061 | 3,129 | 4.49× |
| Instruments Small (prettified) | 10,316 | 3,047 | 3.39× |
| Marine IK Reverse Small (minified) | 1,449 | 884 | 1.64× |
| Marine IK Reverse Small (prettified) | 5,349 | 2,857 | 1.87× |
| Marine IK Small (minified) | 1,577 | 867 | 1.82× |
| Marine IK Small (prettified) | 5,411 | 2,984 | 1.81× |
| Random Small (minified) | 9,728 | 3,340 | 2.91× |
| Random Small (prettified) | 11,226 | 3,923 | 2.86× |
| Twitter Small (minified) | 10,461 | 4,843 | 2.16× |
| Twitter Small (prettified) | 8,772 | 3,870 | 2.27× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 9,869 | 1,895 | 8,468 | 1.17× |
| Prettify | 2,592 | 2,692 | 1,480 | 0.96× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,299 | 223 | 10.31× |
| Double | 370 | 305 | 1.21× |
| Int64 | 337 | 307 | 1.10× |
| String | 1,616 | 880 | 1.84× |
| Uint64 | 305 | 281 | 1.08× |
| Apache Builds (minified) | 15,561 | 5,289 | 2.94× |
| Canada (minified) | 1,905 | 962 | 1.98× |
| Apache Builds (prettified) | 16,559 | 5,364 | 3.09× |
| Canada (prettified) | 5,205 | 3,286 | 1.58× |
| CitmCatalog (minified) | 10,064 | 5,069 | 1.99× |
| CitmCatalog (prettified) | 14,007 | 5,274 | 2.66× |
| Discord (minified) | 12,658 | 5,049 | 2.51× |
| Discord (prettified) | 15,078 | 3,971 | 3.80× |
| Github Events (minified) | 14,980 | 4,924 | 3.04× |
| Google Maps Response (minified) | 11,781 | 3,879 | 3.04× |
| Github Events (prettified) | 15,247 | 4,469 | 3.41× |
| Google Maps Response (prettified) | 16,049 | 5,193 | 3.09× |
| Instruments (minified) | 16,501 | 4,788 | 3.45× |
| Instruments (prettified) | 18,613 | 4,835 | 3.85× |
| Marine IK Reverse (minified) | 1,348 | 649 | 2.08× |
| Marine IK Reverse (prettified) | 5,098 | 3,713 | 1.37× |
| Marine IK (minified) | 1,319 | 642 | 2.06× |
| Marine IK (prettified) | 4,761 | 2,977 | 1.60× |
| Random (minified) | 13,915 | 3,600 | 3.87× |
| Random (prettified) | 14,376 | 3,875 | 3.71× |
| Twitter (minified) | 16,888 | 6,091 | 2.77× |
| Twitter (prettified) | 16,608 | 4,143 | 4.01× |
| Apache Builds Small (minified) | 9,070 | 3,397 | 2.67× |
| Canada Small (minified) | 1,950 | 969 | 2.01× |
| Apache Builds Small (prettified) | 10,978 | 3,379 | 3.25× |
| Canada Small (prettified) | 5,289 | 3,358 | 1.57× |
| CitmCatalog Small (minified) | 7,604 | 2,857 | 2.66× |
| CitmCatalog Small (prettified) | 10,678 | 3,144 | 3.40× |
| Discord Small (minified) | 9,841 | 3,126 | 3.15× |
| Discord Small (prettified) | 11,223 | 2,574 | 4.36× |
| Github Events Small (minified) | 9,529 | 3,924 | 2.43× |
| Google Maps Response Small (minified) | 10,703 | 3,646 | 2.94× |
| Github Events Small (prettified) | 9,959 | 3,489 | 2.85× |
| Google Maps Response Small (prettified) | 17,036 | 4,839 | 3.52× |
| Instruments Small (minified) | 11,420 | 2,538 | 4.50× |
| Instruments Small (prettified) | 13,720 | 2,655 | 5.17× |
| Marine IK Reverse Small (minified) | 1,642 | 836 | 1.97× |
| Marine IK Reverse Small (prettified) | 5,511 | 3,495 | 1.58× |
| Marine IK Small (minified) | 1,681 | 819 | 2.05× |
| Marine IK Small (prettified) | 4,652 | 3,314 | 1.40× |
| Random Small (minified) | 12,713 | 3,567 | 3.56× |
| Random Small (prettified) | 14,205 | 3,473 | 4.09× |
| Twitter Small (minified) | 8,230 | 4,111 | 2.00× |
| Twitter Small (prettified) | 9,031 | 2,987 | 3.02× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 11,203 | 2,609 | 9,168 | 1.22× |
| Prettify | 4,364 | 3,903 | 2,316 | 1.12× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,799 | 199 | 259 | 6.93× |
| Double | 382 | 272 | 366 | 1.04× |
| Int64 | 367 | 316 | 366 | 1.00× |
| String | 1,577 | 929 | 1,451 | 1.09× |
| Uint64 | 339 | 297 | 343 | 0.99× |
| Apache Builds (minified) | 15,979 | 7,890 | 13,784 | 1.16× |
| Canada (minified) | 2,098 | 1,464 | 1,592 | 1.32× |
| Apache Builds (prettified) | 18,401 | 7,236 | 642 | 2.54× |
| Canada (prettified) | 6,100 | 3,306 | 445 | 1.85× |
| CitmCatalog (minified) | 11,752 | 6,292 | 12,476 | 0.94× |
| CitmCatalog (prettified) | 18,512 | 5,776 | 576 | 3.21× |
| Discord (minified) | 13,643 | 6,446 | 13,342 | 1.02× |
| Discord (prettified) | 18,498 | 5,769 | 564 | 3.21× |
| Github Events (minified) | 13,994 | 7,299 | 11,536 | 1.21× |
| Google Maps Response (minified) | 12,906 | 4,570 | 9,557 | 1.35× |
| Github Events (prettified) | 16,462 | 4,857 | 641 | 3.39× |
| Google Maps Response (prettified) | 19,888 | 6,626 | 550 | 3.00× |
| Instruments (minified) | 19,659 | 5,755 | 16,984 | 1.16× |
| Instruments (prettified) | 26,232 | 5,477 | 483 | 4.79× |
| Marine IK Reverse (minified) | 1,364 | 994 | 1,353 | 1.01× |
| Marine IK Reverse (prettified) | 6,345 | 3,707 | 290 | 1.71× |
| Marine IK (minified) | 1,348 | 952 | 1,342 | 1.00× |
| Marine IK (prettified) | 6,397 | 3,098 | 438 | 2.07× |
| Random (minified) | 14,441 | 4,557 | 11,516 | 1.25× |
| Random (prettified) | 17,974 | 4,845 | 538 | 3.71× |
| Twitter (minified) | 15,593 | 7,452 | 12,505 | 1.25× |
| Twitter (prettified) | 18,632 | 5,109 | 537 | 3.65× |
| Apache Builds Small (minified) | 9,930 | 4,511 | 7,425 | 1.34× |
| Canada Small (minified) | 2,031 | 1,543 | 1,740 | 1.17× |
| Apache Builds Small (prettified) | 11,925 | 4,215 | 537 | 2.83× |
| Canada Small (prettified) | 6,295 | 3,586 | 474 | 1.76× |
| CitmCatalog Small (minified) | 8,223 | 3,468 | 7,196 | 1.14× |
| CitmCatalog Small (prettified) | 16,604 | 2,771 | 518 | 5.99× |
| Discord Small (minified) | 11,160 | 4,136 | 7,854 | 1.42× |
| Discord Small (prettified) | 15,158 | 3,118 | 537 | 4.86× |
| Github Events Small (minified) | 8,505 | 5,278 | 6,708 | 1.27× |
| Google Maps Response Small (minified) | 11,206 | 3,855 | 8,666 | 1.29× |
| Github Events Small (prettified) | 11,425 | 3,749 | 523 | 3.05× |
| Google Maps Response Small (prettified) | 22,751 | 5,543 | 525 | 4.10× |
| Instruments Small (minified) | 13,953 | 3,952 | 10,677 | 1.31× |
| Instruments Small (prettified) | 24,218 | 2,999 | 524 | 8.08× |
| Marine IK Reverse Small (minified) | 1,496 | 1,215 | 1,947 | 0.77× |
| Marine IK Reverse Small (prettified) | 6,975 | 3,433 | 547 | 2.03× |
| Marine IK Small (minified) | 1,436 | 1,227 | 1,920 | 0.75× |
| Marine IK Small (prettified) | 6,490 | 3,691 | 560 | 1.76× |
| Random Small (minified) | 12,636 | 4,277 | 8,379 | 1.51× |
| Random Small (prettified) | 20,939 | 4,592 | 531 | 4.56× |
| Twitter Small (minified) | 8,557 | 5,655 | 7,208 | 1.19× |
| Twitter Small (prettified) | 12,107 | 3,567 | 557 | 3.39× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 10,349 | 2,533 | 9,320 | 1.11× |
| Prettify | 4,223 | 3,204 | 2,461 | 1.32× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,600 | 118 | 199 | 8.06× |
| Double | 290 | 161 | 304 | 0.95× |
| Int64 | 247 | 161 | 233 | 1.06× |
| String | 860 | 555 | 831 | 1.03× |
| Uint64 | 285 | 181 | 282 | 1.01× |
| Apache Builds (minified) | 12,790 | 5,173 | 10,258 | 1.25× |
| Canada (minified) | 2,398 | 1,789 | 2,140 | 1.12× |
| Apache Builds (prettified) | 14,108 | 4,397 | 392 | 3.21× |
| Canada (prettified) | 7,391 | 3,617 | 344 (n/c) | 2.04× |
| CitmCatalog (minified) | 8,774 | 2,079 | 7,811 | 1.12× |
| CitmCatalog (prettified) | 17,201 | 3,573 | 318 | 4.81× |
| Discord (minified) | 11,321 | 2,662 | 9,879 | 1.15× |
| Discord (prettified) | 14,090 | 2,352 | 303 | 5.99× |
| Github Events (minified) | 14,417 | 3,308 | 9,198 | 1.57× |
| Google Maps Response (minified) | 9,966 | 1,886 | 9,089 | 1.10× |
| Github Events (prettified) | 15,569 | 2,775 | 394 | 5.61× |
| Google Maps Response (prettified) | 19,041 | 2,961 | 342 | 6.43× |
| Instruments (minified) | 13,495 | 2,079 | 9,440 | 1.43× |
| Instruments (prettified) | 20,556 | 2,424 | 355 | 8.48× |
| Marine IK Reverse (minified) | 1,264 | 944 | 1,202 | 1.05× |
| Marine IK Reverse (prettified) | 6,184 | 3,301 | 290 (n/c) | 1.87× |
| Marine IK (minified) | 1,236 | 748 | 1,057 | 1.17× |
| Marine IK (prettified) | 6,755 | 2,710 | 324 | 2.49× |
| Random (minified) | 12,246 | 2,391 | 8,987 | 1.36× |
| Random (prettified) | 18,604 | 2,737 | 357 | 6.80× |
| Twitter (minified) | 13,319 | 3,923 | 11,357 | 1.17× |
| Twitter (prettified) | 16,748 | 4,019 | 433 | 4.17× |
| Apache Builds Small (minified) | 8,527 | 2,808 | 6,728 (n/c) | 1.27× |
| Canada Small (minified) | 2,357 | 1,493 | 2,115 | 1.11× |
| Apache Builds Small (prettified) | 10,349 | 3,901 | 369 | 2.65× |
| Canada Small (prettified) | 7,374 | 3,101 | 337 | 2.38× |
| CitmCatalog Small (minified) | 8,943 | 1,957 | 6,773 | 1.32× |
| CitmCatalog Small (prettified) | 16,135 | 2,669 | 347 | 6.05× |
| Discord Small (minified) | 11,497 | 2,736 | 9,258 | 1.24× |
| Discord Small (prettified) | 13,441 | 2,175 | 358 | 6.18× |
| Github Events Small (minified) | 11,419 | 2,662 | 10,401 | 1.10× |
| Google Maps Response Small (minified) | 9,502 | 1,766 | 8,699 | 1.09× |
| Github Events Small (prettified) | 13,201 | 2,883 | 396 | 4.58× |
| Google Maps Response Small (prettified) | 16,862 | 2,834 | 335 | 5.95× |
| Instruments Small (minified) | 13,308 | 1,680 | 9,142 | 1.46× |
| Instruments Small (prettified) | 20,832 | 2,392 | 345 | 8.71× |
| Marine IK Reverse Small (minified) | 2,111 | 1,094 | 2,076 | 1.02× |
| Marine IK Reverse Small (prettified) | 8,859 | 3,315 | 325 | 2.67× |
| Marine IK Small (minified) | 1,830 | 1,067 | 2,080 | 0.88× |
| Marine IK Small (prettified) | 9,085 | 3,130 | 324 | 2.90× |
| Random Small (minified) | 12,197 | 1,879 | 8,533 | 1.43× |
| Random Small (prettified) | 16,180 | 2,000 | 298 | 8.09× |
| Twitter Small (minified) | 11,866 | 3,049 | 9,259 | 1.28× |
| Twitter Small (prettified) | 13,601 | 3,272 | 416 | 4.16× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 6,636 | 1,712 | 6,734 | 0.99× |
| Prettify | 2,942 | 2,465 | 1,645 | 1.19× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,024 | 131 | 7.79× |
| Double | 325 | 214 | 1.52× |
| Int64 | 953 | 183 | 5.21× |
| String | 601 | 530 | 1.13× |
| Uint64 | 1,159 | 175 | 6.63× |
| Apache Builds (minified) | 11,326 | 3,556 | 3.18× |
| Canada (minified) | 2,952 | 1,879 | 1.57× |
| Apache Builds (prettified) | 12,076 | 3,593 | 3.36× |
| Canada (prettified) | 7,411 | 3,568 | 2.08× |
| CitmCatalog (minified) | 9,583 | 2,222 | 4.31× |
| CitmCatalog (prettified) | 14,542 | 4,217 | 3.45× |
| Discord (minified) | 11,777 | 3,395 | 3.47× |
| Discord (prettified) | 12,671 | 3,009 | 4.21× |
| Github Events (minified) | 14,193 | 4,776 | 2.97× |
| Google Maps Response (minified) | 9,047 | 2,366 | 3.82× |
| Github Events (prettified) | 13,553 | 4,023 | 3.37× |
| Google Maps Response (prettified) | 12,358 | 3,831 | 3.23× |
| Instruments (minified) | 11,841 | 2,342 | 5.06× |
| Instruments (prettified) | 15,166 | 3,234 | 4.69× |
| Marine IK Reverse (minified) | 1,335 | 823 | 1.62× |
| Marine IK Reverse (prettified) | 5,725 | 3,135 | 1.83× |
| Marine IK (minified) | 1,485 | 820 | 1.81× |
| Marine IK (prettified) | 5,115 | 2,793 | 1.83× |
| Random (minified) | 8,757 | 2,747 | 3.19× |
| Random (prettified) | 9,333 | 2,736 | 3.41× |
| Twitter (minified) | 14,090 | 4,516 | 3.12× |
| Twitter (prettified) | 14,898 | 4,145 | 3.59× |
| Apache Builds Small (minified) | 7,010 | 2,541 | 2.76× |
| Canada Small (minified) | 2,552 | 1,294 | 1.97× |
| Apache Builds Small (prettified) | 8,213 | 2,804 | 2.93× |
| Canada Small (prettified) | 7,060 | 3,367 | 2.10× |
| CitmCatalog Small (minified) | 8,139 | 1,959 | 4.15× |
| CitmCatalog Small (prettified) | 10,283 | 2,898 | 3.55× |
| Discord Small (minified) | 10,934 | 3,152 | 3.47× |
| Discord Small (prettified) | 11,302 | 3,032 | 3.73× |
| Github Events Small (minified) | 9,114 | 3,568 | 2.55× |
| Google Maps Response Small (minified) | 8,358 | 2,129 | 3.93× |
| Github Events Small (prettified) | 10,332 | 3,176 | 3.25× |
| Google Maps Response Small (prettified) | 13,649 | 3,197 | 4.27× |
| Instruments Small (minified) | 10,592 | 2,058 | 5.15× |
| Instruments Small (prettified) | 14,581 | 3,180 | 4.59× |
| Marine IK Reverse Small (minified) | 2,207 | 1,077 | 2.05× |
| Marine IK Reverse Small (prettified) | 5,707 | 3,248 | 1.76× |
| Marine IK Small (minified) | 2,130 | 1,047 | 2.03× |
| Marine IK Small (prettified) | 5,545 | 3,324 | 1.67× |
| Random Small (minified) | 10,165 | 2,786 | 3.65× |
| Random Small (prettified) | 11,165 | 3,125 | 3.57× |
| Twitter Small (minified) | 9,429 | 3,634 | 2.59× |
| Twitter Small (prettified) | 10,438 | 3,276 | 3.19× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 7,336 | 1,745 | 7,599 | 0.97× |
| Prettify | 3,392 | 2,533 | 1,629 | 1.34× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 244 of the 255 write tests, ties 1 and loses 10. Two losses are Minify on macOS, to simdjson On Demand: 7,624 vs 6,475 MB/s on macOS / GCC (0.85×) and 7,597 vs 7,373 MB/s on macOS / Clang (0.97×). The other eight are to the reflection writer. On Linux / GCC it wins minified CitmCatalog (12,537 vs 11,834 MB/s, 0.94×), minified Marine IK Reverse Small (1,985 vs 1,585 MB/s, 0.80×) and minified Marine IK Small (1,951 vs 1,456 MB/s, 0.75×); on macOS / GCC it wins Double (1,004 vs 849 MB/s, 0.85×), Int64 (2,507 vs 2,142 MB/s, 0.85×), minified CitmCatalog (8,950 vs 8,840 MB/s, 0.99×), minified Marine IK Reverse (1,306 vs 1,220 MB/s, 0.93×) and minified CitmCatalog Small (9,475 vs 8,581 MB/s, 0.91×). The tie is Uint64 on macOS / GCC (2,808 vs 2,797 MB/s).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 44 | 1 | 6 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| **Total** | **255** | **244** | **1** | **10** |

#### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,627 | 529 | 3.07× |
| Double | 812 | 441 | 1.84× |
| Int64 | 1,521 | 727 | 2.09× |
| String | 5,135 | 1,708 | 3.01× |
| Uint64 | 1,574 | 648 | 2.43× |
| Apache Builds (minified) | 11,988 | 4,514 | 2.66× |
| Canada (minified) | 1,674 | 1,094 | 1.53× |
| Apache Builds (prettified) | 12,983 | 4,559 | 2.85× |
| Canada (prettified) | 4,901 | 3,095 | 1.58× |
| CitmCatalog (minified) | 9,543 | 4,204 | 2.27× |
| CitmCatalog (prettified) | 12,372 | 6,518 | 1.90× |
| Discord (minified) | 10,973 | 4,321 | 2.54× |
| Discord (prettified) | 12,253 | 3,993 | 3.07× |
| Github Events (minified) | 12,576 | 5,760 | 2.18× |
| Google Maps Response (minified) | 10,258 | 3,701 | 2.77× |
| Github Events (prettified) | 11,862 | 3,475 | 3.41× |
| Google Maps Response (prettified) | 15,642 (n/c) | 5,626 | 2.78× |
| Instruments (minified) | 15,280 | 4,549 | 3.36× |
| Instruments (prettified) | 14,584 | 4,757 | 3.07× |
| Marine IK Reverse (minified) | 1,155 | 678 | 1.70× |
| Marine IK Reverse (prettified) | 4,788 | 3,277 | 1.46× |
| Marine IK (minified) | 1,206 | 731 | 1.65× |
| Marine IK (prettified) | 4,492 | 3,205 | 1.40× |
| Random (minified) | 10,478 | 3,270 | 3.20× |
| Random (prettified) | 12,978 | 4,377 | 2.96× |
| Twitter (minified) | 15,185 | 5,902 | 2.57× |
| Twitter (prettified) | 13,027 | 3,389 | 3.84× |
| Apache Builds Small (minified) | 8,993 | 4,152 | 2.17× |
| Canada Small (minified) | 1,863 | 1,235 | 1.51× |
| Apache Builds Small (prettified) | 11,310 | 4,582 | 2.47× |
| Canada Small (prettified) | 5,037 | 3,135 | 1.61× |
| CitmCatalog Small (minified) | 10,161 | 4,159 | 2.44× |
| CitmCatalog Small (prettified) | 10,006 | 5,122 | 1.95× |
| Discord Small (minified) | 10,832 | 4,200 | 2.58× |
| Discord Small (prettified) | 8,876 | 3,722 | 2.38× |
| Github Events Small (minified) | 14,366 | 5,059 | 2.84× |
| Google Maps Response Small (minified) | 9,304 | 3,992 | 2.33× |
| Github Events Small (prettified) | 12,548 | 4,925 | 2.55× |
| Google Maps Response Small (prettified) | 15,084 | 6,288 | 2.40× |
| Instruments Small (minified) | 16,942 | 3,365 (n/c) | 5.03× |
| Instruments Small (prettified) | 12,380 | 3,034 | 4.08× |
| Marine IK Reverse Small (minified) | 1,525 | 939 | 1.62× |
| Marine IK Reverse Small (prettified) | 5,639 | 3,300 | 1.71× |
| Marine IK Small (minified) | 1,634 | 952 | 1.72× |
| Marine IK Small (prettified) | 5,628 | 3,079 (n/c) | 1.83× |
| Random Small (minified) | 13,047 | 3,860 | 3.38× |
| Random Small (prettified) | 13,716 | 5,299 | 2.59× |
| Twitter Small (minified) | 12,148 | 5,811 | 2.09× |
| Twitter Small (prettified) | 10,263 | 4,554 | 2.25× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 10,197 | 1,907 | 8,927 | 1.14× |
| Prettify | 3,484 | 2,519 | 1,291 | 1.38× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,588 | 973 | 4.71× |
| Double | 938 | 674 | 1.39× |
| Int64 | 2,403 | 1,077 | 2.23× |
| String | 5,392 | 2,180 | 2.47× |
| Uint64 | 2,372 | 955 | 2.48× |
| Apache Builds (minified) | 15,514 | 5,981 | 2.59× |
| Canada (minified) | 1,908 | 995 | 1.92× |
| Apache Builds (prettified) | 17,310 | 5,860 | 2.95× |
| Canada (prettified) | 5,201 | 3,948 | 1.32× |
| CitmCatalog (minified) | 10,054 | 5,827 | 1.73× |
| CitmCatalog (prettified) | 14,021 | 7,064 | 1.98× |
| Discord (minified) | 12,698 | 5,554 | 2.29× |
| Discord (prettified) | 15,461 | 4,564 | 3.39× |
| Github Events (minified) | 15,173 | 5,635 | 2.69× |
| Google Maps Response (minified) | 12,996 | 4,393 | 2.96× |
| Github Events (prettified) | 15,462 | 4,979 | 3.11× |
| Google Maps Response (prettified) | 16,398 | 5,783 | 2.84× |
| Instruments (minified) | 16,639 | 5,184 | 3.21× |
| Instruments (prettified) | 18,816 | 5,542 | 3.40× |
| Marine IK Reverse (minified) | 1,342 | 656 | 2.05× |
| Marine IK Reverse (prettified) | 5,085 | 4,085 | 1.24× |
| Marine IK (minified) | 1,330 | 664 | 2.00× |
| Marine IK (prettified) | 4,833 | 4,091 | 1.18× |
| Random (minified) | 13,924 | 3,953 | 3.52× |
| Random (prettified) | 14,511 | 4,441 | 3.27× |
| Twitter (minified) | 16,928 | 6,748 | 2.51× |
| Twitter (prettified) | 16,633 | 4,777 | 3.48× |
| Apache Builds Small (minified) | 10,667 | 3,622 | 2.95× |
| Canada Small (minified) | 1,995 | 1,008 | 1.98× |
| Apache Builds Small (prettified) | 12,483 | 3,695 | 3.38× |
| Canada Small (prettified) | 4,410 | 3,735 | 1.18× |
| CitmCatalog Small (minified) | 8,752 | 3,130 | 2.80× |
| CitmCatalog Small (prettified) | 11,653 | 3,556 | 3.28× |
| Discord Small (minified) | 11,366 | 3,609 | 3.15× |
| Discord Small (prettified) | 12,290 | 2,828 | 4.35× |
| Github Events Small (minified) | 12,873 | 4,196 | 3.07× |
| Google Maps Response Small (minified) | 11,970 | 3,911 | 3.06× |
| Github Events Small (prettified) | 12,606 | 3,891 | 3.24× |
| Google Maps Response Small (prettified) | 17,871 | 5,470 | 3.27× |
| Instruments Small (minified) | 12,930 | 2,777 | 4.66× |
| Instruments Small (prettified) | 14,981 | 2,956 | 5.07× |
| Marine IK Reverse Small (minified) | 1,693 | 877 | 1.93× |
| Marine IK Reverse Small (prettified) | 5,626 | 3,715 | 1.51× |
| Marine IK Small (minified) | 1,704 | 862 | 1.98× |
| Marine IK Small (prettified) | 4,666 | 3,660 | 1.27× |
| Random Small (minified) | 14,394 | 4,241 | 3.39× |
| Random Small (prettified) | 16,467 | 3,844 | 4.28× |
| Twitter Small (minified) | 9,521 | 4,243 | 2.24× |
| Twitter Small (prettified) | 10,354 | 3,584 | 2.89× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 11,115 | 2,624 | 9,102 | 1.22× |
| Prettify | 4,416 | 3,852 | 2,295 | 1.15× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,305 | 685 | 3,190 | 1.04× |
| Double | 961 | 722 | 845 | 1.14× |
| Int64 | 3,121 | 1,046 | 2,842 | 1.10× |
| String | 4,752 | 2,798 | 4,445 | 1.07× |
| Uint64 | 3,158 | 954 | 3,035 | 1.04× |
| Apache Builds (minified) | 16,150 | 9,377 | 13,912 | 1.16× |
| Canada (minified) | 2,086 | 1,548 | 1,617 | 1.29× |
| Apache Builds (prettified) | 18,577 | 8,249 | 628 | 2.25× |
| Canada (prettified) | 6,108 | 3,954 | 444 | 1.54× |
| CitmCatalog (minified) | 11,834 | 7,262 | 12,537 | 0.94× |
| CitmCatalog (prettified) | 18,557 | 7,923 | 580 | 2.34× |
| Discord (minified) | 13,941 | 7,434 | 13,721 | 1.02× |
| Discord (prettified) | 18,757 | 7,086 | 562 | 2.65× |
| Github Events (minified) | 14,179 | 8,626 | 11,655 | 1.22× |
| Google Maps Response (minified) | 13,888 | 5,068 | 10,297 | 1.35× |
| Github Events (prettified) | 16,840 | 5,400 | 645 | 3.12× |
| Google Maps Response (prettified) | 20,665 | 7,441 | 544 | 2.78× |
| Instruments (minified) | 19,628 | 6,838 | 16,840 | 1.17× |
| Instruments (prettified) | 25,871 | 6,298 | 487 | 4.11× |
| Marine IK Reverse (minified) | 1,364 | 990 | 1,356 | 1.01× |
| Marine IK Reverse (prettified) | 6,358 | 4,075 | 326 | 1.56× |
| Marine IK (minified) | 1,343 | 1,003 | 1,331 | 1.01× |
| Marine IK (prettified) | 6,387 | 4,123 | 373 | 1.55× |
| Random (minified) | 14,455 | 5,233 | 11,614 | 1.24× |
| Random (prettified) | 17,642 | 5,746 | 547 | 3.07× |
| Twitter (minified) | 15,660 | 8,472 | 12,524 | 1.25× |
| Twitter (prettified) | 18,630 | 6,471 | 536 | 2.88× |
| Apache Builds Small (minified) | 11,722 | 5,411 | 8,442 | 1.39× |
| Canada Small (minified) | 2,103 | 1,716 | 1,784 | 1.18× |
| Apache Builds Small (prettified) | 15,524 | 4,897 | 532 | 3.17× |
| Canada Small (prettified) | 6,514 | 3,906 | 470 | 1.67× |
| CitmCatalog Small (minified) | 9,071 | 4,087 | 7,821 | 1.16× |
| CitmCatalog Small (prettified) | 17,641 | 3,378 | 514 | 5.22× |
| Discord Small (minified) | 11,771 | 4,932 | 9,260 | 1.27× |
| Discord Small (prettified) | 16,952 | 3,637 | 528 | 4.66× |
| Github Events Small (minified) | 10,402 | 5,376 | 7,614 | 1.37× |
| Google Maps Response Small (minified) | 12,123 | 4,227 | 9,029 | 1.34× |
| Github Events Small (prettified) | 13,817 | 4,497 | 524 | 3.07× |
| Google Maps Response Small (prettified) | 26,139 | 6,749 | 522 | 3.87× |
| Instruments Small (minified) | 16,020 | 4,587 | 11,760 | 1.36× |
| Instruments Small (prettified) | 28,498 | 3,867 | 523 | 7.37× |
| Marine IK Reverse Small (minified) | 1,585 | 1,281 | 1,985 | 0.80× |
| Marine IK Reverse Small (prettified) | 7,056 | 3,700 | 546 | 1.91× |
| Marine IK Small (minified) | 1,456 | 1,313 | 1,951 | 0.75× |
| Marine IK Small (prettified) | 6,538 | 3,904 | 558 | 1.67× |
| Random Small (minified) | 14,938 | 5,153 | 9,354 | 1.60× |
| Random Small (prettified) | 23,755 | 5,488 | 529 | 4.33× |
| Twitter Small (minified) | 9,904 | 6,620 | 8,007 | 1.24× |
| Twitter Small (prettified) | 14,085 | 4,437 | 562 | 3.17× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 10,329 | 2,535 | 9,371 | 1.10× |
| Prettify | 4,245 | 3,225 | 2,490 | 1.32× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,745 (n/c) | 706 | 2,441 | 1.12× |
| Double | 849 | 582 | 1,004 | 0.85× |
| Int64 | 2,142 | 1,141 | 2,507 | 0.85× |
| String | 6,520 | 2,576 | 5,613 | 1.16× |
| Uint64 | 2,808 | 1,093 | 2,797 | 1.00× |
| Apache Builds (minified) | 13,258 | 5,842 | 10,556 | 1.26× |
| Canada (minified) | 2,394 | 1,884 | 2,150 | 1.11× |
| Apache Builds (prettified) | 14,473 | 5,324 | 392 | 2.72× |
| Canada (prettified) | 5,791 | 4,121 | 304 | 1.41× |
| CitmCatalog (minified) | 8,840 | 2,357 | 8,950 | 0.99× |
| CitmCatalog (prettified) | 18,502 | 3,484 (n/c) | 276 | 5.31× |
| Discord (minified) | 12,218 | 3,072 | 10,509 | 1.16× |
| Discord (prettified) | 14,006 | 2,797 | 344 | 5.01× |
| Github Events (minified) | 13,192 | 4,028 | 11,715 | 1.13× |
| Google Maps Response (minified) | 10,308 | 2,012 | 9,323 | 1.11× |
| Github Events (prettified) | 15,378 | 3,979 | 397 (n/c) | 3.86× |
| Google Maps Response (prettified) | 21,108 | 3,317 | 340 | 6.36× |
| Instruments (minified) | 13,982 | 2,260 | 9,678 | 1.44× |
| Instruments (prettified) | 20,990 | 2,677 | 321 | 7.84× |
| Marine IK Reverse (minified) | 1,220 | 818 | 1,306 | 0.93× |
| Marine IK Reverse (prettified) | 6,339 | 2,970 | 289 | 2.13× |
| Marine IK (minified) | 1,202 (n/c) | 756 | 1,083 | 1.11× |
| Marine IK (prettified) | 6,637 | 3,065 | 326 | 2.17× |
| Random (minified) | 12,379 | 2,287 | 8,776 | 1.41× |
| Random (prettified) | 18,581 | 2,967 | 357 | 6.26× |
| Twitter (minified) | 14,978 | 4,528 | 12,237 | 1.22× |
| Twitter (prettified) | 16,878 | 4,642 | 437 | 3.64× |
| Apache Builds Small (minified) | 9,451 | 3,126 | 7,759 | 1.22× |
| Canada Small (minified) | 2,386 | 1,987 | 2,143 | 1.11× |
| Apache Builds Small (prettified) | 11,594 | 4,611 | 374 | 2.51× |
| Canada Small (prettified) | 6,730 | 3,682 | 337 | 1.83× |
| CitmCatalog Small (minified) | 8,581 | 2,177 | 9,475 | 0.91× |
| CitmCatalog Small (prettified) | 17,988 | 2,879 | 344 | 6.25× |
| Discord Small (minified) | 12,814 | 2,956 | 10,285 | 1.25× |
| Discord Small (prettified) | 14,122 | 2,369 | 359 | 5.96× |
| Github Events Small (minified) | 14,170 | 3,734 | 12,072 | 1.17× |
| Google Maps Response Small (minified) | 10,177 | 1,978 | 9,073 | 1.12× |
| Github Events Small (prettified) | 14,584 (n/c) | 3,403 | 401 | 4.29× |
| Google Maps Response Small (prettified) | 21,089 | 3,176 | 334 | 6.64× |
| Instruments Small (minified) | 14,378 | 2,258 | 9,629 | 1.49× |
| Instruments Small (prettified) | 22,510 | 2,607 | 345 | 8.63× |
| Marine IK Reverse Small (minified) | 2,134 | 1,154 | 2,113 | 1.01× |
| Marine IK Reverse Small (prettified) | 9,524 | 3,775 | 324 | 2.52× |
| Marine IK Small (minified) | 2,155 | 1,165 | 2,120 | 1.02× |
| Marine IK Small (prettified) | 9,848 | 3,802 | 290 | 2.59× |
| Random Small (minified) | 11,021 | 2,152 | 7,641 | 1.44× |
| Random Small (prettified) | 14,809 | 2,723 | 282 | 5.44× |
| Twitter Small (minified) | 13,278 | 3,335 | 10,408 | 1.28× |
| Twitter Small (prettified) | 14,328 | 4,046 | 424 | 3.54× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 6,475 | 1,287 | 7,624 | 0.85× |
| Prettify | 3,221 | 2,469 | 1,659 | 1.30× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,267 | 754 | 1.68× |
| Double | 953 | 702 | 1.36× |
| Int64 | 1,546 | 797 | 1.94× |
| String | 3,524 | 2,050 | 1.72× |
| Uint64 | 1,419 | 829 | 1.71× |
| Apache Builds (minified) | 11,820 | 4,057 | 2.91× |
| Canada (minified) | 2,957 | 1,981 | 1.49× |
| Apache Builds (prettified) | 12,247 | 3,975 | 3.08× |
| Canada (prettified) | 7,390 | 4,036 | 1.83× |
| CitmCatalog (minified) | 9,526 | 2,353 | 4.05× |
| CitmCatalog (prettified) | 14,695 | 4,935 | 2.98× |
| Discord (minified) | 12,362 | 3,922 | 3.15× |
| Discord (prettified) | 12,967 | 3,450 | 3.76× |
| Github Events (minified) | 15,145 | 6,016 | 2.52× |
| Google Maps Response (minified) | 9,354 | 2,631 | 3.56× |
| Github Events (prettified) | 14,779 | 5,244 | 2.82× |
| Google Maps Response (prettified) | 13,476 | 4,394 | 3.07× |
| Instruments (minified) | 12,127 | 2,547 | 4.76× |
| Instruments (prettified) | 15,325 | 3,672 | 4.17× |
| Marine IK Reverse (minified) | 1,422 | 840 | 1.69× |
| Marine IK Reverse (prettified) | 5,735 | 3,279 | 1.75× |
| Marine IK (minified) | 1,482 | 824 | 1.80× |
| Marine IK (prettified) | 5,287 (n/c) | 3,203 | 1.65× |
| Random (minified) | 8,374 | 2,970 | 2.82× |
| Random (prettified) | 10,236 | 2,803 | 3.65× |
| Twitter (minified) | 14,192 | 5,029 | 2.82× |
| Twitter (prettified) | 15,122 | 4,803 | 3.15× |
| Apache Builds Small (minified) | 7,484 (n/c) | 3,523 | 2.12× |
| Canada Small (minified) | 2,295 | 1,451 | 1.58× |
| Apache Builds Small (prettified) | 8,918 | 3,638 | 2.45× |
| Canada Small (prettified) | 7,193 | 3,906 | 1.84× |
| CitmCatalog Small (minified) | 9,308 | 2,182 | 4.27× |
| CitmCatalog Small (prettified) | 12,970 | 3,254 | 3.99× |
| Discord Small (minified) | 11,923 | 4,010 | 2.97× |
| Discord Small (prettified) | 12,472 | 3,596 | 3.47× |
| Github Events Small (minified) | 12,367 | 4,874 | 2.54× |
| Google Maps Response Small (minified) | 9,135 | 2,517 | 3.63× |
| Github Events Small (prettified) | 14,128 | 4,401 | 3.21× |
| Google Maps Response Small (prettified) | 14,449 | 4,333 | 3.33× |
| Instruments Small (minified) | 12,052 | 2,349 | 5.13× |
| Instruments Small (prettified) | 12,600 | 3,730 | 3.38× |
| Marine IK Reverse Small (minified) | 2,280 | 1,169 | 1.95× |
| Marine IK Reverse Small (prettified) | 6,040 | 3,940 | 1.53× |
| Marine IK Small (minified) | 2,245 | 1,166 | 1.92× |
| Marine IK Small (prettified) | 6,100 | 3,941 | 1.55× |
| Random Small (minified) | 11,106 | 3,404 | 3.26× |
| Random Small (prettified) | 13,482 | 3,906 | 3.45× |
| Twitter Small (minified) | 12,274 | 4,480 | 2.74× |
| Twitter Small (prettified) | 12,467 | 4,423 | 2.82× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 7,373 | 1,745 | 7,597 | 0.97× |
| Prettify | 3,411 | 2,517 | 1,642 | 1.36× |

### Losses and ties

The ten freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 5,330 vs Jsonifier's 4,212 MB/s (0.79×). Reused, Jsonifier wins by 2.57× (15,185 vs 5,902).
- **Prettify, Windows / MSVC.** Glaze runs 2,692 vs 2,592 MB/s (0.96×), with simdjson On Demand at 1,480. All three converged. Reused, Jsonifier wins by 1.38× (3,484 vs 2,519).
- **Minify, macOS / GCC.** simdjson On Demand runs 6,734 vs 6,636 MB/s (0.99×), with Glaze at 1,712. It also wins reused, 7,624 vs 6,475 MB/s (0.85×).
- **Minify, macOS / Clang.** simdjson On Demand runs 7,599 vs 7,336 MB/s (0.97×), with Glaze at 1,745. It also wins reused, 7,597 vs 7,373 MB/s (0.97×).
- **Uint64, Linux / GCC.** The reflection writer runs 343 vs 339 MB/s (0.99×), with Glaze at 297. Reused, Jsonifier wins by 1.04× (3,158 vs 3,035).
- **CitmCatalog (minified), Linux / GCC.** The reflection writer runs 12,476 vs 11,752 MB/s (0.94×), with Glaze at 6,292. It also wins reused, 12,537 vs 11,834 MB/s (0.94×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,947 vs 1,496 MB/s (0.77×), with Glaze at 1,215. It also wins reused, 1,985 vs 1,585 MB/s (0.80×).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,920 vs 1,436 MB/s (0.75×), with Glaze at 1,227. It also wins reused, 1,951 vs 1,456 MB/s (0.75×).
- **Double, macOS / GCC.** The reflection writer runs 304 vs 290 MB/s (0.95×), with Glaze at 161, and with the same 1,997 bytes it is faster per call as well (6,267 vs 6,571 ns). It also wins reused, 1,004 vs 849 MB/s (0.85×).
- **Marine IK Small (minified), macOS / GCC.** The reflection writer runs 2,080 vs 1,830 MB/s (0.88×), with Glaze at 1,067. Reused, Jsonifier wins, 2,155 vs 2,120 MB/s.

The one freshly allocated tie is minified Github Events Small on macOS / GCC, against the reflection writer: 11,419 vs 10,401 MB/s, with RSEs of 4.4% and 3.0%. Reused, it becomes a win (14,170 vs 12,072 MB/s).

Five reused non-wins have no freshly allocated counterpart, all on macOS / GCC against the reflection writer. Int64 wins freshly allocated by 1.06× (247 vs 233 MB/s) and loses reused (2,507 vs 2,142 MB/s). Minified CitmCatalog (8,774 vs 7,811), Marine IK Reverse (1,264 vs 1,202) and CitmCatalog Small (8,943 vs 6,773) win freshly allocated and lose reused. Uint64 wins freshly allocated (285 vs 282 MB/s) and ties reused (2,808 vs 2,797 MB/s).

Freshly allocated, the reflection writer takes one full-size document, minified CitmCatalog on Linux / GCC; reused it takes that one plus minified CitmCatalog and Marine IK Reverse on macOS / GCC. Every other full-size document is ahead of it. Freshly allocated, minified documents lead it by up to 1.57× and prettified documents by 14–58×; reused, by up to 1.44× and 14–67×. On the Small documents Jsonifier beats the reflection writer on 40 of the 44 tests freshly allocated, with 1 tie and 3 losses, and on 41 reused, with no ties and 3 losses. Minified Marine IK Small and Marine IK Reverse Small on Linux / GCC lose in both runs. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on the three x86 builds in both runs, by 1.11–1.22× freshly allocated and 1.10–1.22× reused over simdjson On Demand, the next-fastest library on Minify everywhere; on macOS On Demand wins it (0.99× and 0.97× freshly allocated, 0.85× and 0.97× reused). It wins Prettify on all five reused, by 1.15–1.38×, and on four of five freshly allocated, by 1.12–1.34×; the exception is Windows / MSVC, where Glaze runs 2,692 vs 2,592 MB/s (0.96×). Glaze is the next-fastest library on Prettify in every case.

## AVX-512 builds

The results above come from AVX2 and NEON builds. The same 51 write tests also ran on three AVX-512 builds, Linux / Clang, Linux / GCC and Windows / MSVC, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor. As on AVX2, simdjson's reflection writer runs only on the Linux / GCC build.

Freshly allocated, Jsonifier was fastest in 149 of the 153 write tests, tied 2 and lost 2. The two losses are the same two that it loses on the AVX2 Windows / MSVC build: minified Twitter (Glaze 4,792 vs 3,571 MB/s, 0.75×) and Prettify (Glaze 2,926 vs 2,429 MB/s, 0.83×, with simdjson On Demand at 1,373). The two ties are against the reflection writer on Linux / GCC: Int64 (333 vs 333 MB/s) and minified CitmCatalog (9,513 vs 9,490 MB/s). Reused, it was fastest in 150 and lost 3: Minify on Linux / Clang to simdjson On Demand (24,702 vs 22,840 MB/s, 0.92×), and Int64 (2,433 vs 2,176 MB/s, 0.89×) and Uint64 (2,222 vs 2,139 MB/s, 0.96×) on Linux / GCC to the reflection writer. Both Glaze losses become wins reused: minified Twitter by 2.53× (13,602 vs 5,367 MB/s) and Prettify by 1.56× (4,523 vs 2,908). Windows / MSVC wins all 51 tests reused. Freshly allocated, prettified full-size documents lead Glaze by 1.44–8.95× and prettified Small documents by 1.44–6.31×; reused, by 1.34–8.49× and 1.38–6.32×. On Linux / GCC, freshly allocated prettified documents lead the reflection writer by 12.91–63.13× full-size and 11.70–48.70× Small. Minify leads Glaze by 8.00× on Linux / Clang, 8.36× on Linux / GCC and 13.24× on Windows / MSVC freshly allocated, and simdjson On Demand, the next-fastest library on Minify, by 1.05×, 1.21× and 1.09×.

### AVX-512 method

These results come from the Json-Performance branch `benchmarking-avx512-main`, with the same commits as above (Jsonifier [a1328b5](https://github.com/nihilai-collective/jsonifier/commit/a1328b5) [5], Glaze [283c9664](https://github.com/stephenberry/glaze/commit/283c9664) [6], simdjson [4ba1cb3ae](https://github.com/simdjson/simdjson/commit/4ba1cb3ae) [7], BenchmarkSuite [ff0f871](https://github.com/nihilai-collective/benchmarksuite/commit/ff0f871) [8]), run November 27, 2026. The tests, sampling, ranking and convergence limits (RSE under 5%, mean shift under 2.5%) are the same as for the x86 builds above.

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | AMD EPYC 9V45 (cloud VM) | Linux 7.0.0-1020-azure | AVX-512 | AVX2 / SSE4.1 | icelake |
| Linux / GCC 16.2 | AMD EPYC 9V45 (cloud VM) | Linux 7.0.0-1020-azure | AVX-512 | AVX2 / SSE4.1 | icelake |
| Windows / MSVC 19.52 | AMD EPYC 9V45 (cloud VM) | Windows 10.0.26100 | AVX-512 | AVX2 / SSE4.1 | icelake |

Glaze uses AVX512BW for UTF-8 validation and structural skipping on these builds, but its string escaper stays on AVX2 and its float writer on SSE4.1. The runners are shared virtual machines, and the CPU and operating-system images differ from the AVX2 builds, so an AVX-512 result here cannot be compared with the matching AVX2 result to isolate the instruction set. Each comparison below is between libraries within one build.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 49 | 2 | 0 |
| **Total** | **153** | **149** | **2** | **2** |

### Windows / MSVC 19.52 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 847 | 85 | 10.01× |
| Double | 465 | 135 | 3.44× |
| Int64 | 584 | 122 | 4.79× |
| String | 490 | 402 | 1.22× |
| Uint64 | 608 | 121 | 5.02× |
| Apache Builds (minified) | 11,465 | 4,502 | 2.55× |
| Canada (minified) | 1,285 | 688 | 1.87× |
| Apache Builds (prettified) | 10,346 | 3,855 | 2.68× |
| Canada (prettified) | 2,768 | 1,041 | 2.66× |
| CitmCatalog (minified) | 8,763 | 3,185 | 2.75× |
| CitmCatalog (prettified) | 3,712 | 1,971 | 1.88× |
| Discord (minified) | 10,721 | 2,937 | 3.65× |
| Discord (prettified) | 10,980 | 3,115 | 3.52× |
| Github Events (minified) | 10,778 | 4,482 | 2.40× |
| Google Maps Response (minified) | 7,144 | 2,769 | 2.58× |
| Github Events (prettified) | 10,296 | 4,264 | 2.41× |
| Google Maps Response (prettified) | 11,144 | 3,904 | 2.85× |
| Instruments (minified) | 14,629 | 2,948 | 4.96× |
| Instruments (prettified) | 13,628 | 3,579 | 3.81× |
| Marine IK Reverse (minified) | 979 | 484 | 2.02× |
| Marine IK Reverse (prettified) | 2,854 | 1,463 | 1.95× |
| Marine IK (minified) | 969 | 483 | 2.01× |
| Marine IK (prettified) | 2,823 | 1,020 | 2.77× |
| Random (minified) | 3,144 | 2,568 | 1.22× |
| Random (prettified) | 3,421 | 1,994 | 1.72× |
| Twitter (minified) | 3,571 | 4,792 | 0.75× |
| Twitter (prettified) | 3,478 | 2,408 | 1.44× |
| Apache Builds Small (minified) | 8,411 | 3,326 | 2.53× |
| Canada Small (minified) | 1,677 | 886 | 1.89× |
| Apache Builds Small (prettified) | 9,270 | 3,326 | 2.79× |
| Canada Small (prettified) | 4,384 | 2,377 | 1.84× |
| CitmCatalog Small (minified) | 7,963 | 2,315 | 3.44× |
| CitmCatalog Small (prettified) | 7,720 | 3,671 | 2.10× |
| Discord Small (minified) | 8,935 | 2,881 | 3.10× |
| Discord Small (prettified) | 7,087 | 2,977 | 2.38× |
| Github Events Small (minified) | 9,874 | 3,402 | 2.90× |
| Google Maps Response Small (minified) | 7,191 | 2,670 | 2.69× |
| Github Events Small (prettified) | 8,318 | 3,308 | 2.51× |
| Google Maps Response Small (prettified) | 10,495 | 3,559 | 2.95× |
| Instruments Small (minified) | 12,257 | 2,688 | 4.56× |
| Instruments Small (prettified) | 10,571 | 3,418 | 3.09× |
| Marine IK Reverse Small (minified) | 1,470 | 721 | 2.04× |
| Marine IK Reverse Small (prettified) | 5,405 | 2,767 | 1.95× |
| Marine IK Small (minified) | 1,436 | 723 | 1.99× |
| Marine IK Small (prettified) | 5,159 | 2,834 | 1.82× |
| Random Small (minified) | 8,817 | 2,761 | 3.19× |
| Random Small (prettified) | 10,634 | 3,057 | 3.48× |
| Twitter Small (minified) | 8,579 | 4,110 | 2.09× |
| Twitter Small (prettified) | 7,189 | 3,335 | 2.16× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 22,082 | 1,668 | 20,311 | 1.09× |
| Prettify | 2,429 | 2,926 | 1,373 | 0.83× |

### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,225 | 208 | 10.71× |
| Double | 368 | 288 | 1.28× |
| Int64 | 338 | 293 | 1.15× |
| String | 1,653 | 926 | 1.79× |
| Uint64 | 323 | 260 | 1.24× |
| Apache Builds (minified) | 19,087 | 5,586 | 3.42× |
| Canada (minified) | 2,039 | 1,032 | 1.98× |
| Apache Builds (prettified) | 19,485 | 6,252 | 3.12× |
| Canada (prettified) | 5,750 | 3,343 | 1.72× |
| CitmCatalog (minified) | 10,411 | 3,743 | 2.78× |
| CitmCatalog (prettified) | 17,255 | 5,385 | 3.20× |
| Discord (minified) | 15,234 | 3,923 | 3.88× |
| Discord (prettified) | 17,120 | 3,459 | 4.95× |
| Github Events (minified) | 16,066 | 5,981 | 2.69× |
| Google Maps Response (minified) | 12,199 | 3,136 | 3.89× |
| Github Events (prettified) | 16,542 | 5,169 | 3.20× |
| Google Maps Response (prettified) | 17,813 | 5,326 | 3.34× |
| Instruments (minified) | 16,656 | 3,880 | 4.29× |
| Instruments (prettified) | 21,538 | 4,119 | 5.23× |
| Marine IK Reverse (minified) | 1,359 | 671 | 2.02× |
| Marine IK Reverse (prettified) | 4,928 | 3,173 | 1.55× |
| Marine IK (minified) | 1,430 | 628 | 2.28× |
| Marine IK (prettified) | 5,034 | 3,361 | 1.50× |
| Random (minified) | 13,260 | 2,971 | 4.46× |
| Random (prettified) | 16,985 | 3,579 | 4.75× |
| Twitter (minified) | 17,622 | 5,869 | 3.00× |
| Twitter (prettified) | 16,004 | 4,734 | 3.38× |
| Apache Builds Small (minified) | 11,594 | 4,007 | 2.89× |
| Canada Small (minified) | 2,114 | 1,051 | 2.01× |
| Apache Builds Small (prettified) | 13,150 | 4,425 | 2.97× |
| Canada Small (prettified) | 5,791 | 3,428 | 1.69× |
| CitmCatalog Small (minified) | 10,748 | 2,744 | 3.92× |
| CitmCatalog Small (prettified) | 12,528 | 4,385 | 2.86× |
| Discord Small (minified) | 12,190 | 3,893 | 3.13× |
| Discord Small (prettified) | 14,154 | 3,528 | 4.01× |
| Github Events Small (minified) | 14,934 | 4,622 | 3.23× |
| Google Maps Response Small (minified) | 11,699 | 3,031 | 3.86× |
| Github Events Small (prettified) | 15,624 | 4,456 | 3.51× |
| Google Maps Response Small (prettified) | 17,753 | 5,339 | 3.32× |
| Instruments Small (minified) | 15,052 | 3,963 | 3.80× |
| Instruments Small (prettified) | 17,207 | 4,035 | 4.26× |
| Marine IK Reverse Small (minified) | 1,887 | 855 | 2.21× |
| Marine IK Reverse Small (prettified) | 6,038 | 3,834 | 1.57× |
| Marine IK Small (minified) | 1,901 | 864 | 2.20× |
| Marine IK Small (prettified) | 5,711 | 3,973 | 1.44× |
| Random Small (minified) | 14,649 | 3,356 | 4.36× |
| Random Small (prettified) | 19,059 | 4,138 | 4.61× |
| Twitter Small (minified) | 13,212 | 5,311 | 2.49× |
| Twitter Small (prettified) | 11,136 | 4,107 | 2.71× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 22,473 | 2,810 | 21,415 | 1.05× |
| Prettify | 5,910 | 3,861 | 2,510 | 1.53× |

### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,984 | 194 | 254 | 7.82× |
| Double | 364 | 272 | 305 | 1.19× |
| Int64 | 333 | 309 | 333 | 1.00× |
| String | 1,645 | 961 | 1,530 | 1.08× |
| Uint64 | 318 | 285 | 313 | 1.02× |
| Apache Builds (minified) | 20,824 | 5,869 | 14,580 | 1.43× |
| Canada (minified) | 1,812 | 1,296 | 1,161 | 1.40× |
| Apache Builds (prettified) | 26,629 | 6,257 | 603 | 4.26× |
| Canada (prettified) | 5,618 | 3,009 | 435 | 1.87× |
| CitmCatalog (minified) | 9,513 | 3,716 | 9,490 | 1.00× |
| CitmCatalog (prettified) | 24,759 | 5,083 | 565 | 4.87× |
| Discord (minified) | 15,198 | 5,382 | 13,216 | 1.15× |
| Discord (prettified) | 24,647 | 4,974 | 529 | 4.95× |
| Github Events (minified) | 12,716 | 6,637 | 10,336 | 1.23× |
| Google Maps Response (minified) | 13,497 | 3,827 | 11,663 | 1.16× |
| Github Events (prettified) | 16,670 | 5,151 | 680 | 3.24× |
| Google Maps Response (prettified) | 30,902 | 5,547 | 583 | 5.57× |
| Instruments (minified) | 17,283 | 2,323 | 14,118 | 1.22× |
| Instruments (prettified) | 32,291 | 3,608 | 512 | 8.95× |
| Marine IK Reverse (minified) | 1,221 | 915 | 1,029 | 1.19× |
| Marine IK Reverse (prettified) | 6,182 | 3,458 | 222 | 1.79× |
| Marine IK (minified) | 1,236 | 896 | 1,079 (n/c) | 1.14× |
| Marine IK (prettified) | 5,993 | 2,760 | 369 | 2.17× |
| Random (minified) | 14,658 | 3,183 | 10,413 | 1.41× |
| Random (prettified) | 22,540 | 3,741 | 569 | 6.02× |
| Twitter (minified) | 13,113 | 5,641 | 10,338 | 1.27× |
| Twitter (prettified) | 16,618 | 4,261 | 572 | 3.90× |
| Apache Builds Small (minified) | 10,956 | 4,250 | 8,875 | 1.23× |
| Canada Small (minified) | 1,791 | 1,413 | 1,304 | 1.27× |
| Apache Builds Small (prettified) | 13,076 (n/c) | 4,376 | 557 | 2.99× |
| Canada Small (prettified) | 5,692 | 3,429 | 487 | 1.66× |
| CitmCatalog Small (minified) | 7,731 | 2,397 | 7,655 | 1.01× |
| CitmCatalog Small (prettified) | 14,662 | 3,966 | 566 | 3.70× |
| Discord Small (minified) | 11,206 | 3,791 | 8,259 | 1.36× |
| Discord Small (prettified) | 15,901 | 3,983 | 539 | 3.99× |
| Github Events Small (minified) | 14,138 | 5,599 | 10,982 | 1.29× |
| Google Maps Response Small (minified) | 13,516 | 3,646 | 11,216 | 1.21× |
| Github Events Small (prettified) | 15,299 | 4,433 | 611 | 3.45× |
| Google Maps Response Small (prettified) | 29,056 | 5,556 | 597 | 5.23× |
| Instruments Small (minified) | 12,712 | 2,051 | 8,679 | 1.46× |
| Instruments Small (prettified) | 20,433 | 3,237 | 547 | 6.31× |
| Marine IK Reverse Small (minified) | 1,615 | 1,065 | 1,431 | 1.13× |
| Marine IK Reverse Small (prettified) | 8,133 | 3,773 | 568 | 2.16× |
| Marine IK Small (minified) | 1,584 | 1,016 | 1,457 | 1.09× |
| Marine IK Small (prettified) | 7,390 | 3,644 | 555 | 2.03× |
| Random Small (minified) | 14,775 | 3,729 | 10,918 | 1.35× |
| Random Small (prettified) | 22,958 | 4,312 | 587 | 5.32× |
| Twitter Small (minified) | 10,910 | 5,167 | 9,964 | 1.09× |
| Twitter Small (prettified) | 14,366 | 4,077 | 581 | 3.52× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 21,412 | 2,562 | 17,682 | 1.21× |
| Prettify | 5,366 | 3,287 | 2,700 | 1.63× |

### Reused output strings

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 50 | 0 | 1 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 49 | 0 | 2 |
| **Total** | **153** | **150** | **0** | **3** |

#### Windows / MSVC 19.52 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,408 | 431 | 3.27× |
| Double | 695 | 216 | 3.21× |
| Int64 | 1,274 | 666 | 1.91× |
| String | 3,432 | 1,762 | 1.95× |
| Uint64 | 1,482 | 654 | 2.26× |
| Apache Builds (minified) | 11,759 | 4,966 | 2.37× |
| Canada (minified) | 1,646 | 887 | 1.86× |
| Apache Builds (prettified) | 10,729 | 4,206 | 2.55× |
| Canada (prettified) | 4,567 | 2,524 | 1.81× |
| CitmCatalog (minified) | 8,695 | 3,312 | 2.63× |
| CitmCatalog (prettified) | 12,218 | 5,920 | 2.06× |
| Discord (minified) | 12,104 | 3,180 | 3.81× |
| Discord (prettified) | 11,330 | 3,270 | 3.46× |
| Github Events (minified) | 11,858 | 5,308 | 2.23× |
| Google Maps Response (minified) | 7,845 | 3,175 | 2.47× |
| Github Events (prettified) | 10,313 | 4,814 | 2.14× |
| Google Maps Response (prettified) | 11,939 | 4,260 | 2.80× |
| Instruments (minified) | 15,433 | 3,130 | 4.93× |
| Instruments (prettified) | 13,759 | 3,694 | 3.72× |
| Marine IK Reverse (minified) | 1,177 | 573 | 2.05× |
| Marine IK Reverse (prettified) | 4,952 | 2,729 | 1.81× |
| Marine IK (minified) | 1,176 | 575 | 2.04× |
| Marine IK (prettified) | 4,762 | 2,695 | 1.77× |
| Random (minified) | 10,223 | 2,718 | 3.76× |
| Random (prettified) | 11,966 | 3,363 | 3.56× |
| Twitter (minified) | 13,602 | 5,367 | 2.53× |
| Twitter (prettified) | 10,564 | 4,925 | 2.15× |
| Apache Builds Small (minified) | 8,927 | 3,858 | 2.31× |
| Canada Small (minified) | 1,771 | 937 | 1.89× |
| Apache Builds Small (prettified) | 9,872 | 3,737 | 2.64× |
| Canada Small (prettified) | 4,719 | 2,586 | 1.82× |
| CitmCatalog Small (minified) | 9,430 | 2,917 | 3.23× |
| CitmCatalog Small (prettified) | 8,402 | 4,291 | 1.96× |
| Discord Small (minified) | 10,125 | 3,475 | 2.91× |
| Discord Small (prettified) | 8,728 | 3,344 | 2.61× |
| Github Events Small (minified) | 12,274 | 4,302 | 2.85× |
| Google Maps Response Small (minified) | 7,669 | 3,007 | 2.55× |
| Github Events Small (prettified) | 10,988 | 4,044 | 2.72× |
| Google Maps Response Small (prettified) | 12,450 | 4,361 | 2.85× |
| Instruments Small (minified) | 14,842 | 3,164 | 4.69× |
| Instruments Small (prettified) | 11,268 | 3,831 | 2.94× |
| Marine IK Reverse Small (minified) | 1,568 | 763 | 2.06× |
| Marine IK Reverse Small (prettified) | 5,514 | 3,145 | 1.75× |
| Marine IK Small (minified) | 1,523 | 766 | 1.99× |
| Marine IK Small (prettified) | 5,568 | 3,121 | 1.78× |
| Random Small (minified) | 10,802 | 3,080 | 3.51× |
| Random Small (prettified) | 11,699 | 3,597 | 3.25× |
| Twitter Small (minified) | 10,378 | 4,762 | 2.18× |
| Twitter Small (prettified) | 7,801 | 4,339 | 1.80× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 22,878 | 1,715 | 21,331 | 1.07× |
| Prettify | 4,523 | 2,908 | 1,376 | 1.56× |

#### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,133 | 965 | 4.28× |
| Double | 946 | 601 | 1.57× |
| Int64 | 2,336 | 1,164 | 2.01× |
| String | 6,102 | 2,241 | 2.72× |
| Uint64 | 2,263 | 1,107 | 2.04× |
| Apache Builds (minified) | 19,195 | 6,158 | 3.12× |
| Canada (minified) | 2,042 | 1,057 | 1.93× |
| Apache Builds (prettified) | 19,701 | 6,656 | 2.96× |
| Canada (prettified) | 5,730 | 3,611 | 1.59× |
| CitmCatalog (minified) | 10,240 | 4,256 | 2.41× |
| CitmCatalog (prettified) | 16,091 | 6,501 | 2.48× |
| Discord (minified) | 15,497 | 4,158 | 3.73× |
| Discord (prettified) | 18,546 | 3,682 | 5.04× |
| Github Events (minified) | 16,683 | 6,793 | 2.46× |
| Google Maps Response (minified) | 12,733 | 3,371 | 3.78× |
| Github Events (prettified) | 16,650 | 6,021 | 2.77× |
| Google Maps Response (prettified) | 18,531 | 5,731 | 3.23× |
| Instruments (minified) | 16,849 | 4,130 | 4.08× |
| Instruments (prettified) | 21,656 | 4,460 | 4.86× |
| Marine IK Reverse (minified) | 1,420 | 675 | 2.10× |
| Marine IK Reverse (prettified) | 4,962 | 3,638 | 1.36× |
| Marine IK (minified) | 1,429 | 657 (n/c) | 2.18× |
| Marine IK (prettified) | 5,302 | 3,961 | 1.34× |
| Random (minified) | 13,220 | 3,172 | 4.17× |
| Random (prettified) | 17,133 | 3,958 | 4.33× |
| Twitter (minified) | 17,824 | 6,546 | 2.72× |
| Twitter (prettified) | 15,985 | 5,485 | 2.91× |
| Apache Builds Small (minified) | 12,215 | 4,351 | 2.81× |
| Canada Small (minified) | 2,114 | 1,085 | 1.95× |
| Apache Builds Small (prettified) | 14,622 | 4,832 | 3.03× |
| Canada Small (prettified) | 5,944 | 3,752 | 1.58× |
| CitmCatalog Small (minified) | 11,140 | 3,028 | 3.68× |
| CitmCatalog Small (prettified) | 14,135 | 4,940 | 2.86× |
| Discord Small (minified) | 13,560 | 4,280 | 3.17× |
| Discord Small (prettified) | 15,977 | 3,813 | 4.19× |
| Github Events Small (minified) | 16,120 | 5,282 | 3.05× |
| Google Maps Response Small (minified) | 12,238 | 3,196 | 3.83× |
| Github Events Small (prettified) | 17,722 | 5,220 | 3.40× |
| Google Maps Response Small (prettified) | 18,631 | 5,836 | 3.19× |
| Instruments Small (minified) | 16,322 | 4,260 | 3.83× |
| Instruments Small (prettified) | 18,884 | 4,344 | 4.35× |
| Marine IK Reverse Small (minified) | 1,908 | 880 | 2.17× |
| Marine IK Reverse Small (prettified) | 5,823 | 4,210 | 1.38× |
| Marine IK Small (minified) | 1,930 | 889 | 2.17× |
| Marine IK Small (prettified) | 6,006 | 4,361 | 1.38× |
| Random Small (minified) | 15,842 | 3,709 | 4.27× |
| Random Small (prettified) | 19,944 | 4,562 | 4.37× |
| Twitter Small (minified) | 15,261 | 5,591 | 2.73× |
| Twitter Small (prettified) | 12,492 | 4,675 | 2.67× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 22,840 | 2,772 | 24,702 | 0.92× |
| Prettify | 5,886 | 3,816 | 2,539 | 1.54× |

#### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,080 | 1,063 | 2,431 | 1.27× |
| Double | 876 | 558 | 724 | 1.21× |
| Int64 | 2,176 | 1,299 | 2,433 | 0.89× |
| String | 4,449 | 2,743 | 3,984 | 1.12× |
| Uint64 | 2,139 | 1,325 | 2,222 | 0.96× |
| Apache Builds (minified) | 21,344 | 6,480 | 14,221 | 1.50× |
| Canada (minified) | 1,826 | 1,374 | 1,185 | 1.33× |
| Apache Builds (prettified) | 26,609 | 6,292 | 605 | 4.23× |
| Canada (prettified) | 5,638 | 3,404 | 421 | 1.66× |
| CitmCatalog (minified) | 9,652 | 4,070 | 9,354 | 1.03× |
| CitmCatalog (prettified) | 24,060 | 6,043 | 562 | 3.98× |
| Discord (minified) | 15,545 | 5,962 | 13,809 | 1.13× |
| Discord (prettified) | 24,967 | 5,719 | 515 | 4.37× |
| Github Events (minified) | 13,248 | 7,580 | 10,797 | 1.23× |
| Google Maps Response (minified) | 14,839 | 4,274 | 11,745 | 1.26× |
| Github Events (prettified) | 17,144 | 5,916 | 675 | 2.90× |
| Google Maps Response (prettified) | 30,925 | 5,946 | 570 | 5.20× |
| Instruments (minified) | 17,590 | 2,394 | 14,191 | 1.24× |
| Instruments (prettified) | 32,906 | 3,877 | 508 | 8.49× |
| Marine IK Reverse (minified) | 1,230 | 917 | 1,038 | 1.18× |
| Marine IK Reverse (prettified) | 5,903 | 3,757 | 259 (n/c) | 1.57× |
| Marine IK (minified) | 1,243 | 886 | 1,033 | 1.20× |
| Marine IK (prettified) | 6,020 | 3,713 | 276 | 1.62× |
| Random (minified) | 14,082 | 3,715 | 10,725 | 1.31× |
| Random (prettified) | 22,110 | 4,021 | 545 | 5.50× |
| Twitter (minified) | 12,997 | 6,518 | 10,219 | 1.27× |
| Twitter (prettified) | 16,432 | 4,985 | 564 | 3.30× |
| Apache Builds Small (minified) | 12,485 | 4,687 | 10,042 | 1.24× |
| Canada Small (minified) | 1,817 | 1,502 | 1,315 | 1.21× |
| Apache Builds Small (prettified) | 16,267 | 4,849 | 571 | 3.35× |
| Canada Small (prettified) | 5,793 | 3,629 | 474 | 1.60× |
| CitmCatalog Small (minified) | 9,043 | 2,649 | 8,840 | 1.02× |
| CitmCatalog Small (prettified) | 16,799 | 4,583 | 554 | 3.67× |
| Discord Small (minified) | 12,724 | 4,627 | 9,429 | 1.35× |
| Discord Small (prettified) | 17,045 | 4,585 | 555 | 3.72× |
| Github Events Small (minified) | 14,520 | 6,674 | 12,501 | 1.16× |
| Google Maps Response Small (minified) | 13,890 | 3,588 | 11,191 | 1.24× |
| Github Events Small (prettified) | 20,144 | 5,304 | 596 | 3.80× |
| Google Maps Response Small (prettified) | 29,775 | 6,178 | 592 | 4.82× |
| Instruments Small (minified) | 15,014 | 2,163 | 9,877 | 1.52× |
| Instruments Small (prettified) | 23,566 | 3,731 | 556 | 6.32× |
| Marine IK Reverse Small (minified) | 1,646 | 1,134 | 1,547 | 1.06× |
| Marine IK Reverse Small (prettified) | 7,980 | 3,895 | 541 | 2.05× |
| Marine IK Small (minified) | 1,641 | 1,093 | 1,477 | 1.11× |
| Marine IK Small (prettified) | 7,816 | 4,038 | 573 | 1.94× |
| Random Small (minified) | 16,461 | 4,167 | 12,764 | 1.29× |
| Random Small (prettified) | 26,267 | 4,874 | 557 | 5.39× |
| Twitter Small (minified) | 12,472 | 5,646 | 11,069 | 1.13× |
| Twitter Small (prettified) | 17,277 | 4,543 | 597 | 3.80× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 21,674 | 2,604 | 17,840 | 1.21× |
| Prettify | 5,503 | 3,409 | 2,703 | 1.61× |

## Questions from external review

An outside review of a draft raised these concerns, answered against the code as it stands.

### Can an overstore run past the buffer?

No, because every overstore lands inside the string's own size, not past it. `serializeJson` resizes the buffer to the full bound plus 64 bytes before writing, so the write pass works inside memory the string already owns. No write ever reaches capacity or the allocator's edge, and ASan sees only in-bounds writes. The one deliberate out-of-bounds access is a read: the page-safe short-string load reads up to 16 bytes past the source string, and under ASan it runs in a `JSONIFIER_NO_SANITIZE_ADDRESS` helper (see *Strings*).

The bound covers the largest overstore of each writer:

| Writer | Largest write beyond its true length | Covered by |
| --- | --- | --- |
| Bool | 3 bytes (8 written, 5 reserved) | 64-byte tail slack |
| Integer or float | none past the 32 bytes reserved per number | `staticSize` of 32 |
| Key literal over 16 bytes | up to 31 bytes (rounded to a multiple of 32) | 64-byte tail slack |
| Indent run | up to 7 bytes (rounded to 8) | 64-byte tail slack |
| String SIMD block | none (stored only after its escape test passes, and every stored byte is output) | not needed |
| Escape sequence | up to 6 bytes (8 written, at least 2 advanced) | 64-byte tail slack |
| Short string (under 16 bytes) | up to 15 bytes (one 16-byte store) | 64-byte tail slack |

The largest single overstore on any ISA is 31 bytes, under the 64-byte slack. That slack is a fixed constant, not derived from the widest writer, so a future writer with a wider overstore must raise it.

### Does `6n + 2` blow up memory on huge strings?

Yes, the reservation is real. A 100 MB string field asks for about 600 MB before the final shrink, and the string keeps that capacity afterwards unless the caller calls `shrink_to_fit`. How much of it is touched depends on the buffer:

- With `resize_and_overwrite`, nothing is zero-filled. On Linux and macOS, pages that are never written are usually never faulted in, so the cost is mostly address space. On Windows, the allocation still counts against the commit limit.
- Without `resize_and_overwrite`, `resize` zero-fills the whole bound, so every page is touched.

Setting `prescanStrings` in `serialize_options` removes the 6× factor, at the price of one extra pass over every string's bytes during sizing. See *Pre-scanning strings* below. Its throughput cost has not been measured.

### Pre-scanning strings

`prescanStrings` replaces the `6n + 2` string bound with a scan that returns the exact number of bytes the writer will emit for the string, plus 2 for the quotes. The option is off by default and is a compile-time `if constexpr` in the string `get_size_impl`, so the default path is unchanged.

The scan (`string_serializer::escapedSize`) reads 8 bytes at a time and applies the same SWAR flag test the writer's tail path uses (`flagMask`): a word with no `"`, `\` or control byte adds 8 and moves on. A flagged word is summed byte by byte from the size stored in each `escapeTable` entry, the same table the writer uses, where `\b \t \n \f \r " \\` take 2 bytes, other control bytes, NUL included, take 6 (written as `\u00XX`), and everything else takes 1. The tail under 8 bytes is summed the same way, so the count matches the writer's output exactly.

No string writer depends on the `6n + 2` headroom for its overstores: vector stores are all output bytes, and the short-string store and escape store are covered by the 64-byte slack in `serializeJson`, which is unchanged, so the overstore table above holds with an exact string bound. The option only affects strings inside that size pass: numbers, booleans and keys keep their existing bounds, and a lone string passed directly to `serializeJson` keeps its `6n + 2` reservation.

### Is it portable to big-endian targets?

The code has big-endian paths, but none are tested. `packed_blitter`, the bool constants, the digit tables and the SWAR string tail each branch on `std::endian::native` at compile time and use `std::byteswap` where needed. The CI matrix covers only little-endian x86 and ARM, so those branches have never run.

### Why does the String test vary so much across builds?

Freshly allocated, Jsonifier runs String at 582 MB/s under Windows / MSVC (1.27× Glaze's 458), 1,616 and 1,577 MB/s on Linux / Clang and Linux / GCC (1.84× and 1.70×), and 860 and 601 MB/s on macOS / GCC and macOS / Clang (1.55× and 1.13×). With the string reused the spread changes shape: macOS / GCC is the fastest build at 6,520 MB/s (2.53× Glaze, and 1.16× the reflection writer's 5,613), the three x86 builds run 4,752 to 5,392 MB/s (1.70× to 3.01× Glaze), and macOS / Clang is the slowest at 3,524 MB/s (1.72×).

So much of the freshly allocated gap sits in the allocation and release of the output string, which the reused run removes, and what remains on macOS / Clang is a compiler effect rather than an instruction-set one, since macOS / GCC runs the same NEON code more than twice as fast. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

On macOS / GCC, yes, in both runs, by the ranking and per call. Jsonifier and the reflection writer write the same 1,997 bytes, so MB/s and per-call time agree: freshly allocated it runs 304 vs 290 MB/s (0.95×, 6,267 vs 6,571 ns) and reused 1,004 vs 849 MB/s (0.85×). On Linux / GCC, no: Jsonifier leads it freshly allocated (382 vs 366 MB/s, 1.04×, 4,987 vs 5,199 ns) and reused (961 vs 845 MB/s, 1.14×). Jsonifier beats Glaze on Double on all five builds in both runs.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

### Limitations

- **Over-allocation.** Every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`, so string-heavy documents can ask for about 6× their output size (see *Does `6n + 2` blow up memory on huge strings?*).
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.

## Dedication

This paper is dedicated to Sonny Moore (Skrillex), for showing what it looks like when someone's vocation is also their resting state.

## References

1. Nihilai Collective Corp. "CAFBERIHT." Nihilai Collective Corp, 2026. https://nihilai-collective.net/cafberiht
2. memcpy-decomp benchmark harness. https://github.com/nihilai-collective/memcpy-decomp
3. Nihilai Collective Corp. "Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier." Nihilai Collective Corp, 2026.
4. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
5. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
6. Glaze. https://github.com/stephenberry/glaze
7. simdjson. https://github.com/simdjson/simdjson
8. BenchmarkSuite. https://github.com/nihilai-collective/benchmarksuite
