# Jsonifier Serialization: Design and Performance

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, October 2026*  

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

The serializer is compiled once, for the single instruction-set tier chosen at CMake configure time (detected on the build machine, or named by `JSONIFIER_CPU_INSTRUCTIONS`). `serializeJson` calls `serialize<options>::impl` directly, with no CPUID read, function pointer, virtual call or per-call check.

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

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD step, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

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
- **Runtime lengths stay short.** Copies whose length is only known at run time, such as escape sequences and the runs that Minify and Prettify carry over, go through `jsonifierMemcpy` (`utility.hpp`). It sorts the length into a power-of-2 bucket `b`, where `b ≤ length < 2b`, and copies the first `b` bytes and the last `b` bytes. The two copies overlap, so any length from 2 to 255 bytes takes one fixed-size copy pair (a single byte takes one store), with no loop and no tail. Buckets wider than the vector register are split into register-width moves: the 128-byte bucket is 8 moves on AVX2, 4 on AVX-512 and 16 on SSE or NEON. Only lengths of 256 bytes or more reach `std::memcpy`. `jsonifierMemcpyUpTo<maxBytes>` takes a compile-time upper bound and leaves out every bucket above it, so a caller whose length is bounded never pays for the cases it cannot hit.
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

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through the plain wrapper. That size is still a compile-time constant. The only copies whose length is known only at runtime are off the common path: escape sequences inside strings (only when an escapable byte is found), raw JSON passthrough, and the memset for indents deeper than 8 levels.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test this writer leads Glaze by 9.99–17.30× freshly allocated on all five builds.

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
- **Divide by 10^8** for longer values: take the high bits of a 128-bit product with 12379400392853802749 and shift right 90 in total. GCC and Clang use `__uint128_t`, MSVC uses `_umul128`, and other compilers fall back to `mulhiPortable`.

So a 20-digit `uint64_t` is two 10^8 splits, four 10^4 splits and five 4-byte stores, with no loop and no per-digit work. A 3-digit group, as at the front of a 19-digit value, is one 4-byte table load followed by a 2-byte and a 1-byte store, so nothing is written past the 3 digits.

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.30× to 4.90×.

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

