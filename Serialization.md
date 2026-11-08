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

On the Bool test this writer leads Glaze by 10.55–17.26× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.31× to 4.90×.

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

Jsonifier was fastest in 250 of the 255 write tests on five builds, tied 1 and lost 4. With the output string reused across iterations it was fastest in 251 of 255, tied 1 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify and Prettify on every build in both runs. Freshly allocated, the only loss to Glaze is minified Twitter on Windows / MSVC; the other three are against simdjson's reflection writer, which exists only on GCC: minified Marine IK Small on Linux / GCC, and Double and Uint64 on macOS / GCC. The one freshly allocated tie is minified Marine IK Reverse Small on Linux / GCC, also against the reflection writer. Reused, the three losses are minified Marine IK Small and Marine IK Reverse Small on Linux / GCC and String on macOS / GCC, all against the reflection writer, and the one tie is minified CitmCatalog Small on macOS / GCC. Linux / Clang and macOS / Clang win all 51 tests in both runs, and Windows / MSVC wins all 51 reused. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.41–10.33× across the five builds and the reflection writer by 16–66× on the two GCC builds; Small documents lead them by 1.27–12.69× and 11.98–62.82×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 5, 2026, with Jsonifier [86c028c](https://github.com/nihilai-collective/jsonifier/commit/86c028c), Glaze [00e6ce3](https://github.com/stephenberry/glaze/commit/00e6ce3), simdjson [37a1d72](https://github.com/simdjson/simdjson/commit/37a1d72) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 1 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 0 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **250** | **1** | **4** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,197 | 92 | 12.96× |
| Double | 479 | 151 | 3.17× |
| Int64 | 510 | 380 | 1.34× |
| String | 1,390 | 906 | 1.53× |
| Uint64 | 547 | 375 | 1.46× |
| Apache Builds (minified) | 12,428 | 4,322 | 2.88× |
| Canada (minified) | 1,322 | 906 | 1.46× |
| Apache Builds (prettified) | 14,096 | 4,780 | 2.95× |
| Canada (prettified) | 3,045 | 1,482 | 2.05× |
| CitmCatalog (minified) | 8,767 | 4,454 | 1.97× |
| CitmCatalog (prettified) | 4,382 | 2,019 | 2.17× |
| Discord (minified) | 10,477 | 4,073 | 2.57× |
| Discord (prettified) | 13,195 | 4,136 | 3.19× |
| Github Events (minified) | 11,852 | 5,115 | 2.32× |
| Google Maps Response (minified) | 9,276 | 3,390 | 2.74× |
| Github Events (prettified) | 13,750 | 3,416 | 4.03× |
| Google Maps Response (prettified) | 13,949 | 5,101 | 2.73× |
| Instruments (minified) | 13,435 | 4,714 | 2.85× |
| Instruments (prettified) | 18,261 | 4,224 | 4.32× |
| Marine IK Reverse (minified) | 899 | 726 | 1.24× |
| Marine IK Reverse (prettified) | 3,164 | 1,957 | 1.62× |
| Marine IK (minified) | 897 | 635 | 1.41× |
| Marine IK (prettified) | 3,269 (n/c) | 1,379 | 2.37× |
| Random (minified) | 3,644 | 3,058 | 1.19× |
| Random (prettified) | 4,162 | 2,251 | 1.85× |
| Twitter (minified) | 3,855 | 4,478 | 0.86× |
| Twitter (prettified) | 4,083 | 1,686 | 2.42× |
| Apache Builds Small (minified) | 9,515 | 3,625 | 2.63× |
| Canada Small (minified) | 1,859 | 1,192 | 1.56× |
| Apache Builds Small (prettified) | 12,204 | 3,937 | 3.10× |
| Canada Small (prettified) | 5,411 | 2,991 | 1.81× |
| CitmCatalog Small (minified) | 6,734 | 3,294 | 2.04× |
| CitmCatalog Small (prettified) | 10,803 | 3,859 | 2.80× |
| Discord Small (minified) | 7,895 | 3,173 | 2.49× |
| Discord Small (prettified) | 11,588 | 3,219 | 3.60× |
| Github Events Small (minified) | 9,805 | 3,915 | 2.50× |
| Google Maps Response Small (minified) | 8,931 | 3,467 | 2.58× |
| Github Events Small (prettified) | 12,504 | 3,783 | 3.31× |
| Google Maps Response Small (prettified) | 15,766 | 4,644 | 3.40× |
| Instruments Small (minified) | 10,758 | 3,461 | 3.11× |
| Instruments Small (prettified) | 16,321 | 3,818 | 4.27× |
| Marine IK Reverse Small (minified) | 1,535 | 951 | 1.61× |
| Marine IK Reverse Small (prettified) | 6,584 | 2,753 | 2.39× |
| Marine IK Small (minified) | 1,504 | 914 | 1.65× |
| Marine IK Small (prettified) | 6,821 | 2,829 | 2.41× |
| Random Small (minified) | 9,253 | 3,535 | 2.62× |
| Random Small (prettified) | 14,152 | 4,005 | 3.53× |
| Twitter Small (minified) | 9,818 | 4,595 | 2.14× |
| Twitter Small (prettified) | 10,596 | 2,917 (n/c) | 3.63× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,940 | 1,888 | 1,326 | 1.56× |
| Prettify | 2,352 | 2,104 | 1,407 | 1.12× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,512 | 222 | 11.31× |
| Double | 339 | 303 | 1.12× |
| Int64 | 936 | 876 | 1.07× |
| String | 4,364 | 2,015 | 2.17× |
| Uint64 | 926 | 870 | 1.06× |
| Apache Builds (minified) | 16,516 | 5,224 | 3.16× |
| Canada (minified) | 1,938 | 960 | 2.02× |
| Apache Builds (prettified) | 19,016 | 5,323 | 3.57× |
| Canada (prettified) | 4,655 | 3,309 | 1.41× |
| CitmCatalog (minified) | 10,125 | 5,575 | 1.82× |
| CitmCatalog (prettified) | 17,650 | 5,650 | 3.12× |
| Discord (minified) | 13,747 | 5,990 | 2.30× |
| Discord (prettified) | 17,725 | 5,047 | 3.51× |
| Github Events (minified) | 16,802 | 6,407 | 2.62× |
| Google Maps Response (minified) | 11,825 | 3,964 | 2.98× |
| Github Events (prettified) | 19,522 | 5,116 | 3.82× |
| Google Maps Response (prettified) | 18,224 | 5,240 | 3.48× |
| Instruments (minified) | 19,051 | 5,802 | 3.28× |
| Instruments (prettified) | 23,420 | 5,587 | 4.19× |
| Marine IK Reverse (minified) | 1,200 | 621 | 1.93× |
| Marine IK Reverse (prettified) | 5,888 | 3,686 | 1.60× |
| Marine IK (minified) | 1,226 | 612 | 2.00× |
| Marine IK (prettified) | 6,021 | 2,937 (n/c) | 2.05× |
| Random (minified) | 14,467 | 3,574 | 4.05× |
| Random (prettified) | 18,137 | 4,103 | 4.42× |
| Twitter (minified) | 19,251 | 6,999 | 2.75× |
| Twitter (prettified) | 20,870 | 4,648 | 4.49× |
| Apache Builds Small (minified) | 10,124 | 3,520 | 2.88× |
| Canada Small (minified) | 1,688 | 936 | 1.80× |
| Apache Builds Small (prettified) | 13,482 | 3,659 | 3.68× |
| Canada Small (prettified) | 4,585 | 3,624 | 1.27× |
| CitmCatalog Small (minified) | 7,669 | 3,138 | 2.44× |
| CitmCatalog Small (prettified) | 14,999 | 3,677 | 4.08× |
| Discord Small (minified) | 10,262 | 3,420 | 3.00× |
| Discord Small (prettified) | 14,626 | 3,276 | 4.46× |
| Github Events Small (minified) | 10,730 | 4,600 | 2.33× |
| Google Maps Response Small (minified) | 10,762 | 3,710 | 2.90× |
| Github Events Small (prettified) | 13,426 | 3,614 | 3.72× |
| Google Maps Response Small (prettified) | 20,379 | 4,665 | 4.37× |
| Instruments Small (minified) | 12,795 | 3,477 | 3.68× |
| Instruments Small (prettified) | 21,154 | 4,147 | 5.10× |
| Marine IK Reverse Small (minified) | 1,544 | 806 | 1.92× |
| Marine IK Reverse Small (prettified) | 7,167 | 3,687 | 1.94× |
| Marine IK Small (minified) | 1,547 | 829 | 1.86× |
| Marine IK Small (prettified) | 7,195 | 3,733 | 1.93× |
| Random Small (minified) | 13,080 | 3,660 | 3.57× |
| Random Small (prettified) | 19,188 | 4,058 | 4.73× |
| Twitter Small (minified) | 8,998 | 5,053 | 1.78× |
| Twitter Small (prettified) | 11,258 | 2,874 | 3.92× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,686 | 2,779 | 1,757 | 1.33× |
| Prettify | 4,919 | 3,686 | 2,326 | 1.33× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,171 | 199 | 256 | 8.46× |
| Double | 351 | 305 | 341 | 1.03× |
| Int64 | 984 | 860 | 976 | 1.01× |
| String | 3,945 | 2,034 | 3,739 | 1.06× |
| Uint64 | 953 | 891 | 905 | 1.05× |
| Apache Builds (minified) | 20,467 | 8,011 | 13,301 | 1.54× |
| Canada (minified) | 2,150 | 1,441 | 1,029 | 1.49× |
| Apache Builds (prettified) | 23,071 | 6,994 | 609 | 3.30× |
| Canada (prettified) | 6,117 | 3,223 | 390 | 1.90× |
| CitmCatalog (minified) | 12,841 | 5,987 | 10,164 | 1.26× |
| CitmCatalog (prettified) | 20,456 | 5,809 | 570 | 3.52× |
| Discord (minified) | 14,663 | 6,258 | 11,323 | 1.29× |
| Discord (prettified) | 20,083 | 6,191 | 556 | 3.24× |
| Github Events (minified) | 14,480 | 7,016 | 9,827 | 1.47× |
| Google Maps Response (minified) | 15,812 | 4,224 | 9,515 | 1.66× |
| Github Events (prettified) | 18,052 | 5,322 | 579 | 3.39× |
| Google Maps Response (prettified) | 21,009 | 6,761 | 526 | 3.11× |
| Instruments (minified) | 20,138 | 5,734 | 15,599 | 1.29× |
| Instruments (prettified) | 25,842 | 6,212 | 476 | 4.16× |
| Marine IK Reverse (minified) | 1,288 | 934 | 877 | 1.38× |
| Marine IK Reverse (prettified) | 6,478 | 3,680 | 263 | 1.76× |
| Marine IK (minified) | 1,289 | 941 | 883 | 1.37× |
| Marine IK (prettified) | 6,333 | 3,085 | 380 | 2.05× |
| Random (minified) | 16,291 | 4,160 | 12,092 | 1.35× |
| Random (prettified) | 20,458 | 5,127 | 529 | 3.99× |
| Twitter (minified) | 17,286 | 7,399 | 10,112 | 1.71× |
| Twitter (prettified) | 21,155 | 5,312 | 513 | 3.98× |
| Apache Builds Small (minified) | 11,455 | 4,594 | 7,434 | 1.54× |
| Canada Small (minified) | 1,906 | 1,625 | 1,093 | 1.17× |
| Apache Builds Small (prettified) | 14,146 | 3,869 | 618 | 3.66× |
| Canada Small (prettified) | 6,421 | 3,441 | 416 | 1.87× |
| CitmCatalog Small (minified) | 8,036 | 3,500 | 5,394 | 1.49× |
| CitmCatalog Small (prettified) | 15,212 | 4,081 | 518 | 3.73× |
| Discord Small (minified) | 10,089 | 4,161 | 6,920 | 1.46× |
| Discord Small (prettified) | 15,134 | 3,935 | 577 | 3.85× |
| Github Events Small (minified) | 8,493 | 4,879 | 6,207 | 1.37× |
| Google Maps Response Small (minified) | 13,521 | 4,086 | 8,189 | 1.65× |
| Github Events Small (prettified) | 11,384 | 3,915 | 579 | 2.91× |
| Google Maps Response Small (prettified) | 25,949 | 5,925 | 538 | 4.38× |
| Instruments Small (minified) | 12,566 | 3,985 | 7,963 | 1.58× |
| Instruments Small (prettified) | 21,568 | 4,119 | 574 | 5.24× |
| Marine IK Reverse Small (minified) | 1,379 | 1,226 | 1,472 | 0.94× |
| Marine IK Reverse Small (prettified) | 6,120 | 3,852 | 511 | 1.59× |
| Marine IK Small (minified) | 1,312 | 1,204 | 1,476 | 0.89× |
| Marine IK Small (prettified) | 6,715 | 3,794 | 539 | 1.77× |
| Random Small (minified) | 13,259 | 4,425 | 8,277 | 1.60× |
| Random Small (prettified) | 23,730 | 4,986 | 584 | 4.76× |
| Twitter Small (minified) | 8,828 | 5,678 | 6,539 | 1.35× |
| Twitter Small (prettified) | 12,337 | 4,107 | 622 | 3.00× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,336 | 2,446 | 1,803 | 1.36× |
| Prettify | 4,360 | 3,708 | 2,596 | 1.18× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,054 | 130 | 204 | 10.06× |
| Double | 251 | 229 | 329 | 0.76× |
| Int64 | 769 | 495 | 656 | 1.17× |
| String | 1,275 | 908 | 1,062 | 1.20× |
| Uint64 | 679 | 564 | 733 | 0.93× |
| Apache Builds (minified) | 13,325 | 5,429 | 10,036 | 1.33× |
| Canada (minified) | 2,159 | 1,628 | 713 | 1.33× |
| Apache Builds (prettified) | 12,988 | 4,468 | 378 | 2.91× |
| Canada (prettified) | 6,891 | 3,342 | 272 | 2.06× |
| CitmCatalog (minified) | 7,062 | 1,822 | 6,312 | 1.12× |
| CitmCatalog (prettified) | 20,669 | 3,466 | 314 | 5.96× |
| Discord (minified) | 11,900 | 2,664 | 8,678 | 1.37× |
| Discord (prettified) | 13,770 | 2,390 | 298 | 5.76× |
| Github Events (minified) | 14,833 | 3,675 | 11,036 | 1.34× |
| Google Maps Response (minified) | 11,679 | 1,884 | 8,832 | 1.32× |
| Github Events (prettified) | 10,456 | 3,134 | 401 | 3.34× |
| Google Maps Response (prettified) | 20,901 | 2,939 | 347 | 7.11× |
| Instruments (minified) | 15,474 | 2,099 | 2,539 | 6.09× |
| Instruments (prettified) | 16,990 | 2,211 | 316 | 7.68× |
| Marine IK Reverse (minified) | 1,390 | 889 | 797 | 1.56× |
| Marine IK Reverse (prettified) | 6,936 | 3,333 | 294 (n/c) | 2.08× |
| Marine IK (minified) | 1,291 | 788 | 771 | 1.64× |
| Marine IK (prettified) | 6,301 | 2,649 | 284 | 2.38× |
| Random (minified) | 11,849 | 2,061 | 7,490 | 1.58× |
| Random (prettified) | 14,671 | 2,499 | 337 | 5.87× |
| Twitter (minified) | 16,331 | 4,138 | 12,171 | 1.34× |
| Twitter (prettified) | 17,621 | 4,032 | 459 | 4.37× |
| Apache Builds Small (minified) | 9,098 | 2,428 (n/c) | 5,821 | 1.56× |
| Canada Small (minified) | 2,064 | 1,687 | 877 | 1.22× |
| Apache Builds Small (prettified) | 9,033 | 3,163 (n/c) | 339 | 2.86× |
| Canada Small (prettified) | 7,371 | 3,279 | 281 | 2.25× |
| CitmCatalog Small (minified) | 8,420 | 1,810 | 7,120 | 1.18× |
| CitmCatalog Small (prettified) | 14,257 | 2,332 | 294 | 6.11× |
| Discord Small (minified) | 9,600 | 2,264 | 8,053 | 1.19× |
| Discord Small (prettified) | 12,645 | 2,274 | 394 | 5.56× |
| Github Events Small (minified) | 10,509 | 2,885 | 9,278 | 1.13× |
| Google Maps Response Small (minified) | 8,849 | 1,631 | 7,716 | 1.15× |
| Github Events Small (prettified) | 13,140 | 2,685 | 391 | 4.89× |
| Google Maps Response Small (prettified) | 19,404 | 2,629 | 309 | 7.38× |
| Instruments Small (minified) | 12,995 | 1,905 | 6,598 | 1.97× |
| Instruments Small (prettified) | 16,859 | 2,101 | 337 | 8.03× |
| Marine IK Reverse Small (minified) | 1,948 | 981 | 1,403 | 1.39× |
| Marine IK Reverse Small (prettified) | 7,791 | 2,717 | 275 | 2.87× |
| Marine IK Small (minified) | 1,818 | 968 | 1,535 | 1.18× |
| Marine IK Small (prettified) | 8,317 | 3,066 | 303 | 2.71× |
| Random Small (minified) | 12,408 | 2,233 | 8,050 | 1.54× |
| Random Small (prettified) | 14,765 | 2,350 | 296 | 6.28× |
| Twitter Small (minified) | 10,209 | 2,778 | 8,988 | 1.14× |
| Twitter Small (prettified) | 13,716 | 2,920 | 447 | 4.70× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,971 | 1,480 | 1,117 | 1.33× |
| Prettify | 2,924 | 1,995 | 1,640 | 1.47× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,995 | 125 | 15.97× |
| Double | 292 | 213 | 1.37× |
| Int64 | 3,210 | 522 | 6.15× |
| String | 1,120 | 876 | 1.28× |
| Uint64 | 2,886 | 470 | 6.14× |
| Apache Builds (minified) | 11,569 | 2,684 | 4.31× |
| Canada (minified) | 3,018 | 1,711 | 1.76× |
| Apache Builds (prettified) | 17,285 | 3,376 | 5.12× |
| Canada (prettified) | 7,765 | 2,538 | 3.06× |
| CitmCatalog (minified) | 7,359 | 2,162 | 3.40× |
| CitmCatalog (prettified) | 20,146 | 3,257 | 6.19× |
| Discord (minified) | 12,765 | 2,806 | 4.55× |
| Discord (prettified) | 19,262 | 2,686 | 7.17× |
| Github Events (minified) | 15,488 | 3,817 | 4.06× |
| Google Maps Response (minified) | 10,481 | 1,364 | 7.68× |
| Github Events (prettified) | 16,362 | 3,296 | 4.96× |
| Google Maps Response (prettified) | 21,454 | 2,920 | 7.35× |
| Instruments (minified) | 14,076 | 2,090 | 6.73× |
| Instruments (prettified) | 22,410 | 2,170 | 10.33× |
| Marine IK Reverse (minified) | 1,378 | 764 | 1.80× |
| Marine IK Reverse (prettified) | 6,952 | 2,832 | 2.45× |
| Marine IK (minified) | 1,257 | 762 | 1.65× |
| Marine IK (prettified) | 6,642 | 3,041 | 2.18× |
| Random (minified) | 10,393 | 2,189 | 4.75× |
| Random (prettified) | 11,884 | 2,162 | 5.50× |
| Twitter (minified) | 11,687 | 3,214 | 3.64× |
| Twitter (prettified) | 14,289 | 2,950 | 4.84× |
| Apache Builds Small (minified) | 7,224 | 1,960 | 3.69× |
| Canada Small (minified) | 2,044 | 1,231 | 1.66× |
| Apache Builds Small (prettified) | 9,045 | 2,219 | 4.08× |
| Canada Small (prettified) | 6,053 | 2,668 | 2.27× |
| CitmCatalog Small (minified) | 5,775 | 1,345 | 4.29× |
| CitmCatalog Small (prettified) | 11,733 | 1,797 | 6.53× |
| Discord Small (minified) | 10,794 | 1,911 | 5.65× |
| Discord Small (prettified) | 12,124 | 2,048 | 5.92× |
| Github Events Small (minified) | 7,887 | 1,950 | 4.04× |
| Google Maps Response Small (minified) | 6,836 | 1,325 | 5.16× |
| Github Events Small (prettified) | 10,367 | 2,040 | 5.08× |
| Google Maps Response Small (prettified) | 15,816 | 2,062 | 7.67× |
| Instruments Small (minified) | 10,287 | 1,482 | 6.94× |
| Instruments Small (prettified) | 22,788 | 1,796 | 12.69× |
| Marine IK Reverse Small (minified) | 1,899 | 1,009 | 1.88× |
| Marine IK Reverse Small (prettified) | 6,571 | 2,819 | 2.33× |
| Marine IK Small (minified) | 1,553 | 760 | 2.04× |
| Marine IK Small (prettified) | 6,725 | 2,490 | 2.70× |
| Random Small (minified) | 9,707 | 1,698 | 5.72× |
| Random Small (prettified) | 11,668 | 1,831 | 6.37× |
| Twitter Small (minified) | 7,956 | 2,566 | 3.10× |
| Twitter Small (prettified) | 10,679 | 3,019 | 3.54× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,584 | 1,612 | 860 | 1.60× |
| Prettify | 3,003 | 1,913 | 1,222 (n/c) | 1.57× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 251 of the 255 write tests, ties 1 and loses 3, all against the reflection writer on the two GCC builds. On Linux / GCC it loses minified Marine IK Small (1,524 vs 1,347 MB/s, 0.88×) and Marine IK Reverse Small (1,515 vs 1,435 MB/s, 0.95×). On macOS / GCC it loses String (8,849 vs 8,598 MB/s, 0.97×) and ties minified CitmCatalog Small: the reflection writer is ahead, 7,840 vs 7,519 MB/s, but Jsonifier's 6.8% RSE leaves the gap short of significance.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 1 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **251** | **1** | **3** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,758 | 283 | 6.21× |
| Double | 760 | 372 | 2.04× |
| Int64 | 4,007 | 1,057 | 3.79× |
| String | 7,703 | 2,291 | 3.36× |
| Uint64 | 4,843 | 1,059 | 4.57× |
| Apache Builds (minified) | 12,699 | 4,906 | 2.59× |
| Canada (minified) | 1,591 | 1,267 | 1.26× |
| Apache Builds (prettified) | 14,539 | 5,276 | 2.76× |
| Canada (prettified) | 4,699 | 3,292 | 1.43× |
| CitmCatalog (minified) | 8,550 | 4,914 | 1.74× |
| CitmCatalog (prettified) | 15,445 | 7,224 | 2.14× |
| Discord (minified) | 11,026 | 4,544 | 2.43× |
| Discord (prettified) | 13,579 | 4,519 | 3.01× |
| Github Events (minified) | 12,424 | 6,004 | 2.07× |
| Google Maps Response (minified) | 10,258 | 4,042 | 2.54× |
| Github Events (prettified) | 14,367 | 3,849 | 3.73× |
| Google Maps Response (prettified) | 15,328 | 6,134 | 2.50× |
| Instruments (minified) | 14,143 | 4,897 | 2.89× |
| Instruments (prettified) | 18,760 | 5,131 | 3.66× |
| Marine IK Reverse (minified) | 1,015 | 784 | 1.29× |
| Marine IK Reverse (prettified) | 4,998 | 3,512 | 1.42× |
| Marine IK (minified) | 1,009 | 791 | 1.28× |
| Marine IK (prettified) | 4,952 | 3,548 | 1.40× |
| Random (minified) | 11,122 | 3,295 | 3.38× |
| Random (prettified) | 14,380 | 4,280 | 3.36× |
| Twitter (minified) | 15,212 | 4,894 | 3.11× |
| Twitter (prettified) | 14,950 | 3,122 | 4.79× |
| Apache Builds Small (minified) | 10,958 | 4,104 | 2.67× |
| Canada Small (minified) | 1,847 | 1,339 | 1.38× |
| Apache Builds Small (prettified) | 14,306 | 4,227 | 3.38× |
| Canada Small (prettified) | 5,728 | 3,404 | 1.68× |
| CitmCatalog Small (minified) | 7,554 | 4,168 | 1.81× |
| CitmCatalog Small (prettified) | 13,340 | 5,172 | 2.58× |
| Discord Small (minified) | 10,108 | 4,425 (n/c) | 2.28× |
| Discord Small (prettified) | 12,718 | 3,616 | 3.52× |
| Github Events Small (minified) | 12,779 | 4,039 (n/c) | 3.16× |
| Google Maps Response Small (minified) | 9,571 | 3,832 | 2.50× |
| Github Events Small (prettified) | 15,727 | 5,169 | 3.04× |
| Google Maps Response Small (prettified) | 19,361 | 6,388 | 3.03× |
| Instruments Small (minified) | 11,998 | 5,430 (n/c) | 2.21× |
| Instruments Small (prettified) | 17,955 | 3,418 | 5.25× |
| Marine IK Reverse Small (minified) | 1,607 | 972 | 1.65× |
| Marine IK Reverse Small (prettified) | 6,861 | 3,356 | 2.04× |
| Marine IK Small (minified) | 1,609 | 883 | 1.82× |
| Marine IK Small (prettified) | 7,039 | 3,284 | 2.14× |
| Random Small (minified) | 11,777 | 4,239 | 2.78× |
| Random Small (prettified) | 18,187 | 5,092 | 3.57× |
| Twitter Small (minified) | 11,554 | 5,675 | 2.04× |
| Twitter Small (prettified) | 12,456 | 4,380 (n/c) | 2.84× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,942 | 1,885 | 1,320 | 1.56× |
| Prettify | 3,585 | 2,130 | 1,375 | 1.68× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 5,176 | 868 | 5.96× |
| Double | 890 | 646 | 1.38× |
| Int64 | 4,939 | 2,247 | 2.20× |
| String | 11,119 | 5,445 | 2.04× |
| Uint64 | 5,438 | 2,489 | 2.18× |
| Apache Builds (minified) | 16,614 | 5,895 | 2.82× |
| Canada (minified) | 1,951 | 1,000 | 1.95× |
| Apache Builds (prettified) | 19,140 | 5,794 | 3.30× |
| Canada (prettified) | 4,661 | 3,957 | 1.18× |
| CitmCatalog (minified) | 10,128 | 6,389 | 1.59× |
| CitmCatalog (prettified) | 17,688 | 7,718 | 2.29× |
| Discord (minified) | 13,941 | 6,203 | 2.25× |
| Discord (prettified) | 17,924 | 5,820 | 3.08× |
| Github Events (minified) | 17,026 | 7,376 | 2.31× |
| Google Maps Response (minified) | 12,396 | 4,374 | 2.83× |
| Github Events (prettified) | 20,135 | 5,921 | 3.40× |
| Google Maps Response (prettified) | 19,378 | 5,756 | 3.37× |
| Instruments (minified) | 19,215 | 6,627 | 2.90× |
| Instruments (prettified) | 23,849 | 6,512 | 3.66× |
| Marine IK Reverse (minified) | 1,204 | 620 | 1.94× |
| Marine IK Reverse (prettified) | 5,877 | 4,088 | 1.44× |
| Marine IK (minified) | 1,228 | 640 | 1.92× |
| Marine IK (prettified) | 5,964 | 4,109 | 1.45× |
| Random (minified) | 14,443 | 3,981 | 3.63× |
| Random (prettified) | 18,067 | 4,727 | 3.82× |
| Twitter (minified) | 19,184 | 7,884 | 2.43× |
| Twitter (prettified) | 20,976 | 5,687 | 3.69× |
| Apache Builds Small (minified) | 12,043 | 3,757 | 3.21× |
| Canada Small (minified) | 1,738 | 1,006 | 1.73× |
| Apache Builds Small (prettified) | 15,696 | 3,787 | 4.14× |
| Canada Small (prettified) | 4,737 | 3,979 | 1.19× |
| CitmCatalog Small (minified) | 8,611 | 3,186 | 2.70× |
| CitmCatalog Small (prettified) | 17,084 | 4,194 | 4.07× |
| Discord Small (minified) | 11,883 | 4,012 | 2.96× |
| Discord Small (prettified) | 16,827 | 3,568 | 4.72× |
| Github Events Small (minified) | 13,962 | 3,259 | 4.28× |
| Google Maps Response Small (minified) | 11,946 | 4,090 | 2.92× |
| Github Events Small (prettified) | 16,558 | 3,034 | 5.46× |
| Google Maps Response Small (prettified) | 23,221 | 5,434 | 4.27× |
| Instruments Small (minified) | 14,490 | 3,693 | 3.92× |
| Instruments Small (prettified) | 24,292 | 4,647 | 5.23× |
| Marine IK Reverse Small (minified) | 1,594 | 835 | 1.91× |
| Marine IK Reverse Small (prettified) | 7,291 | 4,241 | 1.72× |
| Marine IK Small (minified) | 1,545 | 849 | 1.82× |
| Marine IK Small (prettified) | 7,321 | 4,198 | 1.74× |
| Random Small (minified) | 14,511 | 4,198 | 3.46× |
| Random Small (prettified) | 23,439 | 4,827 | 4.86× |
| Twitter Small (minified) | 10,609 | 5,499 | 1.93× |
| Twitter Small (prettified) | 12,898 | 3,532 | 3.65× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,704 | 2,760 | 1,750 | 1.34× |
| Prettify | 4,920 | 3,737 | 2,361 | 1.32× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,253 | 673 | 3,096 | 1.05× |
| Double | 905 | 716 | 664 | 1.26× |
| Int64 | 5,689 | 2,536 | 4,293 | 1.33× |
| String | 11,527 | 6,152 | 9,456 | 1.22× |
| Uint64 | 6,259 | 2,748 | 4,382 | 1.43× |
| Apache Builds (minified) | 20,694 | 9,574 | 13,214 | 1.57× |
| Canada (minified) | 2,136 | 1,521 | 1,026 | 1.40× |
| Apache Builds (prettified) | 22,953 | 7,952 | 605 | 2.89× |
| Canada (prettified) | 6,100 | 3,822 | 389 | 1.60× |
| CitmCatalog (minified) | 12,831 | 6,925 | 10,261 | 1.25× |
| CitmCatalog (prettified) | 20,480 | 7,945 | 566 | 2.58× |
| Discord (minified) | 15,180 | 6,736 | 11,428 | 1.33× |
| Discord (prettified) | 20,459 | 7,301 | 552 | 2.80× |
| Github Events (minified) | 14,770 | 8,375 | 9,797 | 1.51× |
| Google Maps Response (minified) | 17,275 | 4,925 | 10,207 | 1.69× |
| Github Events (prettified) | 18,290 | 5,952 | 579 | 3.07× |
| Google Maps Response (prettified) | 21,966 | 7,612 | 527 | 2.89× |
| Instruments (minified) | 20,416 | 6,798 | 15,769 | 1.29× |
| Instruments (prettified) | 26,005 | 7,479 | 480 | 3.48× |
| Marine IK Reverse (minified) | 1,276 | 932 | 874 | 1.37× |
| Marine IK Reverse (prettified) | 6,477 | 4,014 | 299 | 1.61× |
| Marine IK (minified) | 1,288 | 990 | 884 | 1.30× |
| Marine IK (prettified) | 6,342 | 4,071 | 368 (n/c) | 1.56× |
| Random (minified) | 16,285 | 4,714 | 12,090 | 1.35× |
| Random (prettified) | 20,081 | 6,200 | 522 | 3.24× |
| Twitter (minified) | 17,388 | 8,508 | 10,181 | 1.71× |
| Twitter (prettified) | 21,131 | 6,835 | 509 | 3.09× |
| Apache Builds Small (minified) | 13,816 | 5,012 | 8,294 | 1.67× |
| Canada Small (minified) | 1,928 | 1,731 | 1,087 | 1.11× |
| Apache Builds Small (prettified) | 18,777 | 4,523 | 615 | 4.15× |
| Canada Small (prettified) | 6,600 | 3,849 | 416 | 1.71× |
| CitmCatalog Small (minified) | 8,610 | 4,064 | 5,763 | 1.49× |
| CitmCatalog Small (prettified) | 16,507 | 4,601 | 522 | 3.59× |
| Discord Small (minified) | 11,662 | 4,987 | 7,350 | 1.59× |
| Discord Small (prettified) | 16,912 | 4,373 | 578 | 3.87× |
| Github Events Small (minified) | 9,834 | 5,661 | 6,967 | 1.41× |
| Google Maps Response Small (minified) | 14,790 | 4,378 | 8,707 | 1.70× |
| Github Events Small (prettified) | 13,471 | 4,703 | 575 | 2.86× |
| Google Maps Response Small (prettified) | 30,029 | 6,982 | 541 | 4.30× |
| Instruments Small (minified) | 14,232 | 4,744 | 8,735 | 1.63× |
| Instruments Small (prettified) | 23,921 | 4,768 | 575 | 5.02× |
| Marine IK Reverse Small (minified) | 1,435 | 1,340 | 1,515 | 0.95× |
| Marine IK Reverse Small (prettified) | 6,338 | 3,888 | 506 | 1.63× |
| Marine IK Small (minified) | 1,347 | 1,268 | 1,524 | 0.88× |
| Marine IK Small (prettified) | 6,914 | 4,064 | 534 | 1.70× |
| Random Small (minified) | 15,564 | 5,016 | 9,153 | 1.70× |
| Random Small (prettified) | 26,766 | 6,078 | 583 | 4.40× |
| Twitter Small (minified) | 10,271 | 6,613 | 7,271 | 1.41× |
| Twitter Small (prettified) | 13,859 | 4,933 | 627 | 2.81× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,344 | 2,437 | 1,809 | 1.37× |
| Prettify | 4,322 | 3,714 | 2,592 | 1.16× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,264 | 809 | 2,418 | 1.35× |
| Double | 902 | 571 | 744 | 1.21× |
| Int64 | 4,271 | 2,546 | 3,221 | 1.33× |
| String | 8,598 | 4,757 | 8,849 | 0.97× |
| Uint64 | 4,889 | 2,045 | 3,054 | 1.60× |
| Apache Builds (minified) | 14,383 | 6,910 | 9,998 | 1.44× |
| Canada (minified) | 2,230 | 1,699 | 859 | 1.31× |
| Apache Builds (prettified) | 14,411 | 5,622 | 354 | 2.56× |
| Canada (prettified) | 6,411 | 3,189 | 313 (n/c) | 2.01× |
| CitmCatalog (minified) | 8,031 | 1,981 | 6,526 | 1.23× |
| CitmCatalog (prettified) | 17,106 | 4,607 | 293 | 3.71× |
| Discord (minified) | 12,839 | 2,891 | 8,751 | 1.47× |
| Discord (prettified) | 13,042 | 2,618 | 334 | 4.98× |
| Github Events (minified) | 15,562 | 3,687 | 10,466 | 1.49× |
| Google Maps Response (minified) | 12,268 | 2,010 | 9,105 | 1.35× |
| Github Events (prettified) | 16,211 | 3,410 | 437 | 4.75× |
| Google Maps Response (prettified) | 23,764 | 3,278 | 281 | 7.25× |
| Instruments (minified) | 15,992 | 2,258 | 8,623 | 1.85× |
| Instruments (prettified) | 15,794 | 2,122 | 347 | 7.44× |
| Marine IK Reverse (minified) | 1,388 | 945 | 804 | 1.47× |
| Marine IK Reverse (prettified) | 6,952 | 3,461 | 272 | 2.01× |
| Marine IK (minified) | 1,309 | 770 | 774 | 1.69× |
| Marine IK (prettified) | 6,310 | 2,953 | 288 | 2.14× |
| Random (minified) | 11,277 | 2,373 | 7,517 | 1.50× |
| Random (prettified) | 14,034 | 2,712 | 352 | 5.18× |
| Twitter (minified) | 16,602 | 4,568 | 12,341 | 1.35× |
| Twitter (prettified) | 17,762 | 4,643 | 408 | 3.83× |
| Apache Builds Small (minified) | 9,545 | 2,436 | 6,566 | 1.45× |
| Canada Small (minified) | 2,292 | 1,946 | 859 | 1.18× |
| Apache Builds Small (prettified) | 8,545 | 3,862 | 361 | 2.21× |
| Canada Small (prettified) | 7,487 | 3,610 | 291 | 2.07× |
| CitmCatalog Small (minified) | 7,519 | 1,999 | 7,840 | 0.96× |
| CitmCatalog Small (prettified) | 17,689 | 2,328 | 299 | 7.60× |
| Discord Small (minified) | 13,235 | 2,421 | 8,040 | 1.65× |
| Discord Small (prettified) | 14,038 | 2,356 | 378 | 5.96× |
| Github Events Small (minified) | 13,868 | 3,336 | 8,764 | 1.58× |
| Google Maps Response Small (minified) | 9,942 (n/c) | 1,732 | 8,185 | 1.21× |
| Github Events Small (prettified) | 14,061 | 2,986 | 384 | 4.71× |
| Google Maps Response Small (prettified) | 21,260 | 2,921 | 309 | 7.28× |
| Instruments Small (minified) | 14,019 (n/c) | 2,121 | 6,464 | 2.17× |
| Instruments Small (prettified) | 17,476 | 2,450 | 331 | 7.13× |
| Marine IK Reverse Small (minified) | 1,720 | 1,077 | 1,433 | 1.20× |
| Marine IK Reverse Small (prettified) | 7,988 | 3,370 | 303 | 2.37× |
| Marine IK Small (minified) | 1,798 | 1,065 | 1,563 | 1.15× |
| Marine IK Small (prettified) | 9,081 | 3,054 | 284 | 2.97× |
| Random Small (minified) | 11,743 | 2,389 | 9,499 | 1.24× |
| Random Small (prettified) | 14,087 | 2,686 | 307 | 5.25× |
| Twitter Small (minified) | 14,446 | 3,136 | 8,711 | 1.66× |
| Twitter Small (prettified) | 14,655 | 3,654 | 444 | 4.01× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,100 | 1,498 | 1,234 | 1.40× |
| Prettify | 3,093 | 1,926 | 1,650 | 1.61× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,891 | 726 | 3.98× |
| Double | 876 | 677 | 1.29× |
| Int64 | 3,721 | 2,112 | 1.76× |
| String | 8,067 | 3,819 | 2.11× |
| Uint64 | 3,536 | 2,261 | 1.56× |
| Apache Builds (minified) | 11,270 | 3,470 | 3.25× |
| Canada (minified) | 3,028 | 1,866 | 1.62× |
| Apache Builds (prettified) | 16,066 | 3,538 | 4.54× |
| Canada (prettified) | 7,578 | 3,967 | 1.91× |
| CitmCatalog (minified) | 6,534 | 2,285 | 2.86× |
| CitmCatalog (prettified) | 20,203 | 3,633 | 5.56× |
| Discord (minified) | 13,572 | 3,145 | 4.32× |
| Discord (prettified) | 20,233 | 2,866 | 7.06× |
| Github Events (minified) | 16,523 | 4,925 | 3.35× |
| Google Maps Response (minified) | 9,630 | 1,984 | 4.85× |
| Github Events (prettified) | 16,269 | 4,199 | 3.87× |
| Google Maps Response (prettified) | 24,694 | 3,201 | 7.72× |
| Instruments (minified) | 13,250 | 2,252 | 5.88× |
| Instruments (prettified) | 17,901 | 2,213 | 8.09× |
| Marine IK Reverse (minified) | 1,559 | 866 (n/c) | 1.80× |
| Marine IK Reverse (prettified) | 6,772 | 3,123 | 2.17× |
| Marine IK (minified) | 1,390 | 780 (n/c) | 1.78× |
| Marine IK (prettified) | 7,308 (n/c) | 3,012 | 2.43× |
| Random (minified) | 11,180 | 2,201 | 5.08× |
| Random (prettified) | 14,375 | 2,457 | 5.85× |
| Twitter (minified) | 12,262 | 3,593 | 3.41× |
| Twitter (prettified) | 14,265 | 3,931 | 3.63× |
| Apache Builds Small (minified) | 7,051 | 2,631 (n/c) | 2.68× |
| Canada Small (minified) | 2,166 | 1,522 | 1.42× |
| Apache Builds Small (prettified) | 12,622 | 2,646 | 4.77× |
| Canada Small (prettified) | 7,547 | 3,313 | 2.28× |
| CitmCatalog Small (minified) | 6,692 | 1,640 | 4.08× |
| CitmCatalog Small (prettified) | 12,884 | 2,132 | 6.04× |
| Discord Small (minified) | 10,406 | 2,420 | 4.30× |
| Discord Small (prettified) | 13,909 | 2,002 | 6.95× |
| Github Events Small (minified) | 10,398 | 2,744 | 3.79× |
| Google Maps Response Small (minified) | 7,591 | 1,623 | 4.68× |
| Github Events Small (prettified) | 12,450 | 2,511 | 4.96× |
| Google Maps Response Small (prettified) | 21,704 | 2,435 | 8.91× |
| Instruments Small (minified) | 12,155 | 1,747 | 6.96× |
| Instruments Small (prettified) | 25,293 | 2,524 | 10.02× |
| Marine IK Reverse Small (minified) | 1,899 (n/c) | 961 | 1.98× |
| Marine IK Reverse Small (prettified) | 7,256 | 2,915 | 2.49× |
| Marine IK Small (minified) | 1,597 | 925 | 1.73× |
| Marine IK Small (prettified) | 7,179 | 3,185 | 2.25× |
| Random Small (minified) | 8,061 | 1,820 | 4.43× |
| Random Small (prettified) | 13,982 | 2,123 | 6.58× |
| Twitter Small (minified) | 9,877 | 3,192 | 3.09× |
| Twitter Small (prettified) | 11,646 | 3,259 | 3.57× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,110 | 1,551 | 930 | 1.36× |
| Prettify | 3,037 | 1,896 | 1,185 | 1.60× |

