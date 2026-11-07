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

On the Bool test this writer leads Glaze by 7.80–51.88× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.32× to 4.30×.

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

Jsonifier was fastest in 247 of the 255 write tests on five builds, tied 1 and lost 7. With the output string reused across iterations it was fastest in 249 of 255, tied none and lost 6. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, four losses are to Glaze: minified Twitter and Prettify on Windows / MSVC, and minified Canada and Canada Small on macOS / GCC. The other three are against simdjson's reflection writer, which exists only on GCC: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and Double on macOS / GCC. The one freshly allocated tie is Uint64 on macOS / GCC, against the reflection writer. Reused, four losses are against the reflection writer (Bool, minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, and String on macOS / GCC) and two against Glaze (minified Canada and Canada Small on macOS / GCC). Linux / Clang and macOS / Clang win all 51 tests in both runs, and Windows / MSVC wins all 51 reused. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.31–7.40× across the five builds and the reflection writer by 16–60× on the two GCC builds; Small documents lead them by 1.22–7.40× and 11.76–63.05×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 7, 2026, with Jsonifier [3e5ab01](https://github.com/nihilai-collective/jsonifier/commit/3e5ab01), Glaze [0a363d4](https://github.com/stephenberry/glaze/commit/0a363d40), simdjson [dfe02cf](https://github.com/simdjson/simdjson/commit/dfe02cfe3) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 47 | 1 | 3 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **247** | **1** | **7** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,266 | 24 | 51.88× |
| Double | 472 | 150 | 3.14× |
| Int64 | 517 | 381 | 1.36× |
| String | 1,468 | 900 | 1.63× |
| Uint64 | 522 | 379 | 1.38× |
| Apache Builds (minified) | 11,972 | 3,882 | 3.08× |
| Canada (minified) | 1,234 | 916 | 1.35× |
| Apache Builds (prettified) | 13,143 | 4,427 | 2.97× |
| Canada (prettified) | 2,952 | 1,482 | 1.99× |
| CitmCatalog (minified) | 8,875 | 4,320 | 2.05× |
| CitmCatalog (prettified) | 4,572 | 1,831 | 2.50× |
| Discord (minified) | 11,383 | 4,198 | 2.71× |
| Discord (prettified) | 12,456 | 3,850 | 3.24× |
| Github Events (minified) | 11,299 | 4,436 | 2.55× |
| Google Maps Response (minified) | 9,622 | 3,576 | 2.69× |
| Github Events (prettified) | 11,426 | 2,796 | 4.09× |
| Google Maps Response (prettified) | 14,884 | 5,287 | 2.81× |
| Instruments (minified) | 13,727 | 3,947 | 3.48× |
| Instruments (prettified) | 15,553 | 3,808 | 4.08× |
| Marine IK Reverse (minified) | 879 | 686 | 1.28× |
| Marine IK Reverse (prettified) | 2,502 | 1,907 | 1.31× |
| Marine IK (minified) | 868 | 624 | 1.39× |
| Marine IK (prettified) | 2,588 | 1,230 | 2.10× |
| Random (minified) | 3,807 | 2,968 | 1.28× |
| Random (prettified) | 4,088 | 2,120 | 1.93× |
| Twitter (minified) | 3,995 | 4,519 | 0.88× |
| Twitter (prettified) | 4,207 | 1,699 | 2.48× |
| Apache Builds Small (minified) | 9,654 | 3,575 | 2.70× |
| Canada Small (minified) | 1,824 | 1,211 | 1.51× |
| Apache Builds Small (prettified) | 10,598 | 4,104 | 2.58× |
| Canada Small (prettified) | 4,433 | 3,106 | 1.43× |
| CitmCatalog Small (minified) | 6,189 | 3,222 | 1.92× |
| CitmCatalog Small (prettified) | 8,727 | 3,931 | 2.22× |
| Discord Small (minified) | 9,655 | 3,406 | 2.83× |
| Discord Small (prettified) | 8,120 | 2,870 | 2.83× |
| Github Events Small (minified) | 9,642 | 4,302 (n/c) | 2.24× |
| Google Maps Response Small (minified) | 8,373 | 3,432 | 2.44× |
| Github Events Small (prettified) | 8,889 | 3,675 | 2.42× |
| Google Maps Response Small (prettified) | 14,030 | 4,765 | 2.94× |
| Instruments Small (minified) | 8,995 | 3,323 (n/c) | 2.71× |
| Instruments Small (prettified) | 9,828 | 3,042 | 3.23× |
| Marine IK Reverse Small (minified) | 1,539 | 952 | 1.62× |
| Marine IK Reverse Small (prettified) | 5,071 | 2,885 | 1.76× |
| Marine IK Small (minified) | 1,504 | 978 | 1.54× |
| Marine IK Small (prettified) | 5,034 | 2,823 | 1.78× |
| Random Small (minified) | 9,226 | 3,065 | 3.01× |
| Random Small (prettified) | 11,867 | 4,043 | 2.94× |
| Twitter Small (minified) | 10,406 | 4,710 | 2.21× |
| Twitter Small (prettified) | 9,218 | 3,176 | 2.90× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,056 (n/c) | 1,715 | 1,341 | 1.78× |
| Prettify | 2,460 | 2,600 | 1,289 | 0.95× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,209 | 214 | 10.32× |
| Double | 327 | 292 | 1.12× |
| Int64 | 942 | 864 | 1.09× |
| String | 4,066 | 1,915 | 2.12× |
| Uint64 | 912 | 819 | 1.11× |
| Apache Builds (minified) | 16,236 | 5,280 | 3.08× |
| Canada (minified) | 1,892 | 937 | 2.02× |
| Apache Builds (prettified) | 18,415 | 5,295 | 3.48× |
| Canada (prettified) | 5,076 | 3,234 | 1.57× |
| CitmCatalog (minified) | 10,080 | 5,025 | 2.01× |
| CitmCatalog (prettified) | 14,944 | 5,236 | 2.85× |
| Discord (minified) | 14,190 | 5,044 | 2.81× |
| Discord (prettified) | 15,989 | 4,084 | 3.92× |
| Github Events (minified) | 16,239 | 4,932 | 3.29× |
| Google Maps Response (minified) | 11,574 | 3,853 | 3.00× |
| Github Events (prettified) | 16,688 | 4,711 | 3.54× |
| Google Maps Response (prettified) | 15,365 | 5,147 | 2.99× |
| Instruments (minified) | 18,172 | 4,524 | 4.02× |
| Instruments (prettified) | 20,412 | 4,885 | 4.18× |
| Marine IK Reverse (minified) | 1,196 | 645 | 1.86× |
| Marine IK Reverse (prettified) | 4,968 | 3,626 | 1.37× |
| Marine IK (minified) | 1,217 | 607 | 2.01× |
| Marine IK (prettified) | 4,835 | 2,661 (n/c) | 1.82× |
| Random (minified) | 14,892 | 3,637 | 4.09× |
| Random (prettified) | 16,328 | 3,868 | 4.22× |
| Twitter (minified) | 19,474 | 6,026 | 3.23× |
| Twitter (prettified) | 18,784 | 4,222 | 4.45× |
| Apache Builds Small (minified) | 9,979 | 3,346 | 2.98× |
| Canada Small (minified) | 1,886 | 903 | 2.09× |
| Apache Builds Small (prettified) | 11,428 | 3,570 | 3.20× |
| Canada Small (prettified) | 4,341 | 3,562 | 1.22× |
| CitmCatalog Small (minified) | 7,574 | 2,748 | 2.76× |
| CitmCatalog Small (prettified) | 10,547 | 3,368 | 3.13× |
| Discord Small (minified) | 10,235 | 2,991 | 3.42× |
| Discord Small (prettified) | 11,406 | 2,471 | 4.62× |
| Github Events Small (minified) | 9,533 | 3,673 | 2.60× |
| Google Maps Response Small (minified) | 10,336 | 3,168 | 3.26× |
| Github Events Small (prettified) | 10,831 | 3,642 | 2.97× |
| Google Maps Response Small (prettified) | 16,407 | 4,774 | 3.44× |
| Instruments Small (minified) | 12,761 | 2,357 | 5.41× |
| Instruments Small (prettified) | 14,957 | 2,393 | 6.25× |
| Marine IK Reverse Small (minified) | 1,533 | 817 | 1.88× |
| Marine IK Reverse Small (prettified) | 5,176 | 3,433 | 1.51× |
| Marine IK Small (minified) | 1,545 | 786 | 1.97× |
| Marine IK Small (prettified) | 5,037 | 3,527 | 1.43× |
| Random Small (minified) | 12,764 | 3,404 | 3.75× |
| Random Small (prettified) | 15,678 | 3,667 | 4.28× |
| Twitter Small (minified) | 8,646 | 3,938 | 2.20× |
| Twitter Small (prettified) | 9,753 | 2,921 | 3.34× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,607 | 2,381 | 1,773 | 1.51× |
| Prettify | 4,493 | 3,624 | 2,262 | 1.24× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,703 | 194 | 253 | 6.73× |
| Double | 348 | 302 | 337 | 1.03× |
| Int64 | 922 | 845 | 909 | 1.01× |
| String | 4,194 | 1,966 | 3,822 | 1.10× |
| Uint64 | 925 | 851 | 872 | 1.06× |
| Apache Builds (minified) | 20,577 | 7,802 | 13,428 | 1.53× |
| Canada (minified) | 2,125 | 1,435 | 1,046 | 1.48× |
| Apache Builds (prettified) | 23,116 | 7,071 | 621 | 3.27× |
| Canada (prettified) | 6,075 | 3,304 | 388 | 1.84× |
| CitmCatalog (minified) | 12,418 | 6,065 | 11,154 | 1.11× |
| CitmCatalog (prettified) | 20,476 | 5,850 | 570 | 3.50× |
| Discord (minified) | 15,088 | 6,349 | 13,121 | 1.15× |
| Discord (prettified) | 20,008 | 5,867 | 525 | 3.41× |
| Github Events (minified) | 14,752 | 7,115 | 11,157 | 1.32× |
| Google Maps Response (minified) | 15,522 | 4,573 | 9,403 | 1.65× |
| Github Events (prettified) | 18,360 | 5,050 | 637 | 3.64× |
| Google Maps Response (prettified) | 20,936 | 6,639 | 540 | 3.15× |
| Instruments (minified) | 19,975 | 5,600 | 15,426 | 1.29× |
| Instruments (prettified) | 26,061 | 5,516 | 469 | 4.72× |
| Marine IK Reverse (minified) | 1,306 | 981 | 878 | 1.33× |
| Marine IK Reverse (prettified) | 6,401 | 3,686 | 279 | 1.74× |
| Marine IK (minified) | 1,272 | 950 | 873 | 1.34× |
| Marine IK (prettified) | 6,296 | 3,101 | 385 | 2.03× |
| Random (minified) | 17,102 | 4,509 | 11,217 | 1.52× |
| Random (prettified) | 19,998 | 4,818 | 520 | 4.15× |
| Twitter (minified) | 17,574 | 7,802 | 10,832 | 1.62× |
| Twitter (prettified) | 20,857 | 4,968 | 529 | 4.20× |
| Apache Builds Small (minified) | 11,999 | 4,721 | 7,022 | 1.71× |
| Canada Small (minified) | 2,055 | 1,583 | 1,058 | 1.30× |
| Apache Builds Small (prettified) | 13,298 | 4,179 | 544 | 3.18× |
| Canada Small (prettified) | 6,242 | 3,696 | 407 | 1.69× |
| CitmCatalog Small (minified) | 7,551 | 3,492 | 6,033 | 1.25× |
| CitmCatalog Small (prettified) | 14,920 | 3,225 | 494 | 4.63× |
| Discord Small (minified) | 9,586 | 4,052 | 7,353 | 1.30× |
| Discord Small (prettified) | 14,759 | 3,148 | 490 | 4.69× |
| Github Events Small (minified) | 8,667 | 4,585 | 6,586 | 1.32× |
| Google Maps Response Small (minified) | 12,924 | 3,910 | 8,332 | 1.55× |
| Github Events Small (prettified) | 10,958 | 3,798 | 528 | 2.89× |
| Google Maps Response Small (prettified) | 25,597 | 5,675 | 522 | 4.51× |
| Instruments Small (minified) | 11,899 | 3,604 | 7,309 (n/c) | 1.63× |
| Instruments Small (prettified) | 21,001 | 2,839 | 520 | 7.40× |
| Marine IK Reverse Small (minified) | 1,385 | 1,179 | 1,516 | 0.91× |
| Marine IK Reverse Small (prettified) | 6,532 | 3,374 | 526 | 1.94× |
| Marine IK Small (minified) | 1,270 | 1,205 | 1,496 | 0.85× |
| Marine IK Small (prettified) | 6,263 | 3,226 | 532 | 1.94× |
| Random Small (minified) | 14,886 | 4,268 | 8,447 | 1.76× |
| Random Small (prettified) | 24,112 | 4,426 | 529 | 5.45× |
| Twitter Small (minified) | 8,740 | 5,482 | 6,603 | 1.32× |
| Twitter Small (prettified) | 12,305 | 3,305 | 563 | 3.72× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,078 | 2,539 | 1,868 | 1.21× |
| Prettify | 3,958 | 3,198 | 2,415 | 1.24× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,959 | 129 | 206 | 9.52× |
| Double | 292 | 220 | 316 | 0.92× |
| Int64 | 822 | 571 | 806 | 1.02× |
| String | 1,491 | 1,158 | 1,318 | 1.13× |
| Uint64 | 827 | 583 | 829 | 1.00× |
| Apache Builds (minified) | 12,790 | 5,159 | 9,479 | 1.35× |
| Canada (minified) | 1,327 | 1,804 | 861 | 0.74× |
| Apache Builds (prettified) | 14,067 | 4,701 | 389 | 2.99× |
| Canada (prettified) | 7,749 | 3,616 | 312 | 2.14× |
| CitmCatalog (minified) | 8,658 | 2,226 | 8,129 | 1.07× |
| CitmCatalog (prettified) | 20,504 | 4,014 | 357 | 5.11× |
| Discord (minified) | 12,888 | 2,908 | 10,269 | 1.26× |
| Discord (prettified) | 14,932 | 2,535 | 367 | 5.89× |
| Github Events (minified) | 16,078 | 3,911 | 12,016 | 1.34× |
| Google Maps Response (minified) | 11,283 | 1,907 | 8,624 | 1.31× |
| Github Events (prettified) | 16,865 | 3,486 | 445 | 4.84× |
| Google Maps Response (prettified) | 20,325 | 3,003 | 340 | 6.77× |
| Instruments (minified) | 14,877 | 2,079 | 8,483 | 1.75× |
| Instruments (prettified) | 18,040 | 2,439 | 310 | 7.40× |
| Marine IK Reverse (minified) | 1,400 | 941 | 818 | 1.49× |
| Marine IK Reverse (prettified) | 6,870 | 3,345 | 314 | 2.05× |
| Marine IK (minified) | 1,392 | 921 | 829 | 1.51× |
| Marine IK (prettified) | 6,994 | 2,990 | 317 | 2.34× |
| Random (minified) | 12,468 | 2,378 | 8,815 | 1.41× |
| Random (prettified) | 15,908 | 2,745 | 354 | 5.80× |
| Twitter (minified) | 14,395 | 4,048 | 12,051 | 1.19× |
| Twitter (prettified) | 17,571 | 4,037 | 437 | 4.35× |
| Apache Builds Small (minified) | 10,190 | 3,172 | 7,107 | 1.43× |
| Canada Small (minified) | 843 | 1,736 | 948 | 0.49× |
| Apache Builds Small (prettified) | 10,867 | 3,943 | 375 | 2.76× |
| Canada Small (prettified) | 7,679 | 3,292 | 299 | 2.33× |
| CitmCatalog Small (minified) | 9,083 | 1,967 | 7,772 | 1.17× |
| CitmCatalog Small (prettified) | 17,158 | 2,689 | 345 | 6.38× |
| Discord Small (minified) | 12,314 | 2,776 | 9,224 | 1.34× |
| Discord Small (prettified) | 14,287 | 2,466 | 357 | 5.79× |
| Github Events Small (minified) | 11,699 | 3,192 | 10,277 | 1.14× |
| Google Maps Response Small (minified) | 10,035 | 1,608 | 8,338 | 1.20× |
| Github Events Small (prettified) | 13,910 | 2,978 | 396 | 4.67× |
| Google Maps Response Small (prettified) | 20,996 | 2,863 | 333 | 7.33× |
| Instruments Small (minified) | 14,569 | 2,017 | 8,113 | 1.80× |
| Instruments Small (prettified) | 17,281 | 2,394 | 343 | 7.22× |
| Marine IK Reverse Small (minified) | 1,947 | 1,085 | 1,689 | 1.15× |
| Marine IK Reverse Small (prettified) | 8,733 | 3,398 | 326 | 2.57× |
| Marine IK Small (minified) | 1,952 | 1,094 | 1,710 | 1.14× |
| Marine IK Small (prettified) | 8,982 | 3,331 | 324 | 2.70× |
| Random Small (minified) | 12,618 | 2,232 | 9,627 | 1.31× |
| Random Small (prettified) | 15,062 | 2,548 | 346 | 5.91× |
| Twitter Small (minified) | 12,897 | 3,592 | 9,327 | 1.38× |
| Twitter Small (prettified) | 14,694 | 3,228 | 417 | 4.55× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,442 | 1,676 | 1,237 | 1.46× |
| Prettify | 3,292 | 2,385 | 1,652 | 1.38× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,033 | 132 | 7.83× |
| Double | 291 | 224 | 1.30× |
| Int64 | 2,957 | 552 | 5.36× |
| String | 1,344 | 1,150 | 1.17× |
| Uint64 | 2,930 | 558 | 5.25× |
| Apache Builds (minified) | 12,162 | 3,543 | 3.43× |
| Canada (minified) | 3,000 | 1,892 | 1.59× |
| Apache Builds (prettified) | 12,100 | 3,592 | 3.37× |
| Canada (prettified) | 7,223 | 3,563 | 2.03× |
| CitmCatalog (minified) | 7,266 | 1,889 | 3.85× |
| CitmCatalog (prettified) | 13,694 | 3,963 | 3.46× |
| Discord (minified) | 12,571 | 3,351 | 3.75× |
| Discord (prettified) | 13,374 | 2,808 | 4.76× |
| Github Events (minified) | 14,249 | 4,787 | 2.98× |
| Google Maps Response (minified) | 8,767 (n/c) | 2,264 | 3.87× |
| Github Events (prettified) | 12,383 | 3,599 | 3.44× |
| Google Maps Response (prettified) | 12,738 | 3,558 | 3.58× |
| Instruments (minified) | 12,767 | 2,169 | 5.89× |
| Instruments (prettified) | 12,736 | 2,667 | 4.78× |
| Marine IK Reverse (minified) | 1,197 | 766 | 1.56× |
| Marine IK Reverse (prettified) | 5,198 | 2,859 | 1.82× |
| Marine IK (minified) | 1,487 | 861 | 1.73× |
| Marine IK (prettified) | 5,525 | 3,008 | 1.84× |
| Random (minified) | 9,710 | 2,779 | 3.49× |
| Random (prettified) | 11,702 | 3,268 | 3.58× |
| Twitter (minified) | 15,473 | 4,538 | 3.41× |
| Twitter (prettified) | 14,026 | 3,933 | 3.57× |
| Apache Builds Small (minified) | 8,309 | 2,873 | 2.89× |
| Canada Small (minified) | 2,506 | 1,502 | 1.67× |
| Apache Builds Small (prettified) | 9,116 | 3,042 | 3.00× |
| Canada Small (prettified) | 6,627 | 3,131 | 2.12× |
| CitmCatalog Small (minified) | 7,294 | 1,804 | 4.04× |
| CitmCatalog Small (prettified) | 11,633 | 2,770 | 4.20× |
| Discord Small (minified) | 10,658 | 2,901 | 3.67× |
| Discord Small (prettified) | 12,231 | 2,893 | 4.23× |
| Github Events Small (minified) | 10,520 | 3,299 | 3.19× |
| Google Maps Response Small (minified) | 8,520 | 2,125 | 4.01× |
| Github Events Small (prettified) | 10,251 | 2,968 | 3.45× |
| Google Maps Response Small (prettified) | 13,455 | 3,345 | 4.02× |
| Instruments Small (minified) | 12,170 | 1,932 | 6.30× |
| Instruments Small (prettified) | 13,930 | 2,969 | 4.69× |
| Marine IK Reverse Small (minified) | 1,905 | 1,031 | 1.85× |
| Marine IK Reverse Small (prettified) | 4,772 | 2,675 | 1.78× |
| Marine IK Small (minified) | 1,864 | 984 | 1.89× |
| Marine IK Small (prettified) | 5,558 | 2,592 | 2.14× |
| Random Small (minified) | 10,404 | 2,781 | 3.74× |
| Random Small (prettified) | 8,598 | 3,171 | 2.71× |
| Twitter Small (minified) | 9,481 | 3,448 | 2.75× |
| Twitter Small (prettified) | 9,969 | 3,030 | 3.29× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,088 | 1,424 | 1,259 | 1.47× |
| Prettify | 3,303 | 2,266 | 1,517 | 1.46× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 249 of the 255 write tests, ties none and loses 6, all on the two GCC builds. On Linux / GCC it loses Bool (3,246 vs 3,170 MB/s, 0.98×), minified Marine IK Small (1,515 vs 1,312 MB/s, 0.87×) and minified Marine IK Reverse Small (1,550 vs 1,426 MB/s, 0.92×) to the reflection writer. On macOS / GCC it loses String to the reflection writer (11,003 vs 9,149 MB/s, 0.83×) and minified Canada (1,876 vs 1,349 MB/s, 0.72×) and Canada Small (1,999 vs 844 MB/s, 0.42×) to Glaze.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 48 | 0 | 3 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **249** | **0** | **6** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,680 | 298 | 5.64× |
| Double | 776 | 386 | 2.01× |
| Int64 | 4,191 | 1,056 | 3.97× |
| String | 7,749 | 2,539 | 3.05× |
| Uint64 | 4,661 | 1,084 | 4.30× |
| Apache Builds (minified) | 12,527 | 4,603 | 2.72× |
| Canada (minified) | 1,518 | 1,286 | 1.18× |
| Apache Builds (prettified) | 13,275 | 5,180 | 2.56× |
| Canada (prettified) | 4,773 | 3,391 | 1.41× |
| CitmCatalog (minified) | 8,915 | 4,788 | 1.86× |
| CitmCatalog (prettified) | 13,523 | 7,120 | 1.90× |
| Discord (minified) | 11,731 | 4,831 | 2.43× |
| Discord (prettified) | 13,003 | 4,606 | 2.82× |
| Github Events (minified) | 12,180 | 5,653 | 2.15× |
| Google Maps Response (minified) | 10,582 | 4,023 (n/c) | 2.63× |
| Github Events (prettified) | 11,900 | 3,191 | 3.73× |
| Google Maps Response (prettified) | 16,380 | 5,663 | 2.89× |
| Instruments (minified) | 14,366 | 4,300 | 3.34× |
| Instruments (prettified) | 15,527 | 4,279 | 3.63× |
| Marine IK Reverse (minified) | 1,037 (n/c) | 750 | 1.38× |
| Marine IK Reverse (prettified) | 3,662 | 3,583 | 1.02× |
| Marine IK (minified) | 1,024 | 795 | 1.29× |
| Marine IK (prettified) | 3,988 | 3,368 | 1.18× |
| Random (minified) | 10,769 | 3,233 | 3.33× |
| Random (prettified) | 12,542 | 4,392 | 2.86× |
| Twitter (minified) | 16,027 | 5,124 | 3.13× |
| Twitter (prettified) | 14,604 | 3,300 | 4.43× |
| Apache Builds Small (minified) | 10,880 | 4,011 | 2.71× |
| Canada Small (minified) | 1,875 | 1,343 | 1.40× |
| Apache Builds Small (prettified) | 11,987 | 4,585 | 2.61× |
| Canada Small (prettified) | 4,935 | 3,397 | 1.45× |
| CitmCatalog Small (minified) | 8,147 | 4,122 | 1.98× |
| CitmCatalog Small (prettified) | 9,716 | 4,940 | 1.97× |
| Discord Small (minified) | 10,851 | 4,394 | 2.47× |
| Discord Small (prettified) | 10,195 | 3,836 | 2.66× |
| Github Events Small (minified) | 12,134 | 5,478 | 2.22× |
| Google Maps Response Small (minified) | 10,054 | 4,224 | 2.38× |
| Github Events Small (prettified) | 12,900 | 4,790 (n/c) | 2.69× |
| Google Maps Response Small (prettified) | 16,455 | 5,406 | 3.04× |
| Instruments Small (minified) | 11,984 | 3,653 (n/c) | 3.28× |
| Instruments Small (prettified) | 12,293 | 2,988 (n/c) | 4.11× |
| Marine IK Reverse Small (minified) | 1,570 | 1,024 | 1.53× |
| Marine IK Reverse Small (prettified) | 5,396 | 3,388 | 1.59× |
| Marine IK Small (minified) | 1,578 | 1,074 | 1.47× |
| Marine IK Small (prettified) | 5,303 | 3,229 | 1.64× |
| Random Small (minified) | 11,644 | 3,962 | 2.94× |
| Random Small (prettified) | 13,927 | 5,180 | 2.69× |
| Twitter Small (minified) | 11,902 | 5,118 (n/c) | 2.33× |
| Twitter Small (prettified) | 11,375 | 4,544 | 2.50× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,007 | 1,841 | 1,339 | 1.63× |
| Prettify | 3,919 | 2,500 | 1,259 | 1.57× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,294 | 943 | 4.56× |
| Double | 822 | 709 | 1.16× |
| Int64 | 4,776 | 2,423 | 1.97× |
| String | 11,555 | 4,245 | 2.72× |
| Uint64 | 4,470 | 2,328 | 1.92× |
| Apache Builds (minified) | 16,395 | 5,889 | 2.78× |
| Canada (minified) | 1,890 | 975 | 1.94× |
| Apache Builds (prettified) | 18,648 | 5,819 | 3.20× |
| Canada (prettified) | 5,098 | 3,903 | 1.31× |
| CitmCatalog (minified) | 10,154 | 5,722 | 1.77× |
| CitmCatalog (prettified) | 14,846 | 7,039 | 2.11× |
| Discord (minified) | 14,456 | 5,592 | 2.59× |
| Discord (prettified) | 16,315 | 4,443 | 3.67× |
| Github Events (minified) | 16,541 | 5,491 | 3.01× |
| Google Maps Response (minified) | 12,281 | 4,289 | 2.86× |
| Github Events (prettified) | 16,902 | 5,313 | 3.18× |
| Google Maps Response (prettified) | 16,159 | 5,618 | 2.88× |
| Instruments (minified) | 18,376 | 5,438 | 3.38× |
| Instruments (prettified) | 20,649 | 5,513 | 3.75× |
| Marine IK Reverse (minified) | 1,203 | 632 | 1.91× |
| Marine IK Reverse (prettified) | 4,973 | 3,998 | 1.24× |
| Marine IK (minified) | 1,216 | 639 | 1.90× |
| Marine IK (prettified) | 4,856 | 3,803 | 1.28× |
| Random (minified) | 14,679 | 3,819 | 3.84× |
| Random (prettified) | 16,336 | 4,528 | 3.61× |
| Twitter (minified) | 19,583 | 6,808 | 2.88× |
| Twitter (prettified) | 18,662 | 5,001 | 3.73× |
| Apache Builds Small (minified) | 11,930 | 3,725 | 3.20× |
| Canada Small (minified) | 1,912 | 979 | 1.95× |
| Apache Builds Small (prettified) | 13,239 | 3,903 | 3.39× |
| Canada Small (prettified) | 4,671 | 3,905 | 1.20× |
| CitmCatalog Small (minified) | 8,303 | 3,023 | 2.75× |
| CitmCatalog Small (prettified) | 11,236 | 3,828 | 2.94× |
| Discord Small (minified) | 11,453 | 2,606 | 4.39× |
| Discord Small (prettified) | 12,520 | 2,633 | 4.75× |
| Github Events Small (minified) | 12,079 | 4,030 | 3.00× |
| Google Maps Response Small (minified) | 11,184 | 3,982 | 2.81× |
| Github Events Small (prettified) | 12,658 | 4,135 | 3.06× |
| Google Maps Response Small (prettified) | 17,716 | 5,424 | 3.27× |
| Instruments Small (minified) | 14,519 | 2,600 | 5.58× |
| Instruments Small (prettified) | 16,194 | 3,009 | 5.38× |
| Marine IK Reverse Small (minified) | 1,575 | 853 | 1.85× |
| Marine IK Reverse Small (prettified) | 5,311 | 3,940 | 1.35× |
| Marine IK Small (minified) | 1,616 | 841 | 1.92× |
| Marine IK Small (prettified) | 5,122 | 3,878 | 1.32× |
| Random Small (minified) | 14,026 | 3,686 | 3.81× |
| Random Small (prettified) | 17,097 | 4,345 | 3.93× |
| Twitter Small (minified) | 10,364 | 4,062 | 2.55× |
| Twitter Small (prettified) | 11,056 | 3,600 | 3.07× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,589 | 2,402 | 1,748 | 1.49× |
| Prettify | 4,590 | 3,615 | 2,233 | 1.27× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,170 | 689 | 3,246 | 0.98× |
| Double | 877 | 618 | 640 | 1.37× |
| Int64 | 5,284 | 2,712 | 4,005 | 1.32× |
| String | 11,181 | 6,129 | 9,521 | 1.17× |
| Uint64 | 6,284 | 2,697 | 4,077 | 1.54× |
| Apache Builds (minified) | 20,482 | 9,221 | 13,276 | 1.54× |
| Canada (minified) | 2,127 | 1,528 | 1,033 | 1.39× |
| Apache Builds (prettified) | 23,333 | 8,053 | 622 | 2.90× |
| Canada (prettified) | 6,055 | 3,901 | 393 | 1.55× |
| CitmCatalog (minified) | 12,459 | 7,035 | 11,270 | 1.11× |
| CitmCatalog (prettified) | 20,490 | 7,910 | 574 | 2.59× |
| Discord (minified) | 15,265 | 7,280 | 13,336 | 1.14× |
| Discord (prettified) | 20,242 | 7,058 | 520 | 2.87× |
| Github Events (minified) | 15,060 | 8,493 | 11,248 | 1.34× |
| Google Maps Response (minified) | 16,952 | 5,022 | 10,149 | 1.67× |
| Github Events (prettified) | 18,647 | 5,868 | 637 | 3.18× |
| Google Maps Response (prettified) | 21,957 | 7,459 | 535 | 2.94× |
| Instruments (minified) | 20,192 | 6,258 | 15,676 | 1.29× |
| Instruments (prettified) | 26,221 | 6,435 | 464 | 4.07× |
| Marine IK Reverse (minified) | 1,302 | 983 | 880 | 1.32× |
| Marine IK Reverse (prettified) | 6,330 | 4,031 | 309 | 1.57× |
| Marine IK (minified) | 1,273 | 996 | 890 | 1.28× |
| Marine IK (prettified) | 6,296 | 4,066 | 337 | 1.55× |
| Random (minified) | 17,068 | 5,134 | 11,145 | 1.53× |
| Random (prettified) | 20,161 | 5,704 | 523 | 3.53× |
| Twitter (minified) | 17,582 | 8,884 | 10,893 | 1.61× |
| Twitter (prettified) | 21,305 | 6,385 | 525 | 3.34× |
| Apache Builds Small (minified) | 14,405 | 5,225 | 7,800 | 1.85× |
| Canada Small (minified) | 2,083 | 1,588 | 1,071 | 1.31× |
| Apache Builds Small (prettified) | 18,255 | 4,678 | 543 | 3.90× |
| Canada Small (prettified) | 6,480 | 4,128 | 398 | 1.57× |
| CitmCatalog Small (minified) | 8,608 | 3,487 | 6,561 | 1.31× |
| CitmCatalog Small (prettified) | 16,726 | 3,537 | 486 | 4.73× |
| Discord Small (minified) | 11,213 | 4,878 | 8,382 | 1.34× |
| Discord Small (prettified) | 16,670 | 3,616 | 504 | 4.61× |
| Github Events Small (minified) | 10,156 | 5,785 | 7,724 | 1.31× |
| Google Maps Response Small (minified) | 14,256 | 4,152 | 8,913 | 1.60× |
| Github Events Small (prettified) | 13,851 | 4,456 | 526 | 3.11× |
| Google Maps Response Small (prettified) | 29,989 | 7,021 | 520 | 4.27× |
| Instruments Small (minified) | 13,803 | 4,350 | 8,579 | 1.61× |
| Instruments Small (prettified) | 24,212 | 2,893 | 520 | 8.37× |
| Marine IK Reverse Small (minified) | 1,426 | 1,273 | 1,550 | 0.92× |
| Marine IK Reverse Small (prettified) | 6,728 | 3,796 | 520 | 1.77× |
| Marine IK Small (minified) | 1,312 | 1,286 | 1,515 | 0.87× |
| Marine IK Small (prettified) | 6,485 | 3,837 | 527 | 1.69× |
| Random Small (minified) | 16,726 | 4,942 | 9,020 | 1.85× |
| Random Small (prettified) | 27,771 | 5,393 | 528 | 5.15× |
| Twitter Small (minified) | 10,285 | 6,484 | 7,618 | 1.35× |
| Twitter Small (prettified) | 14,564 | 4,022 | 555 | 3.62× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,071 | 2,540 | 1,859 | 1.21× |
| Prettify | 3,927 | 3,198 | 2,410 | 1.23× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,564 | 880 | 2,691 | 1.32× |
| Double | 920 | 707 | 722 | 1.27× |
| Int64 | 5,455 | 2,593 | 3,632 | 1.50× |
| String | 9,149 | 5,268 | 11,003 | 0.83× |
| Uint64 | 5,414 | 2,821 | 3,622 | 1.49× |
| Apache Builds (minified) | 13,314 | 6,535 | 9,630 | 1.38× |
| Canada (minified) | 1,349 | 1,876 | 862 | 0.72× |
| Apache Builds (prettified) | 14,372 | 5,427 | 388 | 2.65× |
| Canada (prettified) | 7,775 | 4,093 | 312 | 1.90× |
| CitmCatalog (minified) | 8,684 | 2,343 | 8,159 | 1.06× |
| CitmCatalog (prettified) | 20,570 | 4,626 | 356 | 4.45× |
| Discord (minified) | 13,416 | 3,253 | 10,695 | 1.25× |
| Discord (prettified) | 15,453 | 2,791 | 368 | 5.54× |
| Github Events (minified) | 17,298 | 4,790 | 12,580 | 1.38× |
| Google Maps Response (minified) | 11,605 | 2,019 | 8,965 | 1.29× |
| Github Events (prettified) | 17,763 | 4,253 | 444 | 4.18× |
| Google Maps Response (prettified) | 22,917 | 3,295 | 340 | 6.96× |
| Instruments (minified) | 15,418 | 2,138 | 8,744 | 1.76× |
| Instruments (prettified) | 18,427 | 2,671 | 353 | 6.90× |
| Marine IK Reverse (minified) | 1,402 | 944 | 807 | 1.49× |
| Marine IK Reverse (prettified) | 6,856 | 3,445 | 313 | 1.99× |
| Marine IK (minified) | 1,403 | 940 | 828 | 1.49× |
| Marine IK (prettified) | 7,030 | 3,431 | 316 | 2.05× |
| Random (minified) | 12,582 | 2,548 | 8,668 | 1.45× |
| Random (prettified) | 15,894 | 2,939 | 352 | 5.41× |
| Twitter (minified) | 16,437 | 4,414 | 12,224 | 1.34× |
| Twitter (prettified) | 17,614 | 4,625 | 437 | 3.81× |
| Apache Builds Small (minified) | 11,327 | 3,366 | 7,571 | 1.50× |
| Canada Small (minified) | 844 | 1,999 | 958 | 0.42× |
| Apache Builds Small (prettified) | 12,925 | 4,681 | 365 | 2.76× |
| Canada Small (prettified) | 7,874 | 3,730 | 300 | 2.11× |
| CitmCatalog Small (minified) | 10,007 | 2,201 | 8,380 | 1.19× |
| CitmCatalog Small (prettified) | 17,839 | 2,839 | 346 | 6.28× |
| Discord Small (minified) | 14,141 | 2,583 | 10,185 | 1.39× |
| Discord Small (prettified) | 15,079 | 2,659 | 362 | 5.67× |
| Github Events Small (minified) | 14,745 | 3,798 | 11,600 | 1.27× |
| Google Maps Response Small (minified) | 11,383 | 1,599 | 8,762 | 1.30× |
| Github Events Small (prettified) | 17,115 | 3,421 | 396 | 5.00× |
| Google Maps Response Small (prettified) | 21,878 | 3,127 | 335 | 7.00× |
| Instruments Small (minified) | 16,024 | 2,106 | 8,699 | 1.84× |
| Instruments Small (prettified) | 17,567 | 2,596 | 345 | 6.77× |
| Marine IK Reverse Small (minified) | 1,961 | 1,166 | 1,673 | 1.17× |
| Marine IK Reverse Small (prettified) | 9,023 | 3,806 | 323 | 2.37× |
| Marine IK Small (minified) | 1,979 | 1,165 | 1,719 | 1.15× |
| Marine IK Small (prettified) | 9,444 | 3,815 | 324 | 2.48× |
| Random Small (minified) | 12,524 | 2,549 | 9,959 | 1.26× |
| Random Small (prettified) | 16,518 | 2,970 | 345 | 5.56× |
| Twitter Small (minified) | 14,272 | 3,992 | 10,430 | 1.37× |
| Twitter Small (prettified) | 17,783 | 4,031 | 417 | 4.41× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,472 | 1,676 | 1,237 | 1.47× |
| Prettify | 3,324 | 2,384 | 1,659 | 1.39× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 999 | 764 | 1.31× |
| Double | 906 | 716 | 1.27× |
| Int64 | 3,424 | 2,194 | 1.56× |
| String | 7,844 | 4,277 | 1.83× |
| Uint64 | 3,430 | 2,247 | 1.53× |
| Apache Builds (minified) | 12,480 | 4,322 | 2.89× |
| Canada (minified) | 2,989 | 1,983 | 1.51× |
| Apache Builds (prettified) | 12,369 | 3,985 | 3.10× |
| Canada (prettified) | 7,228 | 4,033 | 1.79× |
| CitmCatalog (minified) | 7,694 | 2,014 | 3.82× |
| CitmCatalog (prettified) | 13,389 | 4,757 | 2.81× |
| Discord (minified) | 13,328 | 3,893 | 3.42× |
| Discord (prettified) | 13,808 | 2,994 | 4.61× |
| Github Events (minified) | 16,082 | 6,192 | 2.60× |
| Google Maps Response (minified) | 8,284 | 2,626 | 3.15× |
| Github Events (prettified) | 13,990 | 4,943 | 2.83× |
| Google Maps Response (prettified) | 14,327 | 4,202 | 3.41× |
| Instruments (minified) | 13,527 | 2,338 | 5.79× |
| Instruments (prettified) | 16,227 | 3,058 | 5.31× |
| Marine IK Reverse (minified) | 1,324 | 787 | 1.68× |
| Marine IK Reverse (prettified) | 5,585 | 2,876 | 1.94× |
| Marine IK (minified) | 1,482 | 880 | 1.68× |
| Marine IK (prettified) | 5,544 | 3,461 | 1.60× |
| Random (minified) | 9,234 | 3,004 | 3.07× |
| Random (prettified) | 13,702 | 3,602 | 3.80× |
| Twitter (minified) | 15,593 | 5,041 | 3.09× |
| Twitter (prettified) | 14,337 | 4,219 | 3.40× |
| Apache Builds Small (minified) | 9,223 | 3,095 | 2.98× |
| Canada Small (minified) | 2,590 | 1,754 | 1.48× |
| Apache Builds Small (prettified) | 10,217 | 3,588 | 2.85× |
| Canada Small (prettified) | 6,649 | 3,700 | 1.80× |
| CitmCatalog Small (minified) | 8,744 | 2,094 | 4.18× |
| CitmCatalog Small (prettified) | 12,777 | 2,995 | 4.27× |
| Discord Small (minified) | 12,343 | 3,720 | 3.32× |
| Discord Small (prettified) | 13,222 | 3,413 | 3.87× |
| Github Events Small (minified) | 12,608 | 4,615 | 2.73× |
| Google Maps Response Small (minified) | 9,509 | 2,517 | 3.78× |
| Github Events Small (prettified) | 13,810 | 3,643 | 3.79× |
| Google Maps Response Small (prettified) | 14,312 | 4,008 | 3.57× |
| Instruments Small (minified) | 14,680 | 2,015 | 7.29× |
| Instruments Small (prettified) | 15,366 | 3,560 | 4.32× |
| Marine IK Reverse Small (minified) | 1,562 | 1,085 | 1.44× |
| Marine IK Reverse Small (prettified) | 4,970 | 3,333 | 1.49× |
| Marine IK Small (minified) | 1,579 | 1,022 | 1.54× |
| Marine IK Small (prettified) | 6,082 | 3,657 | 1.66× |
| Random Small (minified) | 11,342 | 3,402 | 3.33× |
| Random Small (prettified) | 13,124 | 3,633 | 3.61× |
| Twitter Small (minified) | 10,563 | 4,201 | 2.51× |
| Twitter Small (prettified) | 11,066 | 4,005 | 2.76× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,414 | 1,474 | 1,093 | 1.64× |
| Prettify | 2,784 | 2,042 | 1,639 | 1.36× |