Jsonifier was fastest in 228 of the 235 write tests on five builds, tied 2 and lost 5. With the output string reused across iterations it was fastest in 233 of 235, tied 1 and lost 1. Each build runs 47 write tests: five single-value tests, ten corpus documents in minified and prettified form, the same ten documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins every full-size document test and both reformatting tests on every build in both runs. Freshly allocated, four of the five losses are against simdjson's reflection writer, which exists only on GCC: Double on both GCC builds, and minified Marine IK Small and Marine IK Reverse Small on Linux / GCC. The fifth is String on macOS / Clang, lost narrowly to Glaze. Both ties are against the reflection writer on macOS / GCC (String and Uint64). Reused, the one loss and the tie are also against the reflection writer on macOS / GCC: String loses and minified CitmCatalog Small ties. Windows / MSVC and Linux / Clang win all 47 tests in both runs. Prettified documents are where the lead is widest: 1.50–10.56× over Glaze freshly allocated across the five builds, and 16–63× over the reflection writer on the two GCC builds.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 23, 2026, with Jsonifier [b975cd7](https://github.com/nihilai-collective/jsonifier/commit/b975cd7) and BenchmarkSuite [ab75024](https://github.com/nihilai-collective/benchmarksuite/commit/ab75024) on every build. Glaze is [628bec1](https://github.com/stephenberry/glaze/commit/628bec1) on every build except macOS / Clang, which ran [1f8adf7](https://github.com/stephenberry/glaze/commit/1f8adf7). simdjson is [70f83c9](https://github.com/simdjson/simdjson/commit/70f83c9) on the two Linux builds, [2cc1e70](https://github.com/simdjson/simdjson/commit/2cc1e70) on Windows / MSVC and macOS / GCC, and [957ddbc](https://github.com/simdjson/simdjson/commit/957ddbc) on macOS / Clang. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M1. Non-converged results are included in the rankings and in the win, tie and loss counts. Each one is marked n/c in the tables below, and a test with a non-converged result should be read with reduced statistical confidence.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at the top, and as a loss when another library ranks above it.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M1 numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 47 | 44 | 0 | 3 |
| macOS / GCC 16.2 (Apple M1, NEON) | 47 | 44 | 2 | 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 47 | 46 | 0 | 1 |
| **Total** | **235** | **228** | **2** | **5** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,242 | 89 | 13.88× |
| Double | 223 | 141 | 1.58× |
| Int64 | 525 | 356 | 1.47× |
| String | 1,177 | 910 | 1.29× |
| Uint64 | 541 | 370 | 1.46× |
| Canada (minified) | 1,398 | 814 | 1.72× |
| Canada (prettified) | 3,140 | 1,402 | 2.24× |
| CitmCatalog (minified) | 8,821 | 4,435 | 1.99× |
| CitmCatalog (prettified) | 4,531 | 1,953 | 2.32× |
| Discord (minified) | 10,262 | 4,397 | 2.33× |
| Discord (prettified) | 12,536 | 4,328 | 2.90× |
| Google Maps Response (minified) | 7,861 | 3,322 | 2.37× |
| Google Maps Response (prettified) | 13,823 | 5,442 | 2.54× |
| Instruments (minified) | 12,234 | 4,118 | 2.97× |
| Instruments (prettified) | 15,937 | 4,618 | 3.45× |
| Marine IK Reverse (minified) | 885 | 674 | 1.31× |
| Marine IK Reverse (prettified) | 3,230 | 1,920 | 1.68× |
| Marine IK (minified) | 900 | 603 | 1.49× |
| Marine IK (prettified) | 3,056 | 1,356 (n/c) | 2.25× |
| Mesh (minified) | 1,473 | 927 | 1.59× |
| Mesh (prettified) | 2,343 | 1,541 | 1.52× |
| Random (minified) | 3,437 | 2,750 | 1.25× |
| Random (prettified) | 4,160 | 2,194 | 1.90× |
| Twitter (minified) | 14,185 | 4,380 | 3.24× |
| Twitter (prettified) | 15,390 | 3,481 | 4.42× |
| Canada Small (minified) | 2,021 | 1,114 | 1.81× |
| Canada Small (prettified) | 5,953 | 2,932 | 2.03× |
| CitmCatalog Small (minified) | 7,617 | 3,483 | 2.19× |
| CitmCatalog Small (prettified) | 12,715 | 4,561 | 2.79× |
| Discord Small (minified) | 9,622 | 3,554 | 2.71× |
| Discord Small (prettified) | 11,683 | 3,813 | 3.06× |
| Google Maps Response Small (minified) | 7,771 | 3,370 | 2.31× |
| Google Maps Response Small (prettified) | 15,809 | 5,209 | 3.04× |
| Instruments Small (minified) | 8,846 | 3,237 | 2.73× |
| Instruments Small (prettified) | 16,058 | 3,409 | 4.71× |
| Marine IK Reverse Small (minified) | 1,599 | 925 | 1.73× |
| Marine IK Reverse Small (prettified) | 6,419 | 3,037 | 2.11× |
| Marine IK Small (minified) | 1,549 | 932 | 1.66× |
| Marine IK Small (prettified) | 6,635 | 2,998 | 2.21× |
| Mesh Small (minified) | 1,997 | 1,158 | 1.72× |
| Mesh Small (prettified) | 3,812 | 2,021 | 1.89× |
| Random Small (minified) | 7,934 | 3,026 | 2.62× |
| Random Small (prettified) | 14,101 | 4,182 | 3.37× |
| Twitter Small (minified) | 9,665 | 3,540 (n/c) | 2.73× |
| Twitter Small (prettified) | 11,762 | 3,059 | 3.84× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,176 | 1,928 | 1,141 | 1.65× |
| Prettify | 2,475 | 2,433 | 929 | 1.02× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,364 | 217 | 10.90× |
| Double | 340 | 306 | 1.11× |
| Int64 | 961 | 865 | 1.11× |
| String | 4,260 | 2,029 | 2.10× |
| Uint64 | 933 | 859 | 1.09× |
| Canada (minified) | 1,612 | 957 | 1.68× |
| Canada (prettified) | 4,650 | 3,109 | 1.50× |
| CitmCatalog (minified) | 10,698 | 5,429 | 1.97× |
| CitmCatalog (prettified) | 17,517 | 5,549 | 3.16× |
| Discord (minified) | 14,391 | 5,984 | 2.41× |
| Discord (prettified) | 18,268 | 5,059 | 3.61× |
| Google Maps Response (minified) | 11,776 | 4,005 | 2.94× |
| Google Maps Response (prettified) | 19,170 | 4,872 | 3.93× |
| Instruments (minified) | 18,628 | 4,813 | 3.87× |
| Instruments (prettified) | 22,975 | 5,585 | 4.11× |
| Marine IK Reverse (minified) | 846 | 613 | 1.38× |
| Marine IK Reverse (prettified) | 6,119 | 3,686 | 1.66× |
| Marine IK (minified) | 859 | 585 | 1.47× |
| Marine IK (prettified) | 6,054 | 3,028 | 2.00× |
| Mesh (minified) | 1,751 | 987 | 1.77× |
| Mesh (prettified) | 4,100 | 1,695 | 2.42× |
| Random (minified) | 14,392 | 3,434 | 4.19× |
| Random (prettified) | 18,726 | 3,994 | 4.69× |
| Twitter (minified) | 20,594 | 6,020 | 3.42× |
| Twitter (prettified) | 22,847 | 5,573 | 4.10× |
| Canada Small (minified) | 1,639 | 967 | 1.69× |
| Canada Small (prettified) | 3,935 | 3,405 | 1.16× |
| CitmCatalog Small (minified) | 8,719 | 3,487 | 2.50× |
| CitmCatalog Small (prettified) | 16,690 | 3,876 | 4.31× |
| Discord Small (minified) | 10,189 | 3,757 | 2.71× |
| Discord Small (prettified) | 13,973 | 3,280 | 4.26× |
| Google Maps Response Small (minified) | 10,920 | 3,732 | 2.93× |
| Google Maps Response Small (prettified) | 20,355 | 4,705 | 4.33× |
| Instruments Small (minified) | 12,792 | 3,501 | 3.65× |
| Instruments Small (prettified) | 22,622 | 4,051 | 5.58× |
| Marine IK Reverse Small (minified) | 1,289 | 823 | 1.57× |
| Marine IK Reverse Small (prettified) | 7,299 | 4,014 | 1.82× |
| Marine IK Small (minified) | 1,428 | 814 | 1.75× |
| Marine IK Small (prettified) | 6,628 | 3,992 | 1.66× |
| Mesh Small (minified) | 1,756 | 962 | 1.83× |
| Mesh Small (prettified) | 4,277 | 1,644 | 2.60× |
| Random Small (minified) | 12,199 | 3,241 | 3.76× |
| Random Small (prettified) | 21,162 | 4,085 | 5.18× |
| Twitter Small (minified) | 8,657 | 4,760 | 1.82× |
| Twitter Small (prettified) | 11,482 | 4,018 | 2.86× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,553 | 2,764 | 1,318 | 1.29× |
| Prettify | 5,031 | 3,962 | 1,556 | 1.27× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,052 | 205 | 247 | 8.29× |
| Double | 343 | 307 | 346 | 0.99× |
| Int64 | 962 | 830 | 950 | 1.01× |
| String | 4,000 | 2,033 | 3,712 | 1.08× |
| Uint64 | 968 | 878 | 918 | 1.05× |
| Canada (minified) | 2,101 | 1,431 | 1,023 | 1.47× |
| Canada (prettified) | 6,118 | 3,146 | 375 | 1.94× |
| CitmCatalog (minified) | 12,861 | 6,146 | 11,254 | 1.14× |
| CitmCatalog (prettified) | 20,647 | 5,837 | 550 | 3.54× |
| Discord (minified) | 15,362 | 6,255 | 11,365 | 1.35× |
| Discord (prettified) | 20,392 | 6,191 | 519 | 3.29× |
| Google Maps Response (minified) | 15,901 | 4,067 | 10,070 | 1.58× |
| Google Maps Response (prettified) | 20,803 | 6,606 | 522 | 3.15× |
| Instruments (minified) | 20,075 | 5,445 | 15,594 | 1.29× |
| Instruments (prettified) | 25,657 | 6,106 | 472 | 4.20× |
| Marine IK Reverse (minified) | 1,327 | 991 | 877 | 1.34× |
| Marine IK Reverse (prettified) | 6,416 | 3,635 | 267 | 1.77× |
| Marine IK (minified) | 1,319 | 952 | 886 | 1.39× |
| Marine IK (prettified) | 6,532 | 2,984 | 271 | 2.19× |
| Mesh (minified) | 2,422 | 1,618 | 1,294 | 1.50× |
| Mesh (prettified) | 4,587 | 2,562 | 261 | 1.79× |
| Random (minified) | 15,466 | 3,681 | 11,160 | 1.39× |
| Random (prettified) | 19,619 | 4,778 | 500 | 4.11× |
| Twitter (minified) | 19,239 | 6,912 | 12,955 | 1.49× |
| Twitter (prettified) | 24,095 | 4,143 | 522 | 5.82× |
| Canada Small (minified) | 2,033 | 1,566 | 1,060 | 1.30× |
| Canada Small (prettified) | 4,874 | 3,475 | 406 | 1.40× |
| CitmCatalog Small (minified) | 8,008 | 3,558 | 6,847 | 1.17× |
| CitmCatalog Small (prettified) | 18,513 | 4,080 | 541 | 4.54× |
| Discord Small (minified) | 10,721 | 3,988 | 6,667 | 1.61× |
| Discord Small (prettified) | 14,756 | 3,601 | 528 | 4.10× |
| Google Maps Response Small (minified) | 13,175 | 3,814 | 7,989 | 1.65× |
| Google Maps Response Small (prettified) | 24,122 | 5,953 | 537 | 4.05× |
| Instruments Small (minified) | 12,069 | 3,958 | 7,822 | 1.54× |
| Instruments Small (prettified) | 20,914 | 3,497 | 521 | 5.98× |
| Marine IK Reverse Small (minified) | 1,467 | 1,174 | 1,477 | 0.99× |
| Marine IK Reverse Small (prettified) | 7,093 | 3,849 | 532 | 1.84× |
| Marine IK Small (minified) | 1,498 | 1,181 | 1,504 | 1.00× |
| Marine IK Small (prettified) | 6,554 | 3,799 | 497 | 1.73× |
| Mesh Small (minified) | 2,177 | 1,392 | 1,370 | 1.56× |
| Mesh Small (prettified) | 4,388 | 2,062 | 298 | 2.13× |
| Random Small (minified) | 12,386 | 3,737 | 7,650 | 1.62× |
| Random Small (prettified) | 22,983 | 4,518 | 538 | 5.09× |
| Twitter Small (minified) | 8,120 | 5,404 | 6,002 | 1.35× |
| Twitter Small (prettified) | 11,133 | 3,628 | 527 | 3.07× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,243 | 2,363 | 1,400 | 1.37× |
| Prettify | 4,295 | 3,590 | 1,448 | 1.20× |

### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,861 | 110 | 197 | 9.44× |
| Double | 245 | 187 | 253 | 0.97× |
| Int64 | 705 | 469 | 691 | 1.02× |
| String | 1,134 | 972 | 1,130 | 1.00× |
| Uint64 | 704 | 473 | 707 | 1.00× |
| Canada (minified) | 2,037 | 1,436 | 697 | 1.42× |
| Canada (prettified) | 5,633 | 2,911 (n/c) | 228 | 1.93× |
| CitmCatalog (minified) | 7,748 | 1,800 | 6,675 | 1.16× |
| CitmCatalog (prettified) | 17,592 | 3,071 | 277 (n/c) | 5.73× |
| Discord (minified) | 10,385 | 2,534 | 7,950 | 1.31× |
| Discord (prettified) | 12,819 | 2,243 | 308 | 5.72× |
| Google Maps Response (minified) | 9,706 | 1,407 | 7,509 | 1.29× |
| Google Maps Response (prettified) | 17,292 | 2,510 | 278 | 6.89× |
| Instruments (minified) | 12,749 | 1,611 | 7,033 | 1.81× |
| Instruments (prettified) | 15,208 | 2,032 | 302 | 7.48× |
| Marine IK Reverse (minified) | 995 | 751 | 391 | 1.32× |
| Marine IK Reverse (prettified) | 4,911 | 2,298 | 217 (n/c) | 2.14× |
| Marine IK (minified) | 1,159 | 682 | 569 | 1.70× |
| Marine IK (prettified) | 4,090 | 1,905 | 214 (n/c) | 2.15× |
| Mesh (minified) | 2,034 | 984 | 687 | 2.07× |
| Mesh (prettified) | 4,018 | 1,411 | 201 | 2.85× |
| Random (minified) | 10,756 | 1,785 | 7,205 | 1.49× |
| Random (prettified) | 9,016 | 2,137 | 279 | 4.22× |
| Twitter (minified) | 14,038 | 2,967 | 8,104 | 1.73× |
| Twitter (prettified) | 16,638 | 2,952 | 350 | 5.64× |
| Canada Small (minified) | 1,999 | 1,492 | 768 | 1.34× |
| Canada Small (prettified) | 5,942 | 2,770 | 220 | 2.15× |
| CitmCatalog Small (minified) | 7,938 | 1,646 | 6,657 | 1.19× |
| CitmCatalog Small (prettified) | 15,452 | 2,654 | 286 | 5.82× |
| Discord Small (minified) | 9,314 | 2,269 | 6,861 | 1.36× |
| Discord Small (prettified) | 11,342 | 2,189 | 305 | 5.18× |
| Google Maps Response Small (minified) | 8,748 | 1,285 | 7,026 | 1.25× |
| Google Maps Response Small (prettified) | 17,977 | 2,520 | 283 | 7.13× |
| Instruments Small (minified) | 12,166 | 1,516 | 7,362 | 1.65× |
| Instruments Small (prettified) | 14,564 | 1,874 | 227 | 7.77× |
| Marine IK Reverse Small (minified) | 1,696 | 879 | 687 | 1.93× |
| Marine IK Reverse Small (prettified) | 6,162 | 2,689 | 237 (n/c) | 2.29× |
| Marine IK Small (minified) | 1,653 | 866 | 1,366 | 1.21× |
| Marine IK Small (prettified) | 6,594 | 2,753 | 248 | 2.40× |
| Mesh Small (minified) | 1,995 | 910 | 991 | 2.01× |
| Mesh Small (prettified) | 3,764 | 1,266 | 186 | 2.97× |
| Random Small (minified) | 9,830 | 1,641 | 7,671 | 1.28× |
| Random Small (prettified) | 13,236 | 2,374 | 308 | 5.58× |
| Twitter Small (minified) | 10,711 | 2,841 | 7,649 | 1.40× |
| Twitter Small (prettified) | 11,770 (n/c) | 2,827 | 360 | 4.16× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,784 | 1,372 | 770 | 1.30× |
| Prettify | 2,868 | 2,030 | 662 | 1.41× |

### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,029 | 117 | 17.30× |
| Double | 278 | 209 | 1.33× |
| Int64 | 3,250 | 560 | 5.80× |
| String | 1,140 (n/c) | 1,168 | 0.98× |
| Uint64 | 3,439 | 489 | 7.04× |
| Canada (minified) | 2,679 | 1,675 | 1.60× |
| Canada (prettified) | 6,769 | 3,308 | 2.05× |
| CitmCatalog (minified) | 7,190 | 1,775 | 4.05× |
| CitmCatalog (prettified) | 18,748 | 2,809 | 6.67× |
| Discord (minified) | 10,843 | 2,512 | 4.32× |
| Discord (prettified) | 17,956 | 2,390 | 7.51× |
| Google Maps Response (minified) | 9,250 | 1,827 | 5.06× |
| Google Maps Response (prettified) | 19,938 | 2,543 | 7.84× |
| Instruments (minified) | 10,689 | 2,075 | 5.15× |
| Instruments (prettified) | 23,129 | 2,191 | 10.56× |
| Marine IK Reverse (minified) | 1,185 | 891 | 1.33× |
| Marine IK Reverse (prettified) | 6,808 | 2,841 (n/c) | 2.40× |
| Marine IK (minified) | 1,420 | 822 | 1.73× |
| Marine IK (prettified) | 6,760 | 2,572 | 2.63× |
| Mesh (minified) | 2,535 | 1,276 | 1.99× |
| Mesh (prettified) | 4,615 | 1,761 | 2.62× |
| Random (minified) | 8,294 | 1,826 | 4.54× |
| Random (prettified) | 12,244 | 2,139 | 5.72× |
| Twitter (minified) | 10,905 | 3,179 | 3.43× |
| Twitter (prettified) | 20,326 | 3,262 | 6.23× |
| Canada Small (minified) | 2,730 | 1,620 | 1.69× |
| Canada Small (prettified) | 7,476 | 3,080 | 2.43× |
| CitmCatalog Small (minified) | 7,040 | 1,487 | 4.73× |
| CitmCatalog Small (prettified) | 16,914 | 2,104 | 8.04× |
| Discord Small (minified) | 10,224 | 2,509 | 4.07× |
| Discord Small (prettified) | 15,056 | 1,948 | 7.73× |
| Google Maps Response Small (minified) | 8,539 | 1,394 | 6.12× |
| Google Maps Response Small (prettified) | 20,824 | 2,585 | 8.06× |
| Instruments Small (minified) | 13,636 | 1,903 | 7.17× |
| Instruments Small (prettified) | 20,312 | 2,294 | 8.85× |
| Marine IK Reverse Small (minified) | 1,337 | 1,099 | 1.22× |
| Marine IK Reverse Small (prettified) | 6,560 | 2,730 | 2.40× |
| Marine IK Small (minified) | 1,546 | 967 | 1.60× |
| Marine IK Small (prettified) | 8,670 | 3,301 | 2.63× |
| Mesh Small (minified) | 2,533 | 1,234 | 2.05× |
| Mesh Small (prettified) | 4,925 | 1,658 | 2.97× |
| Random Small (minified) | 9,796 | 1,946 | 5.03× |
| Random Small (prettified) | 13,408 | 2,378 | 5.64× |
| Twitter Small (minified) | 9,688 | 2,997 | 3.23× |
| Twitter Small (prettified) | 13,614 | 2,905 | 4.69× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,478 | 1,491 | 912 | 1.66× |
| Prettify | 3,476 | 2,085 | 603 | 1.67× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 233 of the 235 write tests, ties 1 and loses 1, both on macOS / GCC against the reflection writer: String loses, 8,045 vs 9,336 MB/s (0.86×), and minified CitmCatalog Small ties (7,296 vs 6,509 MB/s, Jsonifier's result not converged).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| macOS / GCC 16.2 (Apple M1, NEON) | 47 | 45 | 1 | 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 47 | 47 | 0 | 0 |
| **Total** | **235** | **233** | **1** | **1** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,894 | 270 | 7.01× |
| Double | 820 | 319 | 2.57× |
| Int64 | 4,722 | 1,100 | 4.29× |
| String | 7,473 | 2,392 | 3.12× |
| Uint64 | 5,508 | 1,124 | 4.90× |
| Canada (minified) | 1,695 | 1,096 | 1.55× |
| Canada (prettified) | 5,196 | 3,081 | 1.69× |
| CitmCatalog (minified) | 8,892 | 4,979 | 1.79× |
| CitmCatalog (prettified) | 15,649 | 7,403 | 2.11× |
| Discord (minified) | 10,778 | 5,037 | 2.14× |
| Discord (prettified) | 13,233 | 5,300 | 2.50× |
| Google Maps Response (minified) | 9,383 | 3,894 | 2.41× |
| Google Maps Response (prettified) | 14,613 | 6,494 | 2.25× |
| Instruments (minified) | 12,564 | 4,461 | 2.82× |
| Instruments (prettified) | 16,124 | 5,532 | 2.91× |
| Marine IK Reverse (minified) | 1,022 | 737 | 1.39× |
| Marine IK Reverse (prettified) | 5,056 | 3,328 | 1.52× |
| Marine IK (minified) | 993 | 756 | 1.31× |
| Marine IK (prettified) | 4,966 | 3,357 | 1.48× |
| Mesh (minified) | 1,875 | 1,310 | 1.43× |
| Mesh (prettified) | 3,447 | 2,186 | 1.58× |
| Random (minified) | 9,018 | 2,953 | 3.05× |
| Random (prettified) | 12,847 | 4,462 | 2.88× |
| Twitter (minified) | 14,390 | 5,203 | 2.77× |
| Twitter (prettified) | 15,613 | 3,963 | 3.94× |
| Canada Small (minified) | 1,986 | 1,192 | 1.67× |
| Canada Small (prettified) | 6,022 | 3,229 | 1.86× |
| CitmCatalog Small (minified) | 8,066 | 4,479 | 1.80× |
| CitmCatalog Small (prettified) | 15,539 | 5,862 | 2.65× |
| Discord Small (minified) | 10,845 | 4,865 | 2.23× |
| Discord Small (prettified) | 13,523 | 4,508 | 3.00× |
| Google Maps Response Small (minified) | 9,754 | 3,858 | 2.53× |
| Google Maps Response Small (prettified) | 18,532 | 6,690 | 2.77× |
| Instruments Small (minified) | 11,386 | 3,769 (n/c) | 3.02× |
| Instruments Small (prettified) | 18,424 | 5,231 | 3.52× |
| Marine IK Reverse Small (minified) | 1,699 | 1,002 | 1.70× |
| Marine IK Reverse Small (prettified) | 6,758 | 3,416 | 1.98× |
| Marine IK Small (minified) | 1,649 | 996 | 1.66× |
| Marine IK Small (prettified) | 7,148 | 3,187 | 2.24× |
| Mesh Small (minified) | 2,128 | 1,326 | 1.61× |
| Mesh Small (prettified) | 3,312 | 2,238 | 1.48× |
| Random Small (minified) | 10,062 | 3,706 | 2.71× |
| Random Small (prettified) | 17,220 | 5,285 | 3.26× |
| Twitter Small (minified) | 11,745 | 5,360 | 2.19× |
| Twitter Small (prettified) | 14,699 | 5,351 | 2.75× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,190 | 1,918 | 1,140 | 1.66× |
| Prettify | 3,898 | 2,424 | 924 | 1.61× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 5,084 | 870 | 5.84× |
| Double | 831 | 659 | 1.26× |
| Int64 | 5,235 | 2,339 | 2.24× |
| String | 11,990 | 5,655 | 2.12× |
| Uint64 | 5,691 | 2,576 | 2.21× |
| Canada (minified) | 1,596 | 993 | 1.61× |
| Canada (prettified) | 4,663 | 3,798 | 1.23× |
| CitmCatalog (minified) | 10,649 | 6,380 | 1.67× |
| CitmCatalog (prettified) | 17,757 | 7,586 | 2.34× |
| Discord (minified) | 14,815 | 6,206 | 2.39× |
| Discord (prettified) | 18,691 | 5,873 | 3.18× |
| Google Maps Response (minified) | 12,324 | 4,453 | 2.77× |
| Google Maps Response (prettified) | 19,037 | 5,557 | 3.43× |
| Instruments (minified) | 18,597 | 6,600 | 2.82× |
| Instruments (prettified) | 23,179 | 6,698 | 3.46× |
| Marine IK Reverse (minified) | 834 | 613 | 1.36× |
| Marine IK Reverse (prettified) | 6,060 | 4,022 | 1.51× |
| Marine IK (minified) | 857 | 620 | 1.38× |
| Marine IK (prettified) | 6,146 | 4,044 | 1.52× |
| Mesh (minified) | 1,754 | 1,050 | 1.67× |
| Mesh (prettified) | 4,135 | 1,762 | 2.35× |
| Random (minified) | 14,472 | 3,843 | 3.77× |
| Random (prettified) | 18,780 | 4,659 | 4.03× |
| Twitter (minified) | 20,564 | 6,893 | 2.98× |
| Twitter (prettified) | 22,958 | 6,433 | 3.57× |
| Canada Small (minified) | 1,659 | 1,013 | 1.64× |
| Canada Small (prettified) | 4,475 | 3,691 | 1.21× |
| CitmCatalog Small (minified) | 9,488 | 3,988 | 2.38× |
| CitmCatalog Small (prettified) | 18,281 | 4,425 | 4.13× |
| Discord Small (minified) | 12,040 | 4,137 | 2.91× |
| Discord Small (prettified) | 16,643 | 3,653 | 4.56× |
| Google Maps Response Small (minified) | 11,967 | 4,108 | 2.91× |
| Google Maps Response Small (prettified) | 22,474 | 5,368 | 4.19× |
| Instruments Small (minified) | 14,489 | 4,118 | 3.52× |
| Instruments Small (prettified) | 24,649 | 4,691 | 5.25× |
| Marine IK Reverse Small (minified) | 1,310 | 850 | 1.54× |
| Marine IK Reverse Small (prettified) | 7,552 | 4,497 | 1.68× |
| Marine IK Small (minified) | 1,432 | 838 | 1.71× |
| Marine IK Small (prettified) | 6,767 | 4,436 | 1.53× |
| Mesh Small (minified) | 1,824 | 990 | 1.84× |
| Mesh Small (prettified) | 4,413 | 1,771 | 2.49× |
| Random Small (minified) | 13,782 | 3,884 | 3.55× |
| Random Small (prettified) | 24,365 | 4,784 | 5.09× |
| Twitter Small (minified) | 10,322 | 5,560 | 1.86× |
| Twitter Small (prettified) | 13,631 | 4,761 | 2.86× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,612 | 2,819 | 1,315 | 1.28× |
| Prettify | 5,029 | 3,973 | 1,480 | 1.27× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,277 | 675 | 3,147 | 1.04× |
| Double | 864 | 735 | 647 | 1.18× |
| Int64 | 5,476 | 2,479 | 4,225 | 1.30× |
| String | 11,414 | 6,167 | 8,785 | 1.30× |
| Uint64 | 6,316 | 2,558 | 4,302 | 1.47× |
| Canada (minified) | 2,122 | 1,509 | 1,031 | 1.41× |
| Canada (prettified) | 6,163 | 3,757 | 381 | 1.64× |
| CitmCatalog (minified) | 12,674 | 7,280 | 11,112 | 1.14× |
| CitmCatalog (prettified) | 20,596 | 7,999 | 553 | 2.57× |
| Discord (minified) | 15,653 | 6,881 | 11,543 | 1.36× |
| Discord (prettified) | 20,584 | 7,262 | 516 | 2.83× |
| Google Maps Response (minified) | 17,097 | 5,261 | 10,342 | 1.65× |
| Google Maps Response (prettified) | 21,908 | 7,570 | 520 | 2.89× |
| Instruments (minified) | 20,280 | 6,303 | 15,977 | 1.27× |
| Instruments (prettified) | 25,678 | 7,230 | 478 | 3.55× |
| Marine IK Reverse (minified) | 1,303 | 993 | 866 | 1.31× |
| Marine IK Reverse (prettified) | 6,440 | 4,006 | 266 | 1.61× |
| Marine IK (minified) | 1,325 | 1,002 | 887 | 1.32× |
| Marine IK (prettified) | 6,482 | 4,078 | 286 | 1.59× |
| Mesh (minified) | 2,437 | 1,741 | 1,288 | 1.40× |
| Mesh (prettified) | 4,562 | 2,735 | 259 | 1.67× |
| Random (minified) | 15,362 | 4,141 | 11,571 | 1.33× |
| Random (prettified) | 19,952 | 5,742 | 505 | 3.47× |
| Twitter (minified) | 19,345 | 7,731 | 12,872 | 1.50× |
| Twitter (prettified) | 24,265 | 4,532 | 526 | 5.35× |
| Canada Small (minified) | 2,053 | 1,664 | 1,063 | 1.23× |
| Canada Small (prettified) | 4,996 | 3,828 | 407 | 1.31× |
| CitmCatalog Small (minified) | 9,011 | 4,069 | 7,301 | 1.23× |
| CitmCatalog Small (prettified) | 19,307 | 4,799 | 539 | 4.02× |
| Discord Small (minified) | 12,071 | 4,764 | 7,269 | 1.66× |
| Discord Small (prettified) | 16,464 | 4,025 | 524 | 4.09× |
| Google Maps Response Small (minified) | 14,721 | 4,249 | 8,575 | 1.72× |
| Google Maps Response Small (prettified) | 27,992 | 6,871 | 534 | 4.07× |
| Instruments Small (minified) | 13,619 | 4,596 | 8,488 | 1.60× |
| Instruments Small (prettified) | 23,442 | 3,997 | 526 | 5.86× |
| Marine IK Reverse Small (minified) | 1,518 | 1,260 | 1,503 | 1.01× |
| Marine IK Reverse Small (prettified) | 7,244 | 4,320 | 531 | 1.68× |
| Marine IK Small (minified) | 1,533 | 1,286 | 1,501 | 1.02× |
| Marine IK Small (prettified) | 6,516 | 4,236 | 497 | 1.54× |
| Mesh Small (minified) | 2,312 | 1,466 | 1,408 | 1.58× |
| Mesh Small (prettified) | 4,854 | 2,267 | 298 | 2.14× |
| Random Small (minified) | 14,064 | 4,552 | 8,402 | 1.67× |
| Random Small (prettified) | 25,423 | 5,541 | 544 | 4.59× |
| Twitter Small (minified) | 9,417 | 6,386 | 6,663 | 1.41× |
| Twitter Small (prettified) | 12,251 | 4,357 | 539 | 2.81× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,203 | 2,339 | 1,398 | 1.37× |
| Prettify | 4,279 | 3,647 | 1,482 | 1.17× |

#### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,069 | 732 | 2,591 | 1.18× |
| Double | 756 | 434 | 624 | 1.21× |
| Int64 | 4,686 | 1,951 | 2,810 | 1.67× |
| String | 8,045 | 4,022 | 9,336 | 0.86× |
| Uint64 | 4,558 | 2,149 | 2,872 | 1.59× |
| Canada (minified) | 2,076 | 1,405 | 696 | 1.48× |
| Canada (prettified) | 5,700 (n/c) | 3,224 | 232 | 1.77× |
| CitmCatalog (minified) | 7,852 | 1,826 | 6,175 | 1.27× |
| CitmCatalog (prettified) | 16,258 | 3,025 | 208 | 5.37× |
| Discord (minified) | 10,839 | 2,802 | 8,508 | 1.27× |
| Discord (prettified) | 13,192 | 2,556 | 299 | 5.16× |
| Google Maps Response (minified) | 10,113 | 1,760 | 7,830 | 1.29× |
| Google Maps Response (prettified) | 20,111 | 2,811 | 209 | 7.15× |
| Instruments (minified) | 12,914 | 1,788 | 7,069 | 1.83× |
| Instruments (prettified) | 15,584 | 2,260 | 299 | 6.90× |
| Marine IK Reverse (minified) | 946 | 754 | 396 | 1.26× |
| Marine IK Reverse (prettified) | 4,523 (n/c) | 2,302 (n/c) | 210 (n/c) | 1.97× |
| Marine IK (minified) | 1,203 | 758 | 719 | 1.59× |
| Marine IK (prettified) | 4,240 (n/c) | 2,215 | 267 (n/c) | 1.91× |
| Mesh (minified) | 1,987 | 934 | 674 | 2.13× |
| Mesh (prettified) | 4,188 | 1,377 | 192 | 3.04× |
| Random (minified) | 10,195 | 1,756 | 5,964 | 1.71× |
| Random (prettified) | 11,446 | 2,252 | 274 | 5.08× |
| Twitter (minified) | 12,209 | 3,432 | 8,957 | 1.36× |
| Twitter (prettified) | 16,142 | 3,523 | 349 | 4.58× |
| Canada Small (minified) | 2,059 | 1,577 | 783 | 1.31× |
| Canada Small (prettified) | 6,133 | 2,896 | 235 | 2.12× |
| CitmCatalog Small (minified) | 7,296 (n/c) | 1,801 | 6,509 | 1.12× |
| CitmCatalog Small (prettified) | 14,790 | 2,522 | 210 | 5.86× |
| Discord Small (minified) | 11,738 | 2,774 | 8,328 | 1.41× |
| Discord Small (prettified) | 12,526 | 2,425 | 311 | 5.17× |
| Google Maps Response Small (minified) | 9,780 | 1,730 | 7,462 | 1.31× |
| Google Maps Response Small (prettified) | 18,814 | 2,940 | 297 | 6.40× |
| Instruments Small (minified) | 13,166 (n/c) | 1,904 | 6,771 | 1.94× |
| Instruments Small (prettified) | 13,694 (n/c) | 1,813 | 259 (n/c) | 7.55× |
| Marine IK Reverse Small (minified) | 1,775 | 878 | 692 | 2.02× |
| Marine IK Reverse Small (prettified) | 7,804 | 2,726 | 269 | 2.86× |
| Marine IK Small (minified) | 1,684 | 919 | 1,391 | 1.21× |
| Marine IK Small (prettified) | 7,039 | 2,956 | 279 | 2.38× |
| Mesh Small (minified) | 2,150 | 968 | 1,051 | 2.04× |
| Mesh Small (prettified) | 4,319 | 1,506 | 194 | 2.87× |
| Random Small (minified) | 10,085 | 1,821 | 8,009 | 1.26× |
| Random Small (prettified) | 14,698 | 2,363 | 270 | 6.22× |
| Twitter Small (minified) | 11,290 | 3,498 | 7,678 | 1.47× |
| Twitter Small (prettified) | 13,926 | 3,573 | 333 | 3.90× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,018 | 1,442 (n/c) | 808 | 1.40× |
| Prettify | 2,939 | 2,017 | 663 | 1.46× |

#### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,903 | 719 | 4.04× |
| Double | 778 | 621 | 1.25× |
| Int64 | 3,105 | 2,238 | 1.39× |
| String | 8,201 | 3,958 | 2.07× |
| Uint64 | 3,693 | 1,872 | 1.97× |
| Canada (minified) | 2,705 | 1,985 | 1.36× |
| Canada (prettified) | 6,448 (n/c) | 3,556 | 1.81× |
| CitmCatalog (minified) | 7,425 | 1,829 | 4.06× |
| CitmCatalog (prettified) | 17,610 | 3,111 | 5.66× |
| Discord (minified) | 12,851 | 2,712 | 4.74× |
| Discord (prettified) | 18,552 | 2,823 | 6.57× |
| Google Maps Response (minified) | 10,887 | 1,979 | 5.50× |
| Google Maps Response (prettified) | 23,292 | 3,099 | 7.52× |
| Instruments (minified) | 13,574 | 2,225 | 6.10× |
| Instruments (prettified) | 22,243 | 2,378 | 9.35× |
| Marine IK Reverse (minified) | 1,222 | 855 | 1.43× |
| Marine IK Reverse (prettified) | 6,543 | 3,153 | 2.08× |
| Marine IK (minified) | 1,418 | 890 | 1.59× |
| Marine IK (prettified) | 6,113 | 2,877 | 2.13× |
| Mesh (minified) | 2,811 | 1,352 | 2.08× |
| Mesh (prettified) | 4,718 | 1,883 | 2.51× |
| Random (minified) | 8,397 | 2,078 | 4.04× |
| Random (prettified) | 14,723 | 2,364 | 6.23× |
| Twitter (minified) | 13,222 | 4,011 | 3.30× |
| Twitter (prettified) | 20,671 | 3,749 | 5.51× |
| Canada Small (minified) | 2,840 | 1,896 | 1.50× |
| Canada Small (prettified) | 7,586 | 3,552 | 2.14× |
| CitmCatalog Small (minified) | 8,103 | 1,770 | 4.58× |
| CitmCatalog Small (prettified) | 18,699 | 2,692 | 6.95× |
| Discord Small (minified) | 12,126 | 2,437 | 4.98× |
| Discord Small (prettified) | 15,515 | 2,392 | 6.49× |
| Google Maps Response Small (minified) | 9,369 | 1,640 | 5.71× |
| Google Maps Response Small (prettified) | 20,361 | 3,016 | 6.75× |
| Instruments Small (minified) | 15,449 | 2,207 | 7.00× |
| Instruments Small (prettified) | 27,750 | 2,411 | 11.51× |
| Marine IK Reverse Small (minified) | 2,086 | 1,173 | 1.78× |
| Marine IK Reverse Small (prettified) | 7,521 | 3,157 | 2.38× |
| Marine IK Small (minified) | 1,951 | 1,076 | 1.81× |
| Marine IK Small (prettified) | 9,319 | 3,853 | 2.42× |
| Mesh Small (minified) | 2,867 | 1,404 | 2.04× |
| Mesh Small (prettified) | 5,757 | 1,978 | 2.91× |
| Random Small (minified) | 10,638 | 2,215 | 4.80× |
| Random Small (prettified) | 14,564 (n/c) | 2,513 | 5.79× |
| Twitter Small (minified) | 9,224 | 3,700 | 2.49× |
| Twitter Small (prettified) | 13,048 | 3,904 | 3.34× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,805 | 1,645 | 909 | 1.70× |
| Prettify | 3,047 | 2,142 | 630 | 1.42× |

### Losses and ties

The five freshly allocated losses:

- **Double, Linux / GCC.** The reflection writer runs 346 vs Jsonifier's 343 MB/s (0.99×), a gap Welch's test still separates. Jsonifier is ahead of Glaze (307 MB/s, 1.12×), and ahead of the reflection writer reused (864 vs 647 MB/s, 1.34×).
- **Double, macOS / GCC.** The reflection writer runs 253 vs 245 MB/s (0.97×). Jsonifier is ahead of Glaze (187 MB/s, 1.31×), and ahead of the reflection writer reused (756 vs 624 MB/s, 1.21×).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,504 vs 1,498 MB/s (1.00× to two places). Reused, Jsonifier wins by 1.02× (1,533 vs 1,501).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,477 vs 1,467 MB/s (0.99×). Reused, Jsonifier wins by 1.01× (1,518 vs 1,503).
- **String, macOS / Clang.** Glaze runs 1,168 vs Jsonifier's 1,140 MB/s (0.98×), with Jsonifier's result not converged. Reused, Jsonifier wins by 2.07× (8,201 vs 3,958).

The two freshly allocated ties are on macOS / GCC against the reflection writer: String (1,134 vs 1,130 MB/s) and Uint64 (704 vs 707 MB/s). Reused, Jsonifier wins Uint64 by 1.59× (4,558 vs 2,872) but loses String (8,045 vs 9,336, 0.86×), the only reused loss.

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.14–2.96× and prettified documents by 16–63×. The Small documents follow the full-size ones apart from the two minified Marine IK copies on Linux / GCC: Jsonifier wins 98 of the 100 freshly allocated and 99 of the 100 reused (one tie). Why the reflection writer edges ahead on these few tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.29–1.66× freshly allocated and 1.28–1.70× reused. It wins Prettify on all five builds in both runs, by 1.02–1.67× freshly allocated and 1.17–1.61× reused; the narrowest is Windows / MSVC freshly allocated (2,475 vs 2,433 MB/s). Glaze is the next-fastest library in every case.

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
| String SIMD block | one vector width, stored before the escape check | the `6n + 2` reservation, which the copied bytes never exceed |
| Short string (under 16 bytes) | up to 15 bytes (one 16-byte store) | 64-byte tail slack |

The largest single overstore is 31 bytes, under the 64-byte slack. That slack is a fixed constant, not derived from the widest writer, so a future writer with a wider overstore must raise it.

### Does `6n + 2` blow up memory on huge strings?

Yes, the reservation is real. A 100 MB string field asks for about 600 MB before the final shrink, and the string keeps that capacity afterwards unless the caller calls `shrink_to_fit`. How much of it is touched depends on the buffer:

- With `resize_and_overwrite`, nothing is zero-filled. On Linux and macOS, pages that are never written are usually never faulted in, so the cost is mostly address space. On Windows, the allocation still counts against the commit limit.
- Without `resize_and_overwrite`, `resize` zero-fills the whole bound, so every page is touched.

Setting `prescanStrings` in `serialize_options` removes the 6× factor, at the price of one extra pass over every string's bytes during sizing. See *Pre-scanning strings* below. Its throughput cost has not been measured.

### Pre-scanning strings

`prescanStrings` replaces the `6n + 2` string bound with a scan that returns the exact number of bytes the writer will emit for the string, plus 2 for the quotes. The option is off by default and is a compile-time `if constexpr` in the string `get_size_impl`, so the default path is unchanged.

The scan (`string_serializer::escapedSize`) reads 8 bytes at a time and applies the same SWAR flag test the writer's tail path uses (`flagMask`): a word with no `"`, `\` or control byte adds 8 and moves on. A flagged word is summed byte by byte from `escapeTableSizes`, the same table the writer uses, where `\b \t \n \f \r " \\` take 2 bytes, other control bytes take 6, and everything else takes 1. The tail under 8 bytes is summed the same way. A NUL byte is counted as 1, although the writer's table emits nothing for it, so the bound can exceed the output by a byte but never fall short.

The 64-byte slack in `serializeJson` is unchanged, so the overstore argument above still holds. The option only affects strings: numbers, booleans and keys keep their existing bounds.

### Is it portable to big-endian targets?

The code has big-endian paths, but none are tested. `packed_blitter`, the bool constants, the digit tables and the SWAR string tail each branch on `std::endian::native` at compile time and use `std::byteswap` where needed. The CI matrix covers only little-endian x86 and ARM, so those branches have never run.

### Why is the String test slower on NEON than on AVX2?

The gap is a freshly allocated one. Freshly allocated, Jsonifier runs String at 1,134 MB/s on macOS / GCC (1.17× Glaze's 972, tied with the reflection writer's 1,130) and 1,140 MB/s on macOS / Clang (0.98× Glaze's 1,168). The AVX2 builds run the same test at 1,177–4,260 MB/s and lead Glaze by 1.29× (Windows / MSVC), 2.10× (Linux / Clang) and 1.97× (Linux / GCC). With the string reused the M1 lead over Glaze is 2.07× on macOS / Clang (8,201 vs 3,958 MB/s) and 2.00× on macOS / GCC (8,045 vs 4,022), within the 1.85–3.12× the AVX2 builds show reused. On macOS / GCC the reflection writer is ahead reused, at 9,336 MB/s.

So most of the M1 shortfall sits in the part of the freshly allocated run that the reused run removes, the allocation and release of the output string, rather than in the copy loop. That is an inference from the two runs, not a profile. Two features of the general loop remain candidates for whatever is left:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated it does, narrowly, on both GCC builds (0.97× and 0.99×; see *Losses and ties*). Reused, Jsonifier is ahead on both, and it beats Glaze on Double on all five builds in both runs.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** Every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`, so string-heavy documents can ask for about 6× their output size (see *Does `6n + 2` blow up memory on huge strings?*).
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
