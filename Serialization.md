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

On the Bool test this writer leads Glaze by 8.71–14.11× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.21× to 3.78×.

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

Jsonifier was fastest in 248 of the 255 write tests on five builds, tied 2 and lost 5. With the output string reused across iterations it was fastest in 251 of 255, tied 1 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, the two losses to Glaze are minified Twitter and Prettify on Windows / MSVC; the other three are against simdjson's reflection writer, which exists only on GCC: Int64, minified Marine IK Small and minified Marine IK Reverse Small, all on Linux / GCC. The two freshly allocated ties are Uint64 and minified Github Events Small on macOS / GCC, both against the reflection writer. Reused, the three losses are Bool, minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, and the one tie is String on macOS / GCC, all against the reflection writer. Linux / Clang and macOS / Clang win all 51 tests in both runs, and Windows / MSVC wins all 51 reused. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.32–7.93× across the five builds and the reflection writer by 15–59× on the two GCC builds; Small documents lead them by 1.21–7.27× and 12.08–62.15×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 6, 2026, with Jsonifier [39b6202](https://github.com/nihilai-collective/jsonifier/commit/39b6202), Glaze [0a363d4](https://github.com/stephenberry/glaze/commit/0a363d40), simdjson [dfe02cf](https://github.com/simdjson/simdjson/commit/dfe02cfe3) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 2 | 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **248** | **2** | **5** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,287 | 92 | 13.98× |
| Double | 464 | 152 | 3.05× |
| Int64 | 507 | 375 | 1.35× |
| String | 1,417 | 898 | 1.58× |
| Uint64 | 534 | 374 | 1.43× |
| Apache Builds (minified) | 12,549 | 4,089 | 3.07× |
| Canada (minified) | 1,268 | 911 | 1.39× |
| Apache Builds (prettified) | 12,795 | 4,650 | 2.75× |
| Canada (prettified) | 3,000 | 1,436 | 2.09× |
| CitmCatalog (minified) | 8,749 | 4,457 | 1.96× |
| CitmCatalog (prettified) | 4,489 | 1,920 | 2.34× |
| Discord (minified) | 10,959 | 4,109 | 2.67× |
| Discord (prettified) | 12,213 | 3,864 | 3.16× |
| Github Events (minified) | 11,293 | 4,673 | 2.42× |
| Google Maps Response (minified) | 9,507 | 3,444 | 2.76× |
| Github Events (prettified) | 11,332 | 3,107 | 3.65× |
| Google Maps Response (prettified) | 15,293 | 5,166 | 2.96× |
| Instruments (minified) | 13,949 | 4,056 | 3.44× |
| Instruments (prettified) | 15,178 | 3,839 | 3.95× |
| Marine IK Reverse (minified) | 893 | 710 | 1.26× |
| Marine IK Reverse (prettified) | 2,481 | 1,884 | 1.32× |
| Marine IK (minified) | 900 | 621 | 1.45× |
| Marine IK (prettified) | 2,754 | 1,286 | 2.14× |
| Random (minified) | 3,580 | 3,195 | 1.12× |
| Random (prettified) | 4,082 | 2,116 | 1.93× |
| Twitter (minified) | 3,832 | 4,864 | 0.79× |
| Twitter (prettified) | 4,090 | 1,742 | 2.35× |
| Apache Builds Small (minified) | 9,805 | 3,586 | 2.73× |
| Canada Small (minified) | 1,855 | 1,257 | 1.48× |
| Apache Builds Small (prettified) | 10,649 | 4,104 | 2.59× |
| Canada Small (prettified) | 5,239 | 3,110 | 1.68× |
| CitmCatalog Small (minified) | 7,061 | 3,378 | 2.09× |
| CitmCatalog Small (prettified) | 9,011 | 4,152 | 2.17× |
| Discord Small (minified) | 9,525 | 3,517 | 2.71× |
| Discord Small (prettified) | 8,846 | 2,744 | 3.22× |
| Github Events Small (minified) | 9,816 | 4,159 | 2.36× |
| Google Maps Response Small (minified) | 8,311 | 3,560 | 2.33× |
| Github Events Small (prettified) | 10,673 | 4,021 | 2.65× |
| Google Maps Response Small (prettified) | 14,086 | 4,741 | 2.97× |
| Instruments Small (minified) | 10,506 | 2,720 | 3.86× |
| Instruments Small (prettified) | 11,286 | 2,861 | 3.94× |
| Marine IK Reverse Small (minified) | 1,495 | 950 | 1.57× |
| Marine IK Reverse Small (prettified) | 5,267 | 2,909 | 1.81× |
| Marine IK Small (minified) | 1,507 | 974 | 1.55× |
| Marine IK Small (prettified) | 5,198 | 2,896 | 1.79× |
| Random Small (minified) | 9,162 | 3,512 | 2.61× |
| Random Small (prettified) | 11,599 | 4,144 | 2.80× |
| Twitter Small (minified) | 10,254 | 4,335 | 2.37× |
| Twitter Small (prettified) | 8,567 | 2,913 (n/c) | 2.94× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,038 | 1,841 | 1,369 | 1.65× |
| Prettify | 2,365 | 2,769 | 1,397 | 0.85× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,893 | 208 | 9.11× |
| Double | 320 | 294 | 1.09× |
| Int64 | 909 | 853 | 1.07× |
| String | 4,059 | 1,877 | 2.16× |
| Uint64 | 909 | 837 | 1.09× |
| Apache Builds (minified) | 16,244 | 5,257 | 3.09× |
| Canada (minified) | 1,855 | 920 | 2.02× |
| Apache Builds (prettified) | 18,571 | 5,234 | 3.55× |
| Canada (prettified) | 5,143 | 3,158 | 1.63× |
| CitmCatalog (minified) | 9,968 | 4,942 | 2.02× |
| CitmCatalog (prettified) | 14,996 | 5,176 | 2.90× |
| Discord (minified) | 14,247 | 4,907 | 2.90× |
| Discord (prettified) | 15,908 | 4,287 | 3.71× |
| Github Events (minified) | 16,381 | 4,941 | 3.32× |
| Google Maps Response (minified) | 11,328 | 3,696 | 3.07× |
| Github Events (prettified) | 16,405 | 4,843 | 3.39× |
| Google Maps Response (prettified) | 15,602 | 5,020 | 3.11× |
| Instruments (minified) | 18,168 | 4,527 | 4.01× |
| Instruments (prettified) | 19,800 | 4,843 | 4.09× |
| Marine IK Reverse (minified) | 1,175 | 628 | 1.87× |
| Marine IK Reverse (prettified) | 4,988 | 3,494 | 1.43× |
| Marine IK (minified) | 1,185 | 602 | 1.97× |
| Marine IK (prettified) | 4,659 | 2,658 | 1.75× |
| Random (minified) | 14,399 | 3,433 | 4.19× |
| Random (prettified) | 15,707 | 3,842 | 4.09× |
| Twitter (minified) | 18,986 | 6,446 | 2.95× |
| Twitter (prettified) | 18,288 | 4,712 | 3.88× |
| Apache Builds Small (minified) | 9,543 | 3,353 | 2.85× |
| Canada Small (minified) | 1,865 | 958 | 1.95× |
| Apache Builds Small (prettified) | 10,760 | 3,559 | 3.02× |
| Canada Small (prettified) | 4,160 | 3,440 | 1.21× |
| CitmCatalog Small (minified) | 7,453 | 2,638 | 2.83× |
| CitmCatalog Small (prettified) | 10,247 | 3,207 | 3.20× |
| Discord Small (minified) | 9,956 | 2,674 | 3.72× |
| Discord Small (prettified) | 11,354 | 2,617 | 4.34× |
| Github Events Small (minified) | 9,421 | 3,405 | 2.77× |
| Google Maps Response Small (minified) | 10,917 | 3,622 | 3.01× |
| Github Events Small (prettified) | 10,637 | 3,307 | 3.22× |
| Google Maps Response Small (prettified) | 16,143 | 4,640 | 3.48× |
| Instruments Small (minified) | 12,225 | 2,339 | 5.23× |
| Instruments Small (prettified) | 14,549 | 2,646 | 5.50× |
| Marine IK Reverse Small (minified) | 1,348 | 777 | 1.74× |
| Marine IK Reverse Small (prettified) | 5,113 | 3,690 | 1.39× |
| Marine IK Small (minified) | 1,498 | 791 | 1.90× |
| Marine IK Small (prettified) | 5,032 | 3,406 | 1.48× |
| Random Small (minified) | 11,948 | 3,328 | 3.59× |
| Random Small (prettified) | 14,894 | 3,856 | 3.86× |
| Twitter Small (minified) | 9,027 | 4,013 | 2.25× |
| Twitter Small (prettified) | 9,180 | 3,054 | 3.01× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,538 | 2,686 | 1,743 | 1.32× |
| Prettify | 4,468 | 3,406 | 2,335 | 1.31× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,677 | 193 | 253 | 6.64× |
| Double | 337 | 304 | 332 | 1.02× |
| Int64 | 936 | 832 | 939 | 1.00× |
| String | 4,193 | 2,021 | 3,703 | 1.13× |
| Uint64 | 952 | 848 | 852 | 1.12× |
| Apache Builds (minified) | 20,008 | 7,732 | 13,329 | 1.50× |
| Canada (minified) | 2,082 | 1,410 | 1,023 | 1.48× |
| Apache Builds (prettified) | 22,824 | 7,061 | 628 | 3.23× |
| Canada (prettified) | 5,941 | 3,250 | 387 | 1.83× |
| CitmCatalog (minified) | 12,361 | 5,927 | 11,184 | 1.11× |
| CitmCatalog (prettified) | 20,338 | 5,649 | 578 | 3.60× |
| Discord (minified) | 14,136 | 6,296 | 13,130 | 1.08× |
| Discord (prettified) | 20,548 | 5,965 | 524 | 3.44× |
| Github Events (minified) | 14,471 | 6,973 | 10,984 | 1.32× |
| Google Maps Response (minified) | 15,834 | 4,542 | 9,582 | 1.65× |
| Github Events (prettified) | 18,169 | 5,061 | 625 | 3.59× |
| Google Maps Response (prettified) | 20,597 | 6,536 | 525 | 3.15× |
| Instruments (minified) | 19,882 | 5,693 | 14,900 | 1.33× |
| Instruments (prettified) | 25,941 | 5,389 | 462 | 4.81× |
| Marine IK Reverse (minified) | 1,278 | 967 | 877 | 1.32× |
| Marine IK Reverse (prettified) | 6,335 | 3,551 | 266 | 1.78× |
| Marine IK (minified) | 1,258 | 926 | 873 | 1.36× |
| Marine IK (prettified) | 6,172 | 3,062 | 389 | 2.02× |
| Random (minified) | 17,015 | 4,505 | 11,231 | 1.52× |
| Random (prettified) | 19,811 | 4,804 | 527 | 4.12× |
| Twitter (minified) | 17,246 | 7,625 | 10,803 | 1.60× |
| Twitter (prettified) | 20,778 | 5,032 | 518 | 4.13× |
| Apache Builds Small (minified) | 12,029 | 4,410 | 7,059 | 1.70× |
| Canada Small (minified) | 2,022 | 1,547 | 1,067 | 1.31× |
| Apache Builds Small (prettified) | 15,647 | 4,178 | 531 | 3.75× |
| Canada Small (prettified) | 6,166 | 3,589 | 399 | 1.72× |
| CitmCatalog Small (minified) | 7,723 | 3,396 | 6,032 | 1.28× |
| CitmCatalog Small (prettified) | 14,834 | 3,042 | 493 | 4.88× |
| Discord Small (minified) | 10,290 | 4,061 | 7,796 | 1.32× |
| Discord Small (prettified) | 15,183 | 3,404 | 496 | 4.46× |
| Github Events Small (minified) | 7,912 | 5,274 | 6,324 | 1.25× |
| Google Maps Response Small (minified) | 13,052 | 3,823 | 8,026 | 1.63× |
| Github Events Small (prettified) | 10,929 | 3,666 | 530 | 2.98× |
| Google Maps Response Small (prettified) | 28,502 | 5,854 | 521 | 4.87× |
| Instruments Small (minified) | 12,027 | 3,937 | 8,075 | 1.49× |
| Instruments Small (prettified) | 21,646 | 3,149 | 521 | 6.87× |
| Marine IK Reverse Small (minified) | 1,307 | 1,210 | 1,523 | 0.86× |
| Marine IK Reverse Small (prettified) | 6,732 | 3,420 | 523 | 1.97× |
| Marine IK Small (minified) | 1,301 | 1,177 | 1,523 | 0.85× |
| Marine IK Small (prettified) | 6,401 | 3,563 | 530 | 1.80× |
| Random Small (minified) | 14,687 | 4,264 | 8,235 | 1.78× |
| Random Small (prettified) | 24,240 | 4,269 | 533 | 5.68× |
| Twitter Small (minified) | 9,172 | 5,700 | 6,606 | 1.39× |
| Twitter Small (prettified) | 12,202 | 3,736 | 549 | 3.27× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,085 | 2,558 | 1,849 | 1.21× |
| Prettify | 3,944 | 3,143 | 2,469 | 1.25× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,794 | 127 | 216 | 8.32× |
| Double | 266 | 200 | 250 | 1.06× |
| Int64 | 761 | 478 | 722 | 1.05× |
| String | 1,353 | 1,002 | 1,069 | 1.27× |
| Uint64 | 760 | 511 | 761 | 1.00× |
| Apache Builds (minified) | 11,688 | 4,749 | 8,243 | 1.42× |
| Canada (minified) | 2,283 | 1,480 | 830 | 1.54× |
| Apache Builds (prettified) | 12,613 | 4,237 | 338 | 2.98× |
| Canada (prettified) | 7,438 | 3,478 | 300 | 2.14× |
| CitmCatalog (minified) | 8,676 | 2,000 | 8,166 | 1.06× |
| CitmCatalog (prettified) | 18,210 | 3,593 | 308 | 5.07× |
| Discord (minified) | 11,448 | 2,937 | 9,538 | 1.20× |
| Discord (prettified) | 14,104 | 2,137 | 312 | 6.60× |
| Github Events (minified) | 13,000 | 3,320 | 10,270 | 1.27× |
| Google Maps Response (minified) | 9,846 | 1,779 | 7,615 | 1.29× |
| Github Events (prettified) | 14,253 | 2,914 | 371 | 4.89× |
| Google Maps Response (prettified) | 17,509 | 2,591 | 300 | 6.76× |
| Instruments (minified) | 12,632 | 1,720 | 8,197 | 1.54× |
| Instruments (prettified) | 16,534 | 2,084 | 299 | 7.93× |
| Marine IK Reverse (minified) | 1,397 | 854 | 676 | 1.64× |
| Marine IK Reverse (prettified) | 5,514 | 2,926 | 264 | 1.88× |
| Marine IK (minified) | 1,310 | 798 | 765 | 1.64× |
| Marine IK (prettified) | 5,406 | 2,667 (n/c) | 267 | 2.03× |
| Random (minified) | 10,206 | 2,064 | 7,082 | 1.44× |
| Random (prettified) | 13,151 | 2,162 | 299 | 6.08× |
| Twitter (minified) | 14,688 | 3,436 | 10,364 | 1.42× |
| Twitter (prettified) | 15,036 | 3,374 | 383 | 4.46× |
| Apache Builds Small (minified) | 9,269 | 2,687 | 5,598 | 1.66× |
| Canada Small (minified) | 2,117 | 1,650 | 770 | 1.28× |
| Apache Builds Small (prettified) | 7,555 | 3,509 | 357 | 2.15× |
| Canada Small (prettified) | 7,602 | 3,327 | 299 | 2.28× |
| CitmCatalog Small (minified) | 9,360 | 1,992 | 7,746 | 1.21× |
| CitmCatalog Small (prettified) | 13,318 | 2,166 | 276 | 6.15× |
| Discord Small (minified) | 10,868 | 2,518 | 8,145 | 1.33× |
| Discord Small (prettified) | 13,496 | 2,202 | 336 | 6.13× |
| Github Events Small (minified) | 9,383 | 3,093 | 10,328 | 0.91× |
| Google Maps Response Small (minified) | 10,619 | 1,872 | 8,343 | 1.27× |
| Github Events Small (prettified) | 14,206 | 2,976 | 372 | 4.77× |
| Google Maps Response Small (prettified) | 20,958 | 2,885 | 337 | 7.27× |
| Instruments Small (minified) | 14,730 | 1,968 | 7,997 | 1.84× |
| Instruments Small (prettified) | 17,406 | 2,400 | 344 | 7.25× |
| Marine IK Reverse Small (minified) | 1,925 | 1,098 | 1,670 | 1.15× |
| Marine IK Reverse Small (prettified) | 8,254 | 3,366 | 324 | 2.45× |
| Marine IK Small (minified) | 1,953 | 1,053 | 1,731 | 1.13× |
| Marine IK Small (prettified) | 8,723 | 3,366 | 247 | 2.59× |
| Random Small (minified) | 12,589 | 2,254 | 9,664 | 1.30× |
| Random Small (prettified) | 15,798 | 2,581 | 346 | 6.12× |
| Twitter Small (minified) | 11,619 | 3,365 | 8,803 | 1.32× |
| Twitter Small (prettified) | 15,388 | 3,364 | 420 | 4.57× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,460 | 1,688 | 1,232 | 1.46× |
| Prettify | 3,259 | 2,390 | 1,640 | 1.36× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 989 | 111 | 8.88× |
| Double | 296 | 226 | 1.31× |
| Int64 | 2,992 | 551 | 5.43× |
| String | 1,403 | 1,133 | 1.24× |
| Uint64 | 2,992 | 553 | 5.41× |
| Apache Builds (minified) | 12,162 | 3,463 | 3.51× |
| Canada (minified) | 2,822 | 1,673 | 1.69× |
| Apache Builds (prettified) | 12,112 | 3,576 | 3.39× |
| Canada (prettified) | 5,871 | 3,286 | 1.79× |
| CitmCatalog (minified) | 7,931 | 2,044 | 3.88× |
| CitmCatalog (prettified) | 13,027 | 3,803 | 3.43× |
| Discord (minified) | 11,974 | 3,081 | 3.89× |
| Discord (prettified) | 13,448 | 2,541 | 5.29× |
| Github Events (minified) | 14,390 | 3,968 | 3.63× |
| Google Maps Response (minified) | 9,721 | 2,162 | 4.50× |
| Github Events (prettified) | 10,787 | 3,222 | 3.35× |
| Google Maps Response (prettified) | 13,448 | 3,619 | 3.72× |
| Instruments (minified) | 11,234 | 1,802 | 6.23× |
| Instruments (prettified) | 14,514 | 2,974 | 4.88× |
| Marine IK Reverse (minified) | 1,327 | 849 | 1.56× |
| Marine IK Reverse (prettified) | 5,136 | 3,140 | 1.64× |
| Marine IK (minified) | 1,443 | 752 | 1.92× |
| Marine IK (prettified) | 4,665 | 2,864 | 1.63× |
| Random (minified) | 10,172 | 2,490 | 4.08× |
| Random (prettified) | 9,764 | 2,641 (n/c) | 3.70× |
| Twitter (minified) | 12,681 | 4,145 | 3.06× |
| Twitter (prettified) | 12,497 | 3,322 | 3.76× |
| Apache Builds Small (minified) | 8,630 | 3,110 | 2.78× |
| Canada Small (minified) | 2,516 | 1,562 | 1.61× |
| Apache Builds Small (prettified) | 9,619 | 3,238 | 2.97× |
| Canada Small (prettified) | 6,640 | 3,085 | 2.15× |
| CitmCatalog Small (minified) | 7,725 | 1,824 | 4.23× |
| CitmCatalog Small (prettified) | 10,444 | 2,354 | 4.44× |
| Discord Small (minified) | 8,743 | 2,526 | 3.46× |
| Discord Small (prettified) | 10,357 | 2,701 | 3.83× |
| Github Events Small (minified) | 9,482 | 3,374 | 2.81× |
| Google Maps Response Small (minified) | 8,589 | 1,917 | 4.48× |
| Github Events Small (prettified) | 10,280 | 3,032 | 3.39× |
| Google Maps Response Small (prettified) | 13,162 | 3,313 | 3.97× |
| Instruments Small (minified) | 11,874 | 1,836 | 6.47× |
| Instruments Small (prettified) | 10,412 | 3,072 | 3.39× |
| Marine IK Reverse Small (minified) | 1,549 | 941 | 1.65× |
| Marine IK Reverse Small (prettified) | 5,490 | 3,155 | 1.74× |
| Marine IK Small (minified) | 1,888 | 956 | 1.97× |
| Marine IK Small (prettified) | 5,292 | 3,029 | 1.75× |
| Random Small (minified) | 9,943 | 2,520 | 3.95× |
| Random Small (prettified) | 11,240 | 2,458 | 4.57× |
| Twitter Small (minified) | 9,455 | 3,487 | 2.71× |
| Twitter Small (prettified) | 10,621 | 3,079 | 3.45× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,351 | 1,600 | 1,108 | 1.47× |
| Prettify | 3,026 | 2,288 | 1,464 | 1.32× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 251 of the 255 write tests, ties 1 and loses 3, all against the reflection writer on the two GCC builds. On Linux / GCC it loses Bool (3,089 vs 2,963 MB/s, 0.96×), minified Marine IK Small (1,521 vs 1,401 MB/s, 0.92×) and minified Marine IK Reverse Small (1,523 vs 1,378 MB/s, 0.90×). On macOS / GCC it ties String: the reflection writer is ahead, 9,603 vs 9,264 MB/s, but its 2.2% RSE leaves the gap short of significance.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 1 | 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **251** | **1** | **3** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,659 | 288 | 5.75× |
| Double | 563 | 383 | 1.47× |
| Int64 | 4,179 | 1,121 | 3.73× |
| String | 8,030 | 2,375 | 3.38× |
| Uint64 | 4,361 | 1,154 | 3.78× |
| Apache Builds (minified) | 12,438 | 4,653 | 2.67× |
| Canada (minified) | 1,574 | 1,281 | 1.23× |
| Apache Builds (prettified) | 13,061 | 5,020 | 2.60× |
| Canada (prettified) | 4,713 | 3,328 | 1.42× |
| CitmCatalog (minified) | 8,848 | 4,802 | 1.84× |
| CitmCatalog (prettified) | 13,601 | 7,350 | 1.85× |
| Discord (minified) | 11,662 | 4,771 | 2.44× |
| Discord (prettified) | 12,707 | 4,472 | 2.84× |
| Github Events (minified) | 12,137 | 5,713 | 2.12× |
| Google Maps Response (minified) | 10,464 | 4,051 | 2.58× |
| Github Events (prettified) | 11,710 | 3,541 | 3.31× |
| Google Maps Response (prettified) | 15,949 | 5,726 | 2.79× |
| Instruments (minified) | 14,423 | 4,749 | 3.04× |
| Instruments (prettified) | 15,564 | 4,409 | 3.53× |
| Marine IK Reverse (minified) | 1,026 | 783 | 1.31× |
| Marine IK Reverse (prettified) | 3,607 | 3,548 | 1.02× |
| Marine IK (minified) | 1,037 | 788 | 1.32× |
| Marine IK (prettified) | 4,155 | 3,575 | 1.16× |
| Random (minified) | 11,408 | 3,412 | 3.34× |
| Random (prettified) | 12,642 | 4,278 | 2.95× |
| Twitter (minified) | 15,760 | 5,461 | 2.89× |
| Twitter (prettified) | 14,378 | 3,137 | 4.58× |
| Apache Builds Small (minified) | 11,379 | 4,158 | 2.74× |
| Canada Small (minified) | 1,926 | 1,297 | 1.48× |
| Apache Builds Small (prettified) | 12,254 | 4,322 | 2.84× |
| Canada Small (prettified) | 5,282 | 3,420 | 1.54× |
| CitmCatalog Small (minified) | 7,847 | 4,080 | 1.92× |
| CitmCatalog Small (prettified) | 10,030 | 5,009 | 2.00× |
| Discord Small (minified) | 11,074 | 3,899 | 2.84× |
| Discord Small (prettified) | 9,780 | 4,087 | 2.39× |
| Github Events Small (minified) | 12,531 | 4,643 | 2.70× |
| Google Maps Response Small (minified) | 10,044 | 4,154 | 2.42× |
| Github Events Small (prettified) | 12,610 | 4,959 | 2.54× |
| Google Maps Response Small (prettified) | 16,833 | 5,955 | 2.83× |
| Instruments Small (minified) | 11,859 | 3,314 | 3.58× |
| Instruments Small (prettified) | 12,545 | 4,009 | 3.13× |
| Marine IK Reverse Small (minified) | 1,551 | 1,013 | 1.53× |
| Marine IK Reverse Small (prettified) | 5,494 | 3,333 | 1.65× |
| Marine IK Small (minified) | 1,561 | 1,049 | 1.49× |
| Marine IK Small (prettified) | 5,472 | 3,566 | 1.53× |
| Random Small (minified) | 11,588 | 4,161 | 2.78× |
| Random Small (prettified) | 13,926 | 4,561 | 3.05× |
| Twitter Small (minified) | 12,354 | 5,694 | 2.17× |
| Twitter Small (prettified) | 10,991 | 2,888 | 3.81× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,059 | 1,840 | 1,361 | 1.66× |
| Prettify | 3,950 | 2,765 | 1,408 | 1.43× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,248 | 937 | 4.54× |
| Double | 828 | 678 | 1.22× |
| Int64 | 4,854 | 2,350 | 2.07× |
| String | 11,419 | 4,248 | 2.69× |
| Uint64 | 4,504 | 2,377 | 1.90× |
| Apache Builds (minified) | 16,362 | 5,939 | 2.76× |
| Canada (minified) | 1,845 | 959 | 1.93× |
| Apache Builds (prettified) | 18,293 | 5,649 | 3.24× |
| Canada (prettified) | 4,994 | 3,825 | 1.31× |
| CitmCatalog (minified) | 9,973 | 5,651 | 1.76× |
| CitmCatalog (prettified) | 14,857 | 6,907 | 2.15× |
| Discord (minified) | 14,662 | 5,420 | 2.71× |
| Discord (prettified) | 16,236 | 4,673 | 3.47× |
| Github Events (minified) | 16,422 | 5,733 | 2.86× |
| Google Maps Response (minified) | 11,997 | 4,158 | 2.88× |
| Github Events (prettified) | 16,626 | 5,552 | 2.99× |
| Google Maps Response (prettified) | 16,153 | 5,597 | 2.89× |
| Instruments (minified) | 18,397 | 4,913 | 3.74× |
| Instruments (prettified) | 19,970 | 5,454 | 3.66× |
| Marine IK Reverse (minified) | 1,174 | 627 | 1.87× |
| Marine IK Reverse (prettified) | 5,014 | 3,905 | 1.28× |
| Marine IK (minified) | 1,187 | 624 | 1.90× |
| Marine IK (prettified) | 4,637 | 3,717 | 1.25× |
| Random (minified) | 14,457 | 3,737 | 3.87× |
| Random (prettified) | 15,745 | 4,421 | 3.56× |
| Twitter (minified) | 19,276 | 6,878 | 2.80× |
| Twitter (prettified) | 18,408 | 5,759 | 3.20× |
| Apache Builds Small (minified) | 11,308 | 3,672 | 3.08× |
| Canada Small (minified) | 1,905 | 993 | 1.92× |
| Apache Builds Small (prettified) | 12,802 | 3,870 | 3.31× |
| Canada Small (prettified) | 4,608 | 3,833 | 1.20× |
| CitmCatalog Small (minified) | 8,086 | 3,051 | 2.65× |
| CitmCatalog Small (prettified) | 11,086 | 3,619 | 3.06× |
| Discord Small (minified) | 11,544 | 3,267 | 3.53× |
| Discord Small (prettified) | 12,597 | 2,850 | 4.42× |
| Github Events Small (minified) | 12,038 | 3,761 | 3.20× |
| Google Maps Response Small (minified) | 11,600 | 3,857 | 3.01× |
| Github Events Small (prettified) | 12,417 | 4,125 | 3.01× |
| Google Maps Response Small (prettified) | 17,431 | 5,363 | 3.25× |
| Instruments Small (minified) | 13,721 | 2,575 | 5.33× |
| Instruments Small (prettified) | 15,946 | 2,737 | 5.83× |
| Marine IK Reverse Small (minified) | 1,403 | 804 | 1.75× |
| Marine IK Reverse Small (prettified) | 5,105 | 3,376 | 1.51× |
| Marine IK Small (minified) | 1,557 | 829 | 1.88× |
| Marine IK Small (prettified) | 5,071 | 3,792 | 1.34× |
| Random Small (minified) | 13,309 | 3,709 | 3.59× |
| Random Small (prettified) | 16,540 | 4,310 | 3.84× |
| Twitter Small (minified) | 10,192 | 4,638 | 2.20× |
| Twitter Small (prettified) | 10,649 | 3,540 | 3.01× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,567 | 2,669 | 1,736 | 1.34× |
| Prettify | 4,485 | 3,418 | 2,305 | 1.31× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,963 | 683 | 3,089 | 0.96× |
| Double | 834 | 711 | 639 | 1.17× |
| Int64 | 5,346 | 2,636 | 3,832 | 1.40× |
| String | 11,233 | 6,084 | 9,256 | 1.21× |
| Uint64 | 6,236 | 2,694 | 4,083 | 1.53× |
| Apache Builds (minified) | 20,295 | 9,105 | 13,269 | 1.53× |
| Canada (minified) | 2,070 | 1,510 | 1,016 | 1.37× |
| Apache Builds (prettified) | 23,003 | 7,974 | 614 | 2.88× |
| Canada (prettified) | 6,010 | 3,855 | 385 | 1.56× |
| CitmCatalog (minified) | 12,396 | 6,990 | 11,101 | 1.12× |
| CitmCatalog (prettified) | 20,638 | 7,942 | 559 | 2.60× |
| Discord (minified) | 14,514 | 7,165 | 13,016 | 1.12× |
| Discord (prettified) | 20,744 | 6,950 | 530 | 2.98× |
| Github Events (minified) | 14,659 | 8,312 | 10,787 | 1.36× |
| Google Maps Response (minified) | 17,013 | 5,163 | 10,052 | 1.69× |
| Github Events (prettified) | 18,618 | 5,354 | 625 | 3.48× |
| Google Maps Response (prettified) | 21,984 | 7,240 | 526 | 3.04× |
| Instruments (minified) | 20,119 | 6,463 | 15,654 | 1.29× |
| Instruments (prettified) | 26,013 | 6,428 | 463 | 4.05× |
| Marine IK Reverse (minified) | 1,297 | 962 | 868 | 1.35× |
| Marine IK Reverse (prettified) | 6,369 | 3,972 | 298 | 1.60× |
| Marine IK (minified) | 1,258 | 978 | 874 | 1.29× |
| Marine IK (prettified) | 6,216 | 4,032 | 392 | 1.54× |
| Random (minified) | 17,115 | 5,107 | 11,274 | 1.52× |
| Random (prettified) | 19,860 | 5,675 | 523 | 3.50× |
| Twitter (minified) | 17,539 | 8,571 | 10,865 | 1.61× |
| Twitter (prettified) | 20,688 | 6,380 | 516 | 3.24× |
| Apache Builds Small (minified) | 13,932 | 5,021 | 8,071 | 1.73× |
| Canada Small (minified) | 2,084 | 1,686 | 1,071 | 1.24× |
| Apache Builds Small (prettified) | 17,569 | 4,533 | 530 | 3.88× |
| Canada Small (prettified) | 6,400 | 4,034 | 412 | 1.59× |
| CitmCatalog Small (minified) | 8,401 | 3,989 | 6,411 | 1.31× |
| CitmCatalog Small (prettified) | 16,205 | 3,277 | 489 | 4.94× |
| Discord Small (minified) | 11,322 | 4,812 | 8,258 | 1.37× |
| Discord Small (prettified) | 16,157 | 3,849 | 502 | 4.20× |
| Github Events Small (minified) | 9,610 | 6,085 | 7,513 | 1.28× |
| Google Maps Response Small (minified) | 13,749 | 4,210 | 8,734 | 1.57× |
| Github Events Small (prettified) | 14,049 | 4,499 | 528 | 3.12× |
| Google Maps Response Small (prettified) | 31,999 | 6,897 | 526 | 4.64× |
| Instruments Small (minified) | 14,126 | 4,588 | 8,613 | 1.64× |
| Instruments Small (prettified) | 24,424 | 3,235 | 518 | 7.55× |
| Marine IK Reverse Small (minified) | 1,378 | 1,312 | 1,523 | 0.90× |
| Marine IK Reverse Small (prettified) | 6,844 | 3,729 | 527 | 1.84× |
| Marine IK Small (minified) | 1,401 | 1,246 | 1,521 | 0.92× |
| Marine IK Small (prettified) | 6,493 | 4,025 | 526 | 1.61× |
| Random Small (minified) | 16,218 | 5,036 | 8,890 | 1.82× |
| Random Small (prettified) | 27,649 | 5,291 | 534 | 5.23× |
| Twitter Small (minified) | 10,226 | 6,418 | 7,342 | 1.39× |
| Twitter Small (prettified) | 14,180 | 4,207 | 550 | 3.37× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,090 | 2,549 | 1,846 | 1.21× |
| Prettify | 3,934 | 3,156 | 2,401 | 1.25× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,458 | 764 | 2,397 | 1.44× |
| Double | 790 | 642 | 641 | 1.23× |
| Int64 | 3,875 | 2,241 | 3,206 | 1.21× |
| String | 9,264 | 4,874 | 9,603 | 0.96× |
| Uint64 | 4,792 | 2,545 | 3,326 | 1.44× |
| Apache Builds (minified) | 12,776 | 6,086 | 9,108 | 1.40× |
| Canada (minified) | 2,299 | 1,729 | 832 (n/c) | 1.33× |
| Apache Builds (prettified) | 13,281 | 4,553 | 350 | 2.92× |
| Canada (prettified) | 7,432 | 3,934 | 300 | 1.89× |
| CitmCatalog (minified) | 8,717 | 2,357 | 8,213 | 1.06× |
| CitmCatalog (prettified) | 17,884 | 4,057 | 340 | 4.41× |
| Discord (minified) | 12,430 | 2,883 | 9,472 | 1.31× |
| Discord (prettified) | 13,302 | 2,390 | 312 | 5.57× |
| Github Events (minified) | 14,906 | 4,078 | 11,701 | 1.27× |
| Google Maps Response (minified) | 11,265 | 1,707 | 7,937 | 1.42× |
| Github Events (prettified) | 15,300 | 3,435 | 359 | 4.45× |
| Google Maps Response (prettified) | 18,305 | 2,834 | 293 | 6.46× |
| Instruments (minified) | 13,949 | 1,863 | 7,374 | 1.89× |
| Instruments (prettified) | 15,690 | 2,321 | 323 | 6.76× |
| Marine IK Reverse (minified) | 1,284 | 886 | 682 | 1.45× |
| Marine IK Reverse (prettified) | 5,627 | 2,905 | 260 | 1.94× |
| Marine IK (minified) | 1,407 | 910 | 757 | 1.55× |
| Marine IK (prettified) | 6,670 | 3,443 | 269 | 1.94× |
| Random (minified) | 10,303 | 2,161 | 7,095 | 1.45× |
| Random (prettified) | 14,064 | 2,715 | 289 | 5.18× |
| Twitter (minified) | 13,882 | 3,889 | 10,722 | 1.29× |
| Twitter (prettified) | 16,672 | 4,115 | 383 | 4.05× |
| Apache Builds Small (minified) | 9,168 (n/c) | 2,837 | 6,685 | 1.37× |
| Canada Small (minified) | 2,274 | 1,949 | 915 | 1.17× |
| Apache Builds Small (prettified) | 12,858 | 3,504 | 304 | 3.67× |
| Canada Small (prettified) | 7,881 | 3,698 | 300 | 2.13× |
| CitmCatalog Small (minified) | 9,845 | 2,065 | 8,101 | 1.22× |
| CitmCatalog Small (prettified) | 16,452 | 2,543 | 297 | 6.47× |
| Discord Small (minified) | 12,814 | 2,353 | 9,515 | 1.35× |
| Discord Small (prettified) | 13,548 | 2,476 | 338 | 5.47× |
| Github Events Small (minified) | 12,958 (n/c) | 3,622 | 11,271 | 1.15× |
| Google Maps Response Small (minified) | 11,420 | 1,987 | 8,942 | 1.28× |
| Github Events Small (prettified) | 16,179 | 3,249 | 379 | 4.98× |
| Google Maps Response Small (prettified) | 22,155 | 3,183 | 337 | 6.96× |
| Instruments Small (minified) | 15,707 | 2,085 | 8,740 | 1.80× |
| Instruments Small (prettified) | 19,026 | 2,599 | 342 | 7.32× |
| Marine IK Reverse Small (minified) | 1,985 | 1,162 | 1,683 | 1.18× |
| Marine IK Reverse Small (prettified) | 8,993 | 3,811 | 325 | 2.36× |
| Marine IK Small (minified) | 1,980 | 1,169 | 1,732 | 1.14× |
| Marine IK Small (prettified) | 8,986 | 3,819 | 325 | 2.35× |
| Random Small (minified) | 13,266 | 2,549 | 9,986 | 1.33× |
| Random Small (prettified) | 15,683 | 2,973 | 346 | 5.27× |
| Twitter Small (minified) | 14,386 | 3,741 | 10,404 | 1.38× |
| Twitter Small (prettified) | 14,650 (n/c) | 4,049 | 419 | 3.62× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,440 | 1,402 | 1,238 | 1.74× |
| Prettify | 3,279 | 2,401 | 1,593 | 1.37× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,163 | 747 | 1.56× |
| Double | 874 | 710 | 1.23× |
| Int64 | 3,482 | 2,147 | 1.62× |
| String | 7,861 | 4,248 | 1.85× |
| Uint64 | 3,430 | 2,239 | 1.53× |
| Apache Builds (minified) | 12,530 | 4,066 | 3.08× |
| Canada (minified) | 2,981 | 1,840 | 1.62× |
| Apache Builds (prettified) | 12,293 | 3,941 | 3.12× |
| Canada (prettified) | 6,590 | 4,015 | 1.64× |
| CitmCatalog (minified) | 7,641 | 2,047 | 3.73× |
| CitmCatalog (prettified) | 13,738 | 4,529 | 3.03× |
| Discord (minified) | 12,058 | 3,527 | 3.42× |
| Discord (prettified) | 13,908 | 3,450 | 4.03× |
| Github Events (minified) | 14,302 | 5,933 | 2.41× |
| Google Maps Response (minified) | 8,183 | 2,176 | 3.76× |
| Github Events (prettified) | 11,923 | 4,189 | 2.85× |
| Google Maps Response (prettified) | 15,553 | 4,384 | 3.55× |
| Instruments (minified) | 12,983 | 2,277 | 5.70× |
| Instruments (prettified) | 15,003 | 3,351 | 4.48× |
| Marine IK Reverse (minified) | 1,460 | 863 | 1.69× |
| Marine IK Reverse (prettified) | 5,177 | 3,028 | 1.71× |
| Marine IK (minified) | 1,549 | 805 | 1.92× |
| Marine IK (prettified) | 4,491 | 2,660 | 1.69× |
| Random (minified) | 9,170 | 2,513 | 3.65× |
| Random (prettified) | 11,236 | 3,109 | 3.61× |
| Twitter (minified) | 13,140 | 4,766 | 2.76× |
| Twitter (prettified) | 12,029 | 4,095 | 2.94× |
| Apache Builds Small (minified) | 7,902 | 3,961 | 2.00× |
| Canada Small (minified) | 2,695 | 1,814 | 1.49× |
| Apache Builds Small (prettified) | 8,812 | 3,871 | 2.28× |
| Canada Small (prettified) | 6,843 | 3,904 | 1.75× |
| CitmCatalog Small (minified) | 8,687 | 1,815 | 4.79× |
| CitmCatalog Small (prettified) | 12,412 | 2,809 | 4.42× |
| Discord Small (minified) | 12,458 | 3,979 | 3.13× |
| Discord Small (prettified) | 10,693 | 3,373 | 3.17× |
| Github Events Small (minified) | 12,952 | 4,728 | 2.74× |
| Google Maps Response Small (minified) | 8,741 | 2,173 | 4.02× |
| Github Events Small (prettified) | 13,863 | 4,166 | 3.33× |
| Google Maps Response Small (prettified) | 13,932 | 3,957 | 3.52× |
| Instruments Small (minified) | 14,679 | 2,085 | 7.04× |
| Instruments Small (prettified) | 15,463 | 3,057 | 5.06× |
| Marine IK Reverse Small (minified) | 1,660 | 1,095 | 1.52× |
| Marine IK Reverse Small (prettified) | 4,932 | 3,760 | 1.31× |
| Marine IK Small (minified) | 1,943 | 1,083 | 1.79× |
| Marine IK Small (prettified) | 5,626 | 3,592 | 1.57× |
| Random Small (minified) | 11,320 | 2,847 | 3.98× |
| Random Small (prettified) | 12,643 | 3,182 | 3.97× |
| Twitter Small (minified) | 13,136 | 4,236 | 3.10× |
| Twitter Small (prettified) | 13,288 | 4,148 | 3.20× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,440 | 1,609 | 1,075 | 1.52× |
| Prettify | 2,689 | 2,302 | 1,630 | 1.17× |