### Losses and ties

The seven freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,519 vs Jsonifier's 3,995 MB/s (0.88×). Reused, Jsonifier wins by 3.13× (16,027 vs 5,124).
- **Prettify, Windows / MSVC.** Glaze runs 2,600 vs 2,460 MB/s (0.95×), with simdjson On Demand at 1,289. Reused, Jsonifier wins by 1.57× (3,919 vs 2,500).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,496 vs 1,270 MB/s (0.85×), with Glaze at 1,205. It also wins reused, 1,515 vs 1,312 MB/s (0.87×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,516 vs 1,385 MB/s (0.91×), with Glaze at 1,179. It also wins reused, 1,550 vs 1,426 MB/s (0.92×).
- **Double, macOS / GCC.** The reflection writer runs 316 vs 292 MB/s (0.92×), with Glaze at 220. Reused, Jsonifier wins by 1.27× (920 vs 722).
- **Canada (minified), macOS / GCC.**- **Canada (minified), macOS / GCC.** Glaze runs 1,804 vs 1,327 MB/s (0.74×), with the reflection writer at 861. Glaze also wins reused, 1,876 vs 1,349 MB/s (0.72×).
- **Canada Small (minified), macOS / GCC.** Glaze runs 1,736 vs 843 MB/s (0.49×), and the reflection writer is also ahead at 948. Both stay ahead reused: Glaze at 1,999 and the reflection writer at 958, against 844 MB/s (0.42×).

The one freshly allocated tie is Uint64 on macOS / GCC against the reflection writer (829 vs 827 MB/s, Glaze at 583), which Jsonifier wins reused by 1.49× (5,414 vs 3,622).

Two reused non-wins have no freshly allocated counterpart, both against the reflection writer: Bool on Linux / GCC, which wins freshly allocated by 6.73× (1,703 vs 253 MB/s) and loses reused (0.98×), and String on macOS / GCC, which wins freshly allocated (1,491 MB/s against the reflection writer's 1,318 and Glaze's 1,158) and loses reused (0.83×).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.07–2.03× and prettified documents by 16–60×; reused, by 1.06–2.06× and 15–67×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 41 of the 44 Small document tests in both runs. It loses the two minified Marine IK Small tests on Linux / GCC and minified Canada Small on macOS / GCC each time. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.21–1.78× freshly allocated and 1.21–1.64× reused. It wins Prettify on all five reused, by 1.23–1.57×, and on four of five freshly allocated, by 1.24–1.46×; the exception is Windows / MSVC, where Glaze runs 2,600 vs 2,460 MB/s (0.95×). Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,491 MB/s on macOS / GCC (1.29× Glaze's 1,158, ahead of the reflection writer's 1,318) and 1,344 MB/s on macOS / Clang (1.17× Glaze's 1,150). The AVX2 builds run the same test at 1,468–4,194 MB/s and lead Glaze by 1.63× (Windows / MSVC), 2.12× (Linux / Clang) and 2.13× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.83× on macOS / Clang (7,844 vs 4,277 MB/s) and 1.74× on macOS / GCC (9,149 vs 5,268), against 1.82–3.05× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 11,003 MB/s (0.83×), a significant loss.

So most of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: reused, both M2 Pro leads over Glaze sit at or just below the bottom of the AVX2 range. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Only freshly allocated on macOS / GCC, where the reflection writer runs 316 vs 292 MB/s (0.92×). Freshly allocated on Linux / GCC, Jsonifier edges it, 348 vs 337 MB/s (1.03×). Reused, Jsonifier leads on both: 877 vs 640 MB/s on Linux / GCC (1.37×) and 920 vs 722 MB/s on macOS / GCC (1.27×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
