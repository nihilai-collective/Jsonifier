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

On the Bool test this writer leads Glaze by 10.79–50.15× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.31× to 4.12×.

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

Jsonifier was fastest in 250 of the 255 write tests on five builds and lost 5. With the output string reused across iterations it was fastest in 252 of 255 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify and Prettify on every build in both runs, and every full-size document test except minified Twitter on Windows / MSVC freshly allocated. Freshly allocated, that Twitter result is the only loss to Glaze; the other four are against simdjson's reflection writer, which exists only on GCC: Uint64 and minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and Double on macOS / GCC. There are no ties in either run. Reused, all three losses are against the reflection writer: String on macOS / GCC and minified Marine IK Small and Marine IK Reverse Small on Linux / GCC. Linux / Clang and macOS / Clang win all 51 tests in both runs. Prettified documents are where the lead is widest: 1.45–10.44× over Glaze freshly allocated across the five builds, and 16–60× over the reflection writer on the two GCC builds.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 28-29, 2026, with Jsonifier [9f4382a](https://github.com/nihilai-collective/jsonifier/commit/9f4382a) and BenchmarkSuite [35ec835](https://github.com/nihilai-collective/benchmarksuite/commit/35ec835) on every build, Glaze [f67610b](https://github.com/stephenberry/glaze/commit/f67610b) ([8ae6d0d](https://github.com/stephenberry/glaze/commit/8ae6d0d) on the two macOS builds), and simdjson [41ad75c](https://github.com/simdjson/simdjson/commit/41ad75c) on Linux, [9262482](https://github.com/simdjson/simdjson/commit/9262482) under MSVC and [ab504c9](https://github.com/simdjson/simdjson/commit/ab504c9) on macOS. Sampling and tie rules match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.51 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

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
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **250** | **0** | **5** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,194 | 24 (n/c) | 50.15× |
| Double | 471 | 151 | 3.13× |
| Int64 | 503 | 373 | 1.35× |
| String | 1,453 | 901 (n/c) | 1.61× |
| Uint64 | 527 | 380 | 1.39× |
| Apache Builds (minified) | 12,363 | 4,028 (n/c) | 3.07× |
| Canada (minified) | 654 | 446 | 1.46× |
| Apache Builds (prettified) | 14,280 | 4,633 | 3.08× |
| Canada (prettified) | 1,359 (n/c) | 764 | 1.78× |
| CitmCatalog (minified) | 4,321 (n/c) | 2,119 (n/c) | 2.04× |
| CitmCatalog (prettified) | 2,200 | 904 | 2.43× |
| Discord (minified) | 10,611 | 3,989 | 2.66× |
| Discord (prettified) | 7,127 (n/c) | 3,875 | 1.84× |
| Github Events (minified) | 11,487 | 4,525 | 2.54× |
| Google Maps Response (minified) | 9,486 | 3,386 (n/c) | 2.80× |
| Github Events (prettified) | 13,693 | 1,508 (n/c) | 9.08× |
| Google Maps Response (prettified) | 13,839 | 4,812 | 2.88× |
| Instruments (minified) | 13,486 | 4,356 | 3.10× |
| Instruments (prettified) | 18,450 | 1,893 (n/c) | 9.75× |
| Marine IK Reverse (minified) | 422 | 349 | 1.21× |
| Marine IK Reverse (prettified) | 1,506 | 943 (n/c) | 1.60× |
| Marine IK (minified) | 431 | 301 | 1.44× |
| Marine IK (prettified) | 3,125 | 1,354 | 2.31× |
| Random (minified) | 3,713 | 2,987 | 1.24× |
| Random (prettified) | 4,153 | 2,154 | 1.93× |
| Twitter (minified) | 3,904 | 4,975 | 0.78× |
| Twitter (prettified) | 4,166 | 1,686 | 2.47× |
| Apache Builds Small (minified) | 9,526 | 3,393 | 2.81× |
| Canada Small (minified) | 1,800 | 1,188 | 1.52× |
| Apache Builds Small (prettified) | 11,758 | 3,862 | 3.04× |
| Canada Small (prettified) | 4,916 | 2,991 | 1.64× |
| CitmCatalog Small (minified) | 6,732 | 3,140 | 2.14× |
| CitmCatalog Small (prettified) | 12,389 | 3,726 | 3.33× |
| Discord Small (minified) | 9,502 | 3,601 | 2.64× |
| Discord Small (prettified) | 11,354 | 2,779 (n/c) | 4.09× |
| Github Events Small (minified) | 9,763 | 3,955 | 2.47× |
| Google Maps Response Small (minified) | 8,929 | 3,512 | 2.54× |
| Github Events Small (prettified) | 12,676 | 3,714 | 3.41× |
| Google Maps Response Small (prettified) | 15,956 | 4,887 | 3.27× |
| Instruments Small (minified) | 10,427 | 3,295 (n/c) | 3.16× |
| Instruments Small (prettified) | 16,348 | 3,190 | 5.12× |
| Marine IK Reverse Small (minified) | 1,454 | 920 | 1.58× |
| Marine IK Reverse Small (prettified) | 6,613 | 2,692 | 2.46× |
| Marine IK Small (minified) | 1,537 | 914 | 1.68× |
| Marine IK Small (prettified) | 6,780 | 2,730 | 2.48× |
| Random Small (minified) | 9,272 | 3,482 | 2.66× |
| Random Small (prettified) | 14,710 | 3,808 | 3.86× |
| Twitter Small (minified) | 9,684 | 4,197 | 2.31× |
| Twitter Small (prettified) | 10,048 | 3,298 | 3.05× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,959 | 1,978 | 1,100 | 1.50× |
| Prettify | 2,449 | 2,149 | 848 | 1.14× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,482 | 208 | 11.96× |
| Double | 340 | 303 | 1.12× |
| Int64 | 931 | 878 | 1.06× |
| String | 4,443 | 1,972 | 2.25× |
| Uint64 | 930 | 887 | 1.05× |
| Apache Builds (minified) | 16,504 | 5,315 | 3.11× |
| Canada (minified) | 1,601 | 956 | 1.67× |
| Apache Builds (prettified) | 19,034 | 5,282 | 3.60× |
| Canada (prettified) | 4,682 | 3,218 | 1.45× |
| CitmCatalog (minified) | 10,278 | 5,625 | 1.83× |
| CitmCatalog (prettified) | 17,437 | 5,668 | 3.08× |
| Discord (minified) | 13,812 | 6,030 | 2.29× |
| Discord (prettified) | 17,571 | 5,100 | 3.45× |
| Github Events (minified) | 16,838 | 6,112 | 2.76× |
| Google Maps Response (minified) | 11,348 | 3,973 | 2.86× |
| Github Events (prettified) | 19,436 | 5,051 | 3.85× |
| Google Maps Response (prettified) | 18,685 | 5,196 | 3.60× |
| Instruments (minified) | 18,731 | 5,380 | 3.48× |
| Instruments (prettified) | 23,061 | 5,609 | 4.11× |
| Marine IK Reverse (minified) | 853 | 614 | 1.39× |
| Marine IK Reverse (prettified) | 5,758 | 3,661 | 1.57× |
| Marine IK (minified) | 856 | 585 | 1.46× |
| Marine IK (prettified) | 6,009 | 2,782 | 2.16× |
| Random (minified) | 14,045 | 3,748 | 3.75× |
| Random (prettified) | 18,175 | 4,069 | 4.47× |
| Twitter (minified) | 19,442 | 6,359 | 3.06× |
| Twitter (prettified) | 20,583 | 4,830 | 4.26× |
| Apache Builds Small (minified) | 10,051 | 3,523 | 2.85× |
| Canada Small (minified) | 1,542 | 913 | 1.69× |
| Apache Builds Small (prettified) | 13,791 | 3,495 | 3.95× |
| Canada Small (prettified) | 4,621 | 3,437 | 1.34× |
| CitmCatalog Small (minified) | 7,420 | 3,088 | 2.40× |
| CitmCatalog Small (prettified) | 14,410 | 3,912 | 3.68× |
| Discord Small (minified) | 10,149 | 3,521 | 2.88× |
| Discord Small (prettified) | 14,576 | 3,360 | 4.34× |
| Github Events Small (minified) | 10,404 | 4,156 | 2.50× |
| Google Maps Response Small (minified) | 10,947 | 3,834 | 2.86× |
| Github Events Small (prettified) | 13,190 | 3,803 | 3.47× |
| Google Maps Response Small (prettified) | 20,262 | 4,729 | 4.28× |
| Instruments Small (minified) | 12,655 | 3,276 | 3.86× |
| Instruments Small (prettified) | 21,413 | 3,943 | 5.43× |
| Marine IK Reverse Small (minified) | 1,317 | 808 | 1.63× |
| Marine IK Reverse Small (prettified) | 6,943 | 3,955 | 1.76× |
| Marine IK Small (minified) | 1,400 | 815 | 1.72× |
| Marine IK Small (prettified) | 7,463 | 3,647 | 2.05× |
| Random Small (minified) | 12,098 | 3,806 | 3.18× |
| Random Small (prettified) | 20,595 | 4,089 | 5.04× |
| Twitter Small (minified) | 8,841 | 5,215 | 1.70× |
| Twitter Small (prettified) | 11,486 | 3,024 | 3.80× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,556 | 2,546 | 1,363 | 1.40× |
| Prettify | 4,834 | 3,624 | 1,484 | 1.33× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,195 | 203 | 249 | 8.82× |
| Double | 342 | 303 | 339 | 1.01× |
| Int64 | 963 | 830 | 950 | 1.01× |
| String | 4,016 | 2,044 | 3,772 | 1.06× |
| Uint64 | 907 | 859 | 920 | 0.99× |
| Apache Builds (minified) | 20,270 | 7,972 | 13,340 | 1.52× |
| Canada (minified) | 2,145 | 1,442 | 1,036 | 1.49× |
| Apache Builds (prettified) | 23,228 | 7,073 | 595 | 3.28× |
| Canada (prettified) | 6,107 | 3,220 | 385 | 1.90× |
| CitmCatalog (minified) | 12,725 | 4,993 | 11,090 | 1.15× |
| CitmCatalog (prettified) | 20,318 | 5,788 | 549 | 3.51× |
| Discord (minified) | 15,107 | 6,167 | 11,333 | 1.33× |
| Discord (prettified) | 20,027 | 6,034 | 542 | 3.32× |
| Github Events (minified) | 14,275 | 7,041 | 9,512 | 1.50× |
| Google Maps Response (minified) | 16,275 | 4,004 | 10,097 | 1.61× |
| Github Events (prettified) | 17,780 | 5,121 | 608 | 3.47× |
| Google Maps Response (prettified) | 20,848 | 6,757 | 530 | 3.09× |
| Instruments (minified) | 20,038 | 5,664 | 15,682 | 1.28× |
| Instruments (prettified) | 25,924 | 6,099 | 498 | 4.25× |
| Marine IK Reverse (minified) | 1,299 | 974 | 880 | 1.33× |
| Marine IK Reverse (prettified) | 6,400 | 3,695 | 270 | 1.73× |
| Marine IK (minified) | 1,302 | 924 | 880 | 1.41× |
| Marine IK (prettified) | 6,327 | 2,981 | 274 | 2.12× |
| Random (minified) | 15,541 | 4,158 | 11,197 | 1.39× |
| Random (prettified) | 19,184 | 5,128 | 538 | 3.74× |
| Twitter (minified) | 17,446 | 7,605 | 9,601 | 1.82× |
| Twitter (prettified) | 21,130 | 5,020 | 550 | 4.21× |
| Apache Builds Small (minified) | 10,964 | 4,296 | 6,606 | 1.66× |
| Canada Small (minified) | 1,782 | 1,507 | 1,055 | 1.18× |
| Apache Builds Small (prettified) | 12,888 | 4,057 | 578 | 3.18× |
| Canada Small (prettified) | 5,583 | 3,402 | 419 | 1.64× |
| CitmCatalog Small (minified) | 7,666 | 3,399 | 5,941 | 1.29× |
| CitmCatalog Small (prettified) | 15,524 | 3,782 | 539 | 4.10× |
| Discord Small (minified) | 10,654 | 4,095 | 6,766 | 1.57× |
| Discord Small (prettified) | 15,391 | 3,926 | 532 | 3.92× |
| Github Events Small (minified) | 8,604 | 5,088 | 5,923 | 1.45× |
| Google Maps Response Small (minified) | 13,397 | 3,897 | 7,967 | 1.68× |
| Github Events Small (prettified) | 10,524 | 4,071 | 556 | 2.59× |
| Google Maps Response Small (prettified) | 23,498 | 5,576 | 528 | 4.21× |
| Instruments Small (minified) | 12,235 | 3,872 | 7,916 | 1.55× |
| Instruments Small (prettified) | 21,444 | 3,711 | 537 | 5.78× |
| Marine IK Reverse Small (minified) | 1,284 | 1,133 | 1,479 | 0.87× |
| Marine IK Reverse Small (prettified) | 6,449 | 3,842 | 538 | 1.68× |
| Marine IK Small (minified) | 1,346 | 1,151 | 1,463 | 0.92× |
| Marine IK Small (prettified) | 6,303 | 3,654 | 501 | 1.72× |
| Random Small (minified) | 12,985 | 4,192 | 7,559 | 1.72× |
| Random Small (prettified) | 21,503 | 4,817 | 569 | 4.46× |
| Twitter Small (minified) | 8,542 | 5,675 | 6,038 | 1.41× |
| Twitter Small (prettified) | 12,447 | 3,901 | 555 | 3.19× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,255 | 2,388 | 1,382 | 1.36× |
| Prettify | 4,223 | 3,668 | 1,449 | 1.15× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,169 | 93 | 197 | 11.03× |
| Double | 243 | 183 | 284 | 0.85× |
| Int64 | 780 | 579 | 654 | 1.19× |
| String | 1,236 | 882 | 1,094 | 1.13× |
| Uint64 | 804 | 512 | 672 (n/c) | 1.20× |
| Apache Builds (minified) | 12,878 | 5,100 | 9,540 | 1.35× |
| Canada (minified) | 2,093 | 1,483 | 735 | 1.41× |
| Apache Builds (prettified) | 14,480 | 4,714 | 388 | 3.07× |
| Canada (prettified) | 7,284 | 3,159 (n/c) | 254 | 2.31× |
| CitmCatalog (minified) | 8,519 | 2,225 | 7,575 | 1.12× |
| CitmCatalog (prettified) | 18,885 | 4,047 | 317 | 4.67× |
| Discord (minified) | 12,488 | 2,923 | 9,720 | 1.28× |
| Discord (prettified) | 13,988 | 2,411 | 362 | 5.80× |
| Github Events (minified) | 16,437 | 3,958 | 11,760 | 1.40× |
| Google Maps Response (minified) | 11,160 | 1,559 (n/c) | 7,723 | 1.45× |
| Github Events (prettified) | 14,943 | 3,125 | 421 | 4.78× |
| Google Maps Response (prettified) | 17,634 | 2,776 | 351 | 6.35× |
| Instruments (minified) | 14,532 | 1,933 | 7,938 | 1.83× |
| Instruments (prettified) | 16,520 | 2,032 | 333 | 8.13× |
| Marine IK Reverse (minified) | 1,255 | 841 | 727 | 1.49× |
| Marine IK Reverse (prettified) | 6,677 | 2,664 | 291 (n/c) | 2.51× |
| Marine IK (minified) | 1,457 | 845 | 633 | 1.72× |
| Marine IK (prettified) | 6,125 | 2,593 | 258 (n/c) | 2.36× |
| Random (minified) | 10,899 | 2,051 | 7,786 | 1.40× |
| Random (prettified) | 14,382 | 2,260 | 358 | 6.36× |
| Twitter (minified) | 14,166 | 3,636 | 11,291 | 1.25× |
| Twitter (prettified) | 16,837 | 3,206 | 436 | 5.25× |
| Apache Builds Small (minified) | 8,870 | 2,639 | 6,851 | 1.29× |
| Canada Small (minified) | 2,378 | 1,665 | 843 | 1.43× |
| Apache Builds Small (prettified) | 10,067 | 3,680 | 351 | 2.74× |
| Canada Small (prettified) | 6,971 | 2,746 | 254 | 2.54× |
| CitmCatalog Small (minified) | 8,130 | 1,766 | 6,963 | 1.17× |
| CitmCatalog Small (prettified) | 15,211 | 2,410 | 333 | 6.31× |
| Discord Small (minified) | 11,103 | 2,566 | 7,987 | 1.39× |
| Discord Small (prettified) | 12,846 | 2,244 | 348 | 5.72× |
| Github Events Small (minified) | 10,386 | 2,868 | 9,471 | 1.10× |
| Google Maps Response Small (minified) | 10,035 | 1,662 | 7,782 | 1.29× |
| Github Events Small (prettified) | 13,895 | 2,667 | 400 | 5.21× |
| Google Maps Response Small (prettified) | 19,745 | 2,634 | 317 | 7.50× |
| Instruments Small (minified) | 13,104 | 1,837 | 3,203 | 4.09× |
| Instruments Small (prettified) | 16,740 | 2,188 | 351 | 7.65× |
| Marine IK Reverse Small (minified) | 1,799 | 1,009 | 1,415 | 1.27× |
| Marine IK Reverse Small (prettified) | 8,051 | 2,847 | 266 | 2.83× |
| Marine IK Small (minified) | 1,749 | 909 | 1,451 | 1.21× |
| Marine IK Small (prettified) | 8,040 | 3,010 | 279 | 2.67× |
| Random Small (minified) | 11,261 | 1,822 | 7,714 | 1.46× |
| Random Small (prettified) | 16,201 | 2,247 | 330 | 7.21× |
| Twitter Small (minified) | 11,102 | 2,536 | 7,583 (n/c) | 1.46× |
| Twitter Small (prettified) | 10,519 | 2,403 | 329 | 4.38× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,873 | 1,360 | 783 | 1.38× |
| Prettify | 2,821 | 2,009 | 645 | 1.40× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,049 | 135 | 15.20× |
| Double | 316 | 228 | 1.39× |
| Int64 | 3,454 | 560 | 6.16× |
| String | 1,394 | 1,167 | 1.19× |
| Uint64 | 3,524 | 565 | 6.24× |
| Apache Builds (minified) | 12,226 | 3,483 | 3.51× |
| Canada (minified) | 3,012 | 1,888 | 1.60× |
| Apache Builds (prettified) | 16,970 | 3,715 | 4.57× |
| Canada (prettified) | 8,499 | 3,490 | 2.44× |
| CitmCatalog (minified) | 8,398 | 2,153 | 3.90× |
| CitmCatalog (prettified) | 20,117 | 3,216 | 6.25× |
| Discord (minified) | 10,952 | 2,574 | 4.26× |
| Discord (prettified) | 19,125 | 2,675 | 7.15× |
| Github Events (minified) | 15,456 | 4,021 | 3.84× |
| Google Maps Response (minified) | 10,474 | 1,829 | 5.73× |
| Github Events (prettified) | 18,081 | 3,636 | 4.97× |
| Google Maps Response (prettified) | 21,595 | 2,922 | 7.39× |
| Instruments (minified) | 14,132 | 2,076 | 6.81× |
| Instruments (prettified) | 24,905 | 2,386 | 10.44× |
| Marine IK Reverse (minified) | 1,542 | 947 | 1.63× |
| Marine IK Reverse (prettified) | 7,460 | 3,422 | 2.18× |
| Marine IK (minified) | 1,556 | 928 | 1.68× |
| Marine IK (prettified) | 7,608 | 3,065 | 2.48× |
| Random (minified) | 11,085 | 2,324 | 4.77× |
| Random (prettified) | 14,552 | 2,676 | 5.44× |
| Twitter (minified) | 16,216 | 4,194 | 3.87× |
| Twitter (prettified) | 18,290 | 4,173 | 4.38× |
| Apache Builds Small (minified) | 8,699 | 2,699 | 3.22× |
| Canada Small (minified) | 2,766 | 1,649 | 1.68× |
| Apache Builds Small (prettified) | 11,631 | 3,086 | 3.77× |
| Canada Small (prettified) | 8,075 | 3,376 | 2.39× |
| CitmCatalog Small (minified) | 8,146 | 1,913 | 4.26× |
| CitmCatalog Small (prettified) | 16,141 | 2,472 | 6.53× |
| Discord Small (minified) | 11,250 | 2,444 | 4.60× |
| Discord Small (prettified) | 16,705 | 2,577 | 6.48× |
| Github Events Small (minified) | 10,216 | 2,847 | 3.59× |
| Google Maps Response Small (minified) | 9,705 | 1,806 | 5.37× |
| Github Events Small (prettified) | 12,659 | 2,800 | 4.52× |
| Google Maps Response Small (prettified) | 21,748 | 2,769 | 7.85× |
| Instruments Small (minified) | 13,870 | 1,988 | 6.98× |
| Instruments Small (prettified) | 25,721 | 2,428 | 10.60× |
| Marine IK Reverse Small (minified) | 2,027 | 1,095 | 1.85× |
| Marine IK Reverse Small (prettified) | 8,758 | 3,233 | 2.71× |
| Marine IK Small (minified) | 2,003 | 1,070 | 1.87× |
| Marine IK Small (prettified) | 9,049 | 3,257 | 2.78× |
| Random Small (minified) | 10,486 | 2,138 | 4.91× |
| Random Small (prettified) | 14,346 | 2,497 | 5.75× |
| Twitter Small (minified) | 10,458 | 3,287 | 3.18× |
| Twitter Small (prettified) | 13,378 | 3,371 | 3.97× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,781 | 1,862 | 907 | 1.49× |
| Prettify | 3,903 | 2,405 | 699 | 1.62× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 252 of the 255 write tests and loses 3, all against the reflection writer: String on macOS / GCC, 8,433 vs 10,365 MB/s (0.81×), and minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, 1,355 vs 1,491 MB/s (0.91×) and 1,295 vs 1,486 MB/s (0.87×).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **252** | **0** | **3** |

#### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,803 | 295 | 6.12× |
| Double | 779 | 384 | 2.03× |
| Int64 | 4,170 | 1,062 | 3.93× |
| String | 7,982 | 2,286 | 3.49× |
| Uint64 | 4,511 | 1,094 | 4.12× |
| Apache Builds (minified) | 12,708 | 4,328 | 2.94× |
| Canada (minified) | 787 | 615 | 1.28× |
| Apache Builds (prettified) | 14,447 | 2,508 (n/c) | 5.76× |
| Canada (prettified) | 2,371 (n/c) | 1,686 (n/c) | 1.41× |
| CitmCatalog (minified) | 8,366 | 2,413 (n/c) | 3.47× |
| CitmCatalog (prettified) | 7,168 | 3,384 (n/c) | 2.12× |
| Discord (minified) | 11,326 | 4,692 | 2.41× |
| Discord (prettified) | 13,753 | 4,497 (n/c) | 3.06× |
| Github Events (minified) | 12,115 | 2,778 (n/c) | 4.36× |
| Google Maps Response (minified) | 10,358 | 4,184 | 2.48× |
| Github Events (prettified) | 14,438 | 1,836 (n/c) | 7.86× |
| Google Maps Response (prettified) | 15,234 | 6,007 | 2.54× |
| Instruments (minified) | 14,427 | 4,709 (n/c) | 3.06× |
| Instruments (prettified) | 18,571 | 2,180 | 8.52× |
| Marine IK Reverse (minified) | 473 | 393 | 1.20× |
| Marine IK Reverse (prettified) | 2,339 | 1,731 | 1.35× |
| Marine IK (minified) | 501 | 388 | 1.29× |
| Marine IK (prettified) | 4,927 | 3,485 | 1.41× |
| Random (minified) | 11,102 | 3,352 | 3.31× |
| Random (prettified) | 13,965 | 4,238 | 3.30× |
| Twitter (minified) | 15,064 | 5,696 | 2.64× |
| Twitter (prettified) | 15,066 | 3,085 | 4.88× |
| Apache Builds Small (minified) | 11,250 | 4,137 | 2.72× |
| Canada Small (minified) | 1,910 | 1,281 | 1.49× |
| Apache Builds Small (prettified) | 14,567 | 4,349 | 3.35× |
| Canada Small (prettified) | 5,324 | 3,301 | 1.61× |
| CitmCatalog Small (minified) | 7,543 | 3,687 | 2.05× |
| CitmCatalog Small (prettified) | 13,687 | 4,604 | 2.97× |
| Discord Small (minified) | 10,403 | 4,419 | 2.35× |
| Discord Small (prettified) | 12,471 | 3,095 | 4.03× |
| Github Events Small (minified) | 13,314 | 5,439 | 2.45× |
| Google Maps Response Small (minified) | 9,732 | 4,015 | 2.42× |
| Github Events Small (prettified) | 15,888 | 3,709 | 4.28× |
| Google Maps Response Small (prettified) | 19,842 | 6,024 | 3.29× |
| Instruments Small (minified) | 11,830 | 3,904 (n/c) | 3.03× |
| Instruments Small (prettified) | 17,847 | 3,076 | 5.80× |
| Marine IK Reverse Small (minified) | 1,495 | 988 | 1.51× |
| Marine IK Reverse Small (prettified) | 6,867 | 3,137 | 2.19× |
| Marine IK Small (minified) | 1,577 | 999 | 1.58× |
| Marine IK Small (prettified) | 7,129 | 3,174 | 2.25× |
| Random Small (minified) | 11,728 | 3,879 | 3.02× |
| Random Small (prettified) | 18,013 | 5,073 | 3.55× |
| Twitter Small (minified) | 12,204 | 5,298 | 2.30× |
| Twitter Small (prettified) | 12,339 | 4,372 | 2.82× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,971 | 1,957 | 1,092 | 1.52× |
| Prettify | 3,867 | 2,132 | 843 | 1.81× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 5,203 | 871 | 5.98× |
| Double | 908 | 658 | 1.38× |
| Int64 | 5,336 | 2,389 | 2.23× |
| String | 11,642 | 5,687 | 2.05× |
| Uint64 | 5,619 | 2,589 | 2.17× |
| Apache Builds (minified) | 16,335 | 5,941 | 2.75× |
| Canada (minified) | 1,603 | 992 | 1.62× |
| Apache Builds (prettified) | 19,195 | 5,806 | 3.31× |
| Canada (prettified) | 4,720 | 3,881 | 1.22× |
| CitmCatalog (minified) | 10,340 | 6,570 | 1.57× |
| CitmCatalog (prettified) | 17,534 | 7,827 | 2.24× |
| Discord (minified) | 14,035 | 6,206 | 2.26× |
| Discord (prettified) | 17,981 | 5,796 | 3.10× |
| Github Events (minified) | 16,921 | 7,511 | 2.25× |
| Google Maps Response (minified) | 12,338 | 4,471 | 2.76× |
| Github Events (prettified) | 20,135 | 5,613 | 3.59× |
| Google Maps Response (prettified) | 19,441 | 5,654 | 3.44× |
| Instruments (minified) | 18,709 | 6,197 | 3.02× |
| Instruments (prettified) | 23,211 | 6,652 | 3.49× |
| Marine IK Reverse (minified) | 854 | 609 | 1.40× |
| Marine IK Reverse (prettified) | 5,778 | 4,028 | 1.43× |
| Marine IK (minified) | 862 | 618 | 1.39× |
| Marine IK (prettified) | 6,008 | 4,008 | 1.50× |
| Random (minified) | 14,153 | 4,039 | 3.50× |
| Random (prettified) | 17,967 | 4,726 | 3.80× |
| Twitter (minified) | 19,605 | 7,378 | 2.66× |
| Twitter (prettified) | 20,759 | 5,844 | 3.55× |
| Apache Builds Small (minified) | 11,563 | 3,672 | 3.15× |
| Canada Small (minified) | 1,553 | 993 | 1.56× |
| Apache Builds Small (prettified) | 16,424 | 3,814 | 4.31× |
| Canada Small (prettified) | 4,191 | 3,833 | 1.09× |
| CitmCatalog Small (minified) | 8,419 | 3,310 | 2.54× |
| CitmCatalog Small (prettified) | 16,156 | 4,267 | 3.79× |
| Discord Small (minified) | 11,324 | 3,853 | 2.94× |
| Discord Small (prettified) | 16,140 | 3,747 | 4.31× |
| Github Events Small (minified) | 12,978 | 4,557 | 2.85× |
| Google Maps Response Small (minified) | 12,087 | 3,918 | 3.09× |
| Github Events Small (prettified) | 17,320 | 4,275 | 4.05× |
| Google Maps Response Small (prettified) | 22,958 | 5,308 | 4.33× |
| Instruments Small (minified) | 14,124 | 3,752 | 3.76× |
| Instruments Small (prettified) | 23,499 | 4,436 | 5.30× |
| Marine IK Reverse Small (minified) | 1,356 | 840 | 1.61× |
| Marine IK Reverse Small (prettified) | 7,027 | 4,364 | 1.61× |
| Marine IK Small (minified) | 1,432 | 780 | 1.84× |
| Marine IK Small (prettified) | 7,450 | 3,973 | 1.88× |
| Random Small (minified) | 13,678 | 4,140 | 3.30× |
| Random Small (prettified) | 22,919 | 4,720 | 4.86× |
| Twitter Small (minified) | 10,618 | 6,023 | 1.76× |
| Twitter Small (prettified) | 13,170 | 3,723 | 3.54× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,580 | 2,565 | 1,363 | 1.40× |
| Prettify | 4,852 | 3,625 | 1,464 | 1.34× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,280 | 678 | 2,477 | 1.32× |
| Double | 824 | 641 | 645 | 1.28× |
| Int64 | 5,521 | 2,531 | 4,217 | 1.31× |
| String | 11,418 | 6,215 | 9,313 | 1.23× |
| Uint64 | 6,211 | 2,753 | 4,357 | 1.43× |
| Apache Builds (minified) | 20,550 | 9,527 | 13,319 | 1.54× |
| Canada (minified) | 2,146 | 1,513 | 1,034 | 1.42× |
| Apache Builds (prettified) | 23,371 | 7,955 | 591 | 2.94× |
| Canada (prettified) | 6,103 | 3,842 | 380 | 1.59× |
| CitmCatalog (minified) | 12,814 | 5,183 | 11,202 | 1.14× |
| CitmCatalog (prettified) | 20,358 | 8,038 | 550 | 2.53× |
| Discord (minified) | 15,322 | 6,820 | 11,489 | 1.33× |
| Discord (prettified) | 20,265 | 7,381 | 536 | 2.75× |
| Github Events (minified) | 14,601 | 8,191 | 9,720 | 1.50× |
| Google Maps Response (minified) | 17,763 | 4,505 | 10,312 | 1.72× |
| Github Events (prettified) | 18,186 | 6,117 | 607 | 2.97× |
| Google Maps Response (prettified) | 22,053 | 7,541 | 526 | 2.92× |
| Instruments (minified) | 20,253 | 6,666 | 15,381 | 1.32× |
| Instruments (prettified) | 25,956 | 7,259 | 495 | 3.58× |
| Marine IK Reverse (minified) | 1,280 | 977 | 881 | 1.31× |
| Marine IK Reverse (prettified) | 6,403 | 4,073 | 305 | 1.57× |
| Marine IK (minified) | 1,302 | 976 | 880 | 1.33× |
| Marine IK (prettified) | 6,313 | 4,031 | 310 | 1.57× |
| Random (minified) | 15,551 | 4,698 | 11,346 | 1.37× |
| Random (prettified) | 19,519 | 6,173 | 538 | 3.16× |
| Twitter (minified) | 17,623 | 9,014 | 9,637 | 1.83× |
| Twitter (prettified) | 21,118 | 6,646 | 548 | 3.18× |
| Apache Builds Small (minified) | 13,327 | 4,928 | 7,419 | 1.80× |
| Canada Small (minified) | 1,857 | 1,689 | 1,061 | 1.10× |
| Apache Builds Small (prettified) | 15,231 | 4,381 | 577 | 3.48× |
| Canada Small (prettified) | 5,815 | 3,937 | 419 | 1.48× |
| CitmCatalog Small (minified) | 8,583 | 3,803 | 6,414 | 1.34× |
| CitmCatalog Small (prettified) | 16,782 | 4,244 | 539 | 3.95× |
| Discord Small (minified) | 11,632 | 4,912 | 7,137 | 1.63× |
| Discord Small (prettified) | 16,904 | 4,420 | 532 | 3.82× |
| Github Events Small (minified) | 10,032 | 5,944 | 6,810 | 1.47× |
| Google Maps Response Small (minified) | 14,832 | 4,197 | 8,477 | 1.75× |
| Github Events Small (prettified) | 12,866 | 4,901 | 552 | 2.63× |
| Google Maps Response Small (prettified) | 26,790 | 6,911 | 527 | 3.88× |
| Instruments Small (minified) | 14,057 | 4,584 | 8,446 | 1.66× |
| Instruments Small (prettified) | 23,912 | 4,081 | 536 | 5.86× |
| Marine IK Reverse Small (minified) | 1,295 | 1,269 | 1,486 | 0.87× |
| Marine IK Reverse Small (prettified) | 6,461 | 4,064 | 537 | 1.59× |
| Marine IK Small (minified) | 1,355 | 1,215 | 1,491 | 0.91× |
| Marine IK Small (prettified) | 6,564 | 3,880 | 500 | 1.69× |
| Random Small (minified) | 14,396 | 4,682 | 8,108 | 1.78× |
| Random Small (prettified) | 24,632 | 5,698 | 571 | 4.32× |
| Twitter Small (minified) | 9,851 | 6,733 | 6,744 | 1.46× |
| Twitter Small (prettified) | 13,613 | 4,725 | 554 | 2.88× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,249 | 2,393 | 1,379 | 1.36× |
| Prettify | 4,277 | 3,673 | 1,429 | 1.16× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,447 | 661 | 2,532 | 1.36× |
| Double | 753 | 600 | 653 | 1.15× |
| Int64 | 5,333 | 2,485 | 3,183 | 1.68× |
| String | 8,433 | 4,104 | 10,365 | 0.81× |
| Uint64 | 5,215 | 2,826 | 3,292 | 1.58× |
| Apache Builds (minified) | 11,995 | 6,189 | 8,092 | 1.48× |
| Canada (minified) | 2,131 | 1,473 | 716 | 1.45× |
| Apache Builds (prettified) | 14,901 | 5,531 | 386 | 2.69× |
| Canada (prettified) | 6,541 | 3,817 | 287 (n/c) | 1.71× |
| CitmCatalog (minified) | 8,287 | 2,159 | 8,032 | 1.03× |
| CitmCatalog (prettified) | 18,587 | 3,767 | 320 | 4.93× |
| Discord (minified) | 14,054 | 3,237 | 10,002 | 1.41× |
| Discord (prettified) | 14,203 | 2,324 | 344 | 6.11× |
| Github Events (minified) | 14,681 | 4,865 | 12,498 | 1.17× |
| Google Maps Response (minified) | 10,747 | 1,661 | 9,319 | 1.15× |
| Github Events (prettified) | 15,991 | 3,625 | 409 | 4.41× |
| Google Maps Response (prettified) | 23,647 | 2,947 | 352 | 8.03× |
| Instruments (minified) | 14,417 | 2,080 | 8,313 | 1.73× |
| Instruments (prettified) | 17,073 | 2,211 | 287 | 7.72× |
| Marine IK Reverse (minified) | 1,195 | 806 | 724 | 1.48× |
| Marine IK Reverse (prettified) | 6,394 | 2,953 | 292 | 2.17× |
| Marine IK (minified) | 1,287 | 829 | 444 | 1.55× |
| Marine IK (prettified) | 6,352 | 2,875 | 269 | 2.21× |
| Random (minified) | 10,912 | 2,131 | 8,055 | 1.35× |
| Random (prettified) | 14,525 | 2,542 | 322 | 5.71× |
| Twitter (minified) | 15,950 | 3,704 | 9,645 | 1.65× |
| Twitter (prettified) | 17,517 | 4,149 | 453 | 4.22× |
| Apache Builds Small (minified) | 9,680 | 3,143 | 7,399 | 1.31× |
| Canada Small (minified) | 2,310 | 1,851 | 937 | 1.25× |
| Apache Builds Small (prettified) | 12,997 | 4,650 | 304 | 2.80× |
| Canada Small (prettified) | 7,183 | 3,503 | 275 | 2.05× |
| CitmCatalog Small (minified) | 9,623 | 1,948 | 7,092 | 1.36× |
| CitmCatalog Small (prettified) | 16,747 | 2,162 | 328 | 7.74× |
| Discord Small (minified) | 13,155 | 2,614 | 8,963 | 1.47× |
| Discord Small (prettified) | 14,263 | 2,457 | 349 | 5.80× |
| Github Events Small (minified) | 13,685 | 3,512 | 10,223 | 1.34× |
| Google Maps Response Small (minified) | 10,512 | 1,816 | 7,629 | 1.38× |
| Github Events Small (prettified) | 14,478 | 3,080 | 391 | 4.70× |
| Google Maps Response Small (prettified) | 21,776 | 2,897 | 314 | 7.52× |
| Instruments Small (minified) | 15,754 | 2,147 | 7,845 | 2.01× |
| Instruments Small (prettified) | 17,740 | 2,377 | 335 | 7.46× |
| Marine IK Reverse Small (minified) | 1,932 | 994 | 1,521 | 1.27× |
| Marine IK Reverse Small (prettified) | 7,703 | 3,431 | 276 | 2.24× |
| Marine IK Small (minified) | 1,786 | 1,008 | 1,590 | 1.12× |
| Marine IK Small (prettified) | 8,919 | 2,910 | 289 | 3.06× |
| Random Small (minified) | 10,363 (n/c) | 1,793 | 8,319 | 1.25× |
| Random Small (prettified) | 13,538 | 2,476 | 281 | 5.47× |
| Twitter Small (minified) | 12,276 | 3,406 | 8,277 | 1.48× |
| Twitter Small (prettified) | 11,352 | 2,863 | 337 | 3.97× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,852 | 1,263 | 732 | 1.47× |
| Prettify | 2,590 | 2,044 | 611 | 1.27× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 3,056 | 763 | 4.00× |
| Double | 926 | 706 | 1.31× |
| Int64 | 3,494 | 2,320 | 1.51× |
| String | 8,241 | 4,789 | 1.72× |
| Uint64 | 4,024 | 2,423 | 1.66× |
| Apache Builds (minified) | 12,663 | 3,928 | 3.22× |
| Canada (minified) | 3,026 | 1,953 | 1.55× |
| Apache Builds (prettified) | 17,543 | 4,168 | 4.21× |
| Canada (prettified) | 8,510 | 3,946 | 2.16× |
| CitmCatalog (minified) | 8,317 | 2,269 | 3.66× |
| CitmCatalog (prettified) | 20,174 | 3,613 | 5.58× |
| Discord (minified) | 10,014 | 2,855 | 3.51× |
| Discord (prettified) | 19,814 | 2,975 | 6.66× |
| Github Events (minified) | 16,312 | 4,862 | 3.36× |
| Google Maps Response (minified) | 10,854 | 1,970 | 5.51× |
| Github Events (prettified) | 19,913 | 4,540 | 4.39× |
| Google Maps Response (prettified) | 24,685 | 3,202 | 7.71× |
| Instruments (minified) | 15,001 | 2,232 | 6.72× |
| Instruments (prettified) | 25,532 | 2,570 | 9.93× |
| Marine IK Reverse (minified) | 1,544 | 946 | 1.63× |
| Marine IK Reverse (prettified) | 7,420 | 3,543 | 2.09× |
| Marine IK (minified) | 1,564 | 949 | 1.65× |
| Marine IK (prettified) | 7,576 | 3,528 | 2.15× |
| Random (minified) | 11,172 | 2,465 | 4.53× |
| Random (prettified) | 17,983 | 2,868 | 6.27× |
| Twitter (minified) | 16,455 | 4,637 | 3.55× |
| Twitter (prettified) | 18,509 | 4,830 | 3.83× |
| Apache Builds Small (minified) | 10,216 | 3,207 | 3.19× |
| Canada Small (minified) | 2,848 | 1,898 | 1.50× |
| Apache Builds Small (prettified) | 13,568 | 3,537 | 3.84× |
| Canada Small (prettified) | 8,443 | 3,920 | 2.15× |
| CitmCatalog Small (minified) | 9,691 | 2,211 | 4.38× |
| CitmCatalog Small (prettified) | 18,922 | 2,858 | 6.62× |
| Discord Small (minified) | 12,474 | 2,944 | 4.24× |
| Discord Small (prettified) | 18,399 | 2,789 | 6.60× |
| Github Events Small (minified) | 13,871 | 3,827 | 3.62× |
| Google Maps Response Small (minified) | 10,392 | 1,956 | 5.31× |
| Github Events Small (prettified) | 16,841 | 3,483 | 4.83× |
| Google Maps Response Small (prettified) | 23,519 | 3,152 | 7.46× |
| Instruments Small (minified) | 15,861 | 2,264 | 7.01× |
| Instruments Small (prettified) | 27,769 | 2,629 | 10.56× |
| Marine IK Reverse Small (minified) | 2,086 | 1,185 | 1.76× |
| Marine IK Reverse Small (prettified) | 9,345 | 3,874 | 2.41× |
| Marine IK Small (minified) | 2,105 | 1,183 | 1.78× |
| Marine IK Small (prettified) | 9,675 | 3,884 | 2.49× |
| Random Small (minified) | 11,446 | 2,487 | 4.60× |
| Random Small (prettified) | 18,489 | 2,899 | 6.38× |
| Twitter Small (minified) | 14,213 | 4,257 | 3.34× |
| Twitter Small (prettified) | 16,694 | 4,338 | 3.85× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,818 | 1,862 | 901 | 1.51× |
| Prettify | 3,904 | 2,421 | 722 | 1.61× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,975 vs Jsonifier's 3,904 MB/s (0.78×). Reused, Jsonifier wins by 2.64× (15,064 vs 5,696).
- **Uint64, Linux / GCC.** The reflection writer runs 920 vs 907 MB/s (0.99×). Reused, Jsonifier wins by 1.43× (6,211 vs 4,357).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,463 vs 1,346 MB/s (0.92×). It also wins reused, 1,491 vs 1,355 MB/s (0.91×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,479 vs 1,284 MB/s (0.87×). It also wins reused, 1,486 vs 1,295 MB/s (0.87×).
- **Double, macOS / GCC.** The reflection writer runs 284 vs 243 MB/s (0.85×). Jsonifier is ahead of Glaze (183 MB/s, 1.33×), and ahead of the reflection writer reused (753 vs 653 MB/s, 1.15×).

The one reused loss with no freshly allocated counterpart is String on macOS / GCC (8,433 vs 10,365 MB/s, 0.81×); freshly allocated, Jsonifier wins it (1,236 vs 1,094).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.12–2.85× and prettified documents by 16–60×. The Small documents mostly follow: on the two GCC builds Jsonifier wins 42 of the 44 Small document tests in each run, losing only minified Marine IK Small and Marine IK Reverse Small on Linux / GCC. Why the reflection writer edges ahead on these few tests has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.36–1.50× freshly allocated and 1.36–1.52× reused, and wins Prettify on all five in both runs, by 1.14–1.62× freshly allocated and 1.16–1.81× reused. Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,236 MB/s on macOS / GCC (1.40× Glaze's 882, ahead of the reflection writer's 1,094) and 1,394 MB/s on macOS / Clang (1.19× Glaze's 1,167). The AVX2 builds run the same test at 1,453–4,443 MB/s and lead Glaze by 1.61× (Windows / MSVC), 2.25× (Linux / Clang) and 1.96× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.72× on macOS / Clang (8,241 vs 4,789 MB/s) and 2.05× on macOS / GCC (8,433 vs 4,104), against 1.84–3.49× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 10,365 MB/s.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes, and part remains in the copy loop, most visibly against the reflection writer on macOS / GCC. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Only freshly allocated on macOS / GCC, 284 vs 243 MB/s (0.85×). On Linux / GCC freshly allocated Jsonifier is narrowly ahead (342 vs 339 MB/s, 1.01×), and reused it leads on both GCC builds, by 1.28× (824 vs 645) and 1.15× (753 vs 653). Jsonifier also beats Glaze on Double on all five builds in both runs.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** Every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`, so string-heavy documents can ask for about 6× their output size (see *Does `6n + 2` blow up memory on huge strings?*).
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
