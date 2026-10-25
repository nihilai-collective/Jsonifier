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

On the Bool test this writer leads Glaze by 10.97–14.74× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.31× to 5.13×.

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

Jsonifier was fastest in 249 of the 255 write tests on five builds and lost 6. With the output string reused across iterations it was fastest in 253 of 255 and lost 2. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and every full-size document test except minified Twitter on Windows / MSVC freshly allocated. Freshly allocated, three losses are against simdjson's reflection writer, which exists only on GCC: Double and Uint64 on macOS / GCC and minified Marine IK Small on Linux / GCC. The other three are against Glaze: minified Twitter and Prettify on Windows / MSVC, and String on macOS / Clang. There are no ties in either run. Reused, both losses are against the reflection writer: String on macOS / GCC and minified Marine IK Small on Linux / GCC. Linux / Clang wins all 51 tests in both runs. Prettified documents are where the lead is widest: 1.46–10.52× over Glaze freshly allocated across the five builds, and 16–74× over the reflection writer on the two GCC builds.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 26, 2026, with Jsonifier [c4f0510](https://github.com/nihilai-collective/jsonifier/commit/c4f0510), Glaze [1476d6b](https://github.com/stephenberry/glaze/commit/1476d6b), simdjson [50f0739](https://github.com/simdjson/simdjson/commit/50f0739) and BenchmarkSuite [35ec835](https://github.com/nihilai-collective/benchmarksuite/commit/35ec835) on every build. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.44 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size.

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
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 0 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| **Total** | **255** | **249** | **0** | **6** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,298 | 91 | 14.29× |
| Double | 222 | 145 | 1.53× |
| Int64 | 486 | 356 | 1.36× |
| String | 1,190 | 823 | 1.44× |
| Uint64 | 569 | 381 | 1.49× |
| Apache Builds (minified) | 11,489 | 4,080 (n/c) | 2.82× |
| Canada (minified) | 1,270 (n/c) | 809 | 1.57× |
| Apache Builds (prettified) | 11,958 | 5,030 | 2.38× |
| Canada (prettified) | 2,763 | 1,268 | 2.18× |
| CitmCatalog (minified) | 7,553 | 4,263 | 1.77× |
| CitmCatalog (prettified) | 4,004 | 1,745 (n/c) | 2.29× |
| Discord (minified) | 10,100 | 4,087 (n/c) | 2.47× |
| Discord (prettified) | 12,583 | 4,379 | 2.87× |
| Github Events (minified) | 11,895 | 4,939 | 2.41× |
| Google Maps Response (minified) | 8,755 | 3,257 | 2.69× |
| Github Events (prettified) | 13,553 | 3,350 | 4.05× |
| Google Maps Response (prettified) | 13,815 | 5,417 | 2.55× |
| Instruments (minified) | 12,464 | 3,572 | 3.49× |
| Instruments (prettified) | 15,725 | 3,839 | 4.10× |
| Marine IK Reverse (minified) | 827 | 606 | 1.37× |
| Marine IK Reverse (prettified) | 2,685 | 1,620 | 1.66× |
| Marine IK (minified) | 824 (n/c) | 523 | 1.58× |
| Marine IK (prettified) | 2,700 | 1,087 | 2.48× |
| Random (minified) | 3,526 | 3,178 | 1.11× |
| Random (prettified) | 4,101 (n/c) | 1,998 (n/c) | 2.05× |
| Twitter (minified) | 3,215 | 4,678 | 0.69× |
| Twitter (prettified) | 3,487 | 1,688 | 2.07× |
| Apache Builds Small (minified) | 9,748 | 3,570 | 2.73× |
| Canada Small (minified) | 1,882 | 1,072 | 1.75× |
| Apache Builds Small (prettified) | 12,626 | 4,190 | 3.01× |
| Canada Small (prettified) | 5,751 | 2,759 (n/c) | 2.08× |
| CitmCatalog Small (minified) | 6,991 | 3,466 | 2.02× |
| CitmCatalog Small (prettified) | 12,532 | 4,002 | 3.13× |
| Discord Small (minified) | 9,349 | 3,536 | 2.64× |
| Discord Small (prettified) | 11,472 | 3,489 | 3.29× |
| Github Events Small (minified) | 9,503 | 3,948 | 2.41× |
| Google Maps Response Small (minified) | 7,578 | 3,186 (n/c) | 2.38× |
| Github Events Small (prettified) | 13,212 | 4,022 | 3.28× |
| Google Maps Response Small (prettified) | 15,561 | 5,160 | 3.02× |
| Instruments Small (minified) | 8,905 | 3,189 (n/c) | 2.79× |
| Instruments Small (prettified) | 14,284 | 3,929 | 3.64× |
| Marine IK Reverse Small (minified) | 1,425 | 836 (n/c) | 1.70× |
| Marine IK Reverse Small (prettified) | 6,841 | 2,616 | 2.61× |
| Marine IK Small (minified) | 1,547 | 919 | 1.68× |
| Marine IK Small (prettified) | 6,819 | 2,535 | 2.69× |
| Random Small (minified) | 8,675 | 3,362 | 2.58× |
| Random Small (prettified) | 13,260 | 4,130 | 3.21× |
| Twitter Small (minified) | 10,149 | 4,613 | 2.20× |
| Twitter Small (prettified) | 9,013 | 3,031 | 2.97× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,707 | 1,768 | 1,096 | 1.53× |
| Prettify | 1,919 | 2,082 | 733 | 0.92× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,502 | 216 | 11.57× |
| Double | 336 | 302 | 1.11× |
| Int64 | 955 | 885 | 1.08× |
| String | 4,501 | 2,015 | 2.23× |
| Uint64 | 981 | 885 | 1.11× |
| Apache Builds (minified) | 16,494 | 5,342 | 3.09× |
| Canada (minified) | 1,611 | 958 | 1.68× |
| Apache Builds (prettified) | 19,108 | 5,359 | 3.57× |
| Canada (prettified) | 4,659 | 3,193 | 1.46× |
| CitmCatalog (minified) | 10,420 | 5,569 | 1.87× |
| CitmCatalog (prettified) | 17,409 | 5,730 | 3.04× |
| Discord (minified) | 14,520 | 5,881 | 2.47× |
| Discord (prettified) | 18,229 | 5,090 | 3.58× |
| Github Events (minified) | 16,868 | 5,883 | 2.87× |
| Google Maps Response (minified) | 11,946 | 3,993 | 2.99× |
| Github Events (prettified) | 19,988 | 5,116 | 3.91× |
| Google Maps Response (prettified) | 18,428 | 5,091 | 3.62× |
| Instruments (minified) | 18,618 | 5,656 | 3.29× |
| Instruments (prettified) | 22,918 | 5,779 | 3.97× |
| Marine IK Reverse (minified) | 819 | 605 | 1.35× |
| Marine IK Reverse (prettified) | 6,033 | 3,704 | 1.63× |
| Marine IK (minified) | 863 | 592 | 1.46× |
| Marine IK (prettified) | 6,160 | 2,899 | 2.12× |
| Random (minified) | 14,756 | 3,833 | 3.85× |
| Random (prettified) | 18,726 | 4,130 | 4.53× |
| Twitter (minified) | 20,115 | 7,039 | 2.86× |
| Twitter (prettified) | 21,675 | 4,701 | 4.61× |
| Apache Builds Small (minified) | 10,584 | 3,525 | 3.00× |
| Canada Small (minified) | 1,631 | 971 | 1.68× |
| Apache Builds Small (prettified) | 13,377 | 3,727 | 3.59× |
| Canada Small (prettified) | 4,945 | 3,192 | 1.55× |
| CitmCatalog Small (minified) | 7,860 | 3,062 | 2.57× |
| CitmCatalog Small (prettified) | 15,015 | 3,704 | 4.05× |
| Discord Small (minified) | 10,652 | 3,403 | 3.13× |
| Discord Small (prettified) | 14,894 | 3,180 | 4.68× |
| Github Events Small (minified) | 11,396 | 4,087 | 2.79× |
| Google Maps Response Small (minified) | 11,211 | 3,881 | 2.89× |
| Github Events Small (prettified) | 13,726 | 3,934 | 3.49× |
| Google Maps Response Small (prettified) | 21,205 | 4,837 | 4.38× |
| Instruments Small (minified) | 12,754 | 3,272 | 3.90× |
| Instruments Small (prettified) | 22,233 | 4,151 | 5.36× |
| Marine IK Reverse Small (minified) | 1,332 | 814 | 1.64× |
| Marine IK Reverse Small (prettified) | 7,601 | 3,776 | 2.01× |
| Marine IK Small (minified) | 1,419 | 810 | 1.75× |
| Marine IK Small (prettified) | 6,686 | 3,731 | 1.79× |
| Random Small (minified) | 13,216 | 3,949 | 3.35× |
| Random Small (prettified) | 21,203 | 4,213 | 5.03× |
| Twitter Small (minified) | 9,022 | 5,350 | 1.69× |
| Twitter Small (prettified) | 11,367 | 3,025 | 3.76× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,780 | 2,705 | 1,364 | 1.40× |
| Prettify | 4,887 | 3,629 | 1,487 | 1.35× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,123 | 204 | 248 | 8.55× |
| Double | 340 | 304 | 338 | 1.00× |
| Int64 | 930 | 805 | 925 | 1.01× |
| String | 3,975 | 1,994 | 3,767 | 1.06× |
| Uint64 | 962 | 859 | 915 | 1.05× |
| Apache Builds (minified) | 20,283 | 8,095 | 13,224 | 1.53× |
| Canada (minified) | 2,099 | 1,428 | 1,032 | 1.47× |
| Apache Builds (prettified) | 23,251 | 7,014 | 585 | 3.31× |
| Canada (prettified) | 6,081 | 3,241 | 385 | 1.88× |
| CitmCatalog (minified) | 12,609 | 5,997 | 11,105 | 1.14× |
| CitmCatalog (prettified) | 20,331 | 5,768 | 540 | 3.52× |
| Discord (minified) | 15,440 | 6,212 | 11,230 | 1.37× |
| Discord (prettified) | 20,128 | 6,123 | 523 | 3.29× |
| Github Events (minified) | 14,354 | 6,829 | 9,183 | 1.56× |
| Google Maps Response (minified) | 16,050 | 4,039 | 9,856 | 1.63× |
| Github Events (prettified) | 17,927 | 5,180 | 604 | 3.46× |
| Google Maps Response (prettified) | 20,998 | 6,831 | 549 | 3.07× |
| Instruments (minified) | 20,078 | 5,727 | 15,005 | 1.34× |
| Instruments (prettified) | 25,883 | 6,016 | 492 | 4.30× |
| Marine IK Reverse (minified) | 1,332 | 987 | 883 | 1.35× |
| Marine IK Reverse (prettified) | 6,288 | 3,604 | 269 | 1.74× |
| Marine IK (minified) | 1,333 | 1,004 | 890 | 1.33× |
| Marine IK (prettified) | 6,588 | 3,140 | 271 | 2.10× |
| Random (minified) | 15,562 | 4,190 | 11,344 | 1.37× |
| Random (prettified) | 19,792 | 5,089 | 539 | 3.89× |
| Twitter (minified) | 16,972 | 7,689 | 9,792 | 1.73× |
| Twitter (prettified) | 21,082 | 5,156 | 551 | 4.09× |
| Apache Builds Small (minified) | 10,586 | 4,308 | 6,703 | 1.58× |
| Canada Small (minified) | 2,088 | 1,611 | 1,070 | 1.30× |
| Apache Builds Small (prettified) | 16,398 | 4,119 | 581 | 3.98× |
| Canada Small (prettified) | 6,224 | 3,539 | 422 | 1.76× |
| CitmCatalog Small (minified) | 7,537 | 3,313 | 6,002 | 1.26× |
| CitmCatalog Small (prettified) | 15,146 | 4,057 | 528 | 3.73× |
| Discord Small (minified) | 10,418 | 3,994 | 6,705 | 1.55× |
| Discord Small (prettified) | 15,102 | 3,610 | 542 | 4.18× |
| Github Events Small (minified) | 7,907 | 5,202 | 6,044 | 1.31× |
| Google Maps Response Small (minified) | 13,125 | 3,873 | 8,107 | 1.62× |
| Github Events Small (prettified) | 10,842 | 3,853 | 557 | 2.81× |
| Google Maps Response Small (prettified) | 25,746 | 5,897 | 556 | 4.37× |
| Instruments Small (minified) | 11,906 | 4,036 | 7,865 | 1.51× |
| Instruments Small (prettified) | 20,886 | 3,609 | 538 | 5.79× |
| Marine IK Reverse Small (minified) | 1,543 | 1,216 | 1,484 | 1.04× |
| Marine IK Reverse Small (prettified) | 6,757 | 3,695 | 546 | 1.83× |
| Marine IK Small (minified) | 1,321 | 1,236 | 1,506 | 0.88× |
| Marine IK Small (prettified) | 7,118 | 3,868 | 519 | 1.84× |
| Random Small (minified) | 13,625 | 4,168 | 7,538 | 1.81× |
| Random Small (prettified) | 22,788 | 4,960 | 584 | 4.59× |
| Twitter Small (minified) | 8,657 | 5,843 | 6,342 | 1.36× |
| Twitter Small (prettified) | 11,755 | 3,793 | 575 | 3.10× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,318 | 2,417 | 1,430 | 1.37× |
| Prettify | 4,372 | 3,664 | 1,536 | 1.19× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,263 | 134 | 233 | 9.73× |
| Double | 301 | 203 | 330 | 0.91× |
| Int64 | 832 | 578 | 824 | 1.01× |
| String | 1,401 | 1,139 | 1,386 | 1.01× |
| Uint64 | 749 | 583 | 839 | 0.89× |
| Apache Builds (minified) | 13,880 | 5,275 | 10,267 | 1.35× |
| Canada (minified) | 2,627 | 1,644 | 865 | 1.60× |
| Apache Builds (prettified) | 12,808 | 4,442 | 333 | 2.88× |
| Canada (prettified) | 7,882 | 3,663 | 305 | 2.15× |
| CitmCatalog (minified) | 9,254 | 2,231 | 8,124 | 1.14× |
| CitmCatalog (prettified) | 20,342 | 4,044 | 276 | 5.03× |
| Discord (minified) | 10,967 | 2,296 | 8,218 | 1.33× |
| Discord (prettified) | 12,397 | 2,172 | 328 | 5.71× |
| Github Events (minified) | 15,646 | 3,692 | 11,606 | 1.35× |
| Google Maps Response (minified) | 10,869 | 1,746 | 8,028 | 1.35× |
| Github Events (prettified) | 15,749 | 3,173 | 434 | 4.96× |
| Google Maps Response (prettified) | 19,181 | 2,392 | 316 | 8.02× |
| Instruments (minified) | 15,697 | 1,738 | 8,501 | 1.85× |
| Instruments (prettified) | 18,065 | 2,423 | 374 | 7.45× |
| Marine IK Reverse (minified) | 1,447 | 897 | 802 | 1.61× |
| Marine IK Reverse (prettified) | 6,787 | 3,335 | 261 | 2.04× |
| Marine IK (minified) | 1,271 | 858 | 703 | 1.48× |
| Marine IK (prettified) | 7,236 | 2,817 | 261 (n/c) | 2.57× |
| Random (minified) | 11,010 | 2,271 | 8,558 | 1.29× |
| Random (prettified) | 14,828 | 2,497 | 321 | 5.94× |
| Twitter (minified) | 14,704 | 4,133 | 10,086 | 1.46× |
| Twitter (prettified) | 17,768 | 3,279 | 411 | 5.42× |
| Apache Builds Small (minified) | 7,709 | 2,359 | 5,611 | 1.37× |
| Canada Small (minified) | 1,890 | 1,531 | 761 | 1.23× |
| Apache Builds Small (prettified) | 8,336 | 3,151 | 283 | 2.65× |
| Canada Small (prettified) | 6,688 | 2,691 | 240 | 2.49× |
| CitmCatalog Small (minified) | 7,260 | 1,543 (n/c) | 6,513 (n/c) | 1.11× |
| CitmCatalog Small (prettified) | 16,974 (n/c) | 2,420 | 344 | 7.02× |
| Discord Small (minified) | 11,234 | 2,311 | 8,658 | 1.30× |
| Discord Small (prettified) | 13,202 | 2,310 | 336 | 5.72× |
| Github Events Small (minified) | 8,941 | 2,389 | 7,494 | 1.19× |
| Google Maps Response Small (minified) | 9,239 | 1,732 | 7,455 | 1.24× |
| Github Events Small (prettified) | 12,971 (n/c) | 2,413 | 393 | 5.38× |
| Google Maps Response Small (prettified) | 20,872 | 2,517 | 306 | 8.29× |
| Instruments Small (minified) | 12,164 | 1,648 | 3,380 | 3.60× |
| Instruments Small (prettified) | 14,858 | 2,156 | 336 | 6.89× |
| Marine IK Reverse Small (minified) | 1,778 | 1,094 | 1,325 | 1.34× |
| Marine IK Reverse Small (prettified) | 8,424 | 2,801 | 261 | 3.01× |
| Marine IK Small (minified) | 1,630 | 1,018 | 1,435 | 1.14× |
| Marine IK Small (prettified) | 9,114 | 3,132 | 294 | 2.91× |
| Random Small (minified) | 13,091 | 2,101 | 8,834 | 1.48× |
| Random Small (prettified) | 14,779 | 1,897 | 295 | 7.79× |
| Twitter Small (minified) | 12,742 | 3,423 | 9,769 | 1.30× |
| Twitter Small (prettified) | 14,485 | 3,277 | 363 | 4.42× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,442 | 1,669 | 880 | 1.46× |
| Prettify | 3,193 | 2,189 | 619 | 1.46× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,672 (n/c) | 126 | 13.30× |
| Double | 316 | 211 | 1.50× |
| Int64 | 2,836 | 526 | 5.39× |
| String | 943 | 1,067 | 0.88× |
| Uint64 | 3,316 | 515 | 6.44× |
| Apache Builds (minified) | 9,999 | 3,137 | 3.19× |
| Canada (minified) | 2,601 (n/c) | 1,410 (n/c) | 1.84× |
| Apache Builds (prettified) | 14,784 | 3,411 | 4.33× |
| Canada (prettified) | 8,116 | 3,126 | 2.60× |
| CitmCatalog (minified) | 7,407 | 1,769 | 4.19× |
| CitmCatalog (prettified) | 18,716 | 2,586 | 7.24× |
| Discord (minified) | 12,806 | 2,580 | 4.96× |
| Discord (prettified) | 19,424 | 2,689 | 7.22× |
| Github Events (minified) | 15,590 | 4,019 | 3.88× |
| Google Maps Response (minified) | 10,028 | 1,635 | 6.13× |
| Github Events (prettified) | 18,111 | 3,625 | 5.00× |
| Google Maps Response (prettified) | 21,538 | 2,896 | 7.44× |
| Instruments (minified) | 13,258 | 2,084 | 6.36× |
| Instruments (prettified) | 24,850 | 2,363 | 10.52× |
| Marine IK Reverse (minified) | 1,320 | 948 (n/c) | 1.39× |
| Marine IK Reverse (prettified) | 6,041 | 3,027 | 2.00× |
| Marine IK (minified) | 1,385 | 818 | 1.69× |
| Marine IK (prettified) | 7,651 | 2,723 | 2.81× |
| Random (minified) | 9,152 | 1,955 | 4.68× |
| Random (prettified) | 14,745 | 2,532 | 5.82× |
| Twitter (minified) | 13,496 | 3,729 | 3.62× |
| Twitter (prettified) | 16,267 | 3,990 | 4.08× |
| Apache Builds Small (minified) | 7,626 | 2,633 | 2.90× |
| Canada Small (minified) | 2,274 | 1,530 | 1.49× |
| Apache Builds Small (prettified) | 8,088 | 2,167 | 3.73× |
| Canada Small (prettified) | 8,126 | 2,568 | 3.16× |
| CitmCatalog Small (minified) | 6,032 | 1,697 | 3.55× |
| CitmCatalog Small (prettified) | 13,616 | 2,191 | 6.21× |
| Discord Small (minified) | 11,091 | 1,991 | 5.57× |
| Discord Small (prettified) | 11,313 | 2,092 | 5.41× |
| Github Events Small (minified) | 9,671 | 2,668 | 3.63× |
| Google Maps Response Small (minified) | 8,282 | 1,481 | 5.59× |
| Github Events Small (prettified) | 11,987 | 2,550 | 4.70× |
| Google Maps Response Small (prettified) | 18,105 | 2,316 | 7.82× |
| Instruments Small (minified) | 12,171 | 1,536 | 7.93× |
| Instruments Small (prettified) | 18,854 | 1,708 | 11.04× |
| Marine IK Reverse Small (minified) | 1,598 | 815 | 1.96× |
| Marine IK Reverse Small (prettified) | 7,347 | 2,706 | 2.71× |
| Marine IK Small (minified) | 1,791 | 992 | 1.81× |
| Marine IK Small (prettified) | 8,267 | 3,071 | 2.69× |
| Random Small (minified) | 9,229 | 1,889 | 4.89× |
| Random Small (prettified) | 12,719 | 1,770 | 7.19× |
| Twitter Small (minified) | 9,490 | 2,978 | 3.19× |
| Twitter Small (prettified) | 12,179 | 3,075 | 3.96× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,495 | 1,699 | 735 | 1.47× |
| Prettify | 3,541 | 2,200 | 615 | 1.61× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 253 of the 255 write tests and loses 2, both against the reflection writer: String on macOS / GCC, 10,091 vs 11,032 MB/s (0.91×), and minified Marine IK Small on Linux / GCC, 1,436 vs 1,517 MB/s (0.95×).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **253** | **0** | **2** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,910 | 252 | 7.58× |
| Double | 750 (n/c) | 310 | 2.42× |
| Int64 | 4,692 | 1,058 | 4.43× |
| String | 7,518 | 2,340 | 3.21× |
| Uint64 | 5,462 | 1,060 | 5.15× |
| Apache Builds (minified) | 11,350 | 5,072 | 2.24× |
| Canada (minified) | 1,537 | 985 | 1.56× |
| Apache Builds (prettified) | 13,723 | 5,653 | 2.43× |
| Canada (prettified) | 4,833 | 2,757 | 1.75× |
| CitmCatalog (minified) | 8,666 | 4,498 | 1.93× |
| CitmCatalog (prettified) | 13,573 | 7,310 | 1.86× |
| Discord (minified) | 10,716 | 4,960 (n/c) | 2.16× |
| Discord (prettified) | 13,320 | 5,055 (n/c) | 2.64× |
| Github Events (minified) | 11,858 | 6,167 | 1.92× |
| Google Maps Response (minified) | 10,171 | 3,939 | 2.58× |
| Github Events (prettified) | 14,680 | 4,133 | 3.55× |
| Google Maps Response (prettified) | 14,857 | 6,119 | 2.43× |
| Instruments (minified) | 12,788 | 4,182 | 3.06× |
| Instruments (prettified) | 14,389 | 4,948 | 2.91× |
| Marine IK Reverse (minified) | 925 | 655 | 1.41× |
| Marine IK Reverse (prettified) | 4,421 | 2,984 | 1.48× |
| Marine IK (minified) | 938 | 691 | 1.36× |
| Marine IK (prettified) | 4,353 | 2,993 | 1.45× |
| Random (minified) | 8,754 | 3,512 | 2.49× |
| Random (prettified) | 11,864 | 4,238 | 2.80× |
| Twitter (minified) | 13,041 | 4,817 | 2.71× |
| Twitter (prettified) | 13,588 | 3,670 | 3.70× |
| Apache Builds Small (minified) | 11,218 | 4,564 (n/c) | 2.46× |
| Canada Small (minified) | 1,426 (n/c) | 1,216 | 1.17× |
| Apache Builds Small (prettified) | 15,024 | 4,810 (n/c) | 3.12× |
| Canada Small (prettified) | 5,979 | 3,297 (n/c) | 1.81× |
| CitmCatalog Small (minified) | 7,881 | 4,356 | 1.81× |
| CitmCatalog Small (prettified) | 14,512 | 5,367 | 2.70× |
| Discord Small (minified) | 10,572 | 4,981 | 2.12× |
| Discord Small (prettified) | 13,528 | 4,459 (n/c) | 3.03× |
| Github Events Small (minified) | 12,409 | 5,174 (n/c) | 2.40× |
| Google Maps Response Small (minified) | 10,337 | 4,316 | 2.40× |
| Github Events Small (prettified) | 16,425 | 5,426 (n/c) | 3.03× |
| Google Maps Response Small (prettified) | 19,977 | 5,813 (n/c) | 3.44× |
| Instruments Small (minified) | 12,055 | 3,390 (n/c) | 3.56× |
| Instruments Small (prettified) | 17,910 | 5,348 | 3.35× |
| Marine IK Reverse Small (minified) | 1,530 | 1,011 | 1.51× |
| Marine IK Reverse Small (prettified) | 7,066 | 3,762 | 1.88× |
| Marine IK Small (minified) | 1,654 | 985 | 1.68× |
| Marine IK Small (prettified) | 7,459 | 3,067 | 2.43× |
| Random Small (minified) | 10,796 | 4,220 | 2.56× |
| Random Small (prettified) | 17,180 | 5,262 | 3.27× |
| Twitter Small (minified) | 11,910 | 5,434 | 2.19× |
| Twitter Small (prettified) | 12,302 | 5,477 | 2.25× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,914 | 1,719 | 981 | 1.70× |
| Prettify | 2,913 | 2,059 | 632 | 1.41× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 5,059 | 921 | 5.50× |
| Double | 920 | 684 | 1.35× |
| Int64 | 5,295 | 2,336 | 2.27× |
| String | 12,313 | 5,729 | 2.15× |
| Uint64 | 5,690 | 2,522 | 2.26× |
| Apache Builds (minified) | 16,194 | 6,191 | 2.62× |
| Canada (minified) | 1,618 | 997 | 1.62× |
| Apache Builds (prettified) | 19,189 | 5,906 | 3.25× |
| Canada (prettified) | 4,589 | 3,886 | 1.18× |
| CitmCatalog (minified) | 10,430 | 6,363 | 1.64× |
| CitmCatalog (prettified) | 17,993 | 7,856 | 2.29× |
| Discord (minified) | 14,849 | 6,232 | 2.38× |
| Discord (prettified) | 18,537 | 5,864 | 3.16× |
| Github Events (minified) | 17,121 | 7,134 | 2.40× |
| Google Maps Response (minified) | 12,347 | 4,608 | 2.68× |
| Github Events (prettified) | 19,841 | 5,864 | 3.38× |
| Google Maps Response (prettified) | 19,166 | 5,579 | 3.44× |
| Instruments (minified) | 18,599 | 6,451 | 2.88× |
| Instruments (prettified) | 23,111 | 6,885 | 3.36× |
| Marine IK Reverse (minified) | 826 | 604 | 1.37× |
| Marine IK Reverse (prettified) | 6,106 | 4,084 | 1.50× |
| Marine IK (minified) | 873 | 618 | 1.41× |
| Marine IK (prettified) | 6,168 | 4,034 | 1.53× |
| Random (minified) | 14,857 | 4,235 | 3.51× |
| Random (prettified) | 18,864 | 4,816 | 3.92× |
| Twitter (minified) | 20,070 | 7,753 | 2.59× |
| Twitter (prettified) | 21,756 | 5,933 | 3.67× |
| Apache Builds Small (minified) | 11,893 | 3,839 | 3.10× |
| Canada Small (minified) | 1,647 | 1,012 | 1.63× |
| Apache Builds Small (prettified) | 15,903 | 4,014 | 3.96× |
| Canada Small (prettified) | 5,054 | 3,696 | 1.37× |
| CitmCatalog Small (minified) | 8,685 | 3,304 | 2.63× |
| CitmCatalog Small (prettified) | 16,767 | 4,116 | 4.07× |
| Discord Small (minified) | 11,588 | 3,779 | 3.07× |
| Discord Small (prettified) | 16,511 | 3,248 | 5.08× |
| Github Events Small (minified) | 12,865 | 4,184 | 3.07× |
| Google Maps Response Small (minified) | 12,621 | 4,265 | 2.96× |
| Github Events Small (prettified) | 16,906 | 4,462 | 3.79× |
| Google Maps Response Small (prettified) | 23,448 | 5,302 | 4.42× |
| Instruments Small (minified) | 14,483 | 3,746 | 3.87× |
| Instruments Small (prettified) | 24,537 | 4,683 | 5.24× |
| Marine IK Reverse Small (minified) | 1,347 | 854 | 1.58× |
| Marine IK Reverse Small (prettified) | 8,051 | 4,270 | 1.89× |
| Marine IK Small (minified) | 1,418 | 858 | 1.65× |
| Marine IK Small (prettified) | 6,747 | 4,254 | 1.59× |
| Random Small (minified) | 14,590 | 4,324 | 3.37× |
| Random Small (prettified) | 24,074 | 4,766 | 5.05× |
| Twitter Small (minified) | 10,506 | 5,765 | 1.82× |
| Twitter Small (prettified) | 13,085 | 3,235 | 4.04× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,772 | 2,728 | 1,373 | 1.38× |
| Prettify | 4,887 | 3,658 | 1,495 | 1.34× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,227 | 673 | 2,628 | 1.23× |
| Double | 901 | 719 | 646 | 1.25× |
| Int64 | 5,411 | 2,501 | 4,179 | 1.29× |
| String | 11,035 | 6,151 | 9,188 | 1.20× |
| Uint64 | 6,317 | 2,754 | 4,390 | 1.44× |
| Apache Builds (minified) | 20,469 | 9,452 | 13,374 | 1.53× |
| Canada (minified) | 2,093 | 1,488 | 1,026 | 1.41× |
| Apache Builds (prettified) | 23,512 | 7,914 | 583 | 2.97× |
| Canada (prettified) | 6,013 | 3,867 | 383 | 1.55× |
| CitmCatalog (minified) | 12,798 | 7,077 | 11,163 | 1.15× |
| CitmCatalog (prettified) | 20,101 | 8,064 | 544 | 2.49× |
| Discord (minified) | 15,652 | 6,648 | 11,262 | 1.39× |
| Discord (prettified) | 20,409 | 7,116 | 528 | 2.87× |
| Github Events (minified) | 14,564 | 8,234 | 9,457 | 1.54× |
| Google Maps Response (minified) | 17,337 | 4,467 | 10,354 | 1.67× |
| Github Events (prettified) | 17,572 | 5,979 | 601 | 2.94× |
| Google Maps Response (prettified) | 22,502 | 7,635 | 547 | 2.95× |
| Instruments (minified) | 20,275 | 6,280 | 15,818 | 1.28× |
| Instruments (prettified) | 26,550 | 7,248 | 494 | 3.66× |
| Marine IK Reverse (minified) | 1,331 | 980 | 878 | 1.36× |
| Marine IK Reverse (prettified) | 6,257 | 4,013 | 297 | 1.56× |
| Marine IK (minified) | 1,333 | 1,046 | 886 | 1.27× |
| Marine IK (prettified) | 6,597 | 4,109 | 315 | 1.61× |
| Random (minified) | 15,558 | 4,710 | 11,380 | 1.37× |
| Random (prettified) | 19,936 | 6,070 | 541 | 3.28× |
| Twitter (minified) | 17,094 | 9,048 | 9,788 | 1.75× |
| Twitter (prettified) | 21,097 | 6,569 | 556 | 3.21× |
| Apache Builds Small (minified) | 13,769 | 4,712 | 7,600 | 1.81× |
| Canada Small (minified) | 2,125 | 1,698 | 1,080 | 1.25× |
| Apache Builds Small (prettified) | 18,130 | 4,576 | 579 | 3.96× |
| Canada Small (prettified) | 6,353 | 3,856 | 417 | 1.65× |
| CitmCatalog Small (minified) | 8,454 | 3,915 | 6,488 | 1.30× |
| CitmCatalog Small (prettified) | 16,847 | 4,457 | 521 | 3.78× |
| Discord Small (minified) | 11,613 | 4,825 | 6,932 | 1.68× |
| Discord Small (prettified) | 16,716 | 4,159 | 541 | 4.02× |
| Github Events Small (minified) | 9,519 | 6,111 | 7,022 | 1.36× |
| Google Maps Response Small (minified) | 14,797 | 4,070 | 8,856 | 1.67× |
| Github Events Small (prettified) | 13,030 | 4,867 | 557 | 2.68× |
| Google Maps Response Small (prettified) | 29,538 | 7,147 | 558 | 4.13× |
| Instruments Small (minified) | 13,328 | 4,726 | 8,148 | 1.64× |
| Instruments Small (prettified) | 23,925 | 3,970 | 536 | 6.03× |
| Marine IK Reverse Small (minified) | 1,582 | 1,359 | 1,513 | 1.05× |
| Marine IK Reverse Small (prettified) | 6,888 | 4,062 | 548 | 1.70× |
| Marine IK Small (minified) | 1,436 | 1,306 | 1,517 | 0.95× |
| Marine IK Small (prettified) | 7,239 | 4,034 | 513 | 1.79× |
| Random Small (minified) | 15,540 | 4,917 | 8,106 | 1.92× |
| Random Small (prettified) | 25,334 | 6,052 | 580 | 4.19× |
| Twitter Small (minified) | 10,020 | 6,497 | 6,720 | 1.49× |
| Twitter Small (prettified) | 13,498 | 4,575 | 574 | 2.95× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,332 | 2,406 | 1,427 | 1.38× |
| Prettify | 4,410 | 3,682 | 1,506 | 1.20× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,561 | 870 | 2,833 | 1.26× |
| Double | 804 | 676 | 752 | 1.07× |
| Int64 | 5,518 | 2,588 | 3,540 | 1.56× |
| String | 10,091 | 5,312 | 11,032 | 0.91× |
| Uint64 | 4,755 | 2,821 | 3,688 | 1.29× |
| Apache Builds (minified) | 14,390 | 6,533 | 10,460 | 1.38× |
| Canada (minified) | 2,623 | 1,758 | 865 | 1.49× |
| Apache Builds (prettified) | 12,777 | 5,038 | 367 | 2.54× |
| Canada (prettified) | 7,935 | 4,164 | 310 | 1.91× |
| CitmCatalog (minified) | 9,317 | 2,344 | 8,146 | 1.14× |
| CitmCatalog (prettified) | 21,085 | 4,170 | 301 (n/c) | 5.06× |
| Discord (minified) | 12,688 | 2,664 | 9,302 | 1.36× |
| Discord (prettified) | 14,671 | 2,178 | 332 | 6.74× |
| Github Events (minified) | 16,260 | 4,917 | 12,495 | 1.30× |
| Google Maps Response (minified) | 11,069 | 1,847 | 7,607 | 1.46× |
| Github Events (prettified) | 17,051 | 3,686 | 442 | 4.63× |
| Google Maps Response (prettified) | 18,212 | 3,211 | 319 | 5.67× |
| Instruments (minified) | 16,183 | 2,110 | 8,632 | 1.87× |
| Instruments (prettified) | 18,438 | 2,664 | 374 | 6.92× |
| Marine IK Reverse (minified) | 1,450 | 905 | 790 | 1.60× |
| Marine IK Reverse (prettified) | 6,074 | 3,455 | 281 (n/c) | 1.76× |
| Marine IK (minified) | 1,237 | 828 | 446 | 1.49× |
| Marine IK (prettified) | 7,245 | 3,246 | 262 | 2.23× |
| Random (minified) | 11,854 | 2,538 | 8,875 | 1.34× |
| Random (prettified) | 15,097 | 2,656 | 293 | 5.68× |
| Twitter (minified) | 14,995 | 4,211 | 9,813 | 1.53× |
| Twitter (prettified) | 16,194 | 4,404 | 434 | 3.68× |
| Apache Builds Small (minified) | 8,832 | 2,557 | 6,260 | 1.41× |
| Canada Small (minified) | 2,086 | 1,486 | 773 | 1.40× |
| Apache Builds Small (prettified) | 11,332 | 3,633 | 284 | 3.12× |
| Canada Small (prettified) | 6,231 | 2,916 | 300 | 2.14× |
| CitmCatalog Small (minified) | 7,442 | 1,937 (n/c) | 6,524 | 1.14× |
| CitmCatalog Small (prettified) | 16,541 | 1,952 | 302 | 8.47× |
| Discord Small (minified) | 12,524 | 2,898 | 8,836 | 1.42× |
| Discord Small (prettified) | 14,728 | 2,330 | 343 | 6.32× |
| Github Events Small (minified) | 14,620 | 3,006 | 11,434 | 1.28× |
| Google Maps Response Small (minified) | 10,551 (n/c) | 1,793 | 8,573 | 1.23× |
| Github Events Small (prettified) | 16,831 (n/c) | 2,993 | 389 | 5.62× |
| Google Maps Response Small (prettified) | 21,434 | 2,688 | 302 | 7.97× |
| Instruments Small (minified) | 13,567 (n/c) | 2,183 | 7,128 | 1.90× |
| Instruments Small (prettified) | 16,190 (n/c) | 2,061 | 342 | 7.86× |
| Marine IK Reverse Small (minified) | 1,859 | 953 | 1,335 | 1.39× |
| Marine IK Reverse Small (prettified) | 7,383 | 3,430 | 294 | 2.15× |
| Marine IK Small (minified) | 1,846 | 1,083 | 1,550 | 1.19× |
| Marine IK Small (prettified) | 9,569 | 3,752 | 291 | 2.55× |
| Random Small (minified) | 13,035 | 2,526 | 8,351 | 1.56× |
| Random Small (prettified) | 14,488 | 2,716 | 346 | 5.33× |
| Twitter Small (minified) | 14,975 | 3,973 | 10,818 | 1.38× |
| Twitter Small (prettified) | 17,430 | 3,709 | 403 | 4.70× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,990 | 1,697 | 881 | 1.17× |
| Prettify | 3,236 | 2,199 | 608 | 1.47× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,240 | 761 | 2.94× |
| Double | 770 | 712 | 1.08× |
| Int64 | 3,296 | 2,238 | 1.47× |
| String | 5,702 | 4,620 | 1.23× |
| Uint64 | 2,963 (n/c) | 2,218 | 1.34× |
| Apache Builds (minified) | 10,503 | 3,261 | 3.22× |
| Canada (minified) | 2,499 (n/c) | 1,683 | 1.48× |
| Apache Builds (prettified) | 15,040 | 3,671 | 4.10× |
| Canada (prettified) | 8,564 | 3,458 | 2.48× |
| CitmCatalog (minified) | 7,591 | 2,268 | 3.35× |
| CitmCatalog (prettified) | 18,797 | 2,836 | 6.63× |
| Discord (minified) | 13,552 | 2,870 | 4.72× |
| Discord (prettified) | 20,187 | 3,044 | 6.63× |
| Github Events (minified) | 16,584 | 4,915 | 3.37× |
| Google Maps Response (minified) | 10,398 | 1,839 | 5.66× |
| Github Events (prettified) | 19,897 | 4,516 | 4.41× |
| Google Maps Response (prettified) | 24,688 | 3,212 | 7.69× |
| Instruments (minified) | 15,010 | 2,228 | 6.74× |
| Instruments (prettified) | 25,545 | 2,598 | 9.83× |
| Marine IK Reverse (minified) | 1,343 | 919 | 1.46× |
| Marine IK Reverse (prettified) | 6,796 | 3,215 | 2.11× |
| Marine IK (minified) | 1,351 | 837 | 1.61× |
| Marine IK (prettified) | 6,702 | 3,265 | 2.05× |
| Random (minified) | 9,962 | 1,977 | 5.04× |
| Random (prettified) | 18,212 | 2,896 | 6.29× |
| Twitter (minified) | 14,363 | 3,813 | 3.77× |
| Twitter (prettified) | 17,419 | 3,951 | 4.41× |
| Apache Builds Small (minified) | 7,723 (n/c) | 2,822 (n/c) | 2.74× |
| Canada Small (minified) | 2,018 | 1,758 | 1.15× |
| Apache Builds Small (prettified) | 10,648 | 3,303 | 3.22× |
| Canada Small (prettified) | 7,790 | 3,621 | 2.15× |
| CitmCatalog Small (minified) | 8,449 | 1,878 | 4.50× |
| CitmCatalog Small (prettified) | 16,810 | 2,346 | 7.16× |
| Discord Small (minified) | 10,482 | 2,214 | 4.73× |
| Discord Small (prettified) | 14,448 | 2,020 | 7.15× |
| Github Events Small (minified) | 9,618 | 3,482 | 2.76× |
| Google Maps Response Small (minified) | 8,118 | 1,774 | 4.58× |
| Github Events Small (prettified) | 16,096 | 2,679 | 6.01× |
| Google Maps Response Small (prettified) | 21,793 | 2,673 | 8.15× |
| Instruments Small (minified) | 11,104 | 1,992 (n/c) | 5.57× |
| Instruments Small (prettified) | 27,736 | 2,031 (n/c) | 13.66× |
| Marine IK Reverse Small (minified) | 1,987 | 1,020 | 1.95× |
| Marine IK Reverse Small (prettified) | 8,506 | 3,147 | 2.70× |
| Marine IK Small (minified) | 1,517 (n/c) | 1,042 | 1.46× |
| Marine IK Small (prettified) | 8,478 | 3,614 | 2.35× |
| Random Small (minified) | 9,558 | 2,285 | 4.18× |
| Random Small (prettified) | 17,101 | 2,684 | 6.37× |
| Twitter Small (minified) | 13,480 | 3,693 | 3.65× |
| Twitter Small (prettified) | 15,259 | 4,162 | 3.67× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,507 | 1,714 | 832 | 1.46× |
| Prettify | 3,581 | 2,162 | 569 | 1.66× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,678 vs Jsonifier's 3,215 MB/s (0.69×). Reused, Jsonifier wins by 2.71× (13,041 vs 4,817).
- **Prettify, Windows / MSVC.** Glaze runs 2,082 vs 1,919 MB/s (0.92×). Reused, Jsonifier wins by 1.41× (2,913 vs 2,059).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,506 vs 1,321 MB/s (0.88×). It also wins reused, 1,517 vs 1,436 MB/s (0.95×).
- **Double, macOS / GCC.** The reflection writer runs 330 vs 301 MB/s (0.91×). Jsonifier is ahead of Glaze (203 MB/s, 1.49×), and ahead of the reflection writer reused (804 vs 752 MB/s, 1.07×).
- **Uint64, macOS / GCC.** The reflection writer runs 839 vs 749 MB/s (0.89×). Reused, Jsonifier wins by 1.29× (4,755 vs 3,688).
- **String, macOS / Clang.** Glaze runs 1,067 vs 943 MB/s (0.88×). Reused, Jsonifier wins by 1.23× (5,702 vs 4,620).