### Losses and ties

The four freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,478 vs Jsonifier's 3,855 MB/s (0.86×). Reused, Jsonifier wins by 3.11× (15,212 vs 4,894).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,476 vs 1,312 MB/s (0.89×). It also wins reused, 1,524 vs 1,347 MB/s (0.88×).
- **Double, macOS / GCC.** The reflection writer runs 329 vs 251 MB/s (0.76×). Jsonifier is ahead of Glaze (229 MB/s, 1.10×). Reused, Jsonifier wins, 902 MB/s against the reflection writer's 744 (1.21×) and Glaze's 571.
- **Uint64, macOS / GCC.** The reflection writer runs 733 vs 679 MB/s (0.93×), with Glaze behind Jsonifier at 564 MB/s (1.20×). Reused, Jsonifier wins by 1.60× (4,889 vs 3,054).

The one freshly allocated tie is minified Marine IK Reverse Small on Linux / GCC, where the reflection writer is ahead, 1,472 vs 1,379 MB/s (0.94×), and Glaze is behind at 1,226. Reused it becomes a loss, 1,515 vs 1,435 MB/s (0.95×).

Two reused non-wins have no freshly allocated counterpart, both on macOS / GCC against the reflection writer: String, which wins freshly allocated (1,275 MB/s against the reflection writer's 1,062 and Glaze's 908) and loses reused (0.97×), and minified CitmCatalog Small, which wins freshly allocated (8,420 vs 7,120 MB/s) and ties reused.

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.12–6.09× and prettified documents by 16–66×; reused, minified documents lead it by 1.23–2.59×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 42 of the 44 Small document tests freshly allocated and 41 reused. Freshly allocated it loses minified Marine IK Small and ties minified Marine IK Reverse Small on Linux / GCC; reused it loses both of those and ties minified CitmCatalog Small on macOS / GCC. Why the reflection writer edges ahead on the two Marine IK Small tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.33–1.60× freshly allocated and 1.34–1.56× reused, and wins Prettify on all five in both runs, by 1.12–1.57× freshly allocated and 1.16–1.68× reused. Glaze is the next-fastest library in every case. The narrowest margin is freshly allocated Prettify on Windows / MSVC, 2,352 vs 2,104 MB/s; reused, the same test leads by 1.68× (3,585 vs 2,130).

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,275 MB/s on macOS / GCC (1.40× Glaze's 908, ahead of the reflection writer's 1,062) and 1,120 MB/s on macOS / Clang (1.28× Glaze's 876). The AVX2 builds run the same test at 1,390–4,364 MB/s and lead Glaze by 1.53× (Windows / MSVC), 2.17× (Linux / Clang) and 1.94× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 2.11× on macOS / Clang (8,067 vs 3,819 MB/s) and 1.81× on macOS / GCC (8,598 vs 4,757), against 1.87–3.36× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 8,849 MB/s (0.97×).

So most of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: reused, the macOS / Clang lead over Glaze falls inside the AVX2 range, and macOS / GCC sits just below it. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated, only on macOS / GCC: 329 vs 251 MB/s (0.76×). On Linux / GCC Jsonifier edges it, 351 vs 341 MB/s (1.03×). Reused, Jsonifier leads on both: 905 vs 664 MB/s on Linux / GCC (1.36×) and 902 vs 744 MB/s on macOS / GCC (1.21×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