### Losses and ties

The five freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,864 vs Jsonifier's 3,832 MB/s (0.79×). Reused, Jsonifier wins by 2.89× (15,760 vs 5,461).
- **Prettify, Windows / MSVC.** Glaze runs 2,769 vs 2,365 MB/s (0.85×), with simdjson On Demand at 1,397. Reused, Jsonifier wins by 1.43× (3,950 vs 2,765).
- **Int64, Linux / GCC.** The reflection writer runs 939 vs 936 MB/s; the gap is 0.3%, but both sit at 0.0% RSE, so it is significant. Glaze is behind at 832. Reused, Jsonifier wins by 1.40× (5,346 vs 3,832).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,523 vs 1,301 MB/s (0.85×), with Glaze at 1,177. It also wins reused, 1,521 vs 1,401 MB/s (0.92×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,523 vs 1,307 MB/s (0.86×), with Glaze at 1,210. It also wins reused, 1,523 vs 1,378 MB/s (0.90×).

The two freshly allocated ties are both on macOS / GCC against the reflection writer: Uint64 (761 vs 760 MB/s, Glaze at 511), which Jsonifier wins reused by 1.44× (4,792 vs 3,326), and minified Github Events Small (10,328 vs 9,383 MB/s, 0.91×, at 4.2% and 2.9% RSE), which it wins reused, 12,958 vs 11,271 MB/s (1.15×).

Two reused non-wins have no freshly allocated counterpart, both against the reflection writer: Bool on Linux / GCC, which wins freshly allocated by 6.64× (1,677 vs 253 MB/s) and loses reused (0.96×), and String on macOS / GCC, which wins freshly allocated (1,353 MB/s against the reflection writer's 1,069 and Glaze's 1,002) and ties reused.

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.06–2.75× and prettified documents by 15–59×; reused, minified documents lead it by 1.06–2.76×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 41 of the 44 Small document tests freshly allocated and 42 reused. Freshly allocated it loses the two minified Marine IK Small tests on Linux / GCC and ties minified Github Events Small on macOS / GCC; reused it loses the two Marine IK Small tests. Why the reflection writer edges ahead on the two Marine IK Small tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.21–1.65× freshly allocated and 1.21–1.74× reused. It wins Prettify on all five reused, by 1.17–1.43×, and on four of five freshly allocated, by 1.25–1.36×; the exception is Windows / MSVC, where Glaze runs 2,769 vs 2,365 MB/s (0.85×). Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,353 MB/s on macOS / GCC (1.35× Glaze's 1,002, ahead of the reflection writer's 1,069) and 1,403 MB/s on macOS / Clang (1.24× Glaze's 1,133). The AVX2 builds run the same test at 1,417–4,193 MB/s and lead Glaze by 1.58× (Windows / MSVC), 2.16× (Linux / Clang) and 2.08× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.85× on macOS / Clang (7,861 vs 4,248 MB/s) and 1.90× on macOS / GCC (9,264 vs 4,874), against 1.85–3.38× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 9,603 MB/s (0.96×), but within a statistical tie.

So most of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: reused, both M2 Pro leads over Glaze sit at the bottom of the AVX2 range. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

No, on neither GCC build in either run. Freshly allocated, Jsonifier edges it on Linux / GCC, 337 vs 332 MB/s (1.02×), and on macOS / GCC, 266 vs 250 MB/s (1.06×). Reused, Jsonifier leads on both: 834 vs 639 MB/s on Linux / GCC (1.31×) and 790 vs 641 MB/s on macOS / GCC (1.23×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
