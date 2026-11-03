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

On the Bool test this writer leads Glaze by 10.67–17.25× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.33× to 4.58×.

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

Jsonifier was fastest in 250 of the 255 write tests on five builds, tied 1 and lost 4. With the output string reused across iterations it was fastest in 253 of 255, tied 1 and lost 1. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify and Prettify on every build in both runs, and every full-size document test except minified Twitter on Windows / MSVC freshly allocated. Freshly allocated, that Twitter result is the only loss to Glaze; the other three are against simdjson's reflection writer, which exists only on GCC: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and Double on macOS / GCC. The freshly allocated tie is minified Github Events Small on macOS / GCC against the reflection writer. Reused, the one loss is minified Marine IK Small on Linux / GCC, again against the reflection writer, and the one tie is minified Marine IK Reverse Small on the same build. Linux / Clang and macOS / Clang win all 51 tests in both runs, and Windows / MSVC and macOS / GCC win all 51 reused. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.47–11.45× across the five builds and the reflection writer by 16–60× on the two GCC builds; Small documents lead them by 1.47–10.62× and 12.20–66.93×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 2, 2026, with Jsonifier [2773d05](https://github.com/nihilai-collective/jsonifier/commit/2773d05), Glaze [03a1bab](https://github.com/stephenberry/glaze/commit/03a1bab), simdjson [171066d](https://github.com/simdjson/simdjson/commit/171066d) and BenchmarkSuite [79cbad3](https://github.com/nihilai-collective/benchmarksuite/commit/79cbad3) on every build. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. A tie is therefore usually at the top; the one exception in this sweep is described under *Reused output strings*.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 1 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **250** | **1** | **4** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,221 | 94 | 12.95× |
| Double | 445 | 151 | 2.95× |
| Int64 | 516 | 378 | 1.36× |
| String | 1,444 | 906 | 1.59× |
| Uint64 | 538 | 383 | 1.40× |
| Apache Builds (minified) | 11,700 | 4,372 | 2.68× |
| Canada (minified) | 1,329 | 932 | 1.43× |
| Apache Builds (prettified) | 14,158 | 4,729 | 2.99× |
| Canada (prettified) | 3,075 | 1,559 | 1.97× |
| CitmCatalog (minified) | 8,555 | 4,457 | 1.92× |
| CitmCatalog (prettified) | 4,554 | 1,902 | 2.39× |
| Discord (minified) | 10,882 | 4,280 | 2.54× |
| Discord (prettified) | 13,523 | 4,032 | 3.35× |
| Github Events (minified) | 11,819 | 5,122 | 2.31× |
| Google Maps Response (minified) | 9,248 | 3,450 | 2.68× |
| Github Events (prettified) | 13,614 | 3,380 | 4.03× |
| Google Maps Response (prettified) | 14,033 | 5,139 | 2.73× |
| Instruments (minified) | 12,949 | 4,388 | 2.95× |
| Instruments (prettified) | 18,461 | 4,145 | 4.45× |
| Marine IK Reverse (minified) | 901 | 736 | 1.22× |
| Marine IK Reverse (prettified) | 3,258 | 1,988 | 1.64× |
| Marine IK (minified) | 915 | 629 | 1.46× |
| Marine IK (prettified) | 3,062 | 1,342 (n/c) | 2.28× |
| Random (minified) | 4,082 | 3,027 | 1.35× |
| Random (prettified) | 4,041 | 2,207 | 1.83× |
| Twitter (minified) | 3,687 | 4,802 | 0.77× |
| Twitter (prettified) | 4,134 | 1,736 | 2.38× |
| Apache Builds Small (minified) | 9,941 | 3,651 | 2.72× |
| Canada Small (minified) | 1,835 | 1,185 | 1.55× |
| Apache Builds Small (prettified) | 12,728 | 3,943 | 3.23× |
| Canada Small (prettified) | 4,792 | 3,036 | 1.58× |
| CitmCatalog Small (minified) | 6,739 | 3,248 | 2.07× |
| CitmCatalog Small (prettified) | 11,191 | 4,077 | 2.74× |
| Discord Small (minified) | 9,402 | 3,239 | 2.90× |
| Discord Small (prettified) | 12,096 | 3,111 | 3.89× |
| Github Events Small (minified) | 9,746 | 4,242 | 2.30× |
| Google Maps Response Small (minified) | 9,145 | 3,477 | 2.63× |
| Github Events Small (prettified) | 12,637 | 3,937 | 3.21× |
| Google Maps Response Small (prettified) | 15,838 | 4,738 | 3.34× |
| Instruments Small (minified) | 10,766 | 3,544 | 3.04× |
| Instruments Small (prettified) | 16,697 | 3,338 | 5.00× |
| Marine IK Reverse Small (minified) | 1,527 | 967 | 1.58× |
| Marine IK Reverse Small (prettified) | 6,589 | 2,937 | 2.24× |
| Marine IK Small (minified) | 1,509 | 970 | 1.56× |
| Marine IK Small (prettified) | 6,059 | 2,781 | 2.18× |
| Random Small (minified) | 9,033 | 3,415 | 2.64× |
| Random Small (prettified) | 13,900 | 4,015 | 3.46× |
| Twitter Small (minified) | 10,053 | 4,500 (n/c) | 2.23× |
| Twitter Small (prettified) | 10,715 | 3,481 | 3.08× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,139 | 1,989 | 1,340 | 1.58× |
| Prettify | 2,322 | 2,303 | 1,433 | 1.01× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,526 | 215 | 11.75× |
| Double | 324 | 298 | 1.09× |
| Int64 | 940 | 853 | 1.10× |
| String | 4,298 | 1,997 | 2.15× |
| Uint64 | 914 | 872 | 1.05× |
| Apache Builds (minified) | 16,405 | 5,286 | 3.10× |
| Canada (minified) | 1,950 | 956 | 2.04× |
| Apache Builds (prettified) | 18,992 | 5,405 | 3.51× |
| Canada (prettified) | 4,670 | 3,179 | 1.47× |
| CitmCatalog (minified) | 10,593 | 5,531 | 1.92× |
| CitmCatalog (prettified) | 17,674 | 5,644 | 3.13× |
| Discord (minified) | 14,021 | 6,022 | 2.33× |
| Discord (prettified) | 17,576 | 5,081 | 3.46× |
| Github Events (minified) | 16,687 | 6,335 | 2.63× |
| Google Maps Response (minified) | 11,437 | 3,818 | 3.00× |
| Github Events (prettified) | 19,672 | 4,927 | 3.99× |
| Google Maps Response (prettified) | 18,703 | 5,088 | 3.68× |
| Instruments (minified) | 18,922 | 5,585 | 3.39× |
| Instruments (prettified) | 23,357 | 5,651 | 4.13× |
| Marine IK Reverse (minified) | 1,208 | 605 | 2.00× |
| Marine IK Reverse (prettified) | 5,934 | 3,683 | 1.61× |
| Marine IK (minified) | 1,230 | 598 | 2.06× |
| Marine IK (prettified) | 5,957 | 3,042 | 1.96× |
| Random (minified) | 14,379 | 3,749 | 3.84× |
| Random (prettified) | 18,354 | 4,256 | 4.31× |
| Twitter (minified) | 19,748 | 6,781 | 2.91× |
| Twitter (prettified) | 20,598 | 4,698 | 4.38× |
| Apache Builds Small (minified) | 10,680 | 3,585 | 2.98× |
| Canada Small (minified) | 1,937 | 986 | 1.97× |
| Apache Builds Small (prettified) | 14,370 | 3,762 | 3.82× |
| Canada Small (prettified) | 5,008 | 3,414 | 1.47× |
| CitmCatalog Small (minified) | 7,707 | 2,692 | 2.86× |
| CitmCatalog Small (prettified) | 15,352 | 3,832 | 4.01× |
| Discord Small (minified) | 10,776 | 3,410 | 3.16× |
| Discord Small (prettified) | 14,920 | 3,367 | 4.43× |
| Github Events Small (minified) | 10,559 | 4,294 | 2.46× |
| Google Maps Response Small (minified) | 10,931 | 3,861 | 2.83× |
| Github Events Small (prettified) | 13,292 | 3,728 | 3.57× |
| Google Maps Response Small (prettified) | 21,288 | 4,795 | 4.44× |
| Instruments Small (minified) | 12,970 | 3,455 | 3.75× |
| Instruments Small (prettified) | 22,847 | 4,227 | 5.41× |
| Marine IK Reverse Small (minified) | 1,479 | 802 | 1.84× |
| Marine IK Reverse Small (prettified) | 7,270 | 4,204 | 1.73× |
| Marine IK Small (minified) | 1,635 | 832 | 1.97× |
| Marine IK Small (prettified) | 7,435 | 3,907 | 1.90× |
| Random Small (minified) | 12,718 | 3,893 | 3.27× |
| Random Small (prettified) | 20,729 | 4,157 | 4.99× |
| Twitter Small (minified) | 9,206 | 5,099 | 1.81× |
| Twitter Small (prettified) | 11,829 | 2,766 | 4.28× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,784 | 2,607 | 1,769 | 1.45× |
| Prettify | 4,950 | 3,832 | 2,410 | 1.29× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,127 | 199 | 243 | 8.76× |
| Double | 341 | 305 | 337 | 1.01× |
| Int64 | 971 | 831 | 960 | 1.01× |
| String | 3,895 | 2,032 | 3,655 | 1.07× |
| Uint64 | 948 | 885 | 913 | 1.04× |
| Apache Builds (minified) | 19,796 | 7,941 | 13,309 | 1.49× |
| Canada (minified) | 2,150 | 1,442 | 1,034 | 1.49× |
| Apache Builds (prettified) | 23,275 | 6,869 | 615 | 3.39× |
| Canada (prettified) | 6,115 | 3,166 | 392 | 1.93× |
| CitmCatalog (minified) | 12,890 | 5,955 | 10,177 | 1.27× |
| CitmCatalog (prettified) | 20,347 | 5,969 | 572 | 3.41× |
| Discord (minified) | 15,148 | 6,104 | 11,225 | 1.35× |
| Discord (prettified) | 19,947 | 6,109 | 562 | 3.27× |
| Github Events (minified) | 14,424 | 7,111 | 9,705 | 1.49× |
| Google Maps Response (minified) | 15,462 | 4,252 | 9,892 | 1.56× |
| Github Events (prettified) | 17,831 | 5,205 | 579 | 3.43× |
| Google Maps Response (prettified) | 20,348 | 6,855 | 527 | 2.97× |
| Instruments (minified) | 20,135 | 5,929 | 15,254 | 1.32× |
| Instruments (prettified) | 25,678 | 6,043 | 488 | 4.25× |
| Marine IK Reverse (minified) | 1,282 | 978 | 881 | 1.31× |
| Marine IK Reverse (prettified) | 6,375 | 3,625 | 268 | 1.76× |
| Marine IK (minified) | 1,301 | 951 | 887 | 1.37× |
| Marine IK (prettified) | 6,368 | 2,806 (n/c) | 367 | 2.27× |
| Random (minified) | 15,414 | 4,230 | 12,185 | 1.26× |
| Random (prettified) | 19,240 | 5,066 | 535 | 3.80× |
| Twitter (minified) | 17,370 | 7,810 | 10,061 | 1.73× |
| Twitter (prettified) | 20,764 | 4,840 | 509 | 4.29× |
| Apache Builds Small (minified) | 11,302 | 4,364 | 7,084 | 1.60× |
| Canada Small (minified) | 1,957 | 1,495 | 1,056 | 1.31× |
| Apache Builds Small (prettified) | 12,526 | 3,993 | 604 | 3.14× |
| Canada Small (prettified) | 5,711 | 3,496 | 408 | 1.63× |
| CitmCatalog Small (minified) | 7,492 | 3,393 | 5,364 | 1.40× |
| CitmCatalog Small (prettified) | 14,940 | 3,978 | 524 | 3.76× |
| Discord Small (minified) | 9,569 | 3,978 | 6,250 | 1.53× |
| Discord Small (prettified) | 13,943 | 3,750 | 533 | 3.72× |
| Github Events Small (minified) | 8,268 | 5,066 | 5,631 | 1.47× |
| Google Maps Response Small (minified) | 12,403 | 4,044 | 7,887 | 1.57× |
| Github Events Small (prettified) | 10,898 | 3,859 | 551 | 2.82× |
| Google Maps Response Small (prettified) | 24,026 | 5,840 | 541 | 4.11× |
| Instruments Small (minified) | 12,336 | 3,957 | 7,736 | 1.59× |
| Instruments Small (prettified) | 20,206 | 4,158 | 558 | 4.86× |
| Marine IK Reverse Small (minified) | 1,252 | 1,215 | 1,458 | 0.86× |
| Marine IK Reverse Small (prettified) | 6,458 | 3,859 | 503 | 1.67× |
| Marine IK Small (minified) | 1,298 | 1,230 | 1,465 | 0.89× |
| Marine IK Small (prettified) | 6,409 | 3,925 | 525 | 1.63× |
| Random Small (minified) | 12,426 | 4,217 | 7,977 | 1.56× |
| Random Small (prettified) | 22,179 | 4,733 | 585 | 4.69× |
| Twitter Small (minified) | 8,751 | 5,734 | 6,359 | 1.38× |
| Twitter Small (prettified) | 11,937 | 3,870 | 629 | 3.08× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,276 | 2,443 | 1,796 | 1.34× |
| Prettify | 4,299 | 3,664 | 2,596 | 1.17× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,157 | 125 | 221 | 9.77× |
| Double | 270 | 203 | 270 | 1.00× |
| Int64 | 776 | 535 | 761 | 1.02× |
| String | 1,318 | 1,133 | 1,075 | 1.16× |
| Uint64 | 754 | 486 | 706 | 1.07× |
| Apache Builds (minified) | 12,345 | 5,437 | 8,214 | 1.50× |
| Canada (minified) | 2,163 | 1,505 | 737 | 1.44× |
| Apache Builds (prettified) | 13,514 | 3,780 | 369 | 3.58× |
| Canada (prettified) | 6,732 | 2,996 | 259 | 2.25× |
| CitmCatalog (minified) | 9,223 | 2,218 | 6,541 | 1.41× |
| CitmCatalog (prettified) | 18,823 | 2,965 (n/c) | 316 | 6.35× |
| Discord (minified) | 12,371 | 2,538 | 9,111 | 1.36× |
| Discord (prettified) | 11,491 | 2,202 | 324 | 5.22× |
| Github Events (minified) | 15,800 | 3,749 | 11,433 | 1.38× |
| Google Maps Response (minified) | 11,567 | 1,899 | 8,711 | 1.33× |
| Github Events (prettified) | 13,669 | 2,599 | 412 | 5.26× |
| Google Maps Response (prettified) | 20,084 | 2,929 | 347 | 6.86× |
| Instruments (minified) | 13,510 | 1,874 | 2,330 | 5.80× |
| Instruments (prettified) | 15,143 | 1,940 | 296 | 7.81× |
| Marine IK Reverse (minified) | 1,205 | 900 | 709 | 1.34× |
| Marine IK Reverse (prettified) | 6,475 | 3,094 | 288 | 2.09× |
| Marine IK (minified) | 1,100 | 925 | 734 | 1.19× |
| Marine IK (prettified) | 6,178 | 2,825 | 249 | 2.19× |
| Random (minified) | 11,890 | 1,989 | 6,897 | 1.72× |
| Random (prettified) | 13,400 | 2,116 | 324 | 6.33× |
| Twitter (minified) | 12,688 | 3,780 | 10,675 | 1.19× |
| Twitter (prettified) | 15,508 | 3,363 | 405 | 4.61× |
| Apache Builds Small (minified) | 8,722 | 2,617 | 6,204 | 1.41× |
| Canada Small (minified) | 2,257 | 1,508 | 856 | 1.50× |
| Apache Builds Small (prettified) | 8,449 | 3,718 | 347 | 2.27× |
| Canada Small (prettified) | 7,066 | 3,091 | 240 | 2.29× |
| CitmCatalog Small (minified) | 8,259 | 1,485 | 6,425 | 1.29× |
| CitmCatalog Small (prettified) | 15,042 | 2,180 | 316 | 6.90× |
| Discord Small (minified) | 10,590 | 2,548 | 7,651 | 1.38× |
| Discord Small (prettified) | 11,570 | 1,953 | 351 | 5.92× |
| Github Events Small (minified) | 10,168 (n/c) | 2,480 | 8,862 | 1.15× |
| Google Maps Response Small (minified) | 9,125 | 1,515 | 6,830 (n/c) | 1.34× |
| Github Events Small (prettified) | 11,670 | 2,246 | 370 | 5.20× |
| Google Maps Response Small (prettified) | 19,790 | 2,317 | 296 | 8.54× |
| Instruments Small (minified) | 11,909 | 1,494 | 6,245 | 1.91× |
| Instruments Small (prettified) | 17,071 | 1,809 | 300 | 9.44× |
| Marine IK Reverse Small (minified) | 1,939 | 1,092 | 1,597 | 1.21× |
| Marine IK Reverse Small (prettified) | 6,826 | 3,060 | 243 | 2.23× |
| Marine IK Small (minified) | 1,956 | 1,062 | 1,671 | 1.17× |
| Marine IK Small (prettified) | 8,953 | 3,096 | 323 | 2.89× |
| Random Small (minified) | 10,061 | 1,518 | 7,229 | 1.39× |
| Random Small (prettified) | 15,337 | 2,423 | 339 | 6.33× |
| Twitter Small (minified) | 11,022 | 2,780 | 9,271 | 1.19× |
| Twitter Small (prettified) | 14,912 | 3,087 | 461 | 4.83× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,447 | 1,711 | 1,243 | 1.43× |
| Prettify | 3,135 | 2,303 | 1,340 | 1.36× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,875 | 119 | 15.78× |
| Double | 303 | 218 | 1.39× |
| Int64 | 3,280 | 538 | 6.10× |
| String | 1,292 | 1,027 | 1.26× |
| Uint64 | 3,164 | 526 | 6.01× |
| Apache Builds (minified) | 10,134 | 3,168 | 3.20× |
| Canada (minified) | 2,487 | 1,662 | 1.50× |
| Apache Builds (prettified) | 13,977 | 3,069 | 4.55× |
| Canada (prettified) | 7,097 | 3,243 | 2.19× |
| CitmCatalog (minified) | 7,055 | 1,634 | 4.32× |
| CitmCatalog (prettified) | 17,846 | 2,723 | 6.55× |
| Discord (minified) | 10,851 | 2,337 | 4.64× |
| Discord (prettified) | 17,836 | 2,441 | 7.31× |
| Github Events (minified) | 14,198 | 3,864 | 3.67× |
| Google Maps Response (minified) | 9,751 | 1,629 | 5.99× |
| Github Events (prettified) | 15,508 | 2,964 | 5.23× |
| Google Maps Response (prettified) | 17,050 | 2,919 | 5.84× |
| Instruments (minified) | 11,856 | 1,897 | 6.25× |
| Instruments (prettified) | 24,509 | 2,141 | 11.45× |
| Marine IK Reverse (minified) | 1,558 | 952 | 1.64× |
| Marine IK Reverse (prettified) | 6,733 | 2,913 (n/c) | 2.31× |
| Marine IK (minified) | 1,374 | 745 | 1.84× |
| Marine IK (prettified) | 7,614 | 2,701 (n/c) | 2.82× |
| Random (minified) | 9,331 | 2,339 | 3.99× |
| Random (prettified) | 14,674 | 2,695 | 5.45× |
| Twitter (minified) | 16,140 | 4,206 | 3.84× |
| Twitter (prettified) | 18,422 | 4,185 | 4.40× |
| Apache Builds Small (minified) | 8,905 | 2,682 | 3.32× |
| Canada Small (minified) | 2,624 | 1,519 | 1.73× |
| Apache Builds Small (prettified) | 11,678 | 3,072 | 3.80× |
| Canada Small (prettified) | 7,348 | 3,245 | 2.26× |
| CitmCatalog Small (minified) | 7,684 | 1,818 | 4.23× |
| CitmCatalog Small (prettified) | 15,707 | 2,434 | 6.45× |
| Discord Small (minified) | 11,038 | 2,451 | 4.50× |
| Discord Small (prettified) | 15,824 | 2,568 | 6.16× |
| Github Events Small (minified) | 11,403 | 2,777 | 4.11× |
| Google Maps Response Small (minified) | 9,319 | 1,813 | 5.14× |
| Github Events Small (prettified) | 12,805 | 2,893 | 4.43× |
| Google Maps Response Small (prettified) | 21,681 | 2,734 | 7.93× |
| Instruments Small (minified) | 13,756 | 1,984 | 6.93× |
| Instruments Small (prettified) | 24,709 | 2,327 | 10.62× |
| Marine IK Reverse Small (minified) | 1,931 | 960 | 2.01× |
| Marine IK Reverse Small (prettified) | 7,105 | 2,662 | 2.67× |
| Marine IK Small (minified) | 1,868 | 950 | 1.97× |
| Marine IK Small (prettified) | 8,155 | 3,093 | 2.64× |
| Random Small (minified) | 10,378 | 1,847 | 5.62× |
| Random Small (prettified) | 13,309 | 2,463 | 5.40× |
| Twitter Small (minified) | 9,922 | 3,113 | 3.19× |
| Twitter Small (prettified) | 9,679 | 3,111 | 3.11× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,367 | 1,741 | 1,045 | 1.36× |
| Prettify | 3,319 | 2,310 | 1,507 | 1.44× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 253 of the 255 write tests, ties 1 and loses 1, both on minified Small documents on Linux / GCC. The loss is Marine IK Small, where the reflection writer runs 1,495 vs 1,370 MB/s (0.92×). The tie is Marine IK Reverse Small, and it is not a tie for first: the reflection writer is ahead, 1,480 vs 1,313 MB/s (0.89×), and Jsonifier is scored a tie because Glaze, at 1,308 MB/s, is statistically level with it for second.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 1 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **253** | **1** | **1** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,802 | 292 | 6.17× |
| Double | 455 | 386 | 1.18× |
| Int64 | 4,547 | 1,079 | 4.21× |
| String | 7,483 | 2,310 | 3.24× |
| Uint64 | 4,972 | 1,087 | 4.58× |
| Apache Builds (minified) | 12,498 | 4,940 | 2.53× |
| Canada (minified) | 1,610 | 1,284 | 1.25× |
| Apache Builds (prettified) | 14,495 | 5,251 | 2.76× |
| Canada (prettified) | 4,767 | 3,371 | 1.41× |
| CitmCatalog (minified) | 8,677 | 4,968 | 1.75× |
| CitmCatalog (prettified) | 14,880 | 7,435 | 2.00× |
| Discord (minified) | 11,326 | 4,603 | 2.46× |
| Discord (prettified) | 13,781 | 4,532 | 3.04× |
| Github Events (minified) | 12,399 | 5,934 | 2.09× |
| Google Maps Response (minified) | 10,418 | 3,926 | 2.65× |
| Github Events (prettified) | 14,136 | 3,557 | 3.97× |
| Google Maps Response (prettified) | 15,086 | 6,015 | 2.51× |
| Instruments (minified) | 13,747 | 4,759 | 2.89× |
| Instruments (prettified) | 18,870 | 4,965 | 3.80× |
| Marine IK Reverse (minified) | 1,015 | 797 | 1.27× |
| Marine IK Reverse (prettified) | 5,017 | 3,556 | 1.41× |
| Marine IK (minified) | 1,036 | 802 | 1.29× |
| Marine IK (prettified) | 5,049 | 3,545 | 1.42× |
| Random (minified) | 11,246 | 3,252 | 3.46× |
| Random (prettified) | 14,271 | 4,297 | 3.32× |
| Twitter (minified) | 14,998 | 5,361 | 2.80× |
| Twitter (prettified) | 15,260 | 3,286 | 4.64× |
| Apache Builds Small (minified) | 11,285 | 4,190 | 2.69× |
| Canada Small (minified) | 1,873 | 1,299 | 1.44× |
| Apache Builds Small (prettified) | 14,175 | 4,561 | 3.11× |
| Canada Small (prettified) | 5,048 | 3,464 | 1.46× |
| CitmCatalog Small (minified) | 7,745 | 4,273 | 1.81× |
| CitmCatalog Small (prettified) | 13,796 | 5,083 | 2.71× |
| Discord Small (minified) | 10,541 | 4,319 | 2.44× |
| Discord Small (prettified) | 13,219 | 3,892 | 3.40× |
| Github Events Small (minified) | 12,735 | 5,343 | 2.38× |
| Google Maps Response Small (minified) | 9,524 | 3,928 | 2.42× |
| Github Events Small (prettified) | 15,731 | 5,145 | 3.06× |
| Google Maps Response Small (prettified) | 19,727 | 5,021 | 3.93× |
| Instruments Small (minified) | 12,156 | 4,947 | 2.46× |
| Instruments Small (prettified) | 18,603 | 3,810 (n/c) | 4.88× |
| Marine IK Reverse Small (minified) | 1,628 | 1,056 | 1.54× |
| Marine IK Reverse Small (prettified) | 7,076 | 3,612 | 1.96× |
| Marine IK Small (minified) | 1,593 | 1,060 | 1.50× |
| Marine IK Small (prettified) | 6,812 | 3,651 | 1.87× |
| Random Small (minified) | 11,664 | 4,143 | 2.82× |
| Random Small (prettified) | 18,193 | 4,720 | 3.85× |
| Twitter Small (minified) | 11,992 | 5,380 | 2.23× |
| Twitter Small (prettified) | 12,453 | 4,550 | 2.74× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,160 | 1,994 | 1,340 | 1.58× |
| Prettify | 3,795 | 2,305 | 1,412 | 1.65× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,834 | 871 | 5.55× |
| Double | 812 | 561 | 1.45× |
| Int64 | 5,434 | 2,219 | 2.45× |
| String | 11,570 | 5,331 | 2.17× |
| Uint64 | 5,348 | 2,421 | 2.21× |
| Apache Builds (minified) | 16,561 | 5,959 | 2.78× |
| Canada (minified) | 1,942 | 988 | 1.97× |
| Apache Builds (prettified) | 19,415 | 5,925 | 3.28× |
| Canada (prettified) | 4,672 | 3,923 | 1.19× |
| CitmCatalog (minified) | 10,546 | 6,432 | 1.64× |
| CitmCatalog (prettified) | 17,828 | 7,658 | 2.33× |
| Discord (minified) | 14,043 | 6,407 | 2.19× |
| Discord (prettified) | 18,006 | 5,760 | 3.13× |
| Github Events (minified) | 17,214 | 7,784 | 2.21× |
| Google Maps Response (minified) | 12,377 | 4,423 | 2.80× |
| Github Events (prettified) | 20,128 | 6,106 | 3.30× |
| Google Maps Response (prettified) | 19,578 | 5,657 | 3.46× |
| Instruments (minified) | 18,960 | 6,393 | 2.97× |
| Instruments (prettified) | 23,272 | 6,629 | 3.51× |
| Marine IK Reverse (minified) | 1,212 | 604 | 2.01× |
| Marine IK Reverse (prettified) | 5,939 | 4,125 | 1.44× |
| Marine IK (minified) | 1,225 | 622 | 1.97× |
| Marine IK (prettified) | 6,024 | 4,054 | 1.49× |
| Random (minified) | 14,505 | 4,194 | 3.46× |
| Random (prettified) | 18,316 | 4,911 | 3.73× |
| Twitter (minified) | 19,882 | 7,737 | 2.57× |
| Twitter (prettified) | 20,719 | 5,620 | 3.69× |
| Apache Builds Small (minified) | 12,242 | 3,928 | 3.12× |
| Canada Small (minified) | 1,956 | 1,003 | 1.95× |
| Apache Builds Small (prettified) | 15,331 | 3,908 | 3.92× |
| Canada Small (prettified) | 5,145 | 3,922 | 1.31× |
| CitmCatalog Small (minified) | 8,630 | 3,345 | 2.58× |
| CitmCatalog Small (prettified) | 17,061 | 4,175 | 4.09× |
| Discord Small (minified) | 11,856 | 4,045 | 2.93× |
| Discord Small (prettified) | 16,200 | 3,487 | 4.65× |
| Github Events Small (minified) | 13,368 | 4,948 | 2.70× |
| Google Maps Response Small (minified) | 11,823 | 4,085 | 2.89× |
| Github Events Small (prettified) | 16,831 | 4,121 | 4.08× |
| Google Maps Response Small (prettified) | 22,995 | 5,529 | 4.16× |
| Instruments Small (minified) | 14,470 | 3,902 | 3.71× |
| Instruments Small (prettified) | 24,547 | 4,771 | 5.15× |
| Marine IK Reverse Small (minified) | 1,541 | 868 | 1.78× |
| Marine IK Reverse Small (prettified) | 7,405 | 4,413 | 1.68× |
| Marine IK Small (minified) | 1,688 | 861 | 1.96× |
| Marine IK Small (prettified) | 8,019 | 4,014 | 2.00× |
| Random Small (minified) | 14,118 | 4,294 | 3.29× |
| Random Small (prettified) | 24,652 | 4,951 | 4.98× |
| Twitter Small (minified) | 10,904 | 5,596 | 1.95× |
| Twitter Small (prettified) | 13,367 | 3,442 | 3.88× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,763 | 2,638 | 1,767 | 1.43× |
| Prettify | 4,954 | 3,818 | 2,365 | 1.30× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,246 | 676 | 3,123 | 1.04× |
| Double | 864 | 701 | 634 | 1.23× |
| Int64 | 5,566 | 2,383 | 4,197 | 1.33× |
| String | 11,447 | 6,110 | 9,436 | 1.21× |
| Uint64 | 6,355 | 2,765 | 4,385 | 1.45× |
| Apache Builds (minified) | 19,940 | 9,323 | 13,185 | 1.51× |
| Canada (minified) | 2,137 | 1,525 | 1,031 | 1.40× |
| Apache Builds (prettified) | 23,424 | 7,916 | 611 | 2.96× |
| Canada (prettified) | 6,120 | 3,828 | 392 | 1.60× |
| CitmCatalog (minified) | 12,912 | 6,944 | 10,146 | 1.27× |
| CitmCatalog (prettified) | 20,438 | 8,337 | 568 | 2.45× |
| Discord (minified) | 15,466 | 6,649 | 11,505 | 1.34× |
| Discord (prettified) | 20,129 | 7,266 | 560 | 2.77× |
| Github Events (minified) | 14,622 | 8,267 | 9,832 | 1.49× |
| Google Maps Response (minified) | 17,418 | 4,920 | 10,146 | 1.72× |
| Github Events (prettified) | 18,143 | 5,770 | 579 | 3.14× |
| Google Maps Response (prettified) | 21,982 | 7,770 | 506 | 2.83× |
| Instruments (minified) | 20,315 | 6,590 | 15,485 | 1.31× |
| Instruments (prettified) | 26,092 | 7,255 | 475 | 3.60× |
| Marine IK Reverse (minified) | 1,277 | 981 | 880 | 1.30× |
| Marine IK Reverse (prettified) | 6,405 | 4,054 | 292 | 1.58× |
| Marine IK (minified) | 1,304 | 1,003 | 885 | 1.30× |
| Marine IK (prettified) | 6,359 | 3,885 | 318 | 1.64× |
| Random (minified) | 15,464 | 4,746 | 12,201 | 1.27× |
| Random (prettified) | 19,341 | 6,205 | 520 | 3.12× |
| Twitter (minified) | 17,435 | 8,849 | 10,075 | 1.73× |
| Twitter (prettified) | 20,924 | 6,497 | 502 | 3.22× |
| Apache Builds Small (minified) | 13,102 | 4,877 | 8,141 | 1.61× |
| Canada Small (minified) | 1,977 | 1,719 | 1,065 | 1.15× |
| Apache Builds Small (prettified) | 17,523 | 4,427 | 603 | 3.96× |
| Canada Small (prettified) | 5,957 | 3,833 | 410 | 1.55× |
| CitmCatalog Small (minified) | 8,521 | 3,882 | 5,743 | 1.48× |
| CitmCatalog Small (prettified) | 16,607 | 4,556 | 524 | 3.64× |
| Discord Small (minified) | 11,749 | 4,617 | 7,160 | 1.64× |
| Discord Small (prettified) | 16,323 | 4,139 | 531 | 3.94× |
| Github Events Small (minified) | 10,071 | 5,966 | 6,465 | 1.56× |
| Google Maps Response Small (minified) | 14,621 | 4,277 | 8,570 | 1.71× |
| Github Events Small (prettified) | 13,474 | 4,687 | 550 | 2.87× |
| Google Maps Response Small (prettified) | 29,475 | 6,999 | 540 | 4.21× |
| Instruments Small (minified) | 14,001 | 4,700 | 8,518 | 1.64× |
| Instruments Small (prettified) | 22,551 | 4,629 | 558 | 4.87× |
| Marine IK Reverse Small (minified) | 1,313 | 1,308 | 1,480 | 0.89× |
| Marine IK Reverse Small (prettified) | 6,598 | 4,256 | 505 | 1.55× |
| Marine IK Small (minified) | 1,370 | 1,282 | 1,495 | 0.92× |
| Marine IK Small (prettified) | 6,667 | 4,142 | 525 | 1.61× |
| Random Small (minified) | 14,808 | 5,037 | 8,897 | 1.66× |
| Random Small (prettified) | 25,326 | 5,923 | 584 | 4.28× |
| Twitter Small (minified) | 10,222 | 6,473 | 7,040 | 1.45× |
| Twitter Small (prettified) | 13,877 | 4,244 | 622 | 3.27× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,250 | 2,429 | 1,797 | 1.34× |
| Prettify | 4,284 | 3,642 | 2,542 | 1.18× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,374 | 808 | 2,734 | 1.23× |
| Double | 809 | 613 | 742 | 1.09× |
| Int64 | 5,307 | 1,844 | 3,119 | 1.70× |
| String | 9,149 | 4,050 | 8,710 | 1.05× |
| Uint64 | 5,280 | 2,085 | 2,666 | 1.98× |
| Apache Builds (minified) | 13,889 | 6,885 | 9,265 | 1.50× |
| Canada (minified) | 2,260 | 1,570 | 736 | 1.44× |
| Apache Builds (prettified) | 13,579 | 4,574 | 363 | 2.97× |
| Canada (prettified) | 6,675 | 3,615 | 277 (n/c) | 1.85× |
| CitmCatalog (minified) | 9,328 | 2,348 | 6,657 | 1.40× |
| CitmCatalog (prettified) | 17,027 | 4,074 | 319 (n/c) | 4.18× |
| Discord (minified) | 12,856 | 2,352 | 8,731 | 1.47× |
| Discord (prettified) | 12,535 | 2,362 | 307 (n/c) | 5.31× |
| Github Events (minified) | 17,000 | 4,137 | 10,163 | 1.67× |
| Google Maps Response (minified) | 12,141 | 1,870 | 9,095 | 1.33× |
| Github Events (prettified) | 14,328 | 3,469 | 382 | 4.13× |
| Google Maps Response (prettified) | 23,724 | 3,275 | 281 | 7.24× |
| Instruments (minified) | 14,521 | 2,061 | 7,234 | 2.01× |
| Instruments (prettified) | 16,661 | 2,085 | 347 | 7.99× |
| Marine IK Reverse (minified) | 1,303 | 857 | 756 | 1.52× |
| Marine IK Reverse (prettified) | 6,580 | 3,224 | 280 | 2.04× |
| Marine IK (minified) | 1,152 | 864 | 707 | 1.33× |
| Marine IK (prettified) | 6,216 | 3,171 | 293 (n/c) | 1.96× |
| Random (minified) | 11,004 | 2,364 | 7,014 | 1.57× |
| Random (prettified) | 15,090 | 2,518 | 329 | 5.99× |
| Twitter (minified) | 13,145 | 4,158 | 12,181 | 1.08× |
| Twitter (prettified) | 13,926 | 3,519 | 424 | 3.96× |
| Apache Builds Small (minified) | 8,926 (n/c) | 2,628 | 6,516 | 1.37× |
| Canada Small (minified) | 2,419 | 1,779 | 867 | 1.36× |
| Apache Builds Small (prettified) | 8,935 | 4,238 | 369 | 2.11× |
| Canada Small (prettified) | 7,439 | 2,808 | 256 | 2.65× |
| CitmCatalog Small (minified) | 8,451 | 1,784 | 6,876 | 1.23× |
| CitmCatalog Small (prettified) | 16,061 | 2,411 | 325 | 6.66× |
| Discord Small (minified) | 10,423 | 2,596 | 9,260 | 1.13× |
| Discord Small (prettified) | 12,306 | 2,445 | 333 | 5.03× |
| Github Events Small (minified) | 13,472 | 3,135 | 10,114 | 1.33× |
| Google Maps Response Small (minified) | 9,446 | 1,818 | 7,651 | 1.23× |
| Github Events Small (prettified) | 14,726 | 2,567 | 378 | 5.74× |
| Google Maps Response Small (prettified) | 18,349 | 2,637 | 284 | 6.96× |
| Instruments Small (minified) | 13,166 | 1,685 | 6,438 | 2.04× |
| Instruments Small (prettified) | 16,340 | 1,899 | 282 | 8.60× |
| Marine IK Reverse Small (minified) | 1,987 | 1,169 | 1,669 | 1.19× |
| Marine IK Reverse Small (prettified) | 6,933 | 3,148 | 308 | 2.20× |
| Marine IK Small (minified) | 1,984 | 1,153 | 1,648 | 1.20× |
| Marine IK Small (prettified) | 9,433 | 3,813 | 328 | 2.47× |
| Random Small (minified) | 12,386 | 1,895 | 7,392 | 1.68× |
| Random Small (prettified) | 16,089 | 2,766 | 342 | 5.82× |
| Twitter Small (minified) | 14,160 | 3,270 | 10,290 | 1.38× |
| Twitter Small (prettified) | 16,718 | 3,820 | 463 | 4.38× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,374 | 1,706 | 1,157 | 1.39× |
| Prettify | 3,053 | 2,336 | 1,305 | 1.31× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,930 | 680 | 4.31× |
| Double | 835 | 680 | 1.23× |
| Int64 | 3,444 | 2,268 | 1.52× |
| String | 7,950 | 4,511 | 1.76× |
| Uint64 | 3,719 | 2,026 | 1.84× |
| Apache Builds (minified) | 12,205 | 3,618 | 3.37× |
| Canada (minified) | 2,634 | 1,653 | 1.59× |
| Apache Builds (prettified) | 15,746 | 3,921 | 4.02× |
| Canada (prettified) | 7,377 | 3,440 | 2.14× |
| CitmCatalog (minified) | 7,753 | 1,861 | 4.17× |
| CitmCatalog (prettified) | 18,474 | 3,004 | 6.15× |
| Discord (minified) | 11,130 | 2,863 | 3.89× |
| Discord (prettified) | 18,826 | 2,654 | 7.09× |
| Github Events (minified) | 14,737 | 4,225 | 3.49× |
| Google Maps Response (minified) | 9,995 | 1,846 | 5.42× |
| Github Events (prettified) | 16,224 | 4,086 | 3.97× |
| Google Maps Response (prettified) | 23,645 | 2,777 | 8.52× |
| Instruments (minified) | 12,447 | 2,091 | 5.95× |
| Instruments (prettified) | 25,144 | 2,382 | 10.56× |
| Marine IK Reverse (minified) | 1,540 | 956 | 1.61× |
| Marine IK Reverse (prettified) | 5,823 | 3,316 | 1.76× |
| Marine IK (minified) | 1,565 | 772 | 2.03× |
| Marine IK (prettified) | 7,578 | 3,549 | 2.14× |
| Random (minified) | 9,891 | 2,274 | 4.35× |
| Random (prettified) | 17,843 | 2,895 | 6.16× |
| Twitter (minified) | 16,313 | 4,631 | 3.52× |
| Twitter (prettified) | 18,526 | 4,782 | 3.87× |
| Apache Builds Small (minified) | 10,254 | 2,714 | 3.78× |
| Canada Small (minified) | 2,663 | 1,815 | 1.47× |
| Apache Builds Small (prettified) | 13,520 | 3,518 | 3.84× |
| Canada Small (prettified) | 7,932 | 3,709 | 2.14× |
| CitmCatalog Small (minified) | 9,139 | 2,116 | 4.32× |
| CitmCatalog Small (prettified) | 18,553 | 2,858 | 6.49× |
| Discord Small (minified) | 13,291 | 2,995 | 4.44× |
| Discord Small (prettified) | 17,216 | 2,770 | 6.22× |
| Github Events Small (minified) | 13,295 | 3,782 | 3.52× |
| Google Maps Response Small (minified) | 10,374 | 1,961 | 5.29× |
| Github Events Small (prettified) | 16,866 | 3,527 | 4.78× |
| Google Maps Response Small (prettified) | 23,512 | 3,154 | 7.45× |
| Instruments Small (minified) | 15,843 | 2,277 | 6.96× |
| Instruments Small (prettified) | 27,763 | 2,217 | 12.52× |
| Marine IK Reverse Small (minified) | 1,957 | 1,112 | 1.76× |
| Marine IK Reverse Small (prettified) | 7,088 | 2,897 | 2.45× |
| Marine IK Small (minified) | 1,508 | 1,175 | 1.28× |
| Marine IK Small (prettified) | 8,682 | 3,031 | 2.86× |
| Random Small (minified) | 10,986 | 2,487 | 4.42× |
| Random Small (prettified) | 12,887 | 2,914 | 4.42× |
| Twitter Small (minified) | 10,960 | 3,761 | 2.91× |
| Twitter Small (prettified) | 11,346 | 4,194 | 2.71× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,405 | 1,616 | 1,081 | 1.49× |
| Prettify | 3,065 | 2,308 | 1,224 (n/c) | 1.33× |

