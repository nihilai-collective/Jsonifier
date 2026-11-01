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

On the Bool test this writer leads Glaze by 10.58–50.07× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.32× to 4.67×.

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

Jsonifier was fastest in 247 of the 255 write tests on five builds, tied 3 and lost 5. With the output string reused across iterations it was fastest in 251 of 255, tied 1 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify and Prettify on every build in both runs, and every full-size document test except minified Twitter on Windows / MSVC freshly allocated. Freshly allocated, that Twitter result is the only loss to Glaze; the other four are against simdjson's reflection writer, which exists only on GCC: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and Double and Int64 on macOS / GCC. The freshly allocated ties are Uint64 and minified Github Events Small on macOS / GCC against the reflection writer, and String on macOS / Clang against Glaze. Reused, all three losses are against the reflection writer: String on macOS / GCC and minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and minified Canada Small on Linux / GCC is a tie with Glaze. Linux / Clang wins all 51 tests in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.39–11.66× across the five builds and the reflection writer by 16–61× on the two GCC builds; Small documents lead them by 1.39–12.51× and 12.65–62.45×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 1, 2026, with Jsonifier [c338fdb](https://github.com/nihilai-collective/jsonifier/commit/c338fdb), Glaze [01efb29](https://github.com/stephenberry/glaze/commit/01efb29) and BenchmarkSuite [35ec835](https://github.com/nihilai-collective/benchmarksuite/commit/35ec835) on every build, and simdjson [a78278a](https://github.com/simdjson/simdjson/commit/a78278a) ([40a686a](https://github.com/simdjson/simdjson/commit/40a686a) under MSVC). Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at the top, and as a loss when another library ranks above it.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 47 | 2 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 50 | 1 | 0 |
| **Total** | **255** | **247** | **3** | **5** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,184 | 24 | 50.07× |
| Double | 458 | 155 | 2.95× |
| Int64 | 515 | 377 | 1.37× |
| String | 1,408 | 922 | 1.53× |
| Uint64 | 490 (n/c) | 381 | 1.29× |
| Apache Builds (minified) | 12,274 | 4,246 | 2.89× |
| Canada (minified) | 1,275 | 923 | 1.38× |
| Apache Builds (prettified) | 14,266 | 4,637 | 3.08× |
| Canada (prettified) | 2,933 | 1,561 | 1.88× |
| CitmCatalog (minified) | 8,578 | 4,481 | 1.91× |
| CitmCatalog (prettified) | 4,505 | 1,902 | 2.37× |
| Discord (minified) | 10,599 | 4,069 | 2.60× |
| Discord (prettified) | 13,169 | 3,876 | 3.40× |
| Github Events (minified) | 11,830 | 4,734 | 2.50× |
| Google Maps Response (minified) | 8,844 | 3,313 | 2.67× |
| Github Events (prettified) | 13,369 | 3,115 | 4.29× |
| Google Maps Response (prettified) | 13,942 | 4,621 | 3.02× |
| Instruments (minified) | 12,917 | 4,153 | 3.11× |
| Instruments (prettified) | 18,303 | 4,126 | 4.44× |
| Marine IK Reverse (minified) | 880 | 712 | 1.24× |
| Marine IK Reverse (prettified) | 3,055 | 1,816 | 1.68× |
| Marine IK (minified) | 867 | 610 | 1.42× |
| Marine IK (prettified) | 3,089 | 1,282 | 2.41× |
| Random (minified) | 3,715 | 2,963 | 1.25× |
| Random (prettified) | 4,006 | 2,135 | 1.88× |
| Twitter (minified) | 4,065 | 4,791 | 0.85× |
| Twitter (prettified) | 4,354 | 1,724 | 2.53× |
| Apache Builds Small (minified) | 9,761 | 3,637 | 2.68× |
| Canada Small (minified) | 1,845 | 1,199 | 1.54× |
| Apache Builds Small (prettified) | 12,732 | 3,754 | 3.39× |
| Canada Small (prettified) | 5,555 | 3,023 | 1.84× |
| CitmCatalog Small (minified) | 6,735 | 3,275 | 2.06× |
| CitmCatalog Small (prettified) | 12,034 | 3,985 | 3.02× |
| Discord Small (minified) | 9,239 | 3,198 | 2.89× |
| Discord Small (prettified) | 10,326 | 2,993 | 3.45× |
| Github Events Small (minified) | 10,093 | 3,669 | 2.75× |
| Google Maps Response Small (minified) | 9,130 | 3,450 | 2.65× |
| Github Events Small (prettified) | 12,433 | 3,795 | 3.28× |
| Google Maps Response Small (prettified) | 15,996 | 4,678 | 3.42× |
| Instruments Small (minified) | 10,393 | 3,386 | 3.07× |
| Instruments Small (prettified) | 17,065 | 3,416 | 5.00× |
| Marine IK Reverse Small (minified) | 1,499 | 923 | 1.62× |
| Marine IK Reverse Small (prettified) | 6,475 | 2,730 | 2.37× |
| Marine IK Small (minified) | 1,501 | 899 | 1.67× |
| Marine IK Small (prettified) | 6,736 | 2,785 | 2.42× |
| Random Small (minified) | 9,050 | 3,285 | 2.75× |
| Random Small (prettified) | 14,196 | 3,989 | 3.56× |
| Twitter Small (minified) | 10,143 | 3,838 (n/c) | 2.64× |
| Twitter Small (prettified) | 10,567 | 3,176 | 3.33× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,114 | 2,044 | 1,318 | 1.52× |
| Prettify | 2,498 | 2,131 | 1,349 | 1.17× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,331 | 217 | 10.75× |
| Double | 331 | 255 | 1.30× |
| Int64 | 948 | 858 | 1.11× |
| String | 4,223 | 1,580 | 2.67× |
| Uint64 | 929 | 655 | 1.42× |
| Apache Builds (minified) | 16,061 | 5,177 | 3.10× |
| Canada (minified) | 1,923 | 959 | 2.00× |
| Apache Builds (prettified) | 18,560 | 5,155 | 3.60× |
| Canada (prettified) | 4,579 | 3,287 | 1.39× |
| CitmCatalog (minified) | 10,509 | 5,632 | 1.87× |
| CitmCatalog (prettified) | 17,723 | 5,700 | 3.11× |
| Discord (minified) | 13,338 | 5,898 | 2.26× |
| Discord (prettified) | 17,678 | 5,056 | 3.50× |
| Github Events (minified) | 16,397 | 6,418 | 2.56× |
| Google Maps Response (minified) | 11,860 | 4,001 | 2.96× |
| Github Events (prettified) | 20,005 | 4,978 | 4.02× |
| Google Maps Response (prettified) | 17,940 | 5,125 | 3.50× |
| Instruments (minified) | 18,904 | 5,582 | 3.39× |
| Instruments (prettified) | 23,472 | 5,561 | 4.22× |
| Marine IK Reverse (minified) | 1,202 | 618 | 1.94× |
| Marine IK Reverse (prettified) | 5,805 | 3,445 | 1.68× |
| Marine IK (minified) | 1,228 | 611 | 2.01× |
| Marine IK (prettified) | 6,016 | 3,026 | 1.99× |
| Random (minified) | 14,394 | 3,749 | 3.84× |
| Random (prettified) | 18,235 | 4,134 | 4.41× |
| Twitter (minified) | 19,034 | 6,925 | 2.75× |
| Twitter (prettified) | 20,345 | 5,071 | 4.01× |
| Apache Builds Small (minified) | 9,750 | 3,580 | 2.72× |
| Canada Small (minified) | 1,912 | 973 | 1.96× |
| Apache Builds Small (prettified) | 14,685 | 3,760 | 3.91× |
| Canada Small (prettified) | 5,039 | 3,621 | 1.39× |
| CitmCatalog Small (minified) | 7,580 | 3,161 | 2.40× |
| CitmCatalog Small (prettified) | 14,793 | 3,685 | 4.01× |
| Discord Small (minified) | 10,495 | 3,634 | 2.89× |
| Discord Small (prettified) | 13,763 | 2,985 | 4.61× |
| Github Events Small (minified) | 10,409 | 4,239 | 2.46× |
| Google Maps Response Small (minified) | 11,005 | 3,805 | 2.89× |
| Github Events Small (prettified) | 13,178 | 3,682 | 3.58× |
| Google Maps Response Small (prettified) | 19,792 | 4,820 | 4.11× |
| Instruments Small (minified) | 12,646 | 3,291 | 3.84× |
| Instruments Small (prettified) | 21,379 | 3,906 | 5.47× |
| Marine IK Reverse Small (minified) | 1,537 | 828 | 1.86× |
| Marine IK Reverse Small (prettified) | 6,403 | 3,959 | 1.62× |
| Marine IK Small (minified) | 1,555 | 827 | 1.88× |
| Marine IK Small (prettified) | 7,277 | 4,041 | 1.80× |
| Random Small (minified) | 12,424 | 3,867 | 3.21× |
| Random Small (prettified) | 20,649 | 4,212 | 4.90× |
| Twitter Small (minified) | 8,581 | 5,297 | 1.62× |
| Twitter Small (prettified) | 11,631 | 3,461 | 3.36× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,654 | 2,735 | 1,772 | 1.34× |
| Prettify | 4,912 | 3,888 | 2,393 | 1.26× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,169 | 205 | 251 | 8.65× |
| Double | 353 | 312 | 345 | 1.02× |
| Int64 | 980 | 820 | 966 | 1.02× |
| String | 4,146 | 2,084 | 3,858 | 1.07× |
| Uint64 | 980 | 879 | 929 | 1.06× |
| Apache Builds (minified) | 20,172 | 8,117 | 13,395 | 1.51× |
| Canada (minified) | 2,175 | 1,460 | 1,048 | 1.49× |
| Apache Builds (prettified) | 23,994 | 7,206 | 621 | 3.33× |
| Canada (prettified) | 6,228 | 3,256 | 396 | 1.91× |
| CitmCatalog (minified) | 13,037 | 6,089 | 10,401 | 1.25× |
| CitmCatalog (prettified) | 21,028 | 6,133 | 589 | 3.43× |
| Discord (minified) | 15,539 | 6,326 | 11,777 | 1.32× |
| Discord (prettified) | 20,522 | 6,219 | 541 | 3.30× |
| Github Events (minified) | 14,670 | 7,078 | 9,834 | 1.49× |
| Google Maps Response (minified) | 16,193 | 4,120 | 9,793 | 1.65× |
| Github Events (prettified) | 18,080 | 5,237 | 588 | 3.45× |
| Google Maps Response (prettified) | 21,483 | 6,789 | 539 | 3.16× |
| Instruments (minified) | 20,323 | 5,791 | 16,079 | 1.26× |
| Instruments (prettified) | 26,115 | 6,070 | 491 | 4.30× |
| Marine IK Reverse (minified) | 1,308 | 1,011 | 896 | 1.29× |
| Marine IK Reverse (prettified) | 6,457 | 3,774 | 264 | 1.71× |
| Marine IK (minified) | 1,317 | 976 | 894 | 1.35× |
| Marine IK (prettified) | 6,498 | 3,221 | 368 | 2.02× |
| Random (minified) | 16,148 | 4,219 | 12,001 | 1.35× |
| Random (prettified) | 19,928 | 5,084 | 536 | 3.92× |
| Twitter (minified) | 17,303 | 7,646 | 10,034 | 1.72× |
| Twitter (prettified) | 21,040 | 5,221 | 515 | 4.03× |
| Apache Builds Small (minified) | 11,505 | 4,356 | 7,292 | 1.58× |
| Canada Small (minified) | 1,766 | 1,588 | 1,075 | 1.11× |
| Apache Builds Small (prettified) | 16,303 | 4,262 | 618 | 3.82× |
| Canada Small (prettified) | 6,416 | 3,444 | 421 | 1.86× |
| CitmCatalog Small (minified) | 7,700 | 3,465 | 5,384 | 1.43× |
| CitmCatalog Small (prettified) | 15,102 | 3,962 | 537 | 3.81× |
| Discord Small (minified) | 10,026 | 4,069 | 6,905 | 1.45× |
| Discord Small (prettified) | 15,093 | 3,705 | 570 | 4.07× |
| Github Events Small (minified) | 8,299 | 5,186 | 6,030 | 1.38× |
| Google Maps Response Small (minified) | 13,565 | 3,992 | 8,269 | 1.64× |
| Github Events Small (prettified) | 11,460 | 3,958 | 571 | 2.90× |
| Google Maps Response Small (prettified) | 26,224 | 5,710 | 534 | 4.59× |
| Instruments Small (minified) | 12,522 | 3,910 | 7,708 | 1.62× |
| Instruments Small (prettified) | 21,067 | 3,896 | 566 | 5.41× |
| Marine IK Reverse Small (minified) | 1,244 | 1,260 | 1,504 | 0.83× |
| Marine IK Reverse Small (prettified) | 7,140 | 3,806 | 492 | 1.88× |
| Marine IK Small (minified) | 1,299 | 1,117 | 1,502 | 0.86× |
| Marine IK Small (prettified) | 6,750 | 3,875 | 533 | 1.74× |
| Random Small (minified) | 12,710 | 4,349 | 8,216 | 1.55× |
| Random Small (prettified) | 21,941 | 4,853 | 579 | 4.52× |
| Twitter Small (minified) | 8,747 | 5,771 | 6,496 | 1.35× |
| Twitter Small (prettified) | 12,072 | 3,886 | 628 | 3.11× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,336 | 2,413 | 1,830 | 1.38× |
| Prettify | 4,333 | 3,703 | 2,598 | 1.17× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,240 | 135 | 235 | 9.52× |
| Double | 297 | 221 | 322 | 0.92× |
| Int64 | 710 | 574 | 826 | 0.86× |
| String | 1,561 | 1,134 | 1,312 | 1.19× |
| Uint64 | 844 | 586 | 842 | 1.00× |
| Apache Builds (minified) | 13,980 | 4,926 | 10,227 | 1.37× |
| Canada (minified) | 2,551 | 1,791 | 859 | 1.42× |
| Apache Builds (prettified) | 14,446 | 4,910 | 405 | 2.94× |
| Canada (prettified) | 7,795 | 3,637 | 309 (n/c) | 2.14× |
| CitmCatalog (minified) | 9,319 | 2,212 | 8,104 | 1.15× |
| CitmCatalog (prettified) | 21,074 | 4,107 | 364 | 5.13× |
| Discord (minified) | 13,253 | 2,934 | 9,627 | 1.38× |
| Discord (prettified) | 15,168 | 2,495 | 381 | 6.08× |
| Github Events (minified) | 16,296 | 3,976 | 10,243 | 1.59× |
| Google Maps Response (minified) | 11,651 | 1,866 | 8,781 | 1.33× |
| Github Events (prettified) | 16,954 | 3,564 | 487 | 4.76× |
| Google Maps Response (prettified) | 21,026 | 2,910 | 344 | 7.23× |
| Instruments (minified) | 13,356 | 1,893 | 2,515 | 5.31× |
| Instruments (prettified) | 18,331 | 2,406 | 375 | 7.62× |
| Marine IK Reverse (minified) | 1,379 | 939 | 649 | 1.47× |
| Marine IK Reverse (prettified) | 6,342 | 3,018 | 271 | 2.10× |
| Marine IK (minified) | 1,266 | 828 | 747 | 1.53× |
| Marine IK (prettified) | 5,885 | 2,395 | 279 | 2.46× |
| Random (minified) | 11,424 | 2,167 | 7,342 | 1.56× |
| Random (prettified) | 14,470 | 2,152 | 330 | 6.72× |
| Twitter (minified) | 14,789 | 3,647 | 11,036 | 1.34× |
| Twitter (prettified) | 13,989 | 3,619 | 359 | 3.87× |
| Apache Builds Small (minified) | 10,089 | 2,700 | 6,554 | 1.54× |
| Canada Small (minified) | 2,072 | 1,749 | 947 | 1.18× |
| Apache Builds Small (prettified) | 9,502 | 3,206 | 405 | 2.96× |
| Canada Small (prettified) | 7,059 | 3,294 | 287 | 2.14× |
| CitmCatalog Small (minified) | 8,391 | 1,636 | 6,488 | 1.29× |
| CitmCatalog Small (prettified) | 12,987 | 2,469 | 313 | 5.26× |
| Discord Small (minified) | 9,914 | 2,328 | 7,389 | 1.34× |
| Discord Small (prettified) | 13,793 | 1,988 | 340 | 6.94× |
| Github Events Small (minified) | 10,939 | 3,102 | 8,262 (n/c) | 1.32× |
| Google Maps Response Small (minified) | 10,008 | 1,596 | 7,439 | 1.35× |
| Github Events Small (prettified) | 11,479 (n/c) | 2,610 | 391 | 4.40× |
| Google Maps Response Small (prettified) | 18,510 | 2,532 | 296 | 7.31× |
| Instruments Small (minified) | 11,836 | 1,875 | 6,740 | 1.76× |
| Instruments Small (prettified) | 14,620 | 2,057 | 370 | 7.11× |
| Marine IK Reverse Small (minified) | 1,695 | 1,035 | 1,436 | 1.18× |
| Marine IK Reverse Small (prettified) | 8,020 | 3,228 | 288 | 2.48× |
| Marine IK Small (minified) | 1,957 | 1,090 | 1,389 | 1.41× |
| Marine IK Small (prettified) | 8,609 | 2,859 | 272 | 3.01× |
| Random Small (minified) | 12,274 | 2,246 | 8,075 | 1.52× |
| Random Small (prettified) | 13,774 | 2,359 | 336 | 5.84× |
| Twitter Small (minified) | 10,336 | 2,969 (n/c) | 8,660 | 1.19× |
| Twitter Small (prettified) | 11,680 | 2,798 | 412 | 4.17× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,019 | 1,465 | 1,130 | 1.38× |
| Prettify | 2,970 | 2,099 | 1,444 | 1.42× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,029 | 134 | 15.11× |
| Double | 312 | 224 | 1.40× |
| Int64 | 3,411 | 557 | 6.12× |
| String | 1,184 | 1,171 | 1.01× |
| Uint64 | 3,514 | 566 | 6.20× |
| Apache Builds (minified) | 12,192 | 3,437 | 3.55× |
| Canada (minified) | 2,997 | 1,894 | 1.58× |
| Apache Builds (prettified) | 17,114 | 3,727 | 4.59× |
| Canada (prettified) | 8,450 | 3,334 | 2.53× |
| CitmCatalog (minified) | 8,465 | 2,148 | 3.94× |
| CitmCatalog (prettified) | 20,214 | 3,233 | 6.25× |
| Discord (minified) | 12,893 | 2,548 | 5.06× |
| Discord (prettified) | 18,995 | 2,694 | 7.05× |
| Github Events (minified) | 14,428 | 3,743 | 3.85× |
| Google Maps Response (minified) | 7,741 | 1,651 | 4.69× |
| Github Events (prettified) | 18,117 | 3,160 | 5.73× |
| Google Maps Response (prettified) | 20,081 | 2,558 | 7.85× |
| Instruments (minified) | 11,883 | 1,930 | 6.16× |
| Instruments (prettified) | 24,837 | 2,131 | 11.66× |
| Marine IK Reverse (minified) | 1,540 | 948 | 1.63× |
| Marine IK Reverse (prettified) | 7,439 | 3,365 | 2.21× |
| Marine IK (minified) | 1,558 | 929 | 1.68× |
| Marine IK (prettified) | 6,906 | 3,048 | 2.27× |
| Random (minified) | 11,135 | 2,315 | 4.81× |
| Random (prettified) | 14,494 | 2,685 | 5.40× |
| Twitter (minified) | 16,297 | 4,191 | 3.89× |
| Twitter (prettified) | 18,502 | 4,174 | 4.43× |
| Apache Builds Small (minified) | 8,209 | 2,038 | 4.03× |
| Canada Small (minified) | 2,078 | 1,345 | 1.54× |
| Apache Builds Small (prettified) | 10,534 | 2,453 | 4.29× |
| Canada Small (prettified) | 8,037 | 3,068 | 2.62× |
| CitmCatalog Small (minified) | 7,432 | 1,611 | 4.61× |
| CitmCatalog Small (prettified) | 12,775 | 2,286 | 5.59× |
| Discord Small (minified) | 10,612 | 2,067 | 5.13× |
| Discord Small (prettified) | 14,724 | 2,088 | 7.05× |
| Github Events Small (minified) | 9,645 | 2,398 | 4.02× |
| Google Maps Response Small (minified) | 8,662 | 1,570 | 5.52× |
| Github Events Small (prettified) | 11,716 | 2,643 | 4.43× |
| Google Maps Response Small (prettified) | 20,282 | 2,330 | 8.70× |
| Instruments Small (minified) | 12,588 | 1,692 | 7.44× |
| Instruments Small (prettified) | 21,727 | 1,737 | 12.51× |
| Marine IK Reverse Small (minified) | 1,694 | 922 | 1.84× |
| Marine IK Reverse Small (prettified) | 7,364 | 3,018 | 2.44× |
| Marine IK Small (minified) | 1,914 | 911 | 2.10× |
| Marine IK Small (prettified) | 7,388 | 3,010 | 2.45× |
| Random Small (minified) | 9,660 | 1,780 | 5.43× |
| Random Small (prettified) | 13,341 | 2,160 | 6.18× |
| Twitter Small (minified) | 10,284 | 3,152 | 3.26× |
| Twitter Small (prettified) | 13,289 | 3,049 | 4.36× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,355 | 1,647 | 1,026 | 1.43× |
| Prettify | 3,310 | 2,120 | 1,351 | 1.56× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 251 of the 255 write tests, ties 1 and loses 3, all against the reflection writer: String on macOS / GCC, 9,857 vs 10,982 MB/s (0.90×), and minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, 1,381 vs 1,560 MB/s (0.89×) and 1,337 vs 1,508 MB/s (0.89×). The tie is minified Canada Small on Linux / GCC, 1,774 vs Glaze's 1,777 MB/s.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 1 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **251** | **1** | **3** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,776 | 283 | 6.29× |
| Double | 752 | 380 | 1.98× |
| Int64 | 4,219 | 1,044 | 4.04× |
| String | 7,891 | 2,285 | 3.45× |
| Uint64 | 4,865 | 1,042 | 4.67× |
| Apache Builds (minified) | 12,974 | 4,745 | 2.73× |
| Canada (minified) | 1,594 | 1,269 | 1.26× |
| Apache Builds (prettified) | 14,299 | 5,177 | 2.76× |
| Canada (prettified) | 4,698 | 3,311 | 1.42× |
| CitmCatalog (minified) | 8,478 | 4,922 | 1.72× |
| CitmCatalog (prettified) | 15,136 | 7,443 | 2.03× |
| Discord (minified) | 11,173 | 4,539 | 2.46× |
| Discord (prettified) | 13,911 | 4,375 | 3.18× |
| Github Events (minified) | 12,299 | 5,417 | 2.27× |
| Google Maps Response (minified) | 10,216 | 3,876 | 2.64× |
| Github Events (prettified) | 14,522 | 3,348 | 4.34× |
| Google Maps Response (prettified) | 15,247 | 5,285 | 2.89× |
| Instruments (minified) | 13,651 | 5,061 | 2.70× |
| Instruments (prettified) | 18,820 | 4,681 | 4.02× |
| Marine IK Reverse (minified) | 1,007 | 776 | 1.30× |
| Marine IK Reverse (prettified) | 5,000 | 3,477 | 1.44× |
| Marine IK (minified) | 987 | 766 | 1.29× |
| Marine IK (prettified) | 4,927 | 3,505 | 1.41× |
| Random (minified) | 11,294 | 3,292 | 3.43× |
| Random (prettified) | 14,016 | 4,212 | 3.33× |
| Twitter (minified) | 15,383 | 5,475 | 2.81× |
| Twitter (prettified) | 15,136 | 3,341 | 4.53× |
| Apache Builds Small (minified) | 10,527 | 4,183 | 2.52× |
| Canada Small (minified) | 1,837 | 1,299 | 1.41× |
| Apache Builds Small (prettified) | 14,552 | 4,690 | 3.10× |
| Canada Small (prettified) | 5,789 | 3,419 | 1.69× |
| CitmCatalog Small (minified) | 7,696 | 3,966 | 1.94× |
| CitmCatalog Small (prettified) | 13,445 | 5,094 | 2.64× |
| Discord Small (minified) | 10,234 | 4,380 | 2.34× |
| Discord Small (prettified) | 13,080 | 3,155 | 4.15× |
| Github Events Small (minified) | 13,220 (n/c) | 5,572 | 2.37× |
| Google Maps Response Small (minified) | 9,673 | 3,995 | 2.42× |
| Github Events Small (prettified) | 15,802 | 4,863 | 3.25× |
| Google Maps Response Small (prettified) | 19,653 | 6,164 | 3.19× |
| Instruments Small (minified) | 11,132 | 2,873 (n/c) | 3.88× |
| Instruments Small (prettified) | 18,570 | 4,608 | 4.03× |
| Marine IK Reverse Small (minified) | 1,572 | 1,049 | 1.50× |
| Marine IK Reverse Small (prettified) | 6,889 | 3,105 | 2.22× |
| Marine IK Small (minified) | 1,616 | 1,021 | 1.58× |
| Marine IK Small (prettified) | 7,044 | 3,327 | 2.12× |
| Random Small (minified) | 11,616 | 4,152 | 2.80× |
| Random Small (prettified) | 17,848 | 4,959 | 3.60× |
| Twitter Small (minified) | 11,874 | 4,350 | 2.73× |
| Twitter Small (prettified) | 12,697 | 3,686 (n/c) | 3.44× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,103 | 2,039 | 1,334 | 1.52× |
| Prettify | 3,718 | 2,139 | 1,070 | 1.74× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 5,071 | 855 | 5.93× |
| Double | 851 | 545 | 1.56× |
| Int64 | 5,459 | 2,138 | 2.55× |
| String | 11,700 | 5,377 | 2.18× |
| Uint64 | 5,510 | 2,485 | 2.22× |
| Apache Builds (minified) | 16,265 | 5,788 | 2.81× |
| Canada (minified) | 1,909 | 991 | 1.93× |
| Apache Builds (prettified) | 18,682 | 5,680 | 3.29× |
| Canada (prettified) | 4,593 | 3,838 | 1.20× |
| CitmCatalog (minified) | 10,486 | 6,542 | 1.60× |
| CitmCatalog (prettified) | 17,707 | 7,771 | 2.28× |
| Discord (minified) | 13,555 | 6,136 | 2.21× |
| Discord (prettified) | 17,756 | 5,788 | 3.07× |
| Github Events (minified) | 16,754 | 7,497 | 2.23× |
| Google Maps Response (minified) | 12,583 | 4,520 | 2.78× |
| Github Events (prettified) | 20,226 | 5,734 | 3.53× |
| Google Maps Response (prettified) | 19,176 | 5,625 | 3.41× |
| Instruments (minified) | 19,118 | 6,264 | 3.05× |
| Instruments (prettified) | 23,492 | 6,495 | 3.62× |
| Marine IK Reverse (minified) | 1,202 | 614 | 1.96× |
| Marine IK Reverse (prettified) | 5,869 | 3,926 | 1.49× |
| Marine IK (minified) | 1,236 | 631 | 1.96× |
| Marine IK (prettified) | 5,983 | 4,155 | 1.44× |
| Random (minified) | 14,518 | 4,199 | 3.46× |
| Random (prettified) | 18,280 | 4,894 | 3.74× |
| Twitter (minified) | 19,192 | 7,595 | 2.53× |
| Twitter (prettified) | 20,263 | 6,307 | 3.21× |
| Apache Builds Small (minified) | 12,002 | 3,967 | 3.03× |
| Canada Small (minified) | 1,950 | 1,005 | 1.94× |
| Apache Builds Small (prettified) | 16,617 | 4,017 | 4.14× |
| Canada Small (prettified) | 5,113 | 3,892 | 1.31× |
| CitmCatalog Small (minified) | 8,505 | 3,404 | 2.50× |
| CitmCatalog Small (prettified) | 16,455 | 4,197 | 3.92× |
| Discord Small (minified) | 11,729 | 4,148 | 2.83× |
| Discord Small (prettified) | 15,854 | 3,248 | 4.88× |
| Github Events Small (minified) | 12,914 | 4,558 | 2.83× |
| Google Maps Response Small (minified) | 12,221 | 4,128 | 2.96× |
| Github Events Small (prettified) | 17,150 | 4,246 | 4.04× |
| Google Maps Response Small (prettified) | 22,920 | 5,462 | 4.20× |
| Instruments Small (minified) | 14,728 | 3,812 | 3.86× |
| Instruments Small (prettified) | 24,517 | 4,049 | 6.06× |
| Marine IK Reverse Small (minified) | 1,596 | 856 | 1.86× |
| Marine IK Reverse Small (prettified) | 6,431 | 4,331 | 1.48× |
| Marine IK Small (minified) | 1,627 | 829 | 1.96× |
| Marine IK Small (prettified) | 7,376 | 4,367 | 1.69× |
| Random Small (minified) | 14,218 | 4,461 | 3.19× |
| Random Small (prettified) | 24,104 | 4,925 | 4.89× |
| Twitter Small (minified) | 10,125 | 5,998 | 1.69× |
| Twitter Small (prettified) | 13,429 | 4,066 | 3.30× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,661 | 2,751 | 1,773 | 1.33× |
| Prettify | 4,993 | 3,834 | 2,328 | 1.30× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,303 | 691 | 3,239 | 1.02× |
| Double | 922 | 745 | 664 | 1.24× |
| Int64 | 5,623 | 2,609 | 4,260 | 1.32× |
| String | 11,566 | 6,352 | 9,627 | 1.20× |
| Uint64 | 6,511 | 2,832 | 4,425 | 1.47× |
| Apache Builds (minified) | 20,501 | 9,652 | 13,494 | 1.52× |
| Canada (minified) | 2,181 | 1,545 | 1,052 | 1.41× |
| Apache Builds (prettified) | 23,729 | 8,064 | 616 | 2.94× |
| Canada (prettified) | 6,213 | 3,879 | 388 | 1.60× |
| CitmCatalog (minified) | 13,105 | 7,103 | 10,339 | 1.27× |
| CitmCatalog (prettified) | 21,118 | 8,477 | 572 | 2.49× |
| Discord (minified) | 15,758 | 6,671 | 11,751 | 1.34× |
| Discord (prettified) | 20,707 | 7,411 | 540 | 2.79× |
| Github Events (minified) | 14,811 | 8,072 | 10,017 | 1.48× |
| Google Maps Response (minified) | 17,720 | 4,574 | 10,274 | 1.72× |
| Github Events (prettified) | 18,270 | 5,815 | 582 | 3.14× |
| Google Maps Response (prettified) | 22,844 | 7,587 | 529 | 3.01× |
| Instruments (minified) | 20,545 | 6,669 | 15,981 | 1.29× |
| Instruments (prettified) | 26,431 | 7,146 | 490 | 3.70× |
| Marine IK Reverse (minified) | 1,317 | 1,005 | 897 | 1.31× |
| Marine IK Reverse (prettified) | 6,545 | 4,097 | 291 | 1.60× |
| Marine IK (minified) | 1,319 | 1,015 | 895 | 1.30× |
| Marine IK (prettified) | 6,504 | 4,163 | 323 | 1.56× |
| Random (minified) | 16,206 | 4,758 | 12,341 | 1.31× |
| Random (prettified) | 19,696 | 6,033 | 542 | 3.26× |
| Twitter (minified) | 17,472 | 9,112 | 10,086 | 1.73× |
| Twitter (prettified) | 21,333 | 6,580 | 513 | 3.24× |
| Apache Builds Small (minified) | 13,990 | 4,637 | 8,320 | 1.68× |
| Canada Small (minified) | 1,774 | 1,777 | 1,083 | 1.00× |
| Apache Builds Small (prettified) | 18,154 | 4,341 | 615 | 4.18× |
| Canada Small (prettified) | 6,698 | 3,921 | 418 | 1.71× |
| CitmCatalog Small (minified) | 8,635 | 3,874 | 5,739 | 1.50× |
| CitmCatalog Small (prettified) | 16,909 | 4,445 | 549 | 3.80× |
| Discord Small (minified) | 11,650 | 4,985 | 7,376 | 1.58× |
| Discord Small (prettified) | 16,890 | 4,268 | 568 | 3.96× |
| Github Events Small (minified) | 10,435 | 6,030 | 6,739 | 1.55× |
| Google Maps Response Small (minified) | 14,942 | 4,310 | 8,730 | 1.71× |
| Github Events Small (prettified) | 13,861 | 4,823 | 572 | 2.87× |
| Google Maps Response Small (prettified) | 32,040 | 6,902 | 534 | 4.64× |
| Instruments Small (minified) | 14,035 | 4,852 | 8,545 | 1.64× |
| Instruments Small (prettified) | 24,044 | 3,284 | 563 | 7.32× |
| Marine IK Reverse Small (minified) | 1,337 | 1,281 | 1,508 | 0.89× |
| Marine IK Reverse Small (prettified) | 7,260 | 4,296 | 498 | 1.69× |
| Marine IK Small (minified) | 1,381 | 1,285 | 1,560 | 0.89× |
| Marine IK Small (prettified) | 6,955 | 4,047 | 527 | 1.72× |
| Random Small (minified) | 14,818 | 5,103 | 9,209 | 1.61× |
| Random Small (prettified) | 25,714 | 5,703 | 590 | 4.51× |
| Twitter Small (minified) | 10,184 | 6,668 | 7,413 | 1.37× |
| Twitter Small (prettified) | 13,866 | 4,699 | 623 | 2.95× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,348 | 2,410 | 1,836 | 1.39× |
| Prettify | 4,408 | 3,702 | 2,548 | 1.19× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,561 | 876 | 2,853 | 1.25× |
| Double | 880 | 582 | 750 | 1.17× |
| Int64 | 5,420 | 2,576 | 3,503 | 1.55× |
| String | 9,857 | 5,360 | 10,982 | 0.90× |
| Uint64 | 5,439 | 2,535 | 3,598 | 1.51× |
| Apache Builds (minified) | 14,472 | 6,525 | 10,488 | 1.38× |
| Canada (minified) | 2,549 | 1,862 | 860 | 1.37× |
| Apache Builds (prettified) | 14,977 | 5,536 | 400 | 2.71× |
| Canada (prettified) | 7,816 | 4,131 | 278 (n/c) | 1.89× |
| CitmCatalog (minified) | 9,242 | 2,355 | 8,107 | 1.14× |
| CitmCatalog (prettified) | 21,050 | 4,668 | 362 | 4.51× |
| Discord (minified) | 14,108 | 3,213 | 9,848 | 1.43× |
| Discord (prettified) | 15,567 | 2,775 | 380 | 5.61× |
| Github Events (minified) | 17,590 | 4,872 | 12,524 | 1.40× |
| Google Maps Response (minified) | 12,314 | 2,004 | 8,720 | 1.41× |
| Github Events (prettified) | 18,063 | 4,325 | 487 | 4.18× |
| Google Maps Response (prettified) | 23,485 | 3,309 | 343 | 7.10× |
| Instruments (minified) | 13,927 | 2,263 | 8,445 | 1.65× |
| Instruments (prettified) | 18,598 | 2,496 | 375 | 7.45× |
| Marine IK Reverse (minified) | 1,102 | 937 | 679 | 1.18× |
| Marine IK Reverse (prettified) | 6,441 | 3,198 | 270 (n/c) | 2.01× |
| Marine IK (minified) | 1,264 | 865 | 742 | 1.46× |
| Marine IK (prettified) | 5,680 | 2,857 | 282 | 1.99× |
| Random (minified) | 12,024 | 2,194 | 7,273 | 1.65× |
| Random (prettified) | 14,388 | 2,965 | 329 | 4.85× |
| Twitter (minified) | 14,003 | 4,022 | 10,154 | 1.38× |
| Twitter (prettified) | 16,595 | 4,484 | 393 | 3.70× |
| Apache Builds Small (minified) | 9,672 | 2,736 | 7,542 | 1.28× |
| Canada Small (minified) | 2,272 | 1,660 | 918 | 1.37× |
| Apache Builds Small (prettified) | 10,710 | 4,173 | 368 | 2.57× |
| Canada Small (prettified) | 6,350 | 3,603 | 272 | 1.76× |
| CitmCatalog Small (minified) | 8,500 (n/c) | 1,941 | 6,654 | 1.28× |
| CitmCatalog Small (prettified) | 15,722 | 2,286 | 289 | 6.88× |
| Discord Small (minified) | 12,902 | 2,353 | 8,559 (n/c) | 1.51× |
| Discord Small (prettified) | 11,324 | 2,561 | 326 | 4.42× |
| Github Events Small (minified) | 13,432 | 3,878 | 9,738 | 1.38× |
| Google Maps Response Small (minified) | 9,867 | 1,825 | 7,759 | 1.27× |
| Github Events Small (prettified) | 16,646 | 2,731 | 398 | 6.09× |
| Google Maps Response Small (prettified) | 22,081 | 2,548 | 295 | 8.67× |
| Instruments Small (minified) | 14,480 | 2,248 | 8,096 | 1.79× |
| Instruments Small (prettified) | 15,356 (n/c) | 2,137 | 359 | 7.19× |
| Marine IK Reverse Small (minified) | 1,979 | 1,165 | 1,647 | 1.20× |
| Marine IK Reverse Small (prettified) | 7,384 | 3,315 | 277 | 2.23× |
| Marine IK Small (minified) | 1,994 | 1,007 | 1,696 | 1.18× |
| Marine IK Small (prettified) | 8,791 | 3,099 | 298 | 2.84× |
| Random Small (minified) | 12,635 | 2,241 | 10,046 | 1.26× |
| Random Small (prettified) | 15,836 | 2,351 | 282 | 6.74× |
| Twitter Small (minified) | 11,683 | 3,647 | 9,902 | 1.18× |
| Twitter Small (prettified) | 15,186 | 3,725 | 441 | 4.08× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,072 | 1,486 | 1,191 | 1.39× |
| Prettify | 2,852 | 2,058 | 1,411 | 1.39× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 3,058 | 763 | 4.01× |
| Double | 918 | 632 | 1.45× |
| Int64 | 3,775 | 2,337 | 1.62× |
| String | 8,264 | 4,709 | 1.75× |
| Uint64 | 3,772 | 2,433 | 1.55× |
| Apache Builds (minified) | 11,686 | 3,886 | 3.01× |
| Canada (minified) | 2,988 | 1,977 | 1.51× |
| Apache Builds (prettified) | 17,726 | 4,265 | 4.16× |
| Canada (prettified) | 8,543 | 4,035 | 2.12× |
| CitmCatalog (minified) | 8,488 | 2,264 | 3.75× |
| CitmCatalog (prettified) | 20,217 | 3,579 | 5.65× |
| Discord (minified) | 13,465 | 2,858 | 4.71× |
| Discord (prettified) | 19,655 | 3,032 | 6.48× |
| Github Events (minified) | 13,531 | 3,962 | 3.41× |
| Google Maps Response (minified) | 9,989 | 1,763 | 5.66× |
| Github Events (prettified) | 20,004 | 4,498 | 4.45× |
| Google Maps Response (prettified) | 21,528 | 3,071 | 7.01× |
| Instruments (minified) | 14,451 | 1,942 | 7.44× |
| Instruments (prettified) | 23,644 | 2,410 | 9.81× |
| Marine IK Reverse (minified) | 1,549 | 953 | 1.63× |
| Marine IK Reverse (prettified) | 7,464 | 3,526 | 2.12× |
| Marine IK (minified) | 1,539 | 950 | 1.62× |
| Marine IK (prettified) | 7,605 | 3,343 | 2.27× |
| Random (minified) | 11,183 | 2,466 | 4.54× |
| Random (prettified) | 17,886 | 2,869 | 6.23× |
| Twitter (minified) | 16,266 | 4,628 | 3.52× |
| Twitter (prettified) | 18,566 | 4,836 | 3.84× |
| Apache Builds Small (minified) | 8,451 | 2,794 | 3.02× |
| Canada Small (minified) | 2,242 | 1,519 | 1.48× |
| Apache Builds Small (prettified) | 11,213 | 2,536 | 4.42× |
| Canada Small (prettified) | 6,888 | 2,808 | 2.45× |
| CitmCatalog Small (minified) | 9,271 | 2,067 | 4.49× |
| CitmCatalog Small (prettified) | 17,120 | 2,235 | 7.66× |
| Discord Small (minified) | 12,403 | 2,673 | 4.64× |
| Discord Small (prettified) | 15,672 | 2,402 | 6.52× |
| Github Events Small (minified) | 12,790 | 3,257 | 3.93× |
| Google Maps Response Small (minified) | 9,661 | 1,820 | 5.31× |
| Github Events Small (prettified) | 15,559 | 3,285 | 4.74× |
| Google Maps Response Small (prettified) | 21,703 | 2,630 | 8.25× |
| Instruments Small (minified) | 13,367 | 1,906 | 7.01× |
| Instruments Small (prettified) | 22,758 | 2,262 | 10.06× |
| Marine IK Reverse Small (minified) | 1,867 | 1,103 | 1.69× |
| Marine IK Reverse Small (prettified) | 8,297 | 3,397 | 2.44× |
| Marine IK Small (minified) | 1,797 | 1,075 | 1.67× |
| Marine IK Small (prettified) | 8,853 | 3,453 | 2.56× |
| Random Small (minified) | 10,922 | 2,073 | 5.27× |
| Random Small (prettified) | 14,732 | 2,412 | 6.11× |
| Twitter Small (minified) | 12,274 | 3,591 | 3.42× |
| Twitter Small (prettified) | 14,216 | 4,028 | 3.53× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,357 | 1,613 | 1,086 | 1.46× |
| Prettify | 3,443 | 2,151 | 1,376 | 1.60× |

