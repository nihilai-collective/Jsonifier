# Jsonifier Serialization: Design and Performance

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

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

The `serialize<options>` dispatcher forces inlining (`JSONIFIER_INLINE`) only for POD values and optionals of them (`inline_contained_v`). Objects, maps and vectors go through a plain `inline` function, so the compiler can keep large container bodies out of line instead of expanding every nested type into its caller. Each registered type gets its own `serialize_impl` specialization: objects, maps, vectors, fixed arrays, tuples, strings, chars, enums, numbers, bools, pointers and optionals, raw JSON, null, and variants (via `visit`).

On x86 the whole serializer is compiled once per instruction-set tier, and `serializeJson` picks the tier at run time. CPUID and XCR0 are read once on first use and the result is cached; every call after that compares a cached enum against the compiled tiers in a fold expression and calls that tier's serializer directly. There are no function pointers or virtual calls, and everything described below is compile-time code inside each tier. ARM builds compile one backend.

## Key techniques

### Size first, then write without bounds checks

`get_size_impl` computes an upper bound for the output before a byte is written. Fixed-width types report a `staticSize` (numbers and enums 32, `bool` 5, `char` 8, null 4), so a `vector<double>` is sized as `n × 32` plus separators with no loop. Strings are sized at `6n + 2`, the worst case where every byte becomes a `\u00XX` escape. Object keys contribute `key.size() + 3` (or `+ 4` when prettified) as a compile-time constant.

`serializeJson` adds 64 bytes of slack and calls `resize_and_overwrite` when the buffer has it, so no zero-fill happens. The write pass then trusts the bound completely: no growth checks, no reallocation, and every writer may overstore (write 8 bytes, advance 4) into the slack.

### Writing through `resize_and_overwrite`

When the output buffer has C++23's `resize_and_overwrite` (detected by the `has_resize_and_overwrite` concept), the whole write pass runs inside it. `std::string::resize_and_overwrite(n, op)` grows the buffer to at least `n` without initializing it, calls `op(data, n)`, and sets the final size to whatever `op` returns. One call replaces the usual three steps: zero-filling resize, write, shrinking resize.

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

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD step, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

### Schema text as packed integers

`packed_blitter` turns `"key":` into one constant at compile time. Literals up to 8 bytes become a single 2, 4 or 8-byte integer; longer ones become an array of `uint64_t` rounded up to 16 or a multiple of 32. Writing a member header is one `pow2MemcpyWrapper` store and a pointer bump by the true length.

### Member iteration: CABERIHT

