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
    D -- "POD values inlined, containers outlined" --> E["Member headers<br/>one packed store each"]
    D --> F["Strings<br/>SIMD, SWAR, scalar"]
    D --> G["Integers<br/>4 digits per store"]
    D --> H["Floats<br/>zmij shortest round-trip"]
```

The `serialize<options>` dispatcher is a force-inlined (`JSONIFIER_INLINE`) forwarder for every value type: it strips cv and reference qualifiers and calls that type's `serialize_impl` specialization directly. Each registered type gets its own `serialize_impl` specialization: objects, maps, vectors, fixed arrays, tuples, strings, chars, enums, numbers, bools, pointers and optionals, raw JSON, null, and variants (via `visit`).

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
	JSONIFIER_INLINE uint64_t operator()(write_buffer_ptr ptrNew, uint64_t) noexcept {
		const write_buffer_ptr bufferPtr = serialize<options>::impl(object, ptrNew, 0);
		return static_cast<uint64_t>(bufferPtr - ptrNew);
	}
	value_type& object;
};
```

- **`operator()` is force-inlined.** `JSONIFIER_INLINE` makes the compiler expand the serializer into the standard library's call site, instead of leaving an out-of-line function that the library calls. The write pass then runs as part of `resize_and_overwrite`'s own body, with the buffer pointer in a register and no extra call frame. Whether `resize_and_overwrite` itself inlines into `serializeJson` is still the compiler's choice.
- **The functor is a reference and a function.** It holds only `value_type& object`, and its copy and move operations are deleted, so passing it costs nothing and the object is never copied.
- **The length comes straight from the pointer.** `operator()` returns the end pointer minus the start, so the final size is set from the write itself, with no second pass and no separate size variable.

Buffers without `resize_and_overwrite` take a fallback: `resize` to the bound if the buffer is smaller, write, then `resize` down to the written length. That path zero-fills on growth.

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD register, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

### Schema text as packed integers

`packed_blitter` turns `"key":` into one constant at compile time. Literals up to 8 bytes become a single 2, 4 or 8-byte integer; longer ones become an array of `uint64_t` rounded up to 16 or a multiple of 32. Writing a member header is one `pow2MemcpyWrapper` store and a pointer bump by the true length.

### Member iteration: CABERIHT