### Losses and ties

The five freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,791 vs Jsonifier's 4,065 MB/s (0.85×). Reused, Jsonifier wins by 2.81× (15,383 vs 5,475).
- **Int64, macOS / GCC.** The reflection writer runs 826 vs 710 MB/s (0.86×). Jsonifier is ahead of Glaze (574 MB/s, 1.24×), and ahead of the reflection writer reused, by 1.55× (5,420 vs 3,503).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,502 vs 1,299 MB/s (0.86×). It also wins reused, 1,560 vs 1,381 MB/s (0.89×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,504 vs 1,244 MB/s (0.83×), and Glaze edges it too at 1,260 MB/s. The reflection writer also wins reused, 1,508 vs 1,337 MB/s (0.89×).
- **Double, macOS / GCC.** The reflection writer runs 322 vs 297 MB/s (0.92×). Jsonifier is ahead of Glaze (221 MB/s, 1.35×), and ahead of the reflection writer reused (880 vs 750 MB/s, 1.17×).

The one reused loss with no freshly allocated counterpart is String on macOS / GCC (9,857 vs 10,982 MB/s, 0.90×); freshly allocated, Jsonifier wins it (1,561 vs 1,312).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.15–5.31× and prettified documents by 16–61×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 41 of the 44 Small document tests freshly allocated and 42 reused, losing only minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and tying minified Github Events Small on macOS / GCC freshly allocated. That tie is despite a 1.32× lead (10,939 vs 8,262 MB/s), because the reflection result did not converge (14.5% RSE), which leaves the gap short of significance. These counts are against the reflection writer only: reused minified Canada Small on Linux / GCC beats it but ties Glaze. Why the reflection writer edges ahead on these few tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.34–1.52× freshly allocated and 1.33–1.52× reused, and wins Prettify on all five in both runs, by 1.17–1.56× freshly allocated and 1.19–1.74× reused. Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,561 MB/s on macOS / GCC (1.38× Glaze's 1,134, ahead of the reflection writer's 1,312) and 1,184 MB/s on macOS / Clang (a tie with Glaze's 1,171). The AVX2 builds run the same test at 1,408–4,223 MB/s and lead Glaze by 1.53× (Windows / MSVC), 2.67× (Linux / Clang) and 1.99× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.75× on macOS / Clang (8,264 vs 4,709 MB/s) and 1.84× on macOS / GCC (9,857 vs 5,360), against 1.82–3.45× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 10,982 MB/s.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes, and part remains in the copy loop, most visibly against the reflection writer on macOS / GCC. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Only freshly allocated on macOS / GCC, 322 vs 297 MB/s (0.92×). On Linux / GCC freshly allocated Jsonifier is narrowly ahead (353 vs 345 MB/s, 1.02×), and reused it leads on both GCC builds, by 1.39× (922 vs 664) and 1.17× (880 vs 750). Jsonifier also beats Glaze on Double on all five builds in both runs.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** Every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`, so string-heavy documents can ask for about 6× their output size (see *Does `6n + 2` blow up memory on huge strings?*).
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