Object members are walked with CABERIHT, Constexpr Aggregated Bases for Efficient Runtime Iteration of Heterogeneous Types. It is CAFBERIHT without the filter; the full pattern, with compile-time filtering, is described in [its own paper](https://nihilai-collective.net/cafberiht). Each member becomes a base class, and one fold expression over those bases writes the whole object in declaration order.

1. **One base per member.** `get_serialize_base` expands an index sequence over the `core<T>::parseValue` tuple and wraps each entry in `json_entity_serialize<options, entity>`. Each base carries its member's `name`, `memberPtr` and `isItLast` as compile-time constants.
2. **Aggregate the bases.** `serialize_map<bases...>` inherits from every one of them, so a type with 12 members gets one type with 12 bases.
3. **Fold over them.** `iterateValues` is a single fold, `((bufferPtr = bases::processIndex(value, bufferPtr, indent)), ...)`. Each `processIndex` is a static function of its own base, so every call is direct and resolved at compile time.

Inside each `processIndex`, nothing about the member is decided at runtime:

- the header `"name":` is that base's own `packed_blitter` constant;
- the value type is known, so `serialize<options>::impl` picks its writer with no dispatch;
- the trailing comma comes from `if constexpr (!isItLast)`, not an index check.

The sizing pass uses the same pattern: `size_getter_map` aggregates `json_entity_size` bases and folds `processIndex` over them, so both passes walk members the same way.

The result is no virtual calls, no `std::tuple` `get<I>` recursion, no runtime member index and no loop over members. A whole object becomes straight-line code: a run of fixed stores for headers and commas, with the value writers in between. The CAFBERIHT paper measures this same fold-over-bases dispatch as zero overhead against a hand-written baseline by assembly diff.

That is the distinction from CAFBERIHT. There, the bases are also filtered at compile time by tag, so only the components that match join the aggregate. Jsonifier's serializer keeps every registered member, so it has no filter step: the aggregate is exactly the members in `core<T>`. Runtime key exclusion (`jsonifierExcludedKeys`) is a separate mechanism, a set lookup inside each `processIndex` (see Limitations).

### Copies have a compile-time power-of-2 size

Nearly every copy on the write path has a power-of-2 size fixed at compile time: 2, 4 or 8 bytes. `pow2MemcpyWrapper<size>` takes the size as a template argument and `static_assert`s `std::has_single_bit(size)`, so a copy of any other size will not compile. A `memcpy` of 2, 4 or 8 bytes known at compile time lowers to a single unaligned load and store, with no call, no length branch and no tail loop.

What this does to the generated code:

| Copy | Typical x86-64 | Typical ARM64 |
| --- | --- | --- |
| Runtime length | `call memcpy` (or an inline length dispatch with branches and a tail loop) | `bl memcpy` |
| Compile-time 7 bytes | two overlapping 4-byte moves, or 4 + 2 + 1 | several loads and stores |
| Compile-time 2 / 4 / 8 bytes | one `mov` load, one `mov` store | one `ldrh`/`ldr` and one `strh`/`str` |
| Compile-time 2 / 4 / 8 bytes of constant data | one immediate store, no load (`mov qword ptr [rdi], imm`) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers for the whole member loop.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Inlining and scheduling.** Each copy is one or two instructions, so a whole object's serializer inlines into straight-line code. The compiler can then interleave stores from neighbouring members with the value work between them.

That is why the writers round up and overstore instead of copying exact lengths. A 5-byte `false` is one 8-byte store. A 3-digit group is a 4-byte load followed by a 2-byte and a 1-byte store. A 7-byte key literal is one 8-byte store. The pointer then advances by the true length, and the size pass's slack absorbs the extra bytes.

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through the plain wrapper. That size is still a compile-time constant. The only copies whose length is known only at runtime are off the common path: escape sequences inside strings (only when an escapable byte is found), raw JSON passthrough, and the memset for indents deeper than 8 levels.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test, which serializes individual bools one at a time in a loop, this writer runs at 976–2,435 MB/s freshly allocated on all five builds, 10.4–12.5× Glaze's 90–216 MB/s. simdjson's reflection writer runs 244 MB/s on Linux / GCC and 202 MB/s on macOS / GCC, so the lead over it is 8.7× and 7.1×. With the string reused, the reflection writer closes most of that gap on Linux / GCC (2,980 against Jsonifier's 3,264 MB/s, 1.10×) and passes it on macOS / GCC (2,503 against 2,069 MB/s, 0.83×), which puts most of its freshly allocated cost in allocation rather than in the write.

### Strings: picked by length

`string_serializer::impl` sorts every string into one of three length classes before it touches a byte. In all three the escapable bytes are the same: `"`, `\` and anything below 0x20.

1. **Under 16 bytes: one page-safe vector.** If the source does not sit within 16 bytes of the end of a 4 KiB page, `pageSafeShortImpl` loads a full 16-byte vector, masks its escape bits to the string's true length and, when nothing is set, stores all 16 bytes and advances by the length. A short clean key or value is one load, one test and one store, with no branch on its length. Reading past the string's end cannot fault, because the page check guarantees the 16 bytes are mapped; under ASan the load moves into a `JSONIFIER_NO_SANITIZE_ADDRESS` helper so the sanitizer does not flag it. Near a page end the string falls back to two overlapping 4- or 2-byte loads (`smallImpl`) below 8 bytes, or SWAR on 64-bit words (`swarFinish`) from 8 to 15.
2. **16 to 64 bytes: head and tail.** `mediumImpl` loads one vector from the start of the string and one ending at its last byte, tests both and, when both are clean, stores both. The two overlap in the middle, so any length in the range is covered with no loop and no remainder: 16-byte vectors up to 32 bytes, then 32-byte vectors on AVX2, or two 16-byte head-and-tail pairs over 32-byte halves where the widest register is 16 bytes. A string with any escapable byte returns `false` and takes the general path.
3. **Everything else: copy first, check second.** The general path walks the widest available SIMD width down to the narrowest, storing each block unconditionally and testing it afterwards. A clean block advances the full width; a dirty one advances to the first escapable byte, writes its escape from a 256-entry table and resumes. The last partial block is one 16-byte load ending at the string's last byte (`overlappedFinish`), shifted down to the bytes not yet written.

### Integers: exact length first, then 4 digits per store

Integers go through `to_chars` in `i_to_str.hpp` in three steps.

1. **Width.** In `serialize_impl`, every integer narrower than 64 bits is widened to `uint64_t` or `int64_t`, so one writer per signedness covers all eight integer types.
2. **Sign.** A negative value writes `'-'` and then takes its absolute value without a branch: `(v ^ (v >> 63)) - (v >> 63)` on the unsigned type. Because the result is unsigned, `INT64_MIN` works too.
3. **Length.** A balanced tree of comparisons against powers of ten picks the exact digit count, 1 to 20, in at most 5 compares. Each count has its own fixed writer (`to_chars_internal<v_type, N>`), so the writer knows at compile time where every digit goes and never reverses or shifts its output.

The writers use three lookup tables built at compile time: 100 two-digit entries, 1,000 three-digit entries and 10,000 four-digit entries, each digit group pre-packed as characters. Writing four digits is one 4-byte copy from the 10,000-entry table.

Divisions are replaced by multiplies:

- **Divide by 10^4** inside an 8-digit block: multiply by 3518437209 and shift right 45.
- **Divide by 10^8** for longer values: take the high bits of a 128-bit product with 12379400392853802749 and shift right 90 in total. GCC and Clang use `__uint128_t`, MSVC uses `_umul128`, and other compilers fall back to a portable `mulhi`.

So a 20-digit `uint64_t` is two 10^8 splits, four 10^4 splits and five 4-byte stores, with no loop and no per-digit work. A 3-digit group, as at the front of a 19-digit value, is one 4-byte table load followed by a 2-byte and a 1-byte store, so nothing is written past the 3 digits.

On the Int64 and Uint64 tests, which serialize individual values one at a time in a loop, Jsonifier is ahead of Glaze on every build that converged them freshly allocated: Int64 by 1.03× on Linux / Clang, 1.15× on Linux / GCC, 1.36× on MSVC and 5.98× on M1 Clang, and Uint64 by 1.08×, 1.10×, 1.40× and 5.43× on the same builds. Neither test converged freshly allocated on M1 GCC. On Linux / GCC simdjson's reflection writer is level with it freshly allocated on Int64 (970 against 968 MB/s, a tie) while Jsonifier takes Uint64 by 1.08×. With the string reused, Jsonifier is ahead of the fastest other library on both tests on every build, by 1.19× (Int64, M1 GCC) to 3.79× (Uint64, MSVC).

### Floats

Doubles go through `zmij::detail::write`, a shortest-round-trip formatter vendored in `zmij.hpp`.

## Minified and prettified output

`serialize_options` carries `prettify`, `indentSize` (default 3) and `indentChar` (default space) as template parameters, so the two modes compile to separate code with no runtime mode check.

| Piece | Minified | Prettified |
| --- | --- | --- |
| Member header | `"key":` packed store | `"key": ` packed store |
| Separator | one `,` byte | `indent_table<",\n">` blit |
| Open / close | one `{` / `}` byte | `"{\n"` + indent, `"\n"` + indent, then `}` |
| Size estimate per member | `key + 3` | `key + 4`, plus `2 + indent` per separator |

`indent_table` prebuilds, at compile time, the prefix (`,\n`, `{\n` or `\n`) followed by 8 levels of indent characters as a packed `uint64_t` array. A newline plus indent is a short run of 8-byte stores from that table, rounded up into the slack. Nesting deeper than 8 levels falls back to `memset` for the remainder, marked `[[unlikely]]`.

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in *Two Stages, On Demand*.

## Performance results

Jsonifier was fastest in 112 of the 120 converged serialization tests on five builds, tied 1 and lost 7. With the output string reused across iterations it was fastest in 116 of 119, with no ties and 3 losses. Every non-win is a single-value test; Jsonifier wins every converged document test on every build in both runs. Seven of the eight freshly allocated non-wins, and all three reused losses, are against simdjson's reflection writer, which exists only on GCC: Double and Int64 are losses and Uint64 a tie on Linux / GCC, and Double, Int64, String and Uint64 are losses on macOS / GCC. The eighth is Uint64 on Linux / Clang, where Glaze runs 894 MB/s against Jsonifier's 890; it is the only test in either run in which Jsonifier is behind Glaze. Reused, the reflection writer takes Bool on both GCC builds and String on macOS / GCC. Windows / MSVC and macOS / Clang win all of their tests in both runs, and Linux / Clang wins all 25 reused. Prettified documents are where the lead is widest: 1.37–9.97× over Glaze freshly allocated across the five builds, and 26–59× over the reflection writer on the two GCC builds.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 10, 2026, with Glaze [e194d23](https://github.com/stephenberry/glaze/commit/e194d23), simdjson [7f6f8dc](https://github.com/simdjson/simdjson/commit/7f6f8dc) and BenchmarkSuite [6196208](https://github.com/nihilai-collective/benchmarksuite/commit/6196208) on every build. Jsonifier is [13785b6](https://github.com/nihilai-collective/jsonifier/commit/13785b6) on Windows / MSVC and both macOS builds, and [5aa6104](https://github.com/nihilai-collective/jsonifier/commit/5aa6104) on both Linux builds. Every test runs twice: once with a freshly allocated output string per iteration, and once with a reused one (see Method). Sampling and tie rules are listed under Method below and match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, and its prettified results appear in the tables at a small fraction of the minified throughput. The Minify and Prettify tests reformat existing text instead of serializing objects, so they are left out of the counts.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M1 (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M1 (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.44 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The sweep's "Small" cut-down documents (at most 5 KiB minified) are not counted here. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M1. A test is ranked only if every library in it converged, and tests that do not converge are left out of every ranking, which is why builds report different test counts.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at the top, and as a loss when another library ranks above it. Win, tie and loss counts use only converged results.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M1 numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 24 | 0 | 1 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 | 20 | 1 | 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 22 | 18 | 0 | 4 |
| macOS / Clang 23.1 (Apple M1, NEON) | 25 | 25 | 0 | 0 |
| **Total** | **120** | **112** | **1** | **7** |

Every converged write test per build follows, in MB/s, freshly allocated run first and reused run after it. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,076 | 80 | 13.41× |
| Double | 190 | 126 | 1.51× |
| Int64 | 506 | 327 | 1.55× |
| String | 1,072 | 807 | 1.33× |
| Uint64 | 538 | 374 | 1.44× |
| Canada (minified) | 1,168 | 793 | 1.47× |
| Canada (prettified) | 2,575 | 1,395 | 1.85× |
| CitmCatalog (minified) | 6,850 | 4,390 | 1.56× |
| CitmCatalog (prettified) | 4,280 | 1,852 | 2.31× |
| Discord (minified) | 6,600 | 4,430 | 1.49× |
| Discord (prettified) | 10,364 | 4,515 | 2.30× |
| Google Maps Response (minified) | 7,218 | 3,290 | 2.19× |
| Google Maps Response (prettified) | 14,219 | 5,468 | 2.60× |
| Instruments (minified) | 7,868 | 4,054 | 1.94× |
| Instruments (prettified) | 12,955 | 4,577 | 2.83× |
| Marine IK Reverse (minified) | 778 | 668 | 1.17× |
| Marine IK Reverse (prettified) | 2,723 | 1,789 | 1.52× |
| Marine IK (minified) | 802 | 585 | 1.37× |
| Marine IK (prettified) | 2,593 | 1,210 | 2.14× |
| Mesh (minified) | 1,241 | 918 | 1.35× |
| Mesh (prettified) | 2,011 | 1,473 | 1.36× |
| Random (minified) | 3,287 | 2,807 | 1.17× |
| Random (prettified) | 4,045 | 2,200 | 1.84× |
| Twitter (minified) | 8,147 | 4,402 | 1.85× |
| Twitter (prettified) | 10,597 | 3,436 | 3.08× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,905 | 209 | 9.11× |
| Double | 330 | 299 | 1.11× |
| Int64 | 925 | 876 | 1.06× |
| String | 4,410 | 1,917 | 2.30× |
| Uint64 | 890 | 894 | 1.00× |
| Canada (minified) | 1,540 | 950 | 1.62× |
| Canada (prettified) | 4,443 | 3,229 | 1.38× |
| CitmCatalog (minified) | 10,326 | 5,658 | 1.83× |
| CitmCatalog (prettified) | 17,944 | 5,727 | 3.13× |
| Discord (minified) | 13,809 | 6,039 | 2.29× |
| Discord (prettified) | 17,587 | 5,089 | 3.46× |
| Google Maps Response (minified) | 11,458 | 4,005 | 2.86× |
| Google Maps Response (prettified) | 18,232 | 5,136 | 3.55× |
| Instruments (minified) | 18,291 | 5,668 | 3.23× |
| Instruments (prettified) | 22,858 | 5,677 | 4.03× |
| Marine IK Reverse (minified) | 893 | 616 | 1.45× |
| Marine IK Reverse (prettified) | 5,513 | 3,692 | 1.49× |
| Marine IK (minified) | 842 | 608 | 1.38× |
| Marine IK (prettified) | 5,978 | 3,150 | 1.90× |
| Mesh (minified) | 1,267 | 1,018 | 1.24× |
| Mesh (prettified) | 4,017 | 1,727 | 2.33× |
| Random (minified) | 14,781 | 3,426 | 4.31× |
| Random (prettified) | 18,818 | 3,949 | 4.76× |
| Twitter (minified) | 19,630 | 6,136 | 3.20× |
| Twitter (prettified) | 22,885 | 5,660 | 4.04× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,769 | 203 | 252 | 7.01× |
| Double | 326 | 293 | 339 | 0.96× |
| Int64 | 909 | 832 | 947 | 0.96× |
| String | 3,757 | 1,998 | 3,722 | 1.01× |
| Uint64 | 869 | 870 | 885 | 0.98× |
| Canada (minified) | 2,055 | 1,438 | 1,033 | 1.43× |
| Canada (prettified) | 5,790 | 3,166 | 178 | 1.83× |
| CitmCatalog (minified) | 12,423 | 6,082 | 11,194 | 1.11× |
| CitmCatalog (prettified) | 18,557 | 5,931 | 545 | 3.13× |
| Discord (minified) | 13,349 | 6,132 | 11,075 | 1.21× |
| Discord (prettified) | 18,381 | 6,216 | 523 | 2.96× |
| Google Maps Response (minified) | 10,916 | 4,174 | 9,888 | 1.10× |
| Google Maps Response (prettified) | 18,419 | 6,923 | 527 | 2.66× |
| Instruments (minified) | 19,236 | 5,499 | 15,950 | 1.21× |
| Instruments (prettified) | 24,700 | 6,140 | 496 | 4.02× |
| Marine IK Reverse (minified) | 1,246 | 1,003 | 884 | 1.24× |
| Marine IK (minified) | 1,257 | 941 | 884 | 1.34× |
| Mesh (minified) | 2,369 | 1,592 | 1,279 | 1.49× |
| Mesh (prettified) | 4,406 | 2,703 | 169 | 1.63× |
| Random (minified) | 13,800 | 3,654 | 11,598 | 1.19× |
| Random (prettified) | 18,436 | 4,838 | 523 | 3.81× |
| Twitter (minified) | 16,717 | 6,770 | 13,016 | 1.28× |
| Twitter (prettified) | 20,526 | 4,864 | 526 | 4.22× |

### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,612 | 113 | 214 | 7.54× |
| Double | 252 | 200 | 292 | 0.87× |
| Int64 | 687 | 501 | 749 | 0.92× |
| String | 1,180 | 1,011 | 1,216 | 0.97× |
| Uint64 | 702 | 510 | 766 | 0.92× |
| Canada (minified) | 2,288 | 1,529 | 770 | 1.50× |
| CitmCatalog (minified) | 8,421 | 1,949 | 7,225 | 1.17× |
| CitmCatalog (prettified) | 19,179 | 3,834 | 323 | 5.00× |
| Discord (minified) | 11,454 | 2,767 | 8,831 | 1.30× |
| Discord (prettified) | 13,111 | 2,497 | 346 | 5.25× |
| Google Maps Response (minified) | 10,165 | 1,757 | 8,126 | 1.25× |
| Google Maps Response (prettified) | 15,719 | 2,805 | 306 | 5.60× |
| Instruments (minified) | 13,782 | 1,866 | 7,687 | 1.79× |
| Instruments (prettified) | 16,998 | 2,212 | 337 | 7.68× |
| Marine IK Reverse (minified) | 1,297 | 818 | 729 | 1.59× |
| Marine IK (minified) | 1,283 | 794 | 741 | 1.62× |
| Mesh (minified) | 2,412 | 1,055 | 931 | 2.29× |
| Mesh (prettified) | 3,839 | 1,507 | 89 | 2.55× |
| Random (minified) | 10,890 | 1,925 | 8,254 | 1.32× |
| Random (prettified) | 13,697 | 2,469 | 324 | 5.55× |
| Twitter (minified) | 11,899 | 3,303 | 8,054 | 1.48× |
| Twitter (prettified) | 15,211 | 3,404 | 414 | 4.47× |

### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,449 | 120 | 12.09× |
| Double | 266 | 198 | 1.34× |
| Int64 | 2,615 | 483 | 5.42× |
| String | 1,088 | 1,044 | 1.04× |
| Uint64 | 2,973 | 493 | 6.02× |
| Canada (minified) | 2,468 | 1,447 | 1.71× |
| Canada (prettified) | 6,614 | 3,030 | 2.18× |
| CitmCatalog (minified) | 7,828 | 1,855 | 4.22× |
| CitmCatalog (prettified) | 18,464 | 2,928 | 6.31× |
| Discord (minified) | 11,025 | 2,566 | 4.30× |
| Discord (prettified) | 16,543 | 2,648 | 6.25× |
| Google Maps Response (minified) | 8,186 | 1,599 | 5.12× |
| Google Maps Response (prettified) | 17,608 | 2,723 | 6.47× |
| Instruments (minified) | 10,926 | 1,423 | 7.68× |
| Instruments (prettified) | 17,324 | 1,738 | 9.97× |
| Marine IK Reverse (minified) | 1,343 | 754 | 1.78× |
| Marine IK Reverse (prettified) | 5,549 | 2,580 | 2.15× |
| Marine IK (minified) | 1,335 | 731 | 1.83× |
| Marine IK (prettified) | 4,815 | 2,655 | 1.81× |
| Mesh (minified) | 2,598 | 1,111 | 2.34× |
| Mesh (prettified) | 5,035 | 1,772 | 2.84× |
| Random (minified) | 8,434 | 1,448 | 5.82× |
| Random (prettified) | 14,715 | 2,324 | 6.33× |
| Twitter (minified) | 9,489 | 2,877 | 3.30× |
| Twitter (prettified) | 17,920 | 3,193 | 5.61× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 116 of the 119 converged write tests, with no ties and three losses, all single-value tests against simdjson's reflection writer: Bool on Linux / GCC, and Bool and String on macOS / GCC. Windows / MSVC and Linux / Clang converge all 25 tests; Linux / GCC converges 24, macOS / Clang 23 and macOS / GCC 22.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 24 | 23 | 0 | 1 |
| macOS / GCC 16.2 (Apple M1, NEON) | 22 | 20 | 0 | 2 |
| macOS / Clang 23.1 (Apple M1, NEON) | 23 | 23 | 0 | 0 |
| **Total** | **119** | **116** | **0** | **3** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,554 | 249 | 6.24× |
| Double | 723 | 322 | 2.24× |
| Int64 | 4,121 | 1,121 | 3.68× |
| String | 7,507 | 2,420 | 3.10× |
| Uint64 | 4,766 | 1,114 | 4.28× |
| Canada (minified) | 1,482 | 1,071 | 1.38× |
| Canada (prettified) | 4,415 | 2,954 | 1.49× |
| CitmCatalog (minified) | 6,843 | 4,885 | 1.40× |
| CitmCatalog (prettified) | 14,033 | 7,458 | 1.88× |
| Discord (minified) | 7,301 | 4,782 | 1.53× |
| Discord (prettified) | 10,821 | 5,092 | 2.13× |
| Google Maps Response (minified) | 7,819 | 3,960 | 1.97× |
| Google Maps Response (prettified) | 15,157 | 6,657 | 2.28× |
| Instruments (minified) | 8,008 | 4,641 | 1.73× |
| Instruments (prettified) | 13,049 | 5,276 | 2.47× |
| Marine IK Reverse (minified) | 903 | 718 | 1.26× |
| Marine IK Reverse (prettified) | 4,606 | 3,277 | 1.41× |
| Marine IK (minified) | 906 | 725 | 1.25× |
| Marine IK (prettified) | 4,348 | 3,215 | 1.35× |
| Mesh (minified) | 1,568 | 1,243 | 1.26× |
| Mesh (prettified) | 2,956 | 2,144 | 1.38× |
| Random (minified) | 7,039 | 2,967 | 2.37× |
| Random (prettified) | 11,461 | 4,335 | 2.64× |
| Twitter (minified) | 8,024 | 5,288 | 1.52× |
| Twitter (prettified) | 10,726 | 4,072 | 2.63× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 3,181 | 931 | 3.42× |
| Double | 838 | 682 | 1.23× |
| Int64 | 4,920 | 2,410 | 2.04× |
| String | 11,196 | 5,576 | 2.01× |
| Uint64 | 5,152 | 2,617 | 1.97× |
| Canada (minified) | 1,543 | 996 | 1.55× |
| Canada (prettified) | 4,488 | 3,858 | 1.16× |
| CitmCatalog (minified) | 10,277 | 6,543 | 1.57× |
| CitmCatalog (prettified) | 18,023 | 7,712 | 2.34× |
| Discord (minified) | 14,047 | 6,217 | 2.26× |
| Discord (prettified) | 17,763 | 5,866 | 3.03× |
| Google Maps Response (minified) | 11,988 | 4,576 | 2.62× |
| Google Maps Response (prettified) | 19,429 | 5,642 | 3.44× |
| Instruments (minified) | 18,412 | 6,474 | 2.84× |
| Instruments (prettified) | 22,955 | 6,701 | 3.43× |
| Marine IK Reverse (minified) | 890 | 614 | 1.45× |
| Marine IK Reverse (prettified) | 5,570 | 4,073 | 1.37× |
| Marine IK (minified) | 845 | 633 | 1.33× |
| Marine IK (prettified) | 6,067 | 4,102 | 1.48× |
| Mesh (minified) | 1,273 | 1,056 | 1.21× |
| Mesh (prettified) | 4,068 | 1,804 | 2.26× |
| Random (minified) | 14,739 | 3,771 | 3.91× |
| Random (prettified) | 18,826 | 4,605 | 4.09× |
| Twitter (minified) | 19,646 | 7,512 | 2.62× |
| Twitter (prettified) | 22,820 | 6,526 | 3.50× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,716 | 670 | 3,105 | 0.87× |
| Double | 843 | 731 | 636 | 1.15× |
| Int64 | 5,250 | 2,439 | 4,113 | 1.28× |
| String | 11,183 | 6,086 | 9,446 | 1.18× |
| Uint64 | 4,456 | 2,755 | 4,348 | 1.02× |
| Canada (minified) | 2,058 | 1,516 | 1,033 | 1.36× |
| Canada (prettified) | 5,895 | 3,800 | 178 | 1.55× |
| CitmCatalog (minified) | 12,434 | 7,146 | 11,181 | 1.11× |
| CitmCatalog (prettified) | 18,599 | 8,291 | 553 | 2.24× |
| Discord (minified) | 13,512 | 6,570 | 11,316 | 1.19× |
| Discord (prettified) | 18,576 | 7,346 | 529 | 2.53× |
| Google Maps Response (minified) | 11,312 | 4,696 | 10,341 | 1.09× |
| Google Maps Response (prettified) | 19,101 | 7,832 | 526 | 2.44× |
| Instruments (minified) | 19,649 | 6,428 | 15,452 | 1.27× |
| Instruments (prettified) | 24,990 | 7,459 | 493 | 3.35× |
| Marine IK Reverse (minified) | 1,242 | 1,015 | 890 | 1.22× |
| Marine IK (minified) | 1,256 | 1,007 | 882 | 1.25× |
| Marine IK (prettified) | 6,115 | 4,013 | 200 | 1.52× |
| Mesh (minified) | 2,396 | 1,707 | 1,280 | 1.40× |
| Mesh (prettified) | 4,366 | 2,769 | 163 | 1.58× |
| Random (minified) | 13,872 | 4,124 | 11,748 | 1.18× |
| Random (prettified) | 18,654 | 5,764 | 531 | 3.24× |
| Twitter (minified) | 16,636 | 7,941 | 12,769 | 1.30× |
| Twitter (prettified) | 20,418 | 5,673 | 529 | 3.60× |

#### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,373 | 795 | 2,764 | 0.86× |
| Double | 814 | 463 | 676 | 1.20× |
| Int64 | 3,682 | 2,090 | 3,013 | 1.22× |
| String | 8,486 | 4,728 | 10,050 | 0.84× |
| Uint64 | 3,559 | 2,363 | 3,053 | 1.17× |
| Canada (minified) | 2,286 | 1,616 | 769 | 1.42× |
| CitmCatalog (minified) | 8,474 | 2,064 | 7,224 | 1.17× |
| CitmCatalog (prettified) | 19,256 | 4,426 | 324 | 4.35× |
| Discord (minified) | 11,955 | 3,104 | 9,041 | 1.32× |
| Discord (prettified) | 13,442 | 2,794 | 346 | 4.81× |
| Google Maps Response (minified) | 10,698 | 1,904 | 8,399 | 1.27× |
| Google Maps Response (prettified) | 17,430 | 3,077 | 306 | 5.67× |
| Instruments (minified) | 14,169 | 2,000 | 7,821 | 1.81× |
| Instruments (prettified) | 17,179 | 2,445 | 335 | 7.03× |
| Marine IK Reverse (minified) | 1,301 | 815 | 733 | 1.60× |
| Marine IK (minified) | 1,283 | 821 | 740 | 1.56× |
| Mesh (minified) | 2,425 | 1,091 | 925 | 2.22× |
| Mesh (prettified) | 3,833 | 1,543 | 88 | 2.49× |
| Random (minified) | 11,038 | 2,057 | 7,906 | 1.40× |
| Random (prettified) | 13,661 | 2,686 | 325 | 5.09× |
| Twitter (minified) | 13,504 | 3,869 | 8,897 | 1.52× |
| Twitter (prettified) | 14,931 | 4,246 | 406 | 3.52× |

#### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,962 | 668 | 2.93× |
| Double | 828 | 626 | 1.32× |
| Int64 | 3,416 | 2,101 | 1.63× |
| String | 6,348 | 4,420 | 1.44× |
| Uint64 | 3,206 | 2,201 | 1.46× |
| Canada (minified) | 2,465 | 1,663 | 1.48× |
| Canada (prettified) | 6,690 | 3,349 | 2.00× |
| CitmCatalog (minified) | 7,944 | 1,953 | 4.07× |
| CitmCatalog (prettified) | 18,661 | 3,257 | 5.73× |
| Discord (minified) | 11,534 | 2,885 | 4.00× |
| Discord (prettified) | 16,721 | 2,991 | 5.59× |
| Google Maps Response (minified) | 8,528 | 1,741 | 4.90× |
| Google Maps Response (prettified) | 19,819 | 2,990 | 6.63× |
| Instruments (prettified) | 18,218 | 1,676 | 10.87× |
| Marine IK Reverse (minified) | 1,476 | 692 | 2.13× |
| Marine IK Reverse (prettified) | 5,514 | 2,757 | 2.00× |
| Marine IK (minified) | 1,353 | 710 | 1.91× |
| Marine IK (prettified) | 3,706 | 3,000 | 1.23× |
| Mesh (minified) | 2,257 | 1,116 | 2.02× |
| Mesh (prettified) | 5,009 | 1,837 | 2.73× |
| Random (minified) | 8,165 | 1,786 | 4.57× |
| Random (prettified) | 14,819 | 2,565 | 5.78× |
| Twitter (prettified) | 16,620 | 4,333 | 3.84× |

### Losses

The prettified lead comes from the indent tables: a newline plus indent is a few 8-byte stores, not a loop. Windows / MSVC and macOS / Clang win every test in both runs, and so does Linux / Clang reused. The eight freshly allocated non-wins:

- **Uint64, Linux / Clang.** Glaze runs 894 vs Jsonifier's 890 MB/s (1.00×), a loss by under 1%. Reused, Jsonifier is ahead, 5,152 vs 2,617 MB/s (1.97×).
- **Double, Linux / GCC.** Jsonifier runs 326 vs the reflection writer's 339 MB/s (0.96×), a loss. It is ahead of Glaze (293 MB/s, 1.11×), and ahead of the reflection writer reused (843 vs 636 MB/s, 1.33×).
- **Int64, Linux / GCC.** Jsonifier runs 909 vs 947 MB/s (0.96×), a loss. It is ahead of Glaze (832 MB/s, 1.09×), and ahead of the reflection writer reused (5,250 vs 4,113 MB/s, 1.28×).
- **Uint64, Linux / GCC.** Jsonifier runs 869 vs 885 MB/s, which the t-test cannot separate, so it is a tie; Glaze runs 870. Reused, Jsonifier is ahead of the reflection writer, 4,456 vs 4,348 MB/s (1.02×).
- **Double, macOS / GCC.** Jsonifier runs 252 vs 292 MB/s (0.87×), a loss. It is ahead of Glaze (200 MB/s, 1.26×), and ahead of the reflection writer reused (814 vs 676 MB/s, 1.20×).
- **Int64, macOS / GCC.** Jsonifier runs 687 vs 749 MB/s (0.92×), a loss. Reused it is ahead, 3,682 vs 3,013 MB/s (1.22×).
- **String, macOS / GCC.** Jsonifier runs 1,180 vs 1,216 MB/s (0.97×), a loss. It is ahead of Glaze (1,011 MB/s, 1.17×).
- **Uint64, macOS / GCC.** Jsonifier runs 702 vs 766 MB/s (0.92×), a loss. Reused it is ahead, 3,559 vs 3,053 MB/s (1.17×).

The three reused losses are single-value tests against the reflection writer: Bool on Linux / GCC, 2,716 vs 3,105 MB/s (0.87×); Bool on macOS / GCC, 2,373 vs 2,764 MB/s (0.86×); and String on macOS / GCC, 8,486 vs 10,050 MB/s (0.84×). Freshly allocated, Jsonifier wins Bool by 7.01× on Linux / GCC (1,769 vs 252 MB/s) and 7.54× on macOS / GCC (1,612 vs 214 MB/s).

Every document is ahead of the reflection writer on both GCC builds, freshly allocated and reused. String on Linux / GCC goes to Jsonifier in both runs, 3,757 vs 3,722 MB/s (1.01×) freshly allocated and 11,183 vs 9,446 (1.18×) reused. Minified documents lead by 1.10–2.97× freshly allocated (1.10× on Google Maps Response on Linux / GCC to 2.97× on Canada on macOS / GCC), and prettified documents by 26–59×. Not profiled yet: why the reflection writer edges ahead on single numeric values when the output string is freshly allocated, and on single Bool and String values when it is reused, is not known, and this paper does not guess at its algorithm.


## Questions from external review

An outside review of a draft raised five concerns. Each is answered here against the code as it stands; where the answer is "not measured yet", it says so.

### Can an overstore run past the buffer?

No, because every overstore lands inside the string's own size, not past it. `serializeJson` resizes the buffer to the full bound plus 64 bytes before writing, so the write pass works inside memory the string already owns. No write ever reaches capacity or the allocator's edge, and ASan sees only in-bounds accesses.

The bound covers the largest overstore of each writer:

| Writer | Largest write beyond its true length | Covered by |
| --- | --- | --- |
| Bool | 3 bytes (8 written, 5 reserved) | 64-byte tail slack |
| Integer or float | none past the 32 bytes reserved per number | `staticSize` of 32 |
| Key literal over 16 bytes | up to 31 bytes (rounded to a multiple of 32) | 64-byte tail slack |
| Indent run | up to 7 bytes (rounded to 8) | 64-byte tail slack |
| String SIMD block | one vector width, stored before the escape check | the `6n + 2` reservation, which the copied bytes never exceed |
| Short string (under 16 bytes) | up to 15 bytes (one 16-byte store) | 64-byte tail slack |

The largest single overstore is 31 bytes, under the 64-byte slack. That slack is a fixed constant, not derived from the widest writer, so a future writer with a wider overstore must raise it.

### Does `6n + 2` blow up memory on huge strings?

Yes, the reservation is real. A 100 MB string field asks for about 600 MB before the final shrink, and the string keeps that capacity afterwards unless the caller calls `shrink_to_fit`. How much of it is touched depends on the buffer:

- With `resize_and_overwrite`, nothing is zero-filled. On Linux and macOS, pages that are never written are usually never faulted in, so the cost is mostly address space. On Windows, the allocation still counts against the commit limit.
- Without `resize_and_overwrite`, `resize` zero-fills the whole bound, so every page is touched.

A two-tier bound, counting escapable bytes in strings above a size threshold before reserving, would cut this to roughly `n` plus the escapes found. It is not implemented, and its cost on typical small strings has not been measured.

### Is it portable to big-endian targets?

The code has big-endian paths, but none are tested. `packed_blitter`, the bool constants, the digit tables and the SWAR string tail each branch on `std::endian::native` at compile time and use `std::byteswap` where needed. The CI matrix covers only little-endian x86 and ARM, so those branches have never run.

### Why is the String test slower on NEON than on AVX2?

The gap is a freshly allocated one. Freshly allocated, Jsonifier runs String at 1,180 MB/s on macOS / GCC (1.17× Glaze's 1,011, 0.97× the reflection writer's 1,216) and 1,088 MB/s on macOS / Clang (1.04× Glaze's 1,044). The AVX2 builds run the same test at 1,072–4,410 MB/s and lead Glaze by 1.33× (Windows / MSVC), 2.30× (Linux / Clang) and 1.88× (Linux / GCC). With the string reused the M1 lead over Glaze opens up to 1.44× on macOS / Clang (6,348 vs 4,420 MB/s) and 1.79× on macOS / GCC (8,486 vs 4,728), against the 1.84–3.10× the AVX2 builds show reused. On macOS / GCC the reflection writer is still ahead reused, at 10,050 MB/s.

So most of the M1 shortfall sits in the part of the freshly allocated run that the reused run removes, the allocation and release of the output string, rather than in the copy loop. That is an inference from the two runs, not a profile. Two features of the general loop remain candidates for whatever is left:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

On both GCC builds it wins the freshly allocated run: 339 vs Jsonifier's 326 MB/s on Linux / GCC (0.96×) and 292 vs 252 on macOS / GCC (0.87×). Reused, Jsonifier is ahead on both, 843 vs 636 MB/s (1.33×) on Linux / GCC and 814 vs 676 (1.20×) on macOS / GCC. Jsonifier beats Glaze on the Double test on all five builds freshly allocated, by 1.10–1.51×. Why the reflection writer is faster on the freshly allocated runs has not been measured, and this paper does not guess at its algorithm.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** The size bound is worst case: every number reserves 32 bytes and every string `6n + 2`. A string-heavy document asks for up to about 6× its output size before shrinking. That is cheap with `resize_and_overwrite` but costs peak memory on large payloads.
- **Two walks.** Sizing visits every dynamic container and string length before writing. Types with a `staticSize` skip this, so the cost falls on maps, nested vectors and strings.
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Indent table depth.** Indentation past 8 levels takes the `memset` fallback.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