Object members are walked with CABERIHT, Constexpresh Aggregated Bases for Efficient Runtime Iteration of Heterogeneous Types. It is CAFBERIHT without the filter; the full pattern, with compile-time filtering, is described in [its own paper](https://nihilai-collective.net/cafberiht). Each member becomes a base class, and one fold expression over those bases writes the whole object in declaration order.

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
| Compile-time 2 / 4 / 8 bytes of constant data | one immediate store, no load (`mov qword ptr [rdi], imm`) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers for the whole member loop.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Runtime lengths stay short.** Copies whose length is only known at run time, such as the runs that Minify and Prettify carry over, go through `jsonifierMemcpy` (`utility.hpp`). It sorts the length into a power-of-2 bucket `b`, where `b ≤ length < 2b`, and copies the first `b` bytes and the last `b` bytes. The two copies overlap, so any length from 2 to 255 bytes takes one fixed-size copy pair (a single byte takes one store), with no loop and no tail. Buckets wider than the vector register are split into register-width moves: the 128-byte bucket is 8 moves on AVX2, 4 on AVX-512 and 16 on SSE or NEON. Only lengths of 256 bytes or more reach `std::memcpy`. `jsonifierMemcpyUpTo<maxBytes>` takes a compile-time upper bound and leaves out every bucket above it, so a caller whose length is bounded never pays for the cases it cannot hit.
- **The bucket is chosen by a compare tree, not a jump table.** `dispatchBuckets` is a balanced binary tree of `length < 2^k` tests generated at compile time, so the bucket is found in at most four conditional branches on the length itself, with no `lzcnt` and no indirect jump. A switch on `std::bit_width(length)` compiled to a jump table on GCC and lost to glibc's `memcpy`; the tree compiles to the same branch-only shape on GCC and Clang, and in the [memcpy-decomp](https://github.com/nihilai-collective/memcpy-decomp) harness it beats `std::memcpy` on every range tested, from 1–15 to 1–129 bytes, on all five platforms.
- **Inlining and scheduling.** Each copy is one or two instructions, so a whole object's serializer inlines into straight-line code. The compiler can then interleave stores from neighbouring members with the value work between them.

**What actually reaches `jsonifierMemcpy`.** We counted the length of every call to `jsonifierMemcpy` and `jsonifierMemcpyUpTo`, separately, over one instrumented run. The counters were a temporary patch and are not in the library.

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

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through `jsonifierMemcpy` rather than `pow2MemcpyWrapper`. That is why they appear in the counts above, but `lengthToCopy` is still a compile-time constant, so the force-inlined call folds its length dispatch away. The only copies whose length is known only at runtime are off the common path: escape sequences inside strings (only when an escapable byte is found), raw JSON passthrough, and the memset for indents deeper than 8 levels.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test this writer leads Glaze by 8.01–14.72× freshly allocated on all five builds.

### Strings: picked by length

`string_serializer::impl` sorts every string into one of three length classes before it touches a byte. In all three the escapable bytes are the same: `"`, `\` and anything below 0x20.

1. **Under 16 bytes: one page-safe vector.** If the source does not sit within 16 bytes of the end of a 4 KiB page, `pageSafeShortImpl` loads a full 16-byte vector, masks its escape bits to the string's true length and, when nothing is set, stores all 16 bytes and advances by the length. A short clean key or value is one load, one test and one store, with no branch on its length. Reading past the string's end cannot fault, because the page check guarantees the 16 bytes are mapped; under ASan the load moves into a `JSONIFIER_NO_SANITIZE_ADDRESS` helper so the sanitizer does not flag it. Near a page end the string falls back to two overlapping 4- or 2-byte loads (`smallImpl`) below 8 bytes, or SWAR on 64-bit words (`swarFinish`) from 8 to 15.
2. **16 to 64 bytes: head and tail.** `mediumImpl` loads one vector from the start of the string and one ending at its last byte, tests both and, when both are clean, stores both. The two overlap in the middle, so any length in the range is covered with no loop and no remainder: 16-byte vectors up to 32 bytes, then 32-byte vectors on AVX2, or two 16-byte head-and-tail pairs over 32-byte halves where the widest register is 16 bytes. A string with any escapable byte returns `false` and takes the general path.
3. **Everything else: check first, then store.** The general path walks the widest available SIMD width down to the narrowest, testing each block before storing it. A clean block is stored and advances the full width; a dirty one is split into 16-byte lanes (`escapeLane`), which copy the clean runs at their exact lengths and write each escape from a 256-entry table. The last partial block is one 16-byte load ending at the string's last byte (`overlappedFinish`), with its escape mask shifted down to the bytes not yet written, and clean bytes are copied at their exact length.

### Integers: exact length first, then 4 digits per store

Integers go through `to_chars` in `i_to_str.hpp` in three steps.

1. **Width.** In `serialize_impl`, every integer narrower than 64 bits is widened to `uint64_t` or `int64_t`, so one writer per signedness covers all eight integer types.
2. **Sign.** A negative value writes `'-'` and then takes its absolute value without a branch: `(v ^ (v >> 63)) - (v >> 63)` on the unsigned type. Because the result is unsigned, `INT64_MIN` works too.
3. **Length.** A balanced tree of comparisons against powers of ten picks the exact digit count, 1 to 20, in at most 5 compares. Each count has its own fixed writer (`to_chars_internal<v_type, N>`), so the writer knows at compile time where every digit goes and never reverses or shifts its output.

The writers use three lookup tables built at compile time: 100 two-digit entries, 1,000 three-digit entries and 10,000 four-digit entries, each digit group pre-packed as characters. Writing four digits is one 4-byte copy from the 10,000-entry table.

Divisions are replaced by multiplies:

- **Divide by 10^4** inside an 8-digit block: multiply by 3518437209 and shift right 45.
- **Divide by 10^8** for longer values: take the high bits of a 128-bit product with 12379400392853802749 and shift right 90 in total. GCC and Clang use `__uint128_t`, MSVC uses `_umul128`, and other compilers fall back to `mulhiPortable`.

So a 20-digit `uint64_t` is two 10^8 splits, four 10^4 splits and five 4-byte stores, with no loop and no per-digit work. A 3-digit group, as at the front of a 19-digit value, is one 4-byte table load followed by a 2-byte and a 1-byte store, so nothing is written past the 3 digits.

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.36× to 4.41×.

### Floats

Doubles go through `zmij::detail::write`, a shortest-round-trip formatter vendored in `zmij.hpp`.

## Minified and prettified output

`serialize_options` carries `prettify`, `indentSize` (default 3), `indentChar` (default space) and `prescanStrings` (default false) as template parameters, so the two modes compile to separate code with no runtime mode check.

| Piece | Minified | Prettified |
| --- | --- | --- |
| Member header | `"key":` packed store | `"key": ` packed store |
| Separator | one `,` byte | `indent_table<",\n">` blit |
| Open / close | one `{` / `}` byte | `"{\n"` + indent, `"\n"` + indent, then `}` |
| Size estimate per member | `key + 3` | `key + 4`, plus `2 + indent` per separator |

`indent_table` prebuilds, at compile time, the prefix (`,\n`, `{\n` or `\n`) followed by 8 levels of indent characters as a packed `uint64_t` array. A newline plus indent is a short run of 8-byte stores from that table, rounded up into the slack. Nesting deeper than 8 levels falls back to `memset` for the remainder, marked `[[unlikely]]`.

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in *Two Stages, On Demand*.

## Performance results

Jsonifier was fastest in 246 of the 255 write tests on five builds, tied 3 and lost 6. With the output string reused across iterations it was fastest in 250 of 255, tied 2 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other four are against simdjson's reflection writer, which exists only on GCC: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and Double and minified Marine IK Reverse Small on macOS / GCC. The three freshly allocated ties are all against the reflection writer: Int64 on Linux / GCC, and Int64 and Uint64 on macOS / GCC. Reused, all three losses are against the reflection writer: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC and String on macOS / GCC. The two reused ties are prettified Marine IK Reverse on Windows / MSVC, against Glaze, and Bool on macOS / GCC, against the reflection writer. Linux / Clang and macOS / Clang win all 51 tests in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.31–7.35× across the five builds and the reflection writer by 16–65× on the two GCC builds; Small documents lead them by 1.38–7.39× and 12.24–62.48×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 9, 2026, with Jsonifier [c80a6d5](https://github.com/nihilai-collective/jsonifier/commit/c80a6d5), Glaze [8283164](https://github.com/stephenberry/glaze/commit/82831644), simdjson [f0c2d2a](https://github.com/simdjson/simdjson/commit/f0c2d2ac2) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-42 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-42 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.51 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size. Ratios are computed from unrounded throughput, so one recomputed from the rounded values shown can differ in the last digit.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M2 Pro. Non-converged results are included in the rankings and in the win, tie and loss counts. Each one is marked n/c in the tables below, and a test with a non-converged result should be read with reduced statistical confidence.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. A tie is therefore usually at the top, and every tie in this sweep is.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 1 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 47 | 2 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **246** | **3** | **6** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,298 | 93 | 14.01× |
| Double | 467 | 150 | 3.12× |
| Int64 | 516 | 377 | 1.37× |
| String | 1,452 | 894 | 1.62× |
| Uint64 | 511 | 375 | 1.36× |
| Apache Builds (minified) | 12,388 | 4,057 | 3.05× |
| Canada (minified) | 1,252 | 903 | 1.39× |
| Apache Builds (prettified) | 12,847 | 4,608 | 2.79× |
| Canada (prettified) | 2,871 | 1,445 | 1.99× |
| CitmCatalog (minified) | 8,586 | 4,173 | 2.06× |
| CitmCatalog (prettified) | 4,312 | 1,878 | 2.30× |
| Discord (minified) | 11,115 | 4,213 | 2.64× |
| Discord (prettified) | 12,307 | 3,854 | 3.19× |
| Github Events (minified) | 11,670 | 4,491 (n/c) | 2.60× |
| Google Maps Response (minified) | 9,348 | 3,270 | 2.86× |
| Github Events (prettified) | 11,418 | 2,985 | 3.83× |
| Google Maps Response (prettified) | 14,869 | 4,506 | 3.30× |
| Instruments (minified) | 13,772 | 3,959 | 3.48× |
| Instruments (prettified) | 14,840 | 3,843 | 3.86× |
| Marine IK Reverse (minified) | 877 | 704 | 1.25× |
| Marine IK Reverse (prettified) | 2,461 | 1,882 | 1.31× |
| Marine IK (minified) | 890 | 617 | 1.44× |
| Marine IK (prettified) | 2,727 | 1,239 | 2.20× |
| Random (minified) | 3,561 | 3,053 | 1.17× |
| Random (prettified) | 4,215 | 2,108 | 2.00× |
| Twitter (minified) | 4,043 | 4,866 | 0.83× |
| Twitter (prettified) | 4,194 | 1,716 | 2.44× |
| Apache Builds Small (minified) | 9,672 | 3,503 | 2.76× |
| Canada Small (minified) | 1,784 | 1,202 | 1.48× |
| Apache Builds Small (prettified) | 10,522 | 4,087 | 2.57× |
| Canada Small (prettified) | 4,679 | 3,067 | 1.53× |
| CitmCatalog Small (minified) | 7,174 | 3,332 | 2.15× |
| CitmCatalog Small (prettified) | 9,066 | 3,975 | 2.28× |
| Discord Small (minified) | 9,353 | 3,372 | 2.77× |
| Discord Small (prettified) | 8,857 | 3,074 | 2.88× |
| Github Events Small (minified) | 9,680 | 4,505 | 2.15× |
| Google Maps Response Small (minified) | 9,114 | 3,220 | 2.83× |
| Github Events Small (prettified) | 10,666 | 3,912 | 2.73× |
| Google Maps Response Small (prettified) | 14,386 | 4,936 | 2.91× |
| Instruments Small (minified) | 10,637 | 3,329 | 3.20× |
| Instruments Small (prettified) | 10,470 | 2,866 | 3.65× |
| Marine IK Reverse Small (minified) | 1,438 | 939 | 1.53× |
| Marine IK Reverse Small (prettified) | 5,000 | 2,786 | 1.79× |
| Marine IK Small (minified) | 1,501 | 939 | 1.60× |
| Marine IK Small (prettified) | 5,005 | 2,992 | 1.67× |
| Random Small (minified) | 9,282 | 3,397 | 2.73× |
| Random Small (prettified) | 11,513 | 4,018 | 2.87× |
| Twitter Small (minified) | 10,527 | 4,861 | 2.17× |
| Twitter Small (prettified) | 8,855 | 3,497 | 2.53× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,814 | 1,848 | 1,349 | 1.52× |
| Prettify | 2,087 | 2,410 | 938 | 0.87× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,292 | 217 | 10.57× |
| Double | 338 | 306 | 1.11× |
| Int64 | 900 | 886 | 1.02× |
| String | 4,088 | 1,878 | 2.18× |
| Uint64 | 923 | 853 | 1.08× |
| Apache Builds (minified) | 16,067 | 5,636 | 2.85× |
| Canada (minified) | 1,882 | 949 | 1.98× |
| Apache Builds (prettified) | 18,304 | 5,446 | 3.36× |
| Canada (prettified) | 5,046 | 3,224 | 1.57× |
| CitmCatalog (minified) | 10,263 | 4,980 | 2.06× |
| CitmCatalog (prettified) | 15,290 | 5,314 | 2.88× |
| Discord (minified) | 14,628 | 5,086 | 2.88× |
| Discord (prettified) | 16,200 | 4,024 | 4.03× |
| Github Events (minified) | 16,239 | 5,115 | 3.17× |
| Google Maps Response (minified) | 11,345 | 3,746 | 3.03× |
| Github Events (prettified) | 16,368 | 4,383 | 3.73× |
| Google Maps Response (prettified) | 15,736 | 5,196 | 3.03× |
| Instruments (minified) | 18,433 | 4,483 | 4.11× |
| Instruments (prettified) | 20,260 | 4,817 | 4.21× |
| Marine IK Reverse (minified) | 1,198 | 633 | 1.89× |
| Marine IK Reverse (prettified) | 4,965 | 3,663 | 1.36× |
| Marine IK (minified) | 1,216 | 625 | 1.95× |
| Marine IK (prettified) | 4,800 | 2,828 | 1.70× |
| Random (minified) | 14,510 | 3,574 | 4.06× |
| Random (prettified) | 16,048 | 4,014 | 4.00× |
| Twitter (minified) | 19,372 | 6,022 | 3.22× |
| Twitter (prettified) | 17,979 | 4,787 | 3.76× |
| Apache Builds Small (minified) | 10,580 | 3,424 | 3.09× |
| Canada Small (minified) | 1,855 | 964 | 1.92× |
| Apache Builds Small (prettified) | 11,250 | 3,572 | 3.15× |
| Canada Small (prettified) | 4,768 | 3,454 | 1.38× |
| CitmCatalog Small (minified) | 7,304 | 2,761 | 2.65× |
| CitmCatalog Small (prettified) | 10,264 | 3,265 | 3.14× |
| Discord Small (minified) | 10,137 | 2,993 | 3.39× |
| Discord Small (prettified) | 11,055 | 2,679 | 4.13× |
| Github Events Small (minified) | 8,929 | 3,827 | 2.33× |
| Google Maps Response Small (minified) | 10,318 | 3,546 | 2.91× |
| Github Events Small (prettified) | 10,164 | 3,407 | 2.98× |
| Google Maps Response Small (prettified) | 15,362 | 4,818 | 3.19× |
| Instruments Small (minified) | 12,381 | 2,331 | 5.31× |
| Instruments Small (prettified) | 14,530 | 2,828 | 5.14× |
| Marine IK Reverse Small (minified) | 1,497 | 798 | 1.88× |
| Marine IK Reverse Small (prettified) | 5,343 | 3,471 | 1.54× |
| Marine IK Small (minified) | 1,326 | 800 | 1.66× |
| Marine IK Small (prettified) | 4,836 | 3,440 | 1.41× |
| Random Small (minified) | 11,918 | 3,474 | 3.43× |
| Random Small (prettified) | 15,679 | 3,889 | 4.03× |
| Twitter Small (minified) | 8,802 | 4,140 | 2.13× |
| Twitter Small (prettified) | 9,742 | 3,197 | 3.05× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,575 | 2,370 | 1,722 | 1.51× |
| Prettify | 4,568 | 3,481 | 2,189 | 1.31× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,712 | 197 | 256 | 6.68× |
| Double | 344 | 313 | 338 | 1.02× |
| Int64 | 959 | 929 | 962 | 1.00× |
| String | 4,096 | 2,069 | 3,781 | 1.08× |
| Uint64 | 937 | 822 | 875 | 1.07× |
| Apache Builds (minified) | 20,340 | 7,801 | 13,218 | 1.54× |
| Canada (minified) | 2,123 | 1,436 | 1,030 | 1.48× |
| Apache Builds (prettified) | 22,745 | 7,183 | 616 | 3.17× |
| Canada (prettified) | 6,058 | 3,233 | 387 | 1.87× |
| CitmCatalog (minified) | 12,129 | 6,074 | 11,412 | 1.06× |
| CitmCatalog (prettified) | 21,535 | 5,878 | 578 | 3.66× |
| Discord (minified) | 14,983 | 6,178 | 12,752 | 1.17× |
| Discord (prettified) | 20,497 | 5,726 | 532 | 3.58× |
| Github Events (minified) | 14,750 | 7,120 | 11,186 | 1.32× |
| Google Maps Response (minified) | 15,602 | 4,483 | 9,712 | 1.61× |
| Github Events (prettified) | 18,595 | 4,924 | 626 | 3.78× |
| Google Maps Response (prettified) | 21,064 | 6,466 | 523 | 3.26× |
| Instruments (minified) | 20,107 | 5,621 | 14,357 | 1.40× |
| Instruments (prettified) | 25,956 | 5,454 | 476 | 4.76× |
| Marine IK Reverse (minified) | 1,285 | 981 | 874 | 1.31× |
| Marine IK Reverse (prettified) | 6,404 | 3,672 | 263 (n/c) | 1.74× |
| Marine IK (minified) | 1,254 | 948 | 900 | 1.32× |
| Marine IK (prettified) | 6,255 | 3,110 | 392 | 2.01× |
| Random (minified) | 17,245 | 4,548 | 10,956 | 1.57× |
| Random (prettified) | 20,766 | 4,993 | 530 | 4.16× |
| Twitter (minified) | 17,531 | 7,577 | 11,111 | 1.58× |
| Twitter (prettified) | 20,858 | 5,059 | 524 | 4.12× |
| Apache Builds Small (minified) | 11,262 | 4,660 | 7,301 | 1.54× |
| Canada Small (minified) | 1,976 | 1,530 | 1,058 | 1.29× |
| Apache Builds Small (prettified) | 14,074 | 4,141 | 532 | 3.40× |
| Canada Small (prettified) | 6,245 | 3,501 | 416 | 1.78× |
| CitmCatalog Small (minified) | 7,486 | 3,356 | 5,959 | 1.26× |
| CitmCatalog Small (prettified) | 14,580 | 2,982 | 498 | 4.89× |
| Discord Small (minified) | 9,888 | 3,965 | 7,544 | 1.31× |
| Discord Small (prettified) | 14,806 | 3,098 | 511 | 4.78× |
| Github Events Small (minified) | 8,332 | 5,192 | 6,218 | 1.34× |
| Google Maps Response Small (minified) | 12,774 | 3,815 | 8,141 | 1.57× |
| Github Events Small (prettified) | 11,539 | 3,735 | 539 | 3.09× |
| Google Maps Response Small (prettified) | 23,940 | 5,005 | 528 | 4.78× |
| Instruments Small (minified) | 12,384 | 3,754 | 8,080 | 1.53× |
| Instruments Small (prettified) | 21,331 | 3,007 | 515 | 7.09× |
| Marine IK Reverse Small (minified) | 1,465 | 1,254 | 1,517 | 0.97× |
| Marine IK Reverse Small (prettified) | 6,472 | 3,285 | 514 | 1.97× |
| Marine IK Small (minified) | 1,302 | 1,208 | 1,544 | 0.84× |
| Marine IK Small (prettified) | 6,580 | 3,369 | 538 | 1.95× |
| Random Small (minified) | 13,246 | 4,286 | 8,021 | 1.65× |
| Random Small (prettified) | 24,357 | 4,310 | 527 | 5.65× |
| Twitter Small (minified) | 8,804 | 5,374 | 6,785 | 1.30× |
| Twitter Small (prettified) | 11,803 | 3,319 | 555 | 3.56× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,376 | 2,506 | 1,874 | 1.35× |
| Prettify | 3,969 | 3,184 | 2,487 | 1.25× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,869 | 127 | 218 | 8.56× |
| Double | 271 | 211 | 308 | 0.88× |
| Int64 | 783 | 544 | 788 | 0.99× |
| String | 1,415 | 1,103 | 1,284 | 1.10× |
| Uint64 | 792 | 556 | 791 | 1.00× |
| Apache Builds (minified) | 12,361 | 4,959 | 9,270 | 1.33× |
| Canada (minified) | 2,349 | 1,720 | 828 | 1.37× |
| Apache Builds (prettified) | 13,449 | 4,404 | 371 | 3.05× |
| Canada (prettified) | 7,406 | 3,400 | 290 (n/c) | 2.18× |
| CitmCatalog (minified) | 8,274 | 2,104 | 7,700 | 1.07× |
| CitmCatalog (prettified) | 19,523 | 3,747 | 342 | 5.21× |
| Discord (minified) | 12,426 | 2,786 | 9,807 | 1.27× |
| Discord (prettified) | 12,202 | 2,392 | 349 | 5.10× |
| Github Events (minified) | 15,520 | 3,695 | 11,191 | 1.39× |
| Google Maps Response (minified) | 10,360 | 1,814 | 7,609 | 1.36× |
| Github Events (prettified) | 15,703 | 3,321 | 421 | 4.73× |
| Google Maps Response (prettified) | 20,206 | 2,979 | 309 | 6.78× |
| Instruments (minified) | 14,915 | 1,834 | 8,553 | 1.74× |
| Instruments (prettified) | 17,974 | 2,445 | 354 | 7.35× |
| Marine IK Reverse (minified) | 1,329 | 948 | 681 | 1.40× |
| Marine IK Reverse (prettified) | 6,366 | 2,864 | 287 | 2.22× |
| Marine IK (minified) | 1,214 | 806 | 829 | 1.46× |
| Marine IK (prettified) | 6,930 | 2,984 | 303 | 2.32× |
| Random (minified) | 12,521 | 2,183 | 8,747 | 1.43× |
| Random (prettified) | 15,972 | 2,746 | 356 | 5.82× |
| Twitter (minified) | 16,417 | 4,115 | 12,060 | 1.36× |
| Twitter (prettified) | 17,687 | 2,838 | 436 | 6.23× |
| Apache Builds Small (minified) | 10,124 | 2,954 | 7,236 | 1.40× |
| Canada Small (minified) | 2,454 | 1,742 | 851 | 1.41× |
| Apache Builds Small (prettified) | 12,559 (n/c) | 3,943 | 369 | 3.19× |
| Canada Small (prettified) | 7,660 | 3,269 | 298 | 2.34× |
| CitmCatalog Small (minified) | 9,170 | 1,926 | 7,711 | 1.19× |
| CitmCatalog Small (prettified) | 13,476 | 2,062 (n/c) | 271 | 6.54× |
| Discord Small (minified) | 12,158 | 2,720 | 9,384 | 1.30× |
| Discord Small (prettified) | 13,979 | 2,472 | 357 | 5.66× |
| Github Events Small (minified) | 11,844 | 2,719 | 10,374 | 1.14× |
| Google Maps Response Small (minified) | 10,691 | 1,845 | 8,422 | 1.27× |
| Github Events Small (prettified) | 14,690 | 2,996 | 390 | 4.90× |
| Google Maps Response Small (prettified) | 21,108 | 2,855 | 338 | 7.39× |
| Instruments Small (minified) | 14,892 | 1,733 | 8,173 | 1.82× |
| Instruments Small (prettified) | 17,364 | 2,390 | 343 | 7.27× |
| Marine IK Reverse Small (minified) | 1,573 | 1,084 | 1,658 | 0.95× |
| Marine IK Reverse Small (prettified) | 8,556 | 3,299 | 327 | 2.59× |
| Marine IK Small (minified) | 1,933 | 1,072 | 1,689 | 1.14× |
| Marine IK Small (prettified) | 8,671 | 3,279 | 284 | 2.64× |
| Random Small (minified) | 12,606 | 2,245 | 8,344 | 1.51× |
| Random Small (prettified) | 14,391 | 2,559 | 348 | 5.62× |
| Twitter Small (minified) | 12,243 | 3,554 | 9,456 | 1.29× |
| Twitter Small (prettified) | 15,275 | 3,258 | 405 | 4.69× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,439 | 1,724 | 1,029 | 1.42× |
| Prettify | 3,267 | 2,430 | 1,648 | 1.34× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 944 | 118 | 8.01× |
| Double | 287 | 204 | 1.41× |
| Int64 | 2,809 | 509 | 5.52× |
| String | 1,248 | 997 | 1.25× |
| Uint64 | 2,625 | 467 | 5.62× |
| Apache Builds (minified) | 11,467 | 3,512 | 3.27× |
| Canada (minified) | 3,010 | 1,894 | 1.59× |
| Apache Builds (prettified) | 11,422 | 3,007 | 3.80× |
| Canada (prettified) | 6,982 | 3,062 | 2.28× |
| CitmCatalog (minified) | 6,803 | 2,039 | 3.34× |
| CitmCatalog (prettified) | 14,813 | 3,812 | 3.89× |
| Discord (minified) | 10,640 | 2,852 | 3.73× |
| Discord (prettified) | 11,995 | 2,511 | 4.78× |
| Github Events (minified) | 13,962 | 4,375 | 3.19× |
| Google Maps Response (minified) | 9,591 | 1,755 | 5.47× |
| Github Events (prettified) | 14,160 | 3,661 | 3.87× |
| Google Maps Response (prettified) | 12,094 | 3,151 | 3.84× |
| Instruments (minified) | 12,635 | 2,002 | 6.31× |
| Instruments (prettified) | 12,232 | 2,931 | 4.17× |
| Marine IK Reverse (minified) | 1,284 | 852 | 1.51× |
| Marine IK Reverse (prettified) | 4,676 | 2,975 | 1.57× |
| Marine IK (minified) | 1,366 | 784 | 1.74× |
| Marine IK (prettified) | 4,960 | 2,641 | 1.88× |
| Random (minified) | 10,251 | 2,340 | 4.38× |
| Random (prettified) | 10,885 | 3,248 | 3.35× |
| Twitter (minified) | 13,782 | 4,065 | 3.39× |
| Twitter (prettified) | 13,746 | 3,303 | 4.16× |
| Apache Builds Small (minified) | 8,566 | 2,860 | 3.00× |
| Canada Small (minified) | 2,455 | 1,275 | 1.93× |
| Apache Builds Small (prettified) | 8,790 | 3,154 | 2.79× |
| Canada Small (prettified) | 6,467 | 3,354 | 1.93× |
| CitmCatalog Small (minified) | 7,752 | 1,857 | 4.17× |
| CitmCatalog Small (prettified) | 8,848 | 2,402 | 3.68× |
| Discord Small (minified) | 10,275 | 2,925 | 3.51× |
| Discord Small (prettified) | 11,213 | 2,807 | 4.00× |
| Github Events Small (minified) | 8,356 | 3,338 | 2.50× |
| Google Maps Response Small (minified) | 7,601 | 2,205 | 3.45× |
| Github Events Small (prettified) | 10,514 | 3,046 | 3.45× |
| Google Maps Response Small (prettified) | 13,658 | 3,437 | 3.97× |
| Instruments Small (minified) | 12,977 | 1,975 | 6.57× |
| Instruments Small (prettified) | 14,540 | 3,064 | 4.75× |
| Marine IK Reverse Small (minified) | 1,742 | 1,027 | 1.70× |
| Marine IK Reverse Small (prettified) | 5,568 | 3,163 | 1.76× |
| Marine IK Small (minified) | 1,568 | 846 | 1.85× |
| Marine IK Small (prettified) | 5,251 | 3,052 | 1.72× |
| Random Small (minified) | 8,430 | 2,280 | 3.70× |
| Random Small (prettified) | 10,853 | 2,938 | 3.69× |
| Twitter Small (minified) | 9,628 | 3,072 | 3.13× |
| Twitter Small (prettified) | 11,693 | 3,057 | 3.83× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,629 | 1,750 | 1,129 | 1.50× |
| Prettify | 2,724 | 2,533 | 1,633 | 1.08× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 250 of the 255 write tests, ties 2 and loses 3. On Linux / GCC the reflection writer wins minified Marine IK Small (1,534 vs 1,367 MB/s, 0.89×) and Marine IK Reverse Small (1,523 vs 1,488 MB/s, 0.98×). On macOS / GCC it wins String (10,682 vs 9,810 MB/s, 0.92×) and ties Bool, where Jsonifier leads 2,845 vs 2,717 MB/s with RSEs of 3.0% and 4.1%. Under MSVC, Glaze ties prettified Marine IK Reverse (3,438 vs 3,407 MB/s).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 50 | 1 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 1 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **250** | **2** | **3** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,638 | 292 | 5.60× |
| Double | 776 | 380 | 2.04× |
| Int64 | 4,281 | 1,043 | 4.11× |
| String | 7,759 | 2,466 | 3.15× |
| Uint64 | 4,636 | 1,051 | 4.41× |
| Apache Builds (minified) | 12,777 | 4,482 | 2.85× |
| Canada (minified) | 1,561 | 1,267 | 1.23× |
| Apache Builds (prettified) | 13,297 | 5,113 | 2.60× |
| Canada (prettified) | 4,630 | 3,290 | 1.41× |
| CitmCatalog (minified) | 8,629 | 4,833 | 1.79× |
| CitmCatalog (prettified) | 13,404 | 7,301 | 1.84× |
| Discord (minified) | 11,537 | 4,608 | 2.50× |
| Discord (prettified) | 12,812 | 4,286 | 2.99× |
| Github Events (minified) | 12,101 | 5,719 (n/c) | 2.12× |
| Google Maps Response (minified) | 10,858 | 3,946 | 2.75× |
| Github Events (prettified) | 11,827 | 3,124 | 3.79× |
| Google Maps Response (prettified) | 15,951 | 5,809 | 2.75× |
| Instruments (minified) | 14,290 | 4,694 | 3.04× |
| Instruments (prettified) | 15,525 | 4,394 | 3.53× |
| Marine IK Reverse (minified) | 1,026 | 773 | 1.33× |
| Marine IK Reverse (prettified) | 3,438 | 3,407 | 1.01× |
| Marine IK (minified) | 1,015 | 788 | 1.29× |
| Marine IK (prettified) | 4,135 | 3,461 | 1.19× |
| Random (minified) | 11,233 | 3,341 | 3.36× |
| Random (prettified) | 12,653 | 4,397 | 2.88× |
| Twitter (minified) | 16,059 | 5,183 | 3.10× |
| Twitter (prettified) | 14,280 | 3,312 | 4.31× |
| Apache Builds Small (minified) | 11,125 | 4,019 | 2.77× |
| Canada Small (minified) | 1,823 | 1,357 | 1.34× |
| Apache Builds Small (prettified) | 12,242 | 4,662 | 2.63× |
| Canada Small (prettified) | 4,987 | 3,465 | 1.44× |
| CitmCatalog Small (minified) | 8,271 | 4,126 | 2.00× |
| CitmCatalog Small (prettified) | 9,845 | 5,021 | 1.96× |
| Discord Small (minified) | 10,694 | 4,259 | 2.51× |
| Discord Small (prettified) | 9,985 | 3,826 | 2.61× |
| Github Events Small (minified) | 12,546 | 5,640 | 2.22× |
| Google Maps Response Small (minified) | 10,041 | 3,755 | 2.67× |
| Github Events Small (prettified) | 12,759 | 4,917 | 2.59× |
| Google Maps Response Small (prettified) | 16,636 | 5,089 | 3.27× |
| Instruments Small (minified) | 11,894 | 4,468 | 2.66× |
| Instruments Small (prettified) | 12,381 | 3,218 (n/c) | 3.85× |
| Marine IK Reverse Small (minified) | 1,502 | 926 | 1.62× |
| Marine IK Reverse Small (prettified) | 5,146 | 3,577 | 1.44× |
| Marine IK Small (minified) | 1,587 | 965 | 1.64× |
| Marine IK Small (prettified) | 5,240 | 3,507 | 1.49× |
| Random Small (minified) | 11,666 | 3,890 | 3.00× |
| Random Small (prettified) | 14,054 | 5,215 | 2.69× |
| Twitter Small (minified) | 12,395 | 5,796 | 2.14× |
| Twitter Small (prettified) | 11,001 | 4,930 | 2.23× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,833 | 1,823 | 1,347 | 1.55× |
| Prettify | 3,639 | 2,663 | 1,111 | 1.37× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,578 | 942 | 4.86× |
| Double | 870 | 701 | 1.24× |
| Int64 | 5,021 | 2,393 | 2.10× |
| String | 11,901 | 4,371 | 2.72× |
| Uint64 | 4,488 | 2,444 | 1.84× |
| Apache Builds (minified) | 16,268 | 6,301 | 2.58× |
| Canada (minified) | 1,884 | 980 | 1.92× |
| Apache Builds (prettified) | 18,575 | 5,968 | 3.11× |
| Canada (prettified) | 5,072 | 3,887 | 1.30× |
| CitmCatalog (minified) | 10,334 | 5,753 | 1.80× |
| CitmCatalog (prettified) | 15,334 | 7,044 | 2.18× |
| Discord (minified) | 14,993 | 5,654 | 2.65× |
| Discord (prettified) | 16,572 | 4,668 | 3.55× |
| Github Events (minified) | 16,836 | 6,027 | 2.79× |
| Google Maps Response (minified) | 12,354 | 4,233 | 2.92× |
| Github Events (prettified) | 16,516 | 5,142 | 3.21× |
| Google Maps Response (prettified) | 16,306 | 5,761 | 2.83× |
| Instruments (minified) | 18,485 | 5,241 | 3.53× |
| Instruments (prettified) | 20,279 | 5,664 | 3.58× |
| Marine IK Reverse (minified) | 1,196 | 650 | 1.84× |
| Marine IK Reverse (prettified) | 4,888 | 4,021 | 1.22× |
| Marine IK (minified) | 1,215 | 649 | 1.87× |
| Marine IK (prettified) | 4,858 | 3,912 | 1.24× |
| Random (minified) | 14,621 | 3,902 | 3.75× |
| Random (prettified) | 16,142 | 4,629 | 3.49× |
| Twitter (minified) | 19,873 | 6,877 | 2.89× |
| Twitter (prettified) | 18,036 | 5,808 | 3.11× |
| Apache Builds Small (minified) | 12,321 | 3,651 | 3.37× |
| Canada Small (minified) | 1,889 | 1,008 | 1.87× |
| Apache Builds Small (prettified) | 13,137 | 3,855 | 3.41× |
| Canada Small (prettified) | 4,933 | 3,872 | 1.27× |
| CitmCatalog Small (minified) | 8,451 | 2,977 | 2.84× |
| CitmCatalog Small (prettified) | 11,205 | 3,571 | 3.14× |
| Discord Small (minified) | 11,301 | 3,056 | 3.70× |
| Discord Small (prettified) | 12,341 | 2,968 | 4.16× |
| Github Events Small (minified) | 11,667 | 3,964 | 2.94× |
| Google Maps Response Small (minified) | 11,501 | 3,954 | 2.91× |
| Github Events Small (prettified) | 12,816 | 3,832 | 3.34× |
| Google Maps Response Small (prettified) | 17,143 | 5,412 | 3.17× |
| Instruments Small (minified) | 13,620 | 2,562 | 5.32× |
| Instruments Small (prettified) | 15,644 | 2,931 | 5.34× |
| Marine IK Reverse Small (minified) | 1,548 | 825 | 1.88× |
| Marine IK Reverse Small (prettified) | 5,442 | 3,914 | 1.39× |
| Marine IK Small (minified) | 1,393 | 840 | 1.66× |
| Marine IK Small (prettified) | 5,010 | 3,787 | 1.32× |
| Random Small (minified) | 14,002 | 3,850 | 3.64× |
| Random Small (prettified) | 17,128 | 4,586 | 3.73× |
| Twitter Small (minified) | 10,297 | 3,515 | 2.93× |
| Twitter Small (prettified) | 11,024 | 3,849 | 2.86× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,598 | 2,380 | 1,723 | 1.51× |
| Prettify | 4,562 | 3,511 | 2,194 | 1.30× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,231 | 670 | 3,114 | 1.04× |
| Double | 877 | 705 | 652 | 1.24× |
| Int64 | 5,340 | 2,622 | 3,925 | 1.36× |
| String | 10,736 | 5,701 | 9,319 | 1.15× |
| Uint64 | 6,274 | 2,667 | 4,048 | 1.55× |
| Apache Builds (minified) | 20,180 | 9,228 | 13,424 | 1.50× |
| Canada (minified) | 2,112 | 1,532 | 1,034 | 1.38× |
| Apache Builds (prettified) | 23,289 | 8,067 | 609 | 2.89× |
| Canada (prettified) | 6,110 | 3,949 | 399 | 1.55× |
| CitmCatalog (minified) | 12,180 | 7,098 | 11,377 | 1.07× |
| CitmCatalog (prettified) | 20,370 | 7,627 | 573 | 2.67× |
| Discord (minified) | 15,236 | 7,133 | 13,214 | 1.15× |
| Discord (prettified) | 20,501 | 6,872 | 536 | 2.98× |
| Github Events (minified) | 14,943 | 8,504 | 10,911 | 1.37× |
| Google Maps Response (minified) | 16,890 | 5,226 | 10,360 | 1.63× |
| Github Events (prettified) | 18,371 | 5,759 | 647 | 3.19× |
| Google Maps Response (prettified) | 22,340 | 7,080 | 521 | 3.16× |
| Instruments (minified) | 20,267 | 6,478 | 16,034 | 1.26× |
| Instruments (prettified) | 25,935 | 6,433 | 474 | 4.03× |
| Marine IK Reverse (minified) | 1,283 | 972 | 863 | 1.32× |
| Marine IK Reverse (prettified) | 6,354 | 4,014 | 302 | 1.58× |
| Marine IK (minified) | 1,268 | 1,004 | 882 | 1.26× |
| Marine IK (prettified) | 6,225 | 4,086 | 339 | 1.52× |
| Random (minified) | 17,021 | 5,133 | 11,241 | 1.51× |
| Random (prettified) | 20,936 | 5,757 | 543 | 3.64× |
| Twitter (minified) | 17,301 | 8,926 | 11,050 | 1.57× |
| Twitter (prettified) | 21,112 | 6,522 | 526 | 3.24× |
| Apache Builds Small (minified) | 14,039 | 4,983 | 8,120 | 1.73× |
| Canada Small (minified) | 1,994 | 1,548 | 1,066 | 1.29× |
| Apache Builds Small (prettified) | 18,446 | 4,600 | 531 | 4.01× |
| Canada Small (prettified) | 6,550 | 4,145 | 410 | 1.58× |
| CitmCatalog Small (minified) | 8,564 | 4,011 | 6,447 | 1.33× |
| CitmCatalog Small (prettified) | 15,942 | 3,352 | 498 | 4.76× |
| Discord Small (minified) | 11,433 | 4,740 | 8,347 | 1.37× |
| Discord Small (prettified) | 16,505 | 3,458 | 514 | 4.77× |
| Github Events Small (minified) | 10,216 | 5,991 | 7,411 | 1.38× |
| Google Maps Response Small (minified) | 14,088 | 4,161 | 8,618 | 1.63× |
| Github Events Small (prettified) | 13,742 | 4,394 | 533 | 3.13× |
| Google Maps Response Small (prettified) | 28,657 | 6,933 | 520 | 4.13× |
| Instruments Small (minified) | 13,699 | 4,709 | 8,839 | 1.55× |
| Instruments Small (prettified) | 24,342 | 3,235 | 517 | 7.53× |
| Marine IK Reverse Small (minified) | 1,488 | 1,335 | 1,523 | 0.98× |
| Marine IK Reverse Small (prettified) | 6,717 | 3,775 | 517 | 1.78× |
| Marine IK Small (minified) | 1,367 | 1,275 | 1,534 | 0.89× |
| Marine IK Small (prettified) | 6,712 | 3,645 | 537 | 1.84× |
| Random Small (minified) | 16,439 | 5,087 | 8,874 | 1.85× |
| Random Small (prettified) | 26,468 | 5,310 | 526 | 4.98× |
| Twitter Small (minified) | 10,286 | 6,369 | 7,669 | 1.34× |
| Twitter Small (prettified) | 13,763 | 3,921 | 563 | 3.51× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,396 | 2,510 | 1,860 | 1.35× |
| Prettify | 3,955 | 3,187 | 2,465 | 1.24× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,845 | 819 | 2,717 | 1.05× |
| Double | 877 | 677 | 644 | 1.30× |
| Int64 | 5,286 | 2,450 | 3,433 | 1.54× |
| String | 9,810 | 5,044 | 10,682 | 0.92× |
| Uint64 | 5,136 | 2,666 | 3,201 | 1.60× |
| Apache Builds (minified) | 11,702 | 6,351 | 9,446 | 1.24× |
| Canada (minified) | 2,349 | 1,789 | 830 | 1.31× |
| Apache Builds (prettified) | 13,806 | 5,221 | 373 | 2.64× |
| Canada (prettified) | 7,434 | 3,914 | 296 | 1.90× |
| CitmCatalog (minified) | 8,360 | 2,254 | 7,749 | 1.08× |
| CitmCatalog (prettified) | 19,623 | 4,362 | 341 | 4.50× |
| Discord (minified) | 12,957 | 3,121 | 10,184 | 1.27× |
| Discord (prettified) | 14,531 | 2,685 | 350 | 5.41× |
| Github Events (minified) | 16,578 | 4,540 | 12,247 | 1.35× |
| Google Maps Response (minified) | 10,356 | 2,007 | 8,544 | 1.21× |
| Github Events (prettified) | 16,230 | 4,014 | 442 | 4.04× |
| Google Maps Response (prettified) | 22,744 | 3,301 | 303 | 6.89× |
| Instruments (minified) | 15,382 | 2,098 | 8,649 | 1.78× |
| Instruments (prettified) | 18,351 | 2,667 | 355 | 6.88× |
| Marine IK Reverse (minified) | 1,115 | 843 | 724 | 1.32× |
| Marine IK Reverse (prettified) | 6,140 | 3,088 | 302 | 1.99× |
| Marine IK (minified) | 1,174 | 766 | 822 | 1.43× |
| Marine IK (prettified) | 6,923 | 3,448 | 284 (n/c) | 2.01× |
| Random (minified) | 12,673 | 2,547 | 8,884 | 1.43× |
| Random (prettified) | 16,086 | 2,976 | 351 | 5.41× |
| Twitter (minified) | 16,483 | 4,519 | 12,232 | 1.35× |
| Twitter (prettified) | 17,771 | 4,661 | 403 | 3.81× |
| Apache Builds Small (minified) | 11,202 | 3,216 | 7,795 | 1.44× |
| Canada Small (minified) | 2,495 | 2,004 | 948 | 1.25× |
| Apache Builds Small (prettified) | 12,779 | 4,641 | 370 | 2.75× |
| Canada Small (prettified) | 7,912 | 3,700 | 299 | 2.14× |
| CitmCatalog Small (minified) | 9,795 | 2,176 | 8,473 | 1.16× |
| CitmCatalog Small (prettified) | 18,068 | 2,087 | 347 | 8.66× |
| Discord Small (minified) | 14,109 | 2,883 | 10,084 | 1.40× |
| Discord Small (prettified) | 15,152 | 2,365 | 356 | 6.41× |
| Github Events Small (minified) | 15,233 | 3,765 | 10,147 | 1.50× |
| Google Maps Response Small (minified) | 11,314 | 1,994 | 8,792 | 1.29× |
| Github Events Small (prettified) | 14,904 | 3,354 | 394 | 4.44× |
| Google Maps Response Small (prettified) | 20,290 | 2,822 | 336 | 7.19× |
| Instruments Small (minified) | 15,910 | 2,252 | 8,608 | 1.85× |
| Instruments Small (prettified) | 18,876 | 2,611 | 344 | 7.23× |
| Marine IK Reverse Small (minified) | 1,975 | 1,161 | 1,667 | 1.19× |
| Marine IK Reverse Small (prettified) | 9,056 | 3,795 | 326 | 2.39× |
| Marine IK Small (minified) | 1,983 | 1,174 | 1,735 | 1.14× |
| Marine IK Small (prettified) | 9,236 | 3,821 | 322 | 2.42× |
| Random Small (minified) | 13,331 | 2,552 | 10,122 | 1.32× |
| Random Small (prettified) | 16,524 | 2,993 | 347 | 5.52× |
| Twitter Small (minified) | 13,872 | 3,964 | 10,370 | 1.34× |
| Twitter Small (prettified) | 17,307 (n/c) | 4,034 | 416 | 4.29× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,908 | 1,723 | 1,061 | 1.11× |
| Prettify | 3,309 | 2,403 | 1,539 | 1.38× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,150 | 693 | 1.66× |
| Double | 727 | 641 | 1.13× |
| Int64 | 3,180 | 1,990 | 1.60× |
| String | 6,584 | 4,083 | 1.61× |
| Uint64 | 3,232 | 2,148 | 1.50× |
| Apache Builds (minified) | 11,865 | 3,727 | 3.18× |
| Canada (minified) | 2,343 | 1,972 | 1.19× |
| Apache Builds (prettified) | 11,509 | 3,535 | 3.26× |
| Canada (prettified) | 6,359 | 3,692 | 1.72× |
| CitmCatalog (minified) | 7,102 | 1,905 | 3.73× |
| CitmCatalog (prettified) | 14,677 | 4,915 | 2.99× |
| Discord (minified) | 11,649 | 3,590 | 3.24× |
| Discord (prettified) | 12,292 | 3,091 | 3.98× |
| Github Events (minified) | 14,960 | 5,387 | 2.78× |
| Google Maps Response (minified) | 8,289 | 2,267 | 3.66× |
| Github Events (prettified) | 13,851 | 5,016 | 2.76× |
| Google Maps Response (prettified) | 13,081 | 3,647 | 3.59× |
| Instruments (minified) | 13,362 | 2,435 | 5.49× |
| Instruments (prettified) | 13,020 | 3,183 | 4.09× |
| Marine IK Reverse (minified) | 1,323 | 803 | 1.65× |
| Marine IK Reverse (prettified) | 4,860 | 2,687 | 1.81× |
| Marine IK (minified) | 1,344 | 833 | 1.61× |
| Marine IK (prettified) | 5,026 | 2,974 | 1.69× |
| Random (minified) | 10,169 | 2,449 | 4.15× |
| Random (prettified) | 12,677 | 2,717 | 4.67× |
| Twitter (minified) | 13,280 | 4,309 | 3.08× |
| Twitter (prettified) | 12,038 | 4,010 | 3.00× |
| Apache Builds Small (minified) | 9,268 | 3,029 | 3.06× |
| Canada Small (minified) | 2,234 | 1,729 | 1.29× |
| Apache Builds Small (prettified) | 10,263 | 3,607 | 2.84× |
| Canada Small (prettified) | 6,103 | 3,717 | 1.64× |
| CitmCatalog Small (minified) | 8,970 | 2,084 | 4.30× |
| CitmCatalog Small (prettified) | 12,397 | 3,027 | 4.10× |
| Discord Small (minified) | 11,718 | 3,810 | 3.08× |
| Discord Small (prettified) | 13,138 | 3,345 | 3.93× |
| Github Events Small (minified) | 12,560 | 4,270 | 2.94× |
| Google Maps Response Small (minified) | 7,774 | 2,425 | 3.21× |
| Github Events Small (prettified) | 13,965 | 4,191 | 3.33× |
| Google Maps Response Small (prettified) | 14,354 | 4,321 | 3.32× |
| Instruments Small (minified) | 14,721 | 1,976 | 7.45× |
| Instruments Small (prettified) | 16,011 | 3,522 | 4.55× |
| Marine IK Reverse Small (minified) | 1,967 | 1,124 | 1.75× |
| Marine IK Reverse Small (prettified) | 5,848 | 3,729 | 1.57× |
| Marine IK Small (minified) | 2,030 | 892 | 2.27× |
| Marine IK Small (prettified) | 5,379 | 3,651 | 1.47× |
| Random Small (minified) | 10,388 | 2,942 | 3.53× |
| Random Small (prettified) | 10,484 | 2,881 | 3.64× |
| Twitter Small (minified) | 10,438 | 4,326 | 2.41× |
| Twitter Small (prettified) | 14,592 | 4,453 | 3.28× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,630 | 1,747 | 1,029 | 1.51× |
| Prettify | 3,094 | 2,321 | 1,430 | 1.33× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,866 vs Jsonifier's 4,043 MB/s (0.83×). Reused, Jsonifier wins by 3.10× (16,059 vs 5,183).
- **Prettify, Windows / MSVC.** Glaze runs 2,410 vs 2,087 MB/s (0.87×), with simdjson On Demand at 938. Reused, Jsonifier wins by 1.37× (3,639 vs 2,663).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,544 vs 1,302 MB/s (0.84×), with Glaze at 1,208. It also wins reused, 1,534 vs 1,367 MB/s (0.89×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,517 vs 1,465 MB/s (0.97×), with Glaze at 1,254. It also wins reused, 1,523 vs 1,488 MB/s (0.98×).
- **Double, macOS / GCC.** The reflection writer runs 308 vs 271 MB/s (0.88×), with Glaze at 211. Reused, Jsonifier wins by 1.30× over Glaze (877 vs 677), with the reflection writer at 644.
- **Marine IK Reverse Small (minified), macOS / GCC.** The reflection writer runs 1,658 vs 1,573 MB/s (0.95×), with Glaze at 1,084. Reused, Jsonifier wins by 1.19× (1,975 vs 1,667).

The three freshly allocated ties are all against the reflection writer: Int64 on Linux / GCC (962 vs 959 MB/s, Glaze at 929), which Jsonifier wins reused by 1.36× (5,340 vs 3,925), and Int64 (788 vs 783 MB/s, Glaze at 544) and Uint64 (792 vs 791 MB/s, Glaze at 556) on macOS / GCC, which it wins reused by 1.54× (5,286 vs 3,433) and 1.60× (5,136 vs 3,201).

Three reused non-wins have no freshly allocated counterpart: String on macOS / GCC, which wins freshly allocated (1,415 MB/s against the reflection writer's 1,284 and Glaze's 1,103) and loses reused (0.92×); Bool on macOS / GCC, a reused tie with the reflection writer after an 8.56× freshly allocated lead; and prettified Marine IK Reverse on Windows / MSVC, a reused tie with Glaze after a 1.31× freshly allocated lead.

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.06–2.84× and prettified documents by 16–65×; reused, by 1.07–2.83× and 15–75×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 41 of the 44 Small document tests freshly allocated and 42 reused. The non-wins are minified Marine IK Small and Marine IK Reverse Small on Linux / GCC in both runs, plus minified Marine IK Reverse Small on macOS / GCC freshly allocated. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.35–1.52× freshly allocated and 1.11–1.55× reused. It wins Prettify on all five reused, by 1.24–1.38×, and on four of five freshly allocated, by 1.08–1.34×; the exception is Windows / MSVC, where Glaze runs 2,410 vs 2,087 MB/s (0.87×). Glaze is the next-fastest library in every case.

## Questions from external review

An outside review of a draft raised these concerns, answered against the code as it stands.

### Can an overstore run past the buffer?

No, because every overstore lands inside the string's own size, not past it. `serializeJson` resizes the buffer to the full bound plus 64 bytes before writing, so the write pass works inside memory the string already owns. No write ever reaches capacity or the allocator's edge, and ASan sees only in-bounds accesses.

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

### Why is the String test slower on NEON than on AVX2?

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,415 MB/s on macOS / GCC (1.28× Glaze's 1,103, ahead of the reflection writer's 1,284) and 1,248 MB/s on macOS / Clang (1.25× Glaze's 997). The AVX2 builds run the same test at 1,452–4,096 MB/s and lead Glaze by 1.62× (Windows / MSVC), 2.18× (Linux / Clang) and 1.98× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.61× on macOS / Clang (6,584 vs 4,083 MB/s) and 1.95× on macOS / GCC (9,810 vs 5,044), against 1.88–3.15× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 10,682 MB/s (0.92×), a significant loss.

So most of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: reused, the macOS / GCC lead over Glaze sits inside the AVX2 range and the macOS / Clang lead sits just below it. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated on macOS / GCC, yes: the reflection writer runs 308 vs 271 MB/s (0.88×), a significant loss. Freshly allocated on Linux / GCC, Jsonifier edges it, 344 vs 338 MB/s (1.02×). Reused, Jsonifier leads on both: 877 vs 652 MB/s on Linux / GCC (1.35×) and 877 vs 644 MB/s on macOS / GCC (1.36×). Jsonifier beats Glaze on Double on all five builds in both runs.

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

This paper is dedicated to Sonny Moore (Skrillex), for showing what it looks like when someone’s vocation is also their resting state.
