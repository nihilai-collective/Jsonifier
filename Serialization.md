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

On the Bool test this writer leads Glaze by 8.59–45.83× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on four of the five builds, by 1.04× to 2.78×; on macOS / GCC both tie with the reflection writer (0.96× and 0.97×).

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

Jsonifier was fastest in 241 of the 255 write tests on five builds, tied 4 and lost 10. With the output string reused across iterations it was fastest in 245 of 255, tied 4 and lost 6. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other eight are against simdjson's reflection writer, which exists only on GCC: Double, Int64, minified CitmCatalog, minified Marine IK Reverse, minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, and Double and minified CitmCatalog on macOS / GCC. The four freshly allocated ties are also against the reflection writer, all on macOS / GCC: Uint64 and minified CitmCatalog Small, Github Events Small and Marine IK Reverse Small. Reused, every non-win is against the reflection writer: minified CitmCatalog, Marine IK Reverse, Marine IK Reverse Small and Marine IK Small on Linux / GCC and Double and minified CitmCatalog on macOS / GCC are losses, and Int64, Uint64 and minified CitmCatalog Small and Marine IK Small on macOS / GCC are ties. Linux / Clang and macOS / Clang win all 51 tests in both runs, and Windows / MSVC wins all 51 reused. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.38–8.78× across the five builds and the reflection writer by 14–61× on the two GCC builds; Small documents lead them by 1.42–8.46× and 12–60×. Three AVX-512 builds are reported separately, in *AVX-512 builds* below: there Jsonifier was fastest in 149 of 153 write tests freshly allocated and 150 of 153 reused.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) [4] sweep published November 23, 2026, with Jsonifier [4f52e70](https://github.com/nihilai-collective/jsonifier/commit/4f52e70) [5], Glaze [fc0b48a0](https://github.com/stephenberry/glaze/commit/fc0b48a0) [6], simdjson [344898409](https://github.com/simdjson/simdjson/commit/344898409) [7] and BenchmarkSuite [71afc62](https://github.com/nihilai-collective/benchmarksuite/commit/71afc62) [8] on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3]. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**Ranking.** Libraries are ordered by throughput. Variance is Bessel-corrected, and two neighbouring libraries tie when Welch's t-test (two-sided, p < 0.05) on their per-call times cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. Every tie in this sweep is at the top. Because the verdict is a significance test and not a ratio threshold, a lead of a few percent with tight samples is decisive: on Linux / GCC, Bool reused (3,240 vs the reflection writer's 3,201 MB/s) is a win of 1.01×, while a 7% lead with RSEs of 2.0% and 3.7% can still be a tie (see *Losses and ties*). Because the order follows MB/s, which credits each library its own output size, a library that is faster per call can still rank below one that writes more bytes. No single-value verdict in this sweep turns on that: on Double and Int64, Jsonifier and the reflection writer write the same bytes, so the MB/s order is also the per-call order.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 45 | 0 | 6 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 45 | 4 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **241** | **4** | **10** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,116 | 24 | 45.83× |
| Double | 465 | 164 | 2.84× |
| Int64 | 629 | 154 | 4.10× |
| String | 558 | 464 | 1.20× |
| Uint64 | 616 | 145 | 4.25× |
| Apache Builds (minified) | 11,386 | 4,403 | 2.59× |
| Canada (minified) | 997 | 848 | 1.18× |
| Apache Builds (prettified) | 13,144 | 4,751 | 2.77× |
| Canada (prettified) | 2,688 | 1,445 | 1.86× |
| CitmCatalog (minified) | 9,792 | 4,155 | 2.36× |
| CitmCatalog (prettified) | 4,454 | 1,934 | 2.30× |
| Discord (minified) | 10,719 | 4,102 | 2.61× |
| Discord (prettified) | 11,684 | 4,105 | 2.85× |
| Github Events (minified) | 11,737 | 5,317 | 2.21× |
| Google Maps Response (minified) | 10,293 | 3,436 | 3.00× |
| Github Events (prettified) | 11,196 | 3,382 | 3.31× |
| Google Maps Response (prettified) | 14,658 | 5,205 | 2.82× |
| Instruments (minified) | 15,083 | 3,949 | 3.82× |
| Instruments (prettified) | 14,831 | 4,274 | 3.47× |
| Marine IK Reverse (minified) | 779 | 664 | 1.17× |
| Marine IK Reverse (prettified) | 2,524 | 1,833 | 1.38× |
| Marine IK (minified) | 781 | 594 | 1.31× |
| Marine IK (prettified) | 2,556 | 1,260 | 2.03× |
| Random (minified) | 3,696 | 3,198 | 1.16× |
| Random (prettified) | 4,258 | 2,286 | 1.86× |
| Twitter (minified) | 3,893 | 5,002 | 0.78× |
| Twitter (prettified) | 4,205 | 1,748 | 2.41× |
| Apache Builds Small (minified) | 9,058 | 3,557 | 2.55× |
| Canada Small (minified) | 1,249 | 1,156 | 1.08× |
| Apache Builds Small (prettified) | 9,783 | 3,909 | 2.50× |
| Canada Small (prettified) | 4,518 | 2,936 | 1.54× |
| CitmCatalog Small (minified) | 8,272 | 3,006 | 2.75× |
| CitmCatalog Small (prettified) | 8,156 | 4,149 | 1.97× |
| Discord Small (minified) | 9,648 | 3,335 | 2.89× |
| Discord Small (prettified) | 8,551 | 3,475 | 2.46× |
| Github Events Small (minified) | 10,360 | 4,296 | 2.41× |
| Google Maps Response Small (minified) | 8,677 | 3,444 | 2.52× |
| Github Events Small (prettified) | 10,462 | 3,981 | 2.63× |
| Google Maps Response Small (prettified) | 13,607 | 4,906 | 2.77× |
| Instruments Small (minified) | 11,132 | 3,683 (n/c) | 3.02× |
| Instruments Small (prettified) | 11,726 | 2,488 | 4.71× |
| Marine IK Reverse Small (minified) | 1,222 | 881 | 1.39× |
| Marine IK Reverse Small (prettified) | 4,601 | 3,006 | 1.53× |
| Marine IK Small (minified) | 1,211 | 914 | 1.32× |
| Marine IK Small (prettified) | 4,630 | 3,048 | 1.52× |
| Random Small (minified) | 9,405 | 3,464 | 2.72× |
| Random Small (prettified) | 11,710 | 4,026 | 2.91× |
| Twitter Small (minified) | 10,340 | 4,851 | 2.13× |
| Twitter Small (prettified) | 8,765 | 3,203 (n/c) | 2.74× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,109 | 1,870 | 1,429 | 1.66× |
| Prettify | 2,468 | 2,599 | 1,435 | 0.95× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,141 | 219 | 9.78× |
| Double | 372 | 305 | 1.22× |
| Int64 | 336 | 314 | 1.07× |
| String | 1,567 | 884 | 1.77× |
| Uint64 | 309 | 271 | 1.14× |
| Apache Builds (minified) | 15,747 | 5,677 | 2.77× |
| Canada (minified) | 1,963 | 958 | 2.05× |
| Apache Builds (prettified) | 16,620 | 5,403 | 3.08× |
| Canada (prettified) | 5,194 | 3,342 | 1.55× |
| CitmCatalog (minified) | 10,356 | 5,107 | 2.03× |
| CitmCatalog (prettified) | 14,215 | 5,316 | 2.67× |
| Discord (minified) | 13,317 | 4,872 | 2.73× |
| Discord (prettified) | 15,149 | 4,215 | 3.59× |
| Github Events (minified) | 15,206 | 5,085 | 2.99× |
| Google Maps Response (minified) | 12,062 | 3,650 | 3.30× |
| Github Events (prettified) | 15,353 | 4,496 | 3.42× |
| Google Maps Response (prettified) | 15,822 | 4,994 | 3.17× |
| Instruments (minified) | 16,729 | 4,727 | 3.54× |
| Instruments (prettified) | 18,657 | 4,960 | 3.76× |
| Marine IK Reverse (minified) | 1,243 | 657 | 1.89× |
| Marine IK Reverse (prettified) | 5,100 | 3,668 | 1.39× |
| Marine IK (minified) | 1,243 | 636 | 1.95× |
| Marine IK (prettified) | 5,129 | 2,859 | 1.79× |
| Random (minified) | 14,249 | 3,639 | 3.92× |
| Random (prettified) | 14,651 | 3,875 | 3.78× |
| Twitter (minified) | 16,391 | 6,443 | 2.54× |
| Twitter (prettified) | 16,845 | 4,418 | 3.81× |
| Apache Builds Small (minified) | 9,835 | 3,467 | 2.84× |
| Canada Small (minified) | 1,897 | 970 | 1.96× |
| Apache Builds Small (prettified) | 10,841 | 3,564 | 3.04× |
| Canada Small (prettified) | 5,040 | 3,548 | 1.42× |
| CitmCatalog Small (minified) | 7,874 | 2,797 | 2.81× |
| CitmCatalog Small (prettified) | 10,138 | 3,306 | 3.07× |
| Discord Small (minified) | 10,659 | 3,047 | 3.50× |
| Discord Small (prettified) | 11,111 | 2,480 | 4.48× |
| Github Events Small (minified) | 9,698 | 3,813 | 2.54× |
| Google Maps Response Small (minified) | 10,657 | 3,472 | 3.07× |
| Github Events Small (prettified) | 10,514 | 3,643 | 2.89× |
| Google Maps Response Small (prettified) | 15,441 | 4,627 | 3.34× |
| Instruments Small (minified) | 10,760 | 2,338 | 4.60× |
| Instruments Small (prettified) | 13,898 | 2,969 | 4.68× |
| Marine IK Reverse Small (minified) | 1,478 | 786 | 1.88× |
| Marine IK Reverse Small (prettified) | 5,420 | 3,400 | 1.59× |
| Marine IK Small (minified) | 1,468 | 820 | 1.79× |
| Marine IK Small (prettified) | 5,494 | 3,410 | 1.61× |
| Random Small (minified) | 12,682 | 3,291 | 3.85× |
| Random Small (prettified) | 14,954 | 3,793 | 3.94× |
| Twitter Small (minified) | 8,284 | 4,281 | 1.94× |
| Twitter Small (prettified) | 9,550 | 3,634 | 2.63× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,473 | 2,405 | 1,798 | 1.44× |
| Prettify | 4,170 | 3,502 | 2,287 | 1.19× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,767 | 203 | 253 | 6.98× |
| Double | 392 | 326 | 411 | 0.95× |
| Int64 | 351 | 317 | 370 | 0.95× |
| String | 1,641 | 942 | 1,560 | 1.05× |
| Uint64 | 309 | 281 | 298 | 1.04× |
| Apache Builds (minified) | 16,156 | 7,957 | 13,759 | 1.17× |
| Canada (minified) | 2,129 | 1,457 | 1,615 | 1.32× |
| Apache Builds (prettified) | 18,875 | 7,199 | 638 | 2.62× |
| Canada (prettified) | 6,011 | 3,313 | 443 | 1.81× |
| CitmCatalog (minified) | 11,978 | 6,266 | 12,775 | 0.94× |
| CitmCatalog (prettified) | 18,398 | 5,691 | 575 | 3.23× |
| Discord (minified) | 13,753 | 6,326 | 13,212 | 1.04× |
| Discord (prettified) | 18,050 | 5,972 | 561 | 3.02× |
| Github Events (minified) | 13,467 | 7,280 | 11,570 | 1.16× |
| Google Maps Response (minified) | 12,446 | 4,541 | 9,817 | 1.27× |
| Github Events (prettified) | 16,637 | 4,850 | 653 | 3.43× |
| Google Maps Response (prettified) | 20,126 | 6,545 | 556 | 3.07× |
| Instruments (minified) | 18,826 | 5,393 | 16,139 | 1.17× |
| Instruments (prettified) | 25,404 | 5,289 | 480 | 4.80× |
| Marine IK Reverse (minified) | 1,332 | 1,005 | 1,364 | 0.98× |
| Marine IK Reverse (prettified) | 6,277 | 3,685 | 299 | 1.70× |
| Marine IK (minified) | 1,368 | 962 | 1,349 | 1.01× |
| Marine IK (prettified) | 6,418 | 3,150 | 442 | 2.04× |
| Random (minified) | 14,535 | 4,552 | 11,784 | 1.23× |
| Random (prettified) | 17,757 | 4,903 | 539 | 3.62× |
| Twitter (minified) | 15,813 | 7,776 | 12,830 | 1.23× |
| Twitter (prettified) | 18,858 | 5,264 | 532 | 3.58× |
| Apache Builds Small (minified) | 10,432 | 4,726 | 7,459 | 1.40× |
| Canada Small (minified) | 1,920 | 1,606 | 1,738 | 1.11× |
| Apache Builds Small (prettified) | 14,182 | 4,190 | 538 | 3.38× |
| Canada Small (prettified) | 6,411 | 3,576 | 472 | 1.79× |
| CitmCatalog Small (minified) | 7,941 | 3,302 | 7,047 | 1.13× |
| CitmCatalog Small (prettified) | 15,829 | 3,048 | 508 | 5.19× |
| Discord Small (minified) | 10,762 | 3,919 | 8,082 | 1.33× |
| Discord Small (prettified) | 15,320 | 2,764 | 517 | 5.54× |
| Github Events Small (minified) | 7,701 | 5,117 | 6,231 | 1.24× |
| Google Maps Response Small (minified) | 10,309 | 3,802 | 8,416 | 1.22× |
| Github Events Small (prettified) | 10,751 | 3,717 | 553 | 2.89× |
| Google Maps Response Small (prettified) | 23,466 | 5,793 | 522 | 4.05× |
| Instruments Small (minified) | 13,613 | 3,775 | 10,854 | 1.25× |
| Instruments Small (prettified) | 24,367 | 3,248 | 525 | 7.50× |
| Marine IK Reverse Small (minified) | 1,597 | 1,247 | 1,978 | 0.81× |
| Marine IK Reverse Small (prettified) | 6,387 | 3,242 | 547 | 1.97× |
| Marine IK Small (minified) | 1,456 | 1,256 | 1,922 | 0.76× |
| Marine IK Small (prettified) | 6,799 | 3,271 | 540 | 2.08× |
| Random Small (minified) | 12,798 | 4,209 | 8,509 | 1.50× |
| Random Small (prettified) | 21,440 | 4,477 | 529 | 4.79× |
| Twitter Small (minified) | 8,708 | 6,103 | 7,057 | 1.23× |
| Twitter Small (prettified) | 11,898 | 3,681 | 559 | 3.23× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,257 | 2,576 | 1,905 | 1.26× |
| Prettify | 4,269 | 3,200 | 2,486 | 1.33× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,858 | 132 | 220 | 8.46× |
| Double | 321 | 217 | 340 | 0.94× |
| Int64 | 308 | 191 | 297 | 1.04× |
| String | 857 | 556 | 837 | 1.02× |
| Uint64 | 284 | 180 | 282 | 1.01× |
| Apache Builds (minified) | 12,615 | 5,271 | 10,316 | 1.22× |
| Canada (minified) | 2,613 | 1,783 | 2,132 | 1.23× |
| Apache Builds (prettified) | 13,845 | 4,697 | 390 | 2.95× |
| Canada (prettified) | 8,001 | 3,625 | 352 | 2.21× |
| CitmCatalog (minified) | 8,718 | 2,226 | 9,140 | 0.95× |
| CitmCatalog (prettified) | 21,564 | 4,026 | 361 | 5.36× |
| Discord (minified) | 12,045 | 2,880 | 10,486 | 1.15× |
| Discord (prettified) | 14,023 | 2,511 | 362 | 5.58× |
| Github Events (minified) | 15,809 | 3,889 | 12,017 | 1.32× |
| Google Maps Response (minified) | 10,026 | 1,917 | 8,997 | 1.11× |
| Github Events (prettified) | 16,801 | 3,518 | 442 | 4.78× |
| Google Maps Response (prettified) | 18,989 | 2,976 | 341 | 6.38× |
| Instruments (minified) | 13,505 | 2,102 | 9,469 | 1.43× |
| Instruments (prettified) | 21,200 | 2,414 | 349 | 8.78× |
| Marine IK Reverse (minified) | 1,538 | 942 | 1,295 | 1.19× |
| Marine IK Reverse (prettified) | 7,283 | 3,333 | 322 | 2.19× |
| Marine IK (minified) | 1,482 | 887 | 1,262 | 1.17× |
| Marine IK (prettified) | 7,350 | 2,982 | 323 | 2.46× |
| Random (minified) | 10,454 | 2,374 | 7,037 | 1.49× |
| Random (prettified) | 18,334 | 2,714 | 333 | 6.76× |
| Twitter (minified) | 14,895 | 4,084 | 12,081 | 1.23× |
| Twitter (prettified) | 16,824 | 4,025 | 436 | 4.18× |
| Apache Builds Small (minified) | 9,009 | 3,127 | 7,407 | 1.22× |
| Canada Small (minified) | 2,375 | 1,685 | 2,131 | 1.11× |
| Apache Builds Small (prettified) | 9,932 | 4,045 | 373 | 2.46× |
| Canada Small (prettified) | 7,911 | 3,312 | 334 | 2.39× |
| CitmCatalog Small (minified) | 8,958 | 1,742 | 8,375 | 1.07× |
| CitmCatalog Small (prettified) | 16,858 | 2,611 | 348 | 6.46× |
| Discord Small (minified) | 11,376 | 2,697 | 8,857 | 1.28× |
| Discord Small (prettified) | 13,143 | 2,466 | 358 | 5.33× |
| Github Events Small (minified) | 10,965 | 3,047 | 10,413 | 1.05× |
| Google Maps Response Small (minified) | 9,350 | 1,845 | 8,564 | 1.09× |
| Github Events Small (prettified) | 11,788 | 3,005 | 393 | 3.92× |
| Google Maps Response Small (prettified) | 19,942 | 2,866 | 334 | 6.96× |
| Instruments Small (minified) | 12,955 | 1,781 | 9,191 | 1.41× |
| Instruments Small (prettified) | 19,864 | 2,349 | 341 | 8.46× |
| Marine IK Reverse Small (minified) | 2,096 | 1,083 | 2,090 | 1.00× |
| Marine IK Reverse Small (prettified) | 8,786 | 3,222 | 326 | 2.73× |
| Marine IK Small (minified) | 2,120 | 1,075 | 1,844 | 1.15× |
| Marine IK Small (prettified) | 8,887 | 3,305 | 322 | 2.69× |
| Random Small (minified) | 12,127 | 2,268 | 9,422 | 1.29× |
| Random Small (prettified) | 18,390 | 2,601 | 344 | 7.07× |
| Twitter Small (minified) | 11,399 | 3,443 | 9,147 | 1.25× |
| Twitter Small (prettified) | 13,988 | 3,170 | 419 | 4.41× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,451 | 1,697 | 1,214 | 1.44× |
| Prettify | 3,317 | 2,401 | 1,633 | 1.38× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,032 | 120 | 8.59× |
| Double | 295 | 209 | 1.41× |
| Int64 | 1,149 | 170 | 6.76× |
| String | 645 | 449 | 1.44× |
| Uint64 | 1,127 | 167 | 6.75× |
| Apache Builds (minified) | 9,492 | 3,212 | 2.96× |
| Canada (minified) | 2,371 | 1,476 | 1.61× |
| Apache Builds (prettified) | 9,833 | 2,981 | 3.30× |
| Canada (prettified) | 5,450 | 2,929 | 1.86× |
| CitmCatalog (minified) | 7,348 | 1,667 | 4.41× |
| CitmCatalog (prettified) | 11,429 | 3,147 | 3.63× |
| Discord (minified) | 9,016 | 2,466 | 3.66× |
| Discord (prettified) | 10,446 | 2,184 | 4.78× |
| Github Events (minified) | 10,124 | 3,650 | 2.77× |
| Google Maps Response (minified) | 7,541 | 1,918 | 3.93× |
| Github Events (prettified) | 10,651 | 2,930 | 3.64× |
| Google Maps Response (prettified) | 9,338 | 2,719 | 3.43× |
| Instruments (minified) | 11,301 | 1,879 | 6.01× |
| Instruments (prettified) | 12,266 | 2,952 | 4.16× |
| Marine IK Reverse (minified) | 1,208 | 734 | 1.64× |
| Marine IK Reverse (prettified) | 5,020 | 2,690 | 1.87× |
| Marine IK (minified) | 1,375 | 734 | 1.87× |
| Marine IK (prettified) | 5,258 | 2,775 | 1.89× |
| Random (minified) | 9,683 | 2,423 | 4.00× |
| Random (prettified) | 9,307 | 2,983 | 3.12× |
| Twitter (minified) | 14,109 | 4,294 | 3.29× |
| Twitter (prettified) | 13,483 | 3,901 | 3.46× |
| Apache Builds Small (minified) | 5,958 | 2,901 | 2.05× |
| Canada Small (minified) | 2,097 | 1,512 | 1.39× |
| Apache Builds Small (prettified) | 7,951 | 3,100 | 2.56× |
| Canada Small (prettified) | 7,059 | 3,399 | 2.08× |
| CitmCatalog Small (minified) | 8,700 | 1,965 | 4.43× |
| CitmCatalog Small (prettified) | 12,288 | 2,214 | 5.55× |
| Discord Small (minified) | 10,466 | 2,802 | 3.73× |
| Discord Small (prettified) | 10,797 | 2,797 | 3.86× |
| Github Events Small (minified) | 9,033 | 2,729 | 3.31× |
| Google Maps Response Small (minified) | 7,825 | 2,232 | 3.51× |
| Github Events Small (prettified) | 9,307 | 2,937 | 3.17× |
| Google Maps Response Small (prettified) | 12,557 | 3,474 | 3.61× |
| Instruments Small (minified) | 10,229 | 1,768 | 5.78× |
| Instruments Small (prettified) | 14,054 | 2,785 | 5.05× |
| Marine IK Reverse Small (minified) | 2,014 | 1,010 | 1.99× |
| Marine IK Reverse Small (prettified) | 5,316 | 2,736 | 1.94× |
| Marine IK Small (minified) | 1,689 | 976 | 1.73× |
| Marine IK Small (prettified) | 4,541 | 2,821 | 1.61× |
| Random Small (minified) | 7,575 | 2,104 | 3.60× |
| Random Small (prettified) | 10,233 | 2,310 | 4.43× |
| Twitter Small (minified) | 8,680 | 2,717 | 3.19× |
| Twitter Small (prettified) | 9,025 (n/c) | 2,518 | 3.58× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,994 | 1,422 | 921 | 1.40× |
| Prettify | 2,558 | 1,969 | 1,273 | 1.30× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 245 of the 255 write tests, ties 4 and loses 6, all six to the reflection writer. On Linux / GCC the reflection writer wins minified CitmCatalog (12,838 vs 12,075 MB/s, 0.94×), minified Marine IK Reverse (1,366 vs 1,326 MB/s, 0.97×), minified Marine IK Reverse Small (2,013 vs 1,636 MB/s, 0.81×) and minified Marine IK Small (1,934 vs 1,491 MB/s, 0.77×); on macOS / GCC it wins Double (1,104 vs 1,030 MB/s, 0.93×) and minified CitmCatalog (9,194 vs 8,765 MB/s, 0.95×).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 47 | 0 | 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 45 | 4 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **245** | **4** | **6** |

#### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,638 | 544 | 3.01× |
| Double | 580 | 432 | 1.34× |
| Int64 | 1,463 | 671 | 2.18× |
| String | 4,753 | 1,549 | 3.07× |
| Uint64 | 1,524 | 701 | 2.17× |
| Apache Builds (minified) | 12,051 | 4,979 | 2.42× |
| Canada (minified) | 1,161 | 1,137 | 1.02× |
| Apache Builds (prettified) | 13,356 | 5,328 | 2.51× |
| Canada (prettified) | 4,071 | 3,115 | 1.31× |
| CitmCatalog (minified) | 9,998 | 4,569 | 2.19× |
| CitmCatalog (prettified) | 13,506 | 7,242 | 1.86× |
| Discord (minified) | 11,136 | 4,699 | 2.37× |
| Discord (prettified) | 12,093 | 4,707 | 2.57× |
| Github Events (minified) | 12,344 | 6,146 | 2.01× |
| Google Maps Response (minified) | 11,970 | 4,045 | 2.96× |
| Github Events (prettified) | 11,435 | 3,759 | 3.04× |
| Google Maps Response (prettified) | 16,102 | 5,859 | 2.75× |
| Instruments (minified) | 15,832 | 4,439 | 3.57× |
| Instruments (prettified) | 15,373 | 5,024 | 3.06× |
| Marine IK Reverse (minified) | 867 | 724 | 1.20× |
| Marine IK Reverse (prettified) | 3,633 | 3,291 | 1.10× |
| Marine IK (minified) | 872 | 738 | 1.18× |
| Marine IK (prettified) | 3,763 | 3,291 | 1.14× |
| Random (minified) | 10,432 | 3,459 | 3.02× |
| Random (prettified) | 12,902 | 4,432 | 2.91× |
| Twitter (minified) | 15,136 | 5,619 | 2.69× |
| Twitter (prettified) | 14,201 | 3,339 | 4.25× |
| Apache Builds Small (minified) | 9,772 | 4,117 | 2.37× |
| Canada Small (minified) | 1,270 | 1,213 | 1.05× |
| Apache Builds Small (prettified) | 11,079 | 4,438 | 2.50× |
| Canada Small (prettified) | 4,634 | 3,436 | 1.35× |
| CitmCatalog Small (minified) | 9,312 | 3,930 | 2.37× |
| CitmCatalog Small (prettified) | 9,874 | 4,781 | 2.07× |
| Discord Small (minified) | 10,708 | 4,444 | 2.41× |
| Discord Small (prettified) | 9,504 | 4,372 (n/c) | 2.17× |
| Github Events Small (minified) | 13,806 | 5,325 | 2.59× |
| Google Maps Response Small (minified) | 11,451 | 3,946 | 2.90× |
| Github Events Small (prettified) | 12,759 | 4,853 | 2.63× |
| Google Maps Response Small (prettified) | 16,585 | 6,238 | 2.66× |
| Instruments Small (minified) | 15,902 | 4,718 | 3.37× |
| Instruments Small (prettified) | 12,783 | 3,583 (n/c) | 3.57× |
| Marine IK Reverse Small (minified) | 1,237 | 965 | 1.28× |
| Marine IK Reverse Small (prettified) | 4,742 | 3,520 | 1.35× |
| Marine IK Small (minified) | 1,251 | 1,006 | 1.24× |
| Marine IK Small (prettified) | 4,752 | 3,577 | 1.33× |
| Random Small (minified) | 12,598 | 4,261 | 2.96× |
| Random Small (prettified) | 14,632 | 5,285 | 2.77× |
| Twitter Small (minified) | 12,040 | 5,561 | 2.17× |
| Twitter Small (prettified) | 10,239 | 4,736 (n/c) | 2.16× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,111 | 1,865 | 1,411 | 1.67× |
| Prettify | 3,987 | 2,602 | 1,402 | 1.53× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,015 | 894 | 4.49× |
| Double | 990 | 662 | 1.50× |
| Int64 | 2,361 | 935 | 2.52× |
| String | 5,349 | 2,284 | 2.34× |
| Uint64 | 2,327 | 837 | 2.78× |
| Apache Builds (minified) | 15,733 | 6,440 | 2.44× |
| Canada (minified) | 1,967 | 997 | 1.97× |
| Apache Builds (prettified) | 17,065 | 5,882 | 2.90× |
| Canada (prettified) | 5,220 | 4,010 | 1.30× |
| CitmCatalog (minified) | 10,377 | 5,844 | 1.78× |
| CitmCatalog (prettified) | 14,190 | 7,091 | 2.00× |
| Discord (minified) | 13,104 | 5,520 | 2.37× |
| Discord (prettified) | 15,434 | 4,812 | 3.21× |
| Github Events (minified) | 15,639 | 5,999 | 2.61× |
| Google Maps Response (minified) | 13,147 | 4,082 | 3.22× |
| Github Events (prettified) | 15,649 | 5,325 | 2.94× |
| Google Maps Response (prettified) | 16,235 | 5,606 | 2.90× |
| Instruments (minified) | 16,733 | 5,287 | 3.16× |
| Instruments (prettified) | 19,222 | 5,730 | 3.35× |
| Marine IK Reverse (minified) | 1,236 | 648 | 1.91× |
| Marine IK Reverse (prettified) | 5,097 | 4,058 | 1.26× |
| Marine IK (minified) | 1,243 | 658 | 1.89× |
| Marine IK (prettified) | 5,158 | 4,040 | 1.28× |
| Random (minified) | 14,174 | 4,018 | 3.53× |
| Random (prettified) | 14,815 | 4,469 | 3.31× |
| Twitter (minified) | 16,459 | 7,076 | 2.33× |
| Twitter (prettified) | 16,874 | 5,384 | 3.13× |
| Apache Builds Small (minified) | 11,125 | 3,767 | 2.95× |
| Canada Small (minified) | 1,947 | 1,004 | 1.94× |
| Apache Builds Small (prettified) | 12,715 | 3,711 | 3.43× |
| Canada Small (prettified) | 5,166 | 3,912 | 1.32× |
| CitmCatalog Small (minified) | 8,885 | 3,057 | 2.91× |
| CitmCatalog Small (prettified) | 10,911 | 3,063 | 3.56× |
| Discord Small (minified) | 11,559 | 3,477 | 3.32× |
| Discord Small (prettified) | 12,211 | 2,831 | 4.31× |
| Github Events Small (minified) | 12,183 | 3,681 | 3.31× |
| Google Maps Response Small (minified) | 11,470 | 3,842 | 2.99× |
| Github Events Small (prettified) | 12,486 | 4,136 | 3.02× |
| Google Maps Response Small (prettified) | 16,565 | 5,411 | 3.06× |
| Instruments Small (minified) | 12,887 | 2,551 | 5.05× |
| Instruments Small (prettified) | 15,255 | 3,141 | 4.86× |
| Marine IK Reverse Small (minified) | 1,509 | 839 | 1.80× |
| Marine IK Reverse Small (prettified) | 5,506 | 3,960 | 1.39× |
| Marine IK Small (minified) | 1,589 | 778 | 2.04× |
| Marine IK Small (prettified) | 5,572 | 3,895 | 1.43× |
| Random Small (minified) | 14,572 | 3,757 | 3.88× |
| Random Small (prettified) | 16,560 | 4,545 | 3.64× |
| Twitter Small (minified) | 9,759 | 4,166 | 2.34× |
| Twitter Small (prettified) | 10,558 | 3,957 | 2.67× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,477 | 2,394 | 1,781 | 1.45× |
| Prettify | 4,172 | 3,497 | 2,324 | 1.19× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,240 | 681 | 3,201 | 1.01× |
| Double | 974 | 693 | 942 | 1.03× |
| Int64 | 3,161 | 1,059 | 3,029 | 1.04× |
| String | 4,776 | 2,752 | 4,431 | 1.08× |
| Uint64 | 3,188 | 957 | 3,032 | 1.05× |
| Apache Builds (minified) | 16,298 | 9,421 | 14,028 | 1.16× |
| Canada (minified) | 2,115 | 1,535 | 1,608 | 1.32× |
| Apache Builds (prettified) | 19,000 | 8,042 | 631 | 2.36× |
| Canada (prettified) | 6,009 | 3,928 | 442 | 1.53× |
| CitmCatalog (minified) | 12,075 | 7,438 | 12,838 | 0.94× |
| CitmCatalog (prettified) | 18,536 | 7,828 | 565 | 2.37× |
| Discord (minified) | 13,954 | 7,344 | 12,975 | 1.08× |
| Discord (prettified) | 18,260 | 7,197 | 553 | 2.54× |
| Github Events (minified) | 13,638 | 8,230 | 11,559 | 1.18× |
| Google Maps Response (minified) | 13,648 | 5,078 | 10,234 | 1.33× |
| Github Events (prettified) | 16,670 | 5,766 | 649 | 2.89× |
| Google Maps Response (prettified) | 21,126 | 7,038 | 550 | 3.00× |
| Instruments (minified) | 18,679 | 6,264 | 16,332 | 1.14× |
| Instruments (prettified) | 25,366 | 6,348 | 475 | 4.00× |
| Marine IK Reverse (minified) | 1,326 | 1,002 | 1,366 | 0.97× |
| Marine IK Reverse (prettified) | 6,308 | 4,023 | 332 | 1.57× |
| Marine IK (minified) | 1,370 | 1,003 | 1,337 | 1.03× |
| Marine IK (prettified) | 6,428 | 4,077 | 377 | 1.58× |
| Random (minified) | 14,566 | 5,197 | 12,012 | 1.21× |
| Random (prettified) | 17,520 | 5,742 | 550 | 3.05× |
| Twitter (minified) | 15,742 | 8,746 | 12,762 | 1.23× |
| Twitter (prettified) | 18,954 | 6,564 | 531 | 2.89× |
| Apache Builds Small (minified) | 11,986 | 5,271 | 8,429 | 1.42× |
| Canada Small (minified) | 1,992 | 1,704 | 1,754 | 1.14× |
| Apache Builds Small (prettified) | 15,436 | 4,632 | 537 | 3.33× |
| Canada Small (prettified) | 6,602 | 4,006 | 475 | 1.65× |
| CitmCatalog Small (minified) | 8,838 | 4,021 | 7,693 | 1.15× |
| CitmCatalog Small (prettified) | 17,872 | 3,370 | 508 | 5.30× |
| Discord Small (minified) | 11,971 | 4,942 | 8,680 | 1.38× |
| Discord Small (prettified) | 17,217 | 3,361 | 515 | 5.12× |
| Github Events Small (minified) | 9,789 | 6,062 | 7,516 | 1.30× |
| Google Maps Response Small (minified) | 11,927 | 4,197 | 8,886 | 1.34× |
| Github Events Small (prettified) | 13,203 | 4,627 | 547 | 2.85× |
| Google Maps Response Small (prettified) | 27,418 | 6,820 | 517 | 4.02× |
| Instruments Small (minified) | 14,828 | 4,623 | 11,504 | 1.29× |
| Instruments Small (prettified) | 28,292 | 3,637 | 519 | 7.78× |
| Marine IK Reverse Small (minified) | 1,636 | 1,275 | 2,013 | 0.81× |
| Marine IK Reverse Small (prettified) | 6,501 | 3,839 | 548 | 1.69× |
| Marine IK Small (minified) | 1,491 | 1,284 | 1,934 | 0.77× |
| Marine IK Small (prettified) | 6,863 | 3,806 | 540 | 1.80× |
| Random Small (minified) | 14,233 | 4,934 | 9,356 | 1.52× |
| Random Small (prettified) | 24,366 | 4,976 | 526 | 4.90× |
| Twitter Small (minified) | 10,004 | 6,726 | 8,198 | 1.22× |
| Twitter Small (prettified) | 13,110 | 4,554 | 560 | 2.88× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,337 | 2,564 | 1,913 | 1.30× |
| Prettify | 4,282 | 3,194 | 2,463 | 1.34× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,461 | 765 | 2,706 | 1.28× |
| Double | 1,030 | 705 | 1,104 | 0.93× |
| Int64 | 2,664 | 1,152 | 2,786 | 0.96× |
| String | 6,518 | 2,570 | 5,457 | 1.19× |
| Uint64 | 2,739 | 1,090 | 2,821 | 0.97× |
| Apache Builds (minified) | 13,077 | 6,538 | 10,575 | 1.24× |
| Canada (minified) | 2,612 | 1,868 | 2,130 | 1.23× |
| Apache Builds (prettified) | 14,198 | 5,437 | 388 | 2.61× |
| Canada (prettified) | 7,988 | 4,105 | 351 | 1.95× |
| CitmCatalog (minified) | 8,765 | 2,349 | 9,194 | 0.95× |
| CitmCatalog (prettified) | 21,716 | 4,623 | 360 | 4.70× |
| Discord (minified) | 12,615 | 3,254 | 10,979 | 1.15× |
| Discord (prettified) | 14,543 | 2,800 | 361 | 5.19× |
| Github Events (minified) | 16,713 | 4,730 | 12,625 | 1.32× |
| Google Maps Response (minified) | 10,324 | 1,996 | 8,426 | 1.23× |
| Github Events (prettified) | 17,806 | 4,262 | 443 | 4.18× |
| Google Maps Response (prettified) | 21,147 | 3,290 | 341 | 6.43× |
| Instruments (minified) | 13,804 | 2,244 | 9,595 | 1.44× |
| Instruments (prettified) | 21,275 | 2,660 | 354 | 8.00× |
| Marine IK Reverse (minified) | 1,536 | 945 | 1,298 | 1.18× |
| Marine IK Reverse (prettified) | 7,254 | 3,448 | 322 | 2.10× |
| Marine IK (minified) | 1,483 | 910 | 1,256 | 1.18× |
| Marine IK (prettified) | 7,300 | 3,428 | 324 | 2.13× |
| Random (minified) | 10,618 | 2,533 | 8,819 | 1.20× |
| Random (prettified) | 18,491 | 2,940 | 346 | 6.29× |
| Twitter (minified) | 15,308 | 4,485 | 12,165 | 1.26× |
| Twitter (prettified) | 16,915 | 4,668 | 439 | 3.62× |
| Apache Builds Small (minified) | 9,624 | 3,365 | 7,874 | 1.22× |
| Canada Small (minified) | 2,581 | 2,003 | 2,133 | 1.21× |
| Apache Builds Small (prettified) | 11,088 | 4,710 | 372 | 2.35× |
| Canada Small (prettified) | 8,016 | 3,696 | 335 | 2.17× |
| CitmCatalog Small (minified) | 9,776 | 2,182 | 9,314 | 1.05× |
| CitmCatalog Small (prettified) | 18,421 | 2,856 | 349 | 6.45× |
| Discord Small (minified) | 12,663 | 2,865 | 10,145 | 1.25× |
| Discord Small (prettified) | 14,280 | 2,637 | 358 | 5.42× |
| Github Events Small (minified) | 13,990 | 3,812 | 11,664 | 1.20× |
| Google Maps Response Small (minified) | 10,026 | 1,993 | 9,093 | 1.10× |
| Github Events Small (prettified) | 14,678 (n/c) | 3,444 | 396 | 4.26× |
| Google Maps Response Small (prettified) | 19,276 | 3,174 | 333 | 6.07× |
| Instruments Small (minified) | 14,438 | 2,266 | 9,797 | 1.47× |
| Instruments Small (prettified) | 18,799 | 2,598 | 343 | 7.24× |
| Marine IK Reverse Small (minified) | 2,123 | 1,163 | 2,104 | 1.01× |
| Marine IK Reverse Small (prettified) | 9,277 | 3,788 | 323 | 2.45× |
| Marine IK Small (minified) | 2,097 | 1,164 | 2,121 | 0.99× |
| Marine IK Small (prettified) | 9,566 | 3,803 | 319 | 2.52× |
| Random Small (minified) | 13,164 | 2,549 | 9,874 | 1.33× |
| Random Small (prettified) | 19,424 | 2,984 | 344 | 6.51× |
| Twitter Small (minified) | 12,848 | 3,977 | 10,215 | 1.26× |
| Twitter Small (prettified) | 15,287 | 3,987 | 418 | 3.83× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,476 | 1,692 | 1,238 | 1.46× |
| Prettify | 3,321 | 2,421 | 1,630 | 1.37× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,266 | 764 | 1.66× |
| Double | 856 | 659 | 1.30× |
| Int64 | 1,422 | 808 | 1.76× |
| String | 2,787 | 1,911 | 1.46× |
| Uint64 | 1,378 | 723 | 1.91× |
| Apache Builds (minified) | 10,406 | 3,698 | 2.81× |
| Canada (minified) | 2,411 | 1,554 | 1.55× |
| Apache Builds (prettified) | 9,533 | 2,943 | 3.24× |
| Canada (prettified) | 5,793 | 3,268 | 1.77× |
| CitmCatalog (minified) | 7,031 | 1,787 | 3.93× |
| CitmCatalog (prettified) | 11,354 | 3,927 | 2.89× |
| Discord (minified) | 9,189 | 2,890 | 3.18× |
| Discord (prettified) | 9,401 | 2,517 | 3.73× |
| Github Events (minified) | 10,703 | 4,525 | 2.37× |
| Google Maps Response (minified) | 8,538 | 2,057 | 4.15× |
| Github Events (prettified) | 12,285 | 3,953 | 3.11× |
| Google Maps Response (prettified) | 10,639 | 3,945 | 2.70× |
| Instruments (minified) | 11,679 | 2,357 | 4.96× |
| Instruments (prettified) | 12,272 | 3,273 | 3.75× |
| Marine IK Reverse (minified) | 1,292 | 780 | 1.66× |
| Marine IK Reverse (prettified) | 4,761 | 3,061 | 1.56× |
| Marine IK (minified) | 1,471 | 798 | 1.84× |
| Marine IK (prettified) | 5,203 | 2,973 | 1.75× |
| Random (minified) | 9,680 | 2,744 | 3.53× |
| Random (prettified) | 11,574 | 3,102 | 3.73× |
| Twitter (minified) | 14,244 | 5,033 | 2.83× |
| Twitter (prettified) | 13,527 | 4,792 | 2.82× |
| Apache Builds Small (minified) | 8,086 | 3,624 | 2.23× |
| Canada Small (minified) | 2,763 | 1,824 | 1.52× |
| Apache Builds Small (prettified) | 9,489 | 3,719 | 2.55× |
| Canada Small (prettified) | 6,480 | 3,907 | 1.66× |
| CitmCatalog Small (minified) | 9,966 | 2,192 | 4.55× |
| CitmCatalog Small (prettified) | 13,082 | 3,258 | 4.01× |
| Discord Small (minified) | 11,159 | 3,721 | 3.00× |
| Discord Small (prettified) | 11,697 | 3,313 | 3.53× |
| Github Events Small (minified) | 11,713 | 3,809 | 3.07× |
| Google Maps Response Small (minified) | 7,131 | 2,415 | 2.95× |
| Github Events Small (prettified) | 13,370 | 4,390 | 3.05× |
| Google Maps Response Small (prettified) | 13,786 | 4,022 | 3.43× |
| Instruments Small (minified) | 12,144 | 2,178 | 5.58× |
| Instruments Small (prettified) | 15,157 | 3,163 | 4.79× |
| Marine IK Reverse Small (minified) | 2,087 | 1,094 | 1.91× |
| Marine IK Reverse Small (prettified) | 5,086 | 3,135 | 1.62× |
| Marine IK Small (minified) | 1,860 | 960 | 1.94× |
| Marine IK Small (prettified) | 4,487 | 2,995 | 1.50× |
| Random Small (minified) | 8,520 | 2,756 | 3.09× |
| Random Small (prettified) | 10,168 | 3,108 | 3.27× |
| Twitter Small (minified) | 8,884 | 4,226 | 2.10× |
| Twitter Small (prettified) | 10,308 | 3,442 | 2.99× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,814 | 1,357 | 938 | 1.34× |
| Prettify | 2,675 | 2,069 | 1,205 | 1.29× |

### Losses and ties

The ten freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 5,002 vs Jsonifier's 3,893 MB/s (0.78×). Reused, Jsonifier wins by 2.69× (15,136 vs 5,619).
- **Prettify, Windows / MSVC.** Glaze runs 2,599 vs 2,468 MB/s (0.95×), with simdjson On Demand at 1,435. All three converged. Reused, Jsonifier wins by 1.53× (3,987 vs 2,602).
- **Double, Linux / GCC.** The reflection writer runs 411 vs 392 MB/s (0.95×), with Glaze at 326. Both write the same 1,997 bytes, so this is a per-call loss too (4,633 vs 4,857 ns). Reused, Jsonifier wins by 1.03× over the reflection writer (974 vs 942).
- **Int64, Linux / GCC.** The reflection writer runs 370 vs 351 MB/s (0.95×), with Glaze at 317. All three write the same 1,313 bytes, so this is a per-call loss too. Reused, Jsonifier wins by 1.04× over the reflection writer (3,161 vs 3,029).
- **CitmCatalog (minified), Linux / GCC.** The reflection writer runs 12,775 vs 11,978 MB/s (0.94×), with Glaze at 6,266. It also wins reused, 12,838 vs 12,075 MB/s (0.94×).
- **Marine IK Reverse (minified), Linux / GCC.** The reflection writer runs 1,364 vs 1,332 MB/s (0.98×), with Glaze at 1,005. It also wins reused, 1,366 vs 1,326 MB/s (0.97×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,978 vs 1,597 MB/s (0.81×), with Glaze at 1,247. It also wins reused, 2,013 vs 1,636 MB/s (0.81×).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,922 vs 1,456 MB/s (0.76×), with Glaze at 1,256. It also wins reused, 1,934 vs 1,491 MB/s (0.77×).
- **Double, macOS / GCC.** The reflection writer runs 340 vs 321 MB/s (0.94×), with Glaze at 217, and with the same 1,997 bytes it is faster per call as well. It also wins reused, 1,104 vs 1,030 MB/s (0.93×).
- **CitmCatalog (minified), macOS / GCC.** The reflection writer runs 9,140 vs 8,718 MB/s (0.95×), with Glaze at 2,226. It also wins reused, 9,194 vs 8,765 MB/s (0.95×).

The four freshly allocated ties are against the reflection writer on macOS / GCC: Uint64 (284 vs 282 MB/s), and minified CitmCatalog Small (8,958 vs 8,375 MB/s, with RSEs of 2.0% and 3.7%), Github Events Small (10,965 vs 10,413 MB/s, with RSEs of 6.1% and 5.9%) and Marine IK Reverse Small (2,096 vs 2,090 MB/s). Reused, Uint64 and CitmCatalog Small tie again, and Github Events Small and Marine IK Reverse Small become wins.

Two reused ties have no freshly allocated counterpart, both on macOS / GCC against the reflection writer. Int64 wins freshly allocated by 1.04× (308 vs 297 MB/s) and ties reused (2,664 vs 2,786 MB/s). Minified Marine IK Small wins freshly allocated and ties reused (2,097 vs 2,121 MB/s).

On the two GCC builds the reflection writer takes three full-size documents in both runs: minified CitmCatalog on both builds and minified Marine IK Reverse on Linux / GCC. Every other full-size document is ahead of it. Freshly allocated, minified documents lead it by up to 1.49× and prettified documents by 14–61×; reused, by up to 1.44× and 14–62×. On the Small documents Jsonifier beats the reflection writer on 39 of the 44 tests freshly allocated, with 3 ties and 2 losses, and on 40 reused, with 2 ties and 2 losses. The losses are minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, in both runs. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.26–1.66× freshly allocated and 1.30–1.67× reused. It wins Prettify on all five reused, by 1.19–1.53×, and on four of five freshly allocated, by 1.19–1.38×; the exception is Windows / MSVC, where Glaze runs 2,599 vs 2,468 MB/s (0.95×). Glaze is the next-fastest library in every case.

## AVX-512 builds

The results above come from AVX2 and NEON builds. The same 51 write tests also ran on three AVX-512 builds, Linux / Clang, Linux / GCC and Windows / MSVC, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor. As on AVX2, simdjson's reflection writer runs only on the Linux / GCC build.

Freshly allocated, Jsonifier was fastest in 149 of the 153 write tests and lost 4. Two are the same two that it loses on the AVX2 Windows / MSVC build: minified Twitter (Glaze 4,995 vs 3,592 MB/s, 0.72×) and Prettify (Glaze 3,188 vs 2,464 MB/s, 0.77×, with simdjson On Demand at 1,490). The other two are against the reflection writer on Linux / GCC: Int64 (388 vs 375 MB/s, 0.97×) and Uint64 (351 vs 348 MB/s, 0.99×). Reused, it was fastest in 150 and lost 3, all on Linux / GCC against the reflection writer: Int64 (2,191 vs 2,168 MB/s, 0.99×), Uint64 (2,489 vs 2,323 MB/s, 0.93×) and minified CitmCatalog Small (9,703 vs 8,949 MB/s, 0.92×). Both Glaze losses become wins reused: minified Twitter by 2.36× (13,099 vs 5,548 MB/s) and Prettify by 1.35× (4,394 vs 3,266). Linux / Clang wins all 51 tests in both runs, and Windows / MSVC wins all 51 reused. Freshly allocated, prettified full-size documents lead Glaze by 1.43–8.35× and prettified Small documents by 1.51–5.97×; reused, by 1.34–7.89× and 1.41–5.92×. On Linux / GCC, freshly allocated prettified documents lead the reflection writer by 13.24–66.00× full-size and 12.30–50.45× Small. Minify leads Glaze by 1.40× on Linux / Clang, 1.66× on Linux / GCC and 2.20× on Windows / MSVC freshly allocated, and by 1.41×, 1.70× and 2.03× reused.

### AVX-512 method

These results come from the Json-Performance branch `benchmarking-avx512-main`, with the same commits as above (Jsonifier [4f52e70](https://github.com/nihilai-collective/jsonifier/commit/4f52e70) [5], Glaze [fc0b48a0](https://github.com/stephenberry/glaze/commit/fc0b48a0) [6], simdjson [344898409](https://github.com/simdjson/simdjson/commit/344898409) [7], BenchmarkSuite [71afc62](https://github.com/nihilai-collective/benchmarksuite/commit/71afc62) [8]), run November 23, 2026. The tests, sampling, ranking and convergence limits (RSE under 5%, mean shift under 2.5%) are the same as for the x86 builds above.

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
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 49 | 0 | 2 |
| **Total** | **153** | **149** | **0** | **4** |

### Windows / MSVC 19.52 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 880 | 88 | 9.95× |
| Double | 422 | 143 | 2.96× |
| Int64 | 556 | 134 | 4.14× |
| String | 567 | 416 | 1.36× |
| Uint64 | 560 | 120 | 4.66× |
| Apache Builds (minified) | 11,372 | 4,684 | 2.43× |
| Canada (minified) | 978 | 654 | 1.50× |
| Apache Builds (prettified) | 10,759 | 3,857 | 2.79× |
| Canada (prettified) | 2,105 | 1,129 | 1.87× |
| CitmCatalog (minified) | 9,359 | 3,049 | 3.07× |
| CitmCatalog (prettified) | 2,950 (n/c) | 2,061 | 1.43× |
| Discord (minified) | 10,714 | 2,998 | 3.57× |
| Discord (prettified) | 11,047 | 3,157 | 3.50× |
| Github Events (minified) | 11,249 | 4,910 | 2.29× |
| Google Maps Response (minified) | 7,107 | 2,688 | 2.64× |
| Github Events (prettified) | 10,723 | 4,275 | 2.51× |
| Google Maps Response (prettified) | 10,916 | 3,806 | 2.87× |
| Instruments (minified) | 15,138 | 2,939 | 5.15× |
| Instruments (prettified) | 13,142 | 3,452 | 3.81× |
| Marine IK Reverse (minified) | 674 | 467 | 1.44× |
| Marine IK Reverse (prettified) | 2,081 | 1,425 (n/c) | 1.46× |
| Marine IK (minified) | 677 | 475 | 1.42× |
| Marine IK (prettified) | 2,234 | 1,081 | 2.07× |
| Random (minified) | 3,185 | 2,609 | 1.22× |
| Random (prettified) | 3,797 | 2,004 | 1.89× |
| Twitter (minified) | 3,592 | 4,995 | 0.72× |
| Twitter (prettified) | 3,576 | 2,430 | 1.47× |
| Apache Builds Small (minified) | 8,127 | 3,392 | 2.40× |
| Canada Small (minified) | 1,502 | 939 | 1.60× |
| Apache Builds Small (prettified) | 9,416 | 3,403 | 2.77× |
| Canada Small (prettified) | 4,416 | 2,576 | 1.71× |
| CitmCatalog Small (minified) | 6,147 | 2,744 | 2.24× |
| CitmCatalog Small (prettified) | 7,002 | 3,811 | 1.84× |
| Discord Small (minified) | 9,688 | 3,134 | 3.09× |
| Discord Small (prettified) | 8,531 | 2,990 | 2.85× |
| Github Events Small (minified) | 10,700 | 3,630 | 2.95× |
| Google Maps Response Small (minified) | 7,627 | 2,877 | 2.65× |
| Github Events Small (prettified) | 10,058 | 3,287 | 3.06× |
| Google Maps Response Small (prettified) | 11,678 | 3,784 | 3.09× |
| Instruments Small (minified) | 14,951 | 2,904 | 5.15× |
| Instruments Small (prettified) | 11,563 | 3,471 | 3.33× |
| Marine IK Reverse Small (minified) | 1,187 | 770 | 1.54× |
| Marine IK Reverse Small (prettified) | 4,455 | 2,941 | 1.51× |
| Marine IK Small (minified) | 1,104 | 761 | 1.45× |
| Marine IK Small (prettified) | 4,354 | 2,776 | 1.57× |
| Random Small (minified) | 9,155 | 2,898 | 3.16× |
| Random Small (prettified) | 11,509 | 3,228 | 3.57× |
| Twitter Small (minified) | 8,183 | 4,160 | 1.97× |
| Twitter Small (prettified) | 7,535 | 3,558 | 2.12× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,941 | 1,789 | 1,371 | 2.20× |
| Prettify | 2,464 | 3,188 | 1,490 | 0.77× |

### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,064 | 183 | 11.28× |
| Double | 327 | 245 | 1.34× |
| Int64 | 294 | 246 | 1.20× |
| String | 1,590 | 781 | 2.04× |
| Uint64 | 284 | 228 | 1.24× |
| Apache Builds (minified) | 16,259 | 4,922 | 3.30× |
| Canada (minified) | 1,769 | 858 | 2.06× |
| Apache Builds (prettified) | 17,098 | 5,496 | 3.11× |
| Canada (prettified) | 4,999 | 2,738 | 1.83× |
| CitmCatalog (minified) | 9,008 | 3,491 | 2.58× |
| CitmCatalog (prettified) | 15,053 | 4,506 | 3.34× |
| Discord (minified) | 13,317 | 3,326 | 4.00× |
| Discord (prettified) | 15,626 | 2,939 | 5.32× |
| Github Events (minified) | 14,010 | 5,194 | 2.70× |
| Google Maps Response (minified) | 10,732 | 2,854 | 3.76× |
| Github Events (prettified) | 14,085 | 4,639 | 3.04× |
| Google Maps Response (prettified) | 15,898 | 4,735 | 3.36× |
| Instruments (minified) | 15,441 | 3,495 | 4.42× |
| Instruments (prettified) | 19,557 | 3,750 | 5.22× |
| Marine IK Reverse (minified) | 1,160 | 618 | 1.88× |
| Marine IK Reverse (prettified) | 4,826 | 3,188 | 1.51× |
| Marine IK (minified) | 1,159 | 578 | 2.00× |
| Marine IK (prettified) | 4,613 | 2,392 (n/c) | 1.93× |
| Random (minified) | 12,531 | 2,791 | 4.49× |
| Random (prettified) | 15,184 | 3,334 | 4.55× |
| Twitter (minified) | 15,010 | 5,079 | 2.96× |
| Twitter (prettified) | 14,638 | 4,157 | 3.52× |
| Apache Builds Small (minified) | 10,417 | 3,547 | 2.94× |
| Canada Small (minified) | 1,757 | 872 | 2.02× |
| Apache Builds Small (prettified) | 11,835 | 3,904 | 3.03× |
| Canada Small (prettified) | 4,948 | 2,855 | 1.73× |
| CitmCatalog Small (minified) | 8,236 | 2,587 | 3.18× |
| CitmCatalog Small (prettified) | 9,636 | 3,614 | 2.67× |
| Discord Small (minified) | 10,487 | 3,152 | 3.33× |
| Discord Small (prettified) | 10,957 | 3,007 | 3.64× |
| Github Events Small (minified) | 11,365 | 4,127 | 2.75× |
| Google Maps Response Small (minified) | 10,268 | 2,475 | 4.15× |
| Github Events Small (prettified) | 14,009 | 3,869 | 3.62× |
| Google Maps Response Small (prettified) | 15,248 | 4,591 | 3.32× |
| Instruments Small (minified) | 13,830 | 3,475 | 3.98× |
| Instruments Small (prettified) | 16,152 | 3,604 | 4.48× |
| Marine IK Reverse Small (minified) | 1,439 | 745 | 1.93× |
| Marine IK Reverse Small (prettified) | 5,325 | 3,364 | 1.58× |
| Marine IK Small (minified) | 1,455 | 754 | 1.93× |
| Marine IK Small (prettified) | 5,170 | 3,391 | 1.52× |
| Random Small (minified) | 10,993 | 2,855 | 3.85× |
| Random Small (prettified) | 15,605 | 3,604 | 4.33× |
| Twitter Small (minified) | 10,252 | 4,751 | 2.16× |
| Twitter Small (prettified) | 8,795 | 3,657 | 2.41× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,443 | 2,463 | 1,664 | 1.40× |
| Prettify | 5,444 | 3,365 | 2,163 | 1.62× |

### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,107 | 210 | 260 | 8.12× |
| Double | 386 | 292 | 332 | 1.16× |
| Int64 | 375 | 320 | 388 | 0.97× |
| String | 1,756 | 1,024 | 1,600 | 1.10× |
| Uint64 | 348 | 299 | 351 | 0.99× |
| Apache Builds (minified) | 22,855 | 6,305 | 14,816 | 1.54× |
| Canada (minified) | 1,924 | 1,390 | 1,265 | 1.38× |
| Apache Builds (prettified) | 29,400 | 6,374 | 633 | 4.61× |
| Canada (prettified) | 6,001 | 3,298 | 453 | 1.82× |
| CitmCatalog (minified) | 10,241 | 4,352 | 9,950 | 1.03× |
| CitmCatalog (prettified) | 26,130 | 5,430 | 603 | 4.81× |
| Discord (minified) | 16,862 | 5,697 | 14,095 | 1.20× |
| Discord (prettified) | 24,829 | 5,433 | 544 | 4.57× |
| Github Events (minified) | 12,857 | 7,086 | 11,617 | 1.11× |
| Google Maps Response (minified) | 15,138 | 3,944 | 12,137 | 1.25× |
| Github Events (prettified) | 17,324 | 5,582 | 680 | 3.10× |
| Google Maps Response (prettified) | 31,144 | 5,836 | 588 | 5.34× |
| Instruments (minified) | 18,204 | 2,451 | 15,015 | 1.21× |
| Instruments (prettified) | 33,874 | 4,057 | 513 | 8.35× |
| Marine IK Reverse (minified) | 1,259 | 967 | 1,092 | 1.15× |
| Marine IK Reverse (prettified) | 6,334 | 3,776 | 242 | 1.68× |
| Marine IK (minified) | 1,271 | 929 (n/c) | 1,083 | 1.17× |
| Marine IK (prettified) | 6,319 | 3,311 | 202 (n/c) | 1.91× |
| Random (minified) | 13,911 | 3,420 | 10,653 | 1.31× |
| Random (prettified) | 23,267 | 3,623 | 574 | 6.42× |
| Twitter (minified) | 14,232 | 6,092 | 11,304 | 1.26× |
| Twitter (prettified) | 18,634 | 4,573 | 584 | 4.07× |
| Apache Builds Small (minified) | 11,603 | 4,445 | 9,001 | 1.29× |
| Canada Small (minified) | 1,938 | 1,484 | 1,339 | 1.31× |
| Apache Builds Small (prettified) | 14,311 | 4,570 | 585 | 3.13× |
| Canada Small (prettified) | 6,114 | 3,420 | 497 | 1.79× |
| CitmCatalog Small (minified) | 7,917 | 3,101 | 7,704 | 1.03× |
| CitmCatalog Small (prettified) | 14,206 | 3,931 | 539 | 3.61× |
| Discord Small (minified) | 12,465 | 4,173 | 9,143 | 1.36× |
| Discord Small (prettified) | 16,644 | 4,311 | 546 | 3.86× |
| Github Events Small (minified) | 13,504 | 6,097 | 11,732 | 1.15× |
| Google Maps Response Small (minified) | 13,794 | 3,741 | 11,382 | 1.21× |
| Github Events Small (prettified) | 15,902 | 4,700 | 592 | 3.38× |
| Google Maps Response Small (prettified) | 29,331 | 5,628 | 581 | 5.21× |
| Instruments Small (minified) | 11,972 | 1,989 | 9,286 | 1.29× |
| Instruments Small (prettified) | 20,949 | 3,524 | 536 | 5.94× |
| Marine IK Reverse Small (minified) | 1,677 | 1,031 | 1,499 | 1.12× |
| Marine IK Reverse Small (prettified) | 7,930 | 3,732 | 537 | 2.13× |
| Marine IK Small (minified) | 1,626 | 1,030 | 1,448 | 1.12× |
| Marine IK Small (prettified) | 7,522 | 3,786 | 545 | 1.99× |
| Random Small (minified) | 14,722 | 3,633 | 12,552 | 1.17× |
| Random Small (prettified) | 24,017 | 4,025 | 556 | 5.97× |
| Twitter Small (minified) | 11,594 | 5,287 | 9,266 | 1.25× |
| Twitter Small (prettified) | 15,999 | 3,822 | 584 | 4.19× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 4,182 | 2,521 | 1,966 | 1.66× |
| Prettify | 5,376 | 3,322 | 2,728 | 1.62× |

### Reused output strings

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 48 | 0 | 3 |
| **Total** | **153** | **150** | **0** | **3** |

#### Windows / MSVC 19.52 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,401 | 510 | 2.75× |
| Double | 536 | 374 | 1.43× |
| Int64 | 1,295 | 686 | 1.89× |
| String | 3,302 | 1,853 | 1.78× |
| Uint64 | 1,347 | 625 | 2.15× |
| Apache Builds (minified) | 11,629 | 5,078 | 2.29× |
| Canada (minified) | 1,314 | 919 | 1.43× |
| Apache Builds (prettified) | 11,057 | 4,236 | 2.61× |
| Canada (prettified) | 3,841 | 2,703 | 1.42× |
| CitmCatalog (minified) | 8,970 | 3,479 | 2.58× |
| CitmCatalog (prettified) | 12,714 | 5,976 | 2.13× |
| Discord (minified) | 11,444 | 3,201 | 3.58× |
| Discord (prettified) | 11,326 | 3,460 | 3.27× |
| Github Events (minified) | 11,663 | 5,304 | 2.20× |
| Google Maps Response (minified) | 7,786 | 3,072 | 2.53× |
| Github Events (prettified) | 11,343 | 5,012 | 2.26× |
| Google Maps Response (prettified) | 11,576 | 4,229 | 2.74× |
| Instruments (minified) | 15,788 | 3,168 | 4.98× |
| Instruments (prettified) | 13,520 | 3,774 | 3.58× |
| Marine IK Reverse (minified) | 842 | 571 | 1.47× |
| Marine IK Reverse (prettified) | 3,633 | 2,710 | 1.34× |
| Marine IK (minified) | 847 | 575 | 1.47× |
| Marine IK (prettified) | 3,777 | 2,705 (n/c) | 1.40× |
| Random (minified) | 10,338 | 2,722 | 3.80× |
| Random (prettified) | 11,964 | 3,689 | 3.24× |
| Twitter (minified) | 13,099 | 5,548 | 2.36× |
| Twitter (prettified) | 10,864 | 5,340 | 2.03× |
| Apache Builds Small (minified) | 9,321 | 4,132 | 2.26× |
| Canada Small (minified) | 1,580 | 961 | 1.64× |
| Apache Builds Small (prettified) | 9,572 | 3,760 | 2.55× |
| Canada Small (prettified) | 4,575 | 2,740 | 1.67× |
| CitmCatalog Small (minified) | 10,213 | 3,110 | 3.28× |
| CitmCatalog Small (prettified) | 8,703 | 4,420 | 1.97× |
| Discord Small (minified) | 5,958 | 3,732 | 1.60× |
| Discord Small (prettified) | 8,800 | 3,596 | 2.45× |
| Github Events Small (minified) | 13,245 | 4,349 | 3.05× |
| Google Maps Response Small (minified) | 8,010 | 2,060 | 3.89× |
| Github Events Small (prettified) | 12,072 | 4,147 | 2.91× |
| Google Maps Response Small (prettified) | 13,528 | 4,534 | 2.98× |
| Instruments Small (minified) | 16,166 | 3,410 | 4.74× |
| Instruments Small (prettified) | 12,307 | 4,046 | 3.04× |
| Marine IK Reverse Small (minified) | 1,233 | 809 | 1.52× |
| Marine IK Reverse Small (prettified) | 4,614 | 3,209 | 1.44× |
| Marine IK Small (minified) | 1,171 | 797 | 1.47× |
| Marine IK Small (prettified) | 4,526 | 3,191 | 1.42× |
| Random Small (minified) | 10,975 | 3,266 | 3.36× |
| Random Small (prettified) | 12,838 | 3,995 | 3.21× |
| Twitter Small (minified) | 10,126 | 4,724 | 2.14× |
| Twitter Small (prettified) | 7,802 | 4,556 | 1.71× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,652 | 1,801 | 1,375 | 2.03× |
| Prettify | 4,394 | 3,266 | 1,517 | 1.35× |

#### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 3,677 | 894 | 4.11× |
| Double | 857 | 492 | 1.74× |
| Int64 | 2,108 | 1,026 | 2.05× |
| String | 5,317 | 2,128 | 2.50× |
| Uint64 | 1,977 | 906 | 2.18× |
| Apache Builds (minified) | 16,568 | 5,404 | 3.07× |
| Canada (minified) | 1,782 | 891 | 2.00× |
| Apache Builds (prettified) | 17,237 | 5,907 | 2.92× |
| Canada (prettified) | 4,915 | 3,080 | 1.60× |
| CitmCatalog (minified) | 8,928 | 3,826 | 2.33× |
| CitmCatalog (prettified) | 14,874 | 5,357 | 2.78× |
| Discord (minified) | 13,810 | 3,559 | 3.88× |
| Discord (prettified) | 14,908 | 3,110 | 4.79× |
| Github Events (minified) | 14,455 | 5,962 | 2.42× |
| Google Maps Response (minified) | 11,136 | 3,094 | 3.60× |
| Github Events (prettified) | 14,858 | 5,216 | 2.85× |
| Google Maps Response (prettified) | 16,245 | 5,124 | 3.17× |
| Instruments (minified) | 15,454 | 3,621 | 4.27× |
| Instruments (prettified) | 19,693 | 4,092 | 4.81× |
| Marine IK Reverse (minified) | 1,182 | 611 | 1.93× |
| Marine IK Reverse (prettified) | 4,734 | 3,516 | 1.35× |
| Marine IK (minified) | 1,183 | 609 | 1.94× |
| Marine IK (prettified) | 4,745 | 3,433 | 1.38× |
| Random (minified) | 12,421 | 2,973 | 4.18× |
| Random (prettified) | 15,343 | 3,588 | 4.28× |
| Twitter (minified) | 14,908 | 5,793 | 2.57× |
| Twitter (prettified) | 14,743 | 4,831 | 3.05× |
| Apache Builds Small (minified) | 11,064 | 3,802 | 2.91× |
| Canada Small (minified) | 1,745 | 921 | 1.89× |
| Apache Builds Small (prettified) | 12,527 | 4,378 | 2.86× |
| Canada Small (prettified) | 5,036 | 3,180 | 1.58× |
| CitmCatalog Small (minified) | 9,031 | 2,761 | 3.27× |
| CitmCatalog Small (prettified) | 11,224 | 3,976 | 2.82× |
| Discord Small (minified) | 11,415 | 3,606 | 3.17× |
| Discord Small (prettified) | 13,125 | 3,216 | 4.08× |
| Github Events Small (minified) | 12,252 | 4,586 | 2.67× |
| Google Maps Response Small (minified) | 10,637 | 2,647 | 4.02× |
| Github Events Small (prettified) | 15,864 | 4,466 | 3.55× |
| Google Maps Response Small (prettified) | 16,141 | 5,021 | 3.21× |
| Instruments Small (minified) | 14,514 | 3,700 | 3.92× |
| Instruments Small (prettified) | 17,667 | 3,900 | 4.53× |
| Marine IK Reverse Small (minified) | 1,507 | 757 | 1.99× |
| Marine IK Reverse Small (prettified) | 5,502 | 3,753 | 1.47× |
| Marine IK Small (minified) | 1,465 | 756 | 1.94× |
| Marine IK Small (prettified) | 5,159 | 3,667 | 1.41× |
| Random Small (minified) | 12,565 | 3,129 | 4.02× |
| Random Small (prettified) | 16,423 | 4,161 | 3.95× |
| Twitter Small (minified) | 12,804 | 4,812 | 2.66× |
| Twitter Small (prettified) | 10,685 | 4,151 | 2.57× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,457 | 2,449 | 1,637 | 1.41× |
| Prettify | 5,114 | 3,224 | 2,227 | 1.59× |

#### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,293 | 1,122 | 2,494 | 1.32× |
| Double | 930 | 599 | 764 | 1.22× |
| Int64 | 2,168 | 1,398 | 2,191 | 0.99× |
| String | 4,598 | 2,731 | 4,210 | 1.09× |
| Uint64 | 2,323 | 1,387 | 2,489 | 0.93× |
| Apache Builds (minified) | 22,828 | 6,823 | 15,012 | 1.52× |
| Canada (minified) | 1,930 | 1,421 | 1,262 | 1.36× |
| Apache Builds (prettified) | 29,676 | 6,908 | 631 | 4.30× |
| Canada (prettified) | 6,000 | 3,564 | 454 | 1.68× |
| CitmCatalog (minified) | 10,146 | 4,869 | 10,014 | 1.01× |
| CitmCatalog (prettified) | 26,190 | 6,228 | 600 | 4.21× |
| Discord (minified) | 16,328 | 6,380 | 14,865 | 1.10× |
| Discord (prettified) | 26,279 | 6,043 | 542 | 4.35× |
| Github Events (minified) | 13,868 | 8,023 | 11,596 | 1.20× |
| Google Maps Response (minified) | 15,800 | 4,294 | 12,451 | 1.27× |
| Github Events (prettified) | 17,687 | 6,366 | 686 | 2.78× |
| Google Maps Response (prettified) | 33,155 | 6,411 | 585 | 5.17× |
| Instruments (minified) | 17,148 | 2,548 | 14,263 | 1.20× |
| Instruments (prettified) | 34,198 | 4,334 | 517 | 7.89× |
| Marine IK Reverse (minified) | 1,259 | 942 | 1,069 | 1.18× |
| Marine IK Reverse (prettified) | 6,345 | 3,968 | 277 | 1.60× |
| Marine IK (minified) | 1,272 | 964 | 1,085 | 1.17× |
| Marine IK (prettified) | 6,356 | 3,972 | 299 (n/c) | 1.60× |
| Random (minified) | 14,297 | 3,750 | 10,716 | 1.33× |
| Random (prettified) | 23,339 | 4,184 | 576 | 5.58× |
| Twitter (minified) | 14,297 | 7,060 | 11,260 | 1.27× |
| Twitter (prettified) | 18,801 | 5,312 | 595 | 3.54× |
| Apache Builds Small (minified) | 12,997 | 5,079 | 10,334 | 1.26× |
| Canada Small (minified) | 1,972 | 1,538 | 1,354 | 1.28× |
| Apache Builds Small (prettified) | 15,950 | 4,764 | 559 | 3.35× |
| Canada Small (prettified) | 6,233 | 3,808 | 499 | 1.64× |
| CitmCatalog Small (minified) | 8,949 | 3,297 | 9,703 | 0.92× |
| CitmCatalog Small (prettified) | 17,669 | 4,209 | 559 | 4.20× |
| Discord Small (minified) | 14,175 | 4,903 | 10,118 | 1.40× |
| Discord Small (prettified) | 18,970 | 4,585 | 542 | 4.14× |
| Github Events Small (minified) | 15,245 | 6,635 | 13,865 | 1.10× |
| Google Maps Response Small (minified) | 14,603 | 3,914 | 11,966 | 1.22× |
| Github Events Small (prettified) | 19,328 | 5,774 | 591 | 3.35× |
| Google Maps Response Small (prettified) | 31,298 | 6,376 | 581 | 4.91× |
| Instruments Small (minified) | 15,279 | 2,084 | 9,493 | 1.61× |
| Instruments Small (prettified) | 22,883 | 3,865 | 517 | 5.92× |
| Marine IK Reverse Small (minified) | 1,708 | 1,131 | 1,492 | 1.14× |
| Marine IK Reverse Small (prettified) | 7,747 | 4,181 | 539 | 1.85× |
| Marine IK Small (minified) | 1,634 | 1,071 | 1,475 | 1.11× |
| Marine IK Small (prettified) | 7,638 | 3,968 | 563 | 1.92× |
| Random Small (minified) | 16,824 | 4,148 | 13,574 | 1.24× |
| Random Small (prettified) | 26,366 | 4,583 | 580 | 5.75× |
| Twitter Small (minified) | 12,837 | 5,324 | 10,712 | 1.20× |
| Twitter Small (prettified) | 18,980 | 4,597 | 583 | 4.13× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 4,311 | 2,543 | 1,970 | 1.70× |
| Prettify | 5,390 | 3,157 | 2,587 | 1.71× |

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

Freshly allocated, Jsonifier runs String at 558 MB/s under Windows / MSVC (1.20× Glaze's 464), 1,567 and 1,641 MB/s on Linux / Clang and Linux / GCC (1.77× and 1.74×), and 857 and 645 MB/s on macOS / GCC and macOS / Clang (1.54× and 1.44×). With the string reused the spread changes shape: macOS / GCC is the fastest build at 6,518 MB/s (2.54× Glaze, and 1.19× the reflection writer's 5,457), the three x86 builds run 4,753 to 5,349 MB/s (1.74× to 3.07× Glaze), and macOS / Clang is the slowest at 2,787 MB/s (1.46×).

So much of the freshly allocated gap sits in the allocation and release of the output string, which the reused run removes, and what remains on macOS / Clang is a compiler effect rather than an instruction-set one, since macOS / GCC runs the same NEON code more than twice as fast. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated, yes on both GCC builds, by the ranking and per call. Jsonifier and the reflection writer write the same 1,997 bytes, so MB/s and per-call time agree: 411 vs 392 MB/s on Linux / GCC (0.95×, 4,633 vs 4,857 ns) and 340 vs 321 MB/s on macOS / GCC (0.94×, 5,594 vs 5,941 ns). Reused, Jsonifier leads it on Linux / GCC (974 vs 942 MB/s, 1.03×) and trails on macOS / GCC (1,030 vs 1,104 MB/s, 0.93×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