The one reused loss with no freshly allocated counterpart is String on macOS / GCC (10,091 vs 11,032 MB/s, 0.91×); freshly allocated, Jsonifier wins it (1,401 vs 1,386).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.14–3.04× and prettified documents by 16–74×. The Small documents mostly follow: on the two GCC builds Jsonifier wins 43 of the 44 Small document tests in each run, losing only minified Marine IK Small on Linux / GCC. Why the reflection writer edges ahead on these few tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.37–1.53× freshly allocated and 1.17–1.70× reused. Freshly allocated it wins Prettify on four builds, by 1.19–1.61×, and loses to Glaze on Windows / MSVC (1,919 vs 2,082 MB/s); reused it wins Prettify on all five, by 1.20–1.66×. Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,401 MB/s on macOS / GCC (1.23× Glaze's 1,139, ahead of the reflection writer's 1,386) and 943 MB/s on macOS / Clang, where Glaze is ahead at 1,067 (0.88×). The AVX2 builds run the same test at 1,190–4,501 MB/s and lead Glaze by 1.44× (Windows / MSVC), 2.23× (Linux / Clang) and 1.99× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.23× on macOS / Clang (5,702 vs 4,620 MB/s) and 1.90× on macOS / GCC (10,091 vs 5,312), against 1.79–3.21× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 11,032 MB/s.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes, and part remains in the copy loop, most visibly on macOS / Clang. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Only freshly allocated on macOS / GCC, 330 vs 301 MB/s (0.91×). On Linux / GCC freshly allocated Jsonifier is narrowly ahead (340 vs 338 MB/s, 1.00×), and reused it leads on both GCC builds, by 1.39× (901 vs 646) and 1.07× (804 vs 752). Jsonifier also beats Glaze on Double on all five builds in both runs.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** Every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`, so string-heavy documents can ask for about 6× their output size (see *Does `6n + 2` blow up memory on huge strings?*).
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