### Losses and ties

The four freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,802 vs Jsonifier's 3,687 MB/s (0.77×). Reused, Jsonifier wins by 2.80× (14,998 vs 5,361).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,465 vs 1,298 MB/s (0.89×). It also wins reused, 1,495 vs 1,370 MB/s (0.92×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,458 vs 1,252 MB/s (0.86×); Glaze is behind Jsonifier at 1,215 MB/s. Reused, the reflection writer is still ahead, 1,480 vs 1,313 MB/s (0.89×), the tie described above.
- **Double, macOS / GCC.** The reflection writer runs 270.2 vs 269.9 MB/s (0.999×), a gap of about 0.1% that Welch's test still resolves. Jsonifier is ahead of Glaze (203 MB/s, 1.33×), and ahead of the reflection writer reused (809 vs 742 MB/s, 1.09×).

Every reused non-win also has a freshly allocated counterpart: no test loses reused that wins freshly allocated.

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.19–5.80× and prettified documents by 16–60×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 41 of the 44 Small document tests freshly allocated and 42 reused, losing only minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and tying minified Github Events Small on macOS / GCC freshly allocated. That tie is despite a 1.15× lead (10,168 vs 8,862 MB/s), because Jsonifier's own result did not converge (11.7% RSE), which leaves the gap short of significance; reused, Jsonifier wins it by 1.33× (13,472 vs 10,114). Why the reflection writer edges ahead on the two Marine IK Small tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.34–1.58× freshly allocated and 1.34–1.58× reused, and wins Prettify on all five in both runs, by 1.01–1.44× freshly allocated and 1.18–1.65× reused. Glaze is the next-fastest library in every case. The narrowest margin is freshly allocated Prettify on Windows / MSVC, 2,322 vs 2,303 MB/s; reused, the same test leads by 1.65× (3,795 vs 2,305).

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,318 MB/s on macOS / GCC (1.16× Glaze's 1,133, ahead of the reflection writer's 1,075) and 1,292 MB/s on macOS / Clang (1.26× Glaze's 1,027). The AVX2 builds run the same test at 1,444–4,298 MB/s and lead Glaze by 1.59× (Windows / MSVC), 2.15× (Linux / Clang) and 1.92× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.76× on macOS / Clang (7,950 vs 4,511 MB/s) and 2.26× on macOS / GCC (9,149 vs 4,050), against 1.87–3.24× on the AVX2 builds. On macOS / GCC the reflection writer is behind reused too, at 8,710 MB/s (1.05×).

So most of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: reused, the macOS / GCC lead over Glaze falls inside the AVX2 range, and macOS / Clang sits just below it. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Only freshly allocated on macOS / GCC, and only just: 270.2 vs 269.9 MB/s (0.999×). On Linux / GCC freshly allocated Jsonifier is narrowly ahead (341 vs 337 MB/s, 1.01×), and reused it leads on both GCC builds, by 1.36× (864 vs 634) and 1.09× (809 vs 742). Jsonifier also beats Glaze on Double on all five builds in both runs.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** Every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`, so string-heavy documents can ask for about 6× their output size (see *Does `6n + 2` blow up memory on huge strings?*).
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
