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
    D --> E["Member headers<br/>one packed store each"]
    D --> F["Strings<br/>SIMD, SWAR, scalar"]
    D --> G["Integers<br/>4 digits per store"]
    D --> H["Floats<br/>zmij shortest round-trip"]
```

The `serialize<options>` dispatcher is a force-inlined (`JSONIFIER_INLINE`) forwarder for every value type: it strips cv and reference qualifiers and calls that type's `serialize_impl` specialization directly. Each registered type gets its own `serialize_impl` specialization: objects, maps, vectors, fixed arrays, tuples, strings, chars, enums, numbers, bools, pointers and optionals, raw JSON, null, and variants (via `visit`). Every `impl` is force-inlined; the only outlined (`JSONIFIER_NOINLINE`) calls are the object and array cases of raw JSON.

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
	JSONIFIER_INLINE uint64_t operator()(char* ptrNew, uint64_t) noexcept {
		const write_buffer_ptr start = std::bit_cast<write_buffer_ptr>(ptrNew);
		return static_cast<uint64_t>(serialize<options>::impl(object, start, 0) - start);
	}

	JSONIFIER_INLINE serialize_writer_ro(value_type& __restrict objectNew) noexcept : object{ objectNew } {
	}

	serialize_writer_ro& operator=(const serialize_writer_ro&) noexcept = delete;
	serialize_writer_ro& operator=(serialize_writer_ro&&) noexcept		= delete;
	serialize_writer_ro(const serialize_writer_ro&) noexcept			= delete;
	serialize_writer_ro(serialize_writer_ro&&) noexcept					= delete;

	value_type& __restrict object;
};
```

- **`operator()` is force-inlined.** `JSONIFIER_INLINE` makes the compiler expand the serializer into the standard library's call site, instead of leaving an out-of-line function that the library calls. The write pass then runs as part of `resize_and_overwrite`'s own body, with the buffer pointer in a register and no extra call frame. Whether `resize_and_overwrite` itself inlines into `serializeJson` is still the compiler's choice.
- **The functor is a reference and a function.** It holds only `value_type& object`, and its copy and move operations are deleted. `resize_and_overwrite` takes `op` by value, which compiles only because `serializeJson` passes a prvalue: C++17's guaranteed copy elision constructs the functor directly in the parameter, so passing it costs nothing and the object is never copied.
- **The length comes straight from the pointer.** `operator()` returns the end pointer minus the start, so the final size is set from the write itself, with no second pass and no separate size variable.

Buffers without `resize_and_overwrite` take a fallback: `resize` to the bound if the buffer is smaller, write, then `resize` down to the written length. That path zero-fills on growth.

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD register, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

### Schema text as packed integers

`packed_blitter` turns `"key":` into one constant at compile time. Literals up to 8 bytes become a single 2, 4 or 8-byte integer; longer ones become an array of `uint64_t` rounded up to 16 or a multiple of 32. Writing a member header is one `pow2MemcpyWrapper` store and a pointer bump by the true length.

### Member iteration: CABERIHT

Object members are walked with CABERIHT, Constexpresh Aggregated Bases for Efficient Runtime Iteration of Heterogeneous Types. It is CAFBERIHT without the filter; the full pattern, with compile-time filtering, is described in [its own paper](https://nihilai-collective.net/cafberiht) [1]. Each member becomes a base class, and one fold expression over those bases writes the whole object in declaration order.

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
| Compile-time 2 / 4 / 8 bytes of constant data | 2 or 4 bytes: one immediate store, no load (`mov dword ptr [rdi], imm32`); 8 bytes: one `movabs` of the constant into a register, then one `mov` store (a single `mov qword ptr [rdi], imm32` only when the value fits a sign-extended 32-bit immediate) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers across every member write of the object.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Runtime lengths stay short.** Copies whose length is only known at run time, such as the runs that Minify and Prettify carry over, go through `jsonifierMemcpy` (`utility.hpp`). It sorts the length into a power-of-2 bucket `b`, where `b ≤ length < 2b`, and copies the first `b` bytes and the last `b` bytes. The two copies overlap, so any length from 2 to 255 bytes takes one fixed-size copy pair (a single byte takes one store), with no loop and no tail. Buckets wider than the vector register are split into register-width moves: the 128-byte bucket is 8 moves on AVX2, 4 on AVX-512 and 16 on SSE or NEON. Only lengths of 256 bytes or more reach `std::memcpy`. `jsonifierMemcpyUpTo<maxBytes>` takes a compile-time upper bound and leaves out every bucket above it, so a caller whose length is bounded never pays for the cases it cannot hit.
- **The bucket is chosen by a compare tree, not a jump table.** `dispatchBuckets` is a balanced binary tree of `length < 2^k` tests generated at compile time, so the bucket is found in at most four conditional branches on the length itself, with no `lzcnt` and no indirect jump. A switch on `std::bit_width(length)` compiled to a jump table on GCC and lost to glibc's `memcpy`; the tree compiles to the same branch-only shape on GCC and Clang, and in the [memcpy-decomp](https://github.com/nihilai-collective/memcpy-decomp) [2] harness it beats `std::memcpy` on every range tested, from 1–15 to 1–129 bytes, on all five platforms.
- **Inlining and scheduling.** Each copy is one or two instructions, so a whole object's serializer inlines into straight-line code. The compiler can then interleave stores from neighbouring members with the value work between them.

**What actually reaches `jsonifierMemcpy`.** We counted the length of every call to `jsonifierMemcpy` and `jsonifierMemcpyUpTo`, separately, over one instrumented run of the unit tests in `unit-tests/parsing_tests.hpp`. The counters were a temporary patch and are not in the library.

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

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through `jsonifierMemcpy` rather than `pow2MemcpyWrapper`. That is why they appear in the counts above, but `lengthToCopy` is still a compile-time constant, so the force-inlined call folds its length dispatch away. When serializing an object, the copies whose length is known only at runtime are off the common path: the clean runs and escape sequences of a string block that holds an escapable byte (see *Strings*), raw JSON passthrough, and the memset for indents deeper than 8 levels. The Minify and Prettify reformatters are the exception: they copy each run of input between structural characters at its runtime length, through `jsonifierMemcpy`, as their common path.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test this writer leads Glaze by 7.42–13.29× freshly allocated on all five builds.

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

So a 20-digit `uint64_t` is two 10^8 splits (into 4, 8 and 8 digits), two 10^4 splits (one per 8-digit block) and five 4-byte stores, with no loop and no per-digit work. A 3-digit group, as at the front of a 19-digit value, is one 4-byte table load followed by a 2-byte and a 1-byte store, so nothing is written past the 3 digits.

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.37× to 2.63×.

### Floats

Doubles go through `jsonifier::internal::write`, a shortest-round-trip formatter vendored in `d_to_str.hpp`.

## Minified and prettified output

`serialize_options` carries `prettify`, `indentSize` (default 3), `indentChar` (default space) and `prescanStrings` (default false) as template parameters, so the two modes compile to separate code with no runtime mode check.

| Piece | Minified | Prettified |
| --- | --- | --- |
| Member header | `"key":` packed store | `"key": ` packed store |
| Separator | one `,` byte | `indent_table<",\n">` blit |
| Open / close | one `{` / `}` byte | `"{\n"` + indent, `"\n"` + indent, then `}` |
| Size estimate per member | `key + 3` | `key + 4`, plus `2 + indent` per separator |

`indent_table` prebuilds, at compile time, the prefix (`,\n`, `{\n` or `\n`) followed by 8 levels of indent characters as a packed `uint64_t` array. A newline plus indent is a short run of 8-byte stores from that table, rounded up into the slack. Nesting deeper than 8 levels falls back to `memset` for the remainder, marked `[[unlikely]]`.

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3].

## Performance results

Jsonifier was fastest in 246 of the 255 write tests on five builds, tied 1 and lost 8. With the output string reused across iterations it was fastest in 251 of 255 and lost 4. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other six are against simdjson's reflection writer, which exists only on GCC: Double, Int64, minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, and Double and Int64 on macOS / GCC. The freshly allocated tie is also against the reflection writer: minified CitmCatalog Small on macOS / GCC. Reused, the one loss to Glaze is prettified Marine IK Reverse on Windows / MSVC; against the reflection writer, minified Marine IK Reverse Small and minified Marine IK Small on Linux / GCC and String on macOS / GCC are losses. Linux / Clang and macOS / Clang win all 51 tests in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.25–7.63× across the five builds and the reflection writer by 16–61× on the two GCC builds; Small documents lead them by 1.38–9.34× and 11.59–59.38×. Three AVX-512 builds are reported separately, in *AVX-512 builds* below: there Jsonifier was fastest in 150 of 153 write tests freshly allocated and 151 of 153 reused.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) [4] sweep published November 17, 2026, with Jsonifier [79b3e67](https://github.com/nihilai-collective/jsonifier/commit/79b3e67) [5], Glaze [a53ddd59](https://github.com/stephenberry/glaze/commit/a53ddd59) [6], simdjson [36c5b3cfc](https://github.com/simdjson/simdjson/commit/36c5b3cfc) [7] and BenchmarkSuite [71afc62](https://github.com/nihilai-collective/benchmarksuite/commit/71afc62) [8] on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3]. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-43 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-43 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.52 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size. Ratios are computed from unrounded throughput, so one recomputed from the rounded values shown can differ in the last digit.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M2 Pro. Non-converged results are included in the rankings and in the win, tie and loss counts. Each one is marked n/c in the tables below, and a test with a non-converged result should be read with reduced statistical confidence.

**Ranking.** Libraries are ordered by throughput. Variance is Bessel-corrected, and two neighbouring libraries tie when Welch's t-test (two-sided, p < 0.05) on their per-call times cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. The one tie in this sweep is at the top. Because the verdict is a significance test and not a ratio threshold, a lead of a few percent with tight samples is decisive: on Linux / GCC, Bool reused (3,243 vs the reflection writer's 3,142 MB/s) is a win of 1.03×, while a 9% lead with RSEs above 3% can still be a tie (see *Losses and ties*). Because the order follows MB/s, which credits each library its own output size, a library that is faster per call can still rank below one that writes more bytes. Double on Linux / GCC is such a case: the reflection writer emits 1,997 bytes against Jsonifier's 1,811 and leads in MB/s (356 vs 335), while Jsonifier takes less time per call (5,154 vs 5,354 ns). It counts as a loss.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 47 | 0 | 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 48 | 1 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **246** | **1** | **8** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,121 | 87 | 12.95× |
| Double | 417 | 146 | 2.86× |
| Int64 | 398 | 381 | 1.05× |
| String | 1,164 | 887 | 1.31× |
| Uint64 | 407 | 368 | 1.11× |
| Apache Builds (minified) | 12,237 | 4,117 | 2.97× |
| Canada (minified) | 1,177 | 854 | 1.38× |
| Apache Builds (prettified) | 12,222 | 4,572 | 2.67× |
| Canada (prettified) | 2,685 | 1,422 | 1.89× |
| CitmCatalog (minified) | 8,617 | 4,183 | 2.06× |
| CitmCatalog (prettified) | 4,430 | 1,935 | 2.29× |
| Discord (minified) | 11,191 | 4,143 | 2.70× |
| Discord (prettified) | 11,833 | 3,836 | 3.08× |
| Github Events (minified) | 11,472 | 5,378 | 2.13× |
| Google Maps Response (minified) | 9,233 | 3,388 | 2.73× |
| Github Events (prettified) | 10,878 | 3,336 | 3.26× |
| Google Maps Response (prettified) | 14,676 | 4,959 | 2.96× |
| Instruments (minified) | 13,441 | 3,986 | 3.37× |
| Instruments (prettified) | 13,608 | 4,428 | 3.07× |
| Marine IK Reverse (minified) | 733 | 663 | 1.11× |
| Marine IK Reverse (prettified) | 2,207 | 1,759 | 1.25× |
| Marine IK (minified) | 735 | 588 | 1.25× |
| Marine IK (prettified) | 2,469 | 1,278 | 1.93× |
| Random (minified) | 3,776 | 3,018 | 1.25× |
| Random (prettified) | 4,043 | 2,276 | 1.78× |
| Twitter (minified) | 3,891 | 4,451 | 0.87× |
| Twitter (prettified) | 3,836 | 1,691 | 2.27× |
| Apache Builds Small (minified) | 9,188 | 3,489 | 2.63× |
| Canada Small (minified) | 1,532 | 1,150 | 1.33× |
| Apache Builds Small (prettified) | 9,918 | 3,906 | 2.54× |
| Canada Small (prettified) | 4,564 | 2,811 | 1.62× |
| CitmCatalog Small (minified) | 7,041 | 3,235 | 2.18× |
| CitmCatalog Small (prettified) | 7,301 | 4,261 | 1.71× |
| Discord Small (minified) | 9,208 | 3,494 | 2.64× |
| Discord Small (prettified) | 8,193 | 3,612 | 2.27× |
| Github Events Small (minified) | 9,139 | 3,901 | 2.34× |
| Google Maps Response Small (minified) | 8,673 | 3,425 | 2.53× |
| Github Events Small (prettified) | 9,615 | 3,460 | 2.78× |
| Google Maps Response Small (prettified) | 12,936 | 4,594 | 2.82× |
| Instruments Small (minified) | 10,479 | 3,832 | 2.73× |
| Instruments Small (prettified) | 10,603 | 3,355 | 3.16× |
| Marine IK Reverse Small (minified) | 1,106 | 906 | 1.22× |
| Marine IK Reverse Small (prettified) | 4,114 | 2,933 | 1.40× |
| Marine IK Small (minified) | 976 | 919 | 1.06× |
| Marine IK Small (prettified) | 4,356 | 3,034 | 1.44× |
| Random Small (minified) | 8,696 | 3,316 | 2.62× |
| Random Small (prettified) | 10,883 | 3,963 | 2.75× |
| Twitter Small (minified) | 10,062 | 4,613 | 2.18× |
| Twitter Small (prettified) | 8,191 | 3,148 | 2.60× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,169 | 1,766 | 1,453 | 1.79× |
| Prettify | 2,406 | 2,555 | 1,452 | 0.94× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,197 | 221 | 9.94× |
| Double | 336 | 301 | 1.12× |
| Int64 | 935 | 882 | 1.06× |
| String | 4,302 | 1,919 | 2.24× |
| Uint64 | 954 | 885 | 1.08× |
| Apache Builds (minified) | 16,397 | 5,704 | 2.87× |
| Canada (minified) | 1,964 | 947 | 2.07× |
| Apache Builds (prettified) | 19,041 | 5,313 | 3.58× |
| Canada (prettified) | 5,183 | 3,230 | 1.60× |
| CitmCatalog (minified) | 10,386 | 5,055 | 2.05× |
| CitmCatalog (prettified) | 15,328 | 5,287 | 2.90× |
| Discord (minified) | 14,612 | 4,958 | 2.95× |
| Discord (prettified) | 16,495 | 4,073 | 4.05× |
| Github Events (minified) | 16,107 | 5,342 | 3.02× |
| Google Maps Response (minified) | 11,542 | 3,864 | 2.99× |
| Github Events (prettified) | 16,607 | 4,545 | 3.65× |
| Google Maps Response (prettified) | 15,682 | 5,244 | 2.99× |
| Instruments (minified) | 18,189 | 4,692 | 3.88× |
| Instruments (prettified) | 20,217 | 4,880 | 4.14× |
| Marine IK Reverse (minified) | 1,165 | 643 | 1.81× |
| Marine IK Reverse (prettified) | 5,020 | 3,605 | 1.39× |
| Marine IK (minified) | 1,162 | 636 | 1.83× |
| Marine IK (prettified) | 5,049 | 2,890 | 1.75× |
| Random (minified) | 14,575 | 3,572 | 4.08× |
| Random (prettified) | 15,799 | 3,918 | 4.03× |
| Twitter (minified) | 19,611 | 6,308 | 3.11× |
| Twitter (prettified) | 18,170 | 4,706 | 3.86× |
| Apache Builds Small (minified) | 10,406 | 3,457 | 3.01× |
| Canada Small (minified) | 1,651 | 971 | 1.70× |
| Apache Builds Small (prettified) | 10,281 | 3,641 | 2.82× |
| Canada Small (prettified) | 5,326 | 3,274 | 1.63× |
| CitmCatalog Small (minified) | 7,420 | 2,932 | 2.53× |
| CitmCatalog Small (prettified) | 9,915 | 3,423 | 2.90× |
| Discord Small (minified) | 9,570 | 2,728 | 3.51× |
| Discord Small (prettified) | 10,906 | 2,780 | 3.92× |
| Github Events Small (minified) | 9,580 | 3,772 | 2.54× |
| Google Maps Response Small (minified) | 10,412 | 3,675 | 2.83× |
| Github Events Small (prettified) | 10,442 | 3,483 | 3.00× |
| Google Maps Response Small (prettified) | 15,566 | 4,768 | 3.27× |
| Instruments Small (minified) | 11,878 | 2,466 | 4.82× |
| Instruments Small (prettified) | 14,031 | 2,818 | 4.98× |
| Marine IK Reverse Small (minified) | 1,470 | 828 | 1.78× |
| Marine IK Reverse Small (prettified) | 5,383 | 3,537 | 1.52× |
| Marine IK Small (minified) | 1,378 | 807 | 1.71× |
| Marine IK Small (prettified) | 5,248 | 3,269 | 1.61× |
| Random Small (minified) | 12,911 | 3,220 | 4.01× |
| Random Small (prettified) | 14,900 | 3,839 | 3.88× |
| Twitter Small (minified) | 8,462 | 4,231 | 2.00× |
| Twitter Small (prettified) | 9,645 | 3,131 | 3.08× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,683 | 2,648 | 1,750 | 1.39× |
| Prettify | 4,598 | 3,511 | 2,305 | 1.31× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,751 | 197 | 260 | 6.73× |
| Double | 335 | 319 | 356 | 0.94× |
| Int64 | 948 | 931 | 997 | 0.95× |
| String | 4,299 | 2,117 | 4,091 | 1.05× |
| Uint64 | 951 | 840 | 903 | 1.05× |
| Apache Builds (minified) | 20,278 | 7,824 | 13,408 | 1.51× |
| Canada (minified) | 2,105 | 1,449 | 1,046 | 1.45× |
| Apache Builds (prettified) | 23,330 | 7,234 | 621 | 3.22× |
| Canada (prettified) | 6,111 | 3,294 | 275 | 1.86× |
| CitmCatalog (minified) | 12,453 | 5,913 | 11,200 | 1.11× |
| CitmCatalog (prettified) | 20,133 | 5,692 | 566 | 3.54× |
| Discord (minified) | 15,132 | 6,357 | 13,055 | 1.16× |
| Discord (prettified) | 20,192 | 5,914 | 546 | 3.41× |
| Github Events (minified) | 14,700 | 7,376 | 11,061 | 1.33× |
| Google Maps Response (minified) | 15,683 | 3,655 | 10,306 | 1.52× |
| Github Events (prettified) | 18,539 | 5,244 | 634 | 3.54× |
| Google Maps Response (prettified) | 20,763 | 6,507 | 530 | 3.19× |
| Instruments (minified) | 19,973 | 5,611 | 15,058 | 1.33× |
| Instruments (prettified) | 25,985 | 5,390 | 464 | 4.82× |
| Marine IK Reverse (minified) | 1,298 | 982 | 883 | 1.32× |
| Marine IK Reverse (prettified) | 6,320 | 3,636 | 272 | 1.74× |
| Marine IK (minified) | 1,294 | 958 | 889 | 1.35× |
| Marine IK (prettified) | 6,236 | 2,978 | 394 | 2.09× |
| Random (minified) | 17,956 | 4,606 | 11,361 | 1.58× |
| Random (prettified) | 20,479 | 4,862 | 531 | 4.21× |
| Twitter (minified) | 17,909 | 7,721 | 11,034 | 1.62× |
| Twitter (prettified) | 21,221 | 5,177 | 521 | 4.10× |
| Apache Builds Small (minified) | 12,081 | 4,585 | 7,240 | 1.67× |
| Canada Small (minified) | 1,974 | 1,563 | 1,067 | 1.26× |
| Apache Builds Small (prettified) | 14,267 | 4,211 | 545 | 3.39× |
| Canada Small (prettified) | 5,315 | 3,586 | 411 | 1.48× |
| CitmCatalog Small (minified) | 7,704 | 3,384 | 5,912 | 1.30× |
| CitmCatalog Small (prettified) | 15,000 | 2,981 | 504 | 5.03× |
| Discord Small (minified) | 10,095 | 4,005 | 7,821 | 1.29× |
| Discord Small (prettified) | 14,446 | 3,409 | 503 | 4.24× |
| Github Events Small (minified) | 8,542 | 5,279 | 6,465 | 1.32× |
| Google Maps Response Small (minified) | 13,269 | 3,383 | 7,999 | 1.66× |
| Github Events Small (prettified) | 11,096 | 3,807 | 547 | 2.91× |
| Google Maps Response Small (prettified) | 24,008 | 5,814 | 520 | 4.13× |
| Instruments Small (minified) | 11,948 | 3,572 | 7,831 | 1.53× |
| Instruments Small (prettified) | 20,767 | 3,254 | 509 | 6.38× |
| Marine IK Reverse Small (minified) | 1,451 | 1,194 | 1,507 | 0.96× |
| Marine IK Reverse Small (prettified) | 6,015 | 3,359 | 519 | 1.79× |
| Marine IK Small (minified) | 1,418 | 1,236 | 1,539 | 0.92× |
| Marine IK Small (prettified) | 6,461 | 3,458 | 530 | 1.87× |
| Random Small (minified) | 13,650 | 4,212 | 8,136 | 1.68× |
| Random Small (prettified) | 25,479 | 3,660 | 527 | 6.96× |
| Twitter Small (minified) | 8,932 | 5,498 | 6,786 | 1.32× |
| Twitter Small (prettified) | 12,410 | 3,697 | 558 | 3.36× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,211 | 2,548 | 1,888 | 1.26× |
| Prettify | 4,144 | 3,171 | 2,441 | 1.31× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,605 | 121 | 192 | 8.35× |
| Double | 243 | 211 | 311 | 0.78× |
| Int64 | 657 | 455 | 727 | 0.90× |
| String | 1,269 | 1,045 | 1,089 | 1.17× |
| Uint64 | 746 | 501 | 675 | 1.11× |
| Apache Builds (minified) | 10,194 | 5,096 | 8,832 | 1.15× |
| Canada (minified) | 2,183 | 1,499 | 865 | 1.46× |
| Apache Builds (prettified) | 12,527 | 4,259 | 337 | 2.94× |
| Canada (prettified) | 7,987 | 3,286 | 271 | 2.43× |
| CitmCatalog (minified) | 8,670 | 2,231 | 7,145 | 1.21× |
| CitmCatalog (prettified) | 17,916 | 3,704 | 338 | 4.84× |
| Discord (minified) | 11,927 | 2,520 | 9,781 | 1.22× |
| Discord (prettified) | 13,839 | 2,039 | 319 | 6.79× |
| Github Events (minified) | 14,202 | 3,443 | 10,581 | 1.34× |
| Google Maps Response (minified) | 10,266 | 1,509 | 7,986 | 1.29× |
| Github Events (prettified) | 14,790 | 3,046 | 382 | 4.86× |
| Google Maps Response (prettified) | 18,608 | 2,601 | 305 | 7.15× |
| Instruments (minified) | 14,975 | 2,009 | 8,526 | 1.76× |
| Instruments (prettified) | 17,817 | 2,335 | 337 | 7.63× |
| Marine IK Reverse (minified) | 1,344 | 946 | 735 | 1.42× |
| Marine IK Reverse (prettified) | 6,457 | 3,004 (n/c) | 275 | 2.15× |
| Marine IK (minified) | 1,315 | 852 | 747 | 1.54× |
| Marine IK (prettified) | 6,557 | 2,567 | 273 | 2.55× |
| Random (minified) | 11,576 | 2,222 | 8,108 | 1.43× |
| Random (prettified) | 15,211 | 2,519 | 338 | 6.04× |
| Twitter (minified) | 12,642 | 3,492 | 9,385 | 1.35× |
| Twitter (prettified) | 17,622 | 3,096 | 384 | 5.69× |
| Apache Builds Small (minified) | 9,559 | 2,861 | 7,176 (n/c) | 1.33× |
| Canada Small (minified) | 2,254 | 1,557 | 802 | 1.45× |
| Apache Builds Small (prettified) | 9,516 (n/c) | 3,810 | 324 | 2.50× |
| Canada Small (prettified) | 7,228 | 2,776 | 243 | 2.60× |
| CitmCatalog Small (minified) | 8,223 | 1,802 | 7,545 | 1.09× |
| CitmCatalog Small (prettified) | 16,376 | 2,666 | 325 | 6.14× |
| Discord Small (minified) | 11,427 | 2,493 | 8,316 | 1.37× |
| Discord Small (prettified) | 12,337 (n/c) | 2,045 | 334 | 6.03× |
| Github Events Small (minified) | 8,460 | 2,873 | 7,512 (n/c) | 1.13× |
| Google Maps Response Small (minified) | 10,693 | 1,809 | 8,329 | 1.28× |
| Github Events Small (prettified) | 12,695 (n/c) | 2,895 | 339 | 4.38× |
| Google Maps Response Small (prettified) | 17,582 | 2,751 | 296 | 6.39× |
| Instruments Small (minified) | 13,075 | 1,826 | 6,127 | 2.13× |
| Instruments Small (prettified) | 15,997 | 1,712 | 294 | 9.34× |
| Marine IK Reverse Small (minified) | 1,813 | 1,096 | 1,539 | 1.18× |
| Marine IK Reverse Small (prettified) | 7,768 | 2,947 | 294 | 2.64× |
| Marine IK Small (minified) | 1,763 | 1,002 | 1,550 | 1.14× |
| Marine IK Small (prettified) | 8,213 | 2,767 | 306 | 2.97× |
| Random Small (minified) | 11,550 | 2,051 | 8,200 (n/c) | 1.41× |
| Random Small (prettified) | 14,558 | 2,307 | 316 | 6.31× |
| Twitter Small (minified) | 12,056 | 3,444 | 8,443 | 1.43× |
| Twitter Small (prettified) | 12,197 | 2,752 | 400 | 4.43× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,126 | 1,445 | 1,169 | 1.47× |
| Prettify | 2,932 | 2,173 | 1,331 | 1.35× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 889 | 120 | 7.42× |
| Double | 244 | 203 | 1.20× |
| Int64 | 2,991 | 488 | 6.13× |
| String | 1,273 | 1,028 | 1.24× |
| Uint64 | 2,888 | 502 | 5.75× |
| Apache Builds (minified) | 11,027 | 3,471 | 3.18× |
| Canada (minified) | 2,667 | 1,811 | 1.47× |
| Apache Builds (prettified) | 10,979 | 3,508 | 3.13× |
| Canada (prettified) | 6,584 | 3,220 | 2.04× |
| CitmCatalog (minified) | 7,170 | 1,852 | 3.87× |
| CitmCatalog (prettified) | 13,652 | 3,894 | 3.51× |
| Discord (minified) | 11,264 | 3,376 | 3.34× |
| Discord (prettified) | 12,322 | 2,323 | 5.31× |
| Github Events (minified) | 11,849 | 4,797 | 2.47× |
| Google Maps Response (minified) | 9,695 | 2,348 | 4.13× |
| Github Events (prettified) | 11,648 | 3,916 | 2.97× |
| Google Maps Response (prettified) | 12,943 | 3,186 | 4.06× |
| Instruments (minified) | 11,222 | 2,087 | 5.38× |
| Instruments (prettified) | 15,248 | 2,841 | 5.37× |
| Marine IK Reverse (minified) | 1,285 | 774 | 1.66× |
| Marine IK Reverse (prettified) | 4,898 | 2,942 | 1.66× |
| Marine IK (minified) | 1,427 | 829 | 1.72× |
| Marine IK (prettified) | 5,176 | 2,693 | 1.92× |
| Random (minified) | 10,653 | 2,469 | 4.31× |
| Random (prettified) | 11,676 | 3,292 | 3.55× |
| Twitter (minified) | 14,941 | 4,352 | 3.43× |
| Twitter (prettified) | 12,453 | 3,591 | 3.47× |
| Apache Builds Small (minified) | 7,257 | 3,035 | 2.39× |
| Canada Small (minified) | 2,573 | 1,500 | 1.71× |
| Apache Builds Small (prettified) | 9,085 | 3,042 | 2.99× |
| Canada Small (prettified) | 6,523 | 3,172 | 2.06× |
| CitmCatalog Small (minified) | 7,748 | 1,846 | 4.20× |
| CitmCatalog Small (prettified) | 11,486 | 2,723 | 4.22× |
| Discord Small (minified) | 11,854 | 2,976 | 3.98× |
| Discord Small (prettified) | 12,301 | 3,086 | 3.99× |
| Github Events Small (minified) | 9,959 | 3,505 | 2.84× |
| Google Maps Response Small (minified) | 7,647 (n/c) | 2,298 | 3.33× |
| Github Events Small (prettified) | 11,472 | 3,164 | 3.63× |
| Google Maps Response Small (prettified) | 10,316 | 2,912 | 3.54× |
| Instruments Small (minified) | 10,608 | 1,503 | 7.06× |
| Instruments Small (prettified) | 14,412 | 2,582 | 5.58× |
| Marine IK Reverse Small (minified) | 1,899 | 1,061 | 1.79× |
| Marine IK Reverse Small (prettified) | 5,406 | 3,066 | 1.76× |
| Marine IK Small (minified) | 1,854 | 966 | 1.92× |
| Marine IK Small (prettified) | 4,587 | 3,325 | 1.38× |
| Random Small (minified) | 7,227 | 2,517 | 2.87× |
| Random Small (prettified) | 8,448 | 2,325 | 3.63× |
| Twitter Small (minified) | 9,811 | 3,412 | 2.88× |
| Twitter Small (prettified) | 11,136 | 3,317 | 3.36× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,603 | 1,660 | 1,199 | 1.57× |
| Prettify | 3,053 | 2,143 | 1,321 | 1.42× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 251 of the 255 write tests and loses 4. On Windows / MSVC Glaze wins prettified Marine IK Reverse (3,323 vs 3,155 MB/s, 0.95×). On Linux / GCC the reflection writer wins minified Marine IK Reverse Small (1,531 vs 1,489 MB/s, 0.97×) and minified Marine IK Small (1,524 vs 1,446 MB/s, 0.95×), and on macOS / GCC it wins String (11,102 vs 9,325 MB/s, 0.84×).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **251** | **0** | **4** |

#### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,628 | 548 | 2.97× |
| Double | 548 | 433 | 1.27× |
| Int64 | 3,912 | 1,653 | 2.37× |
| String | 8,332 | 3,434 | 2.43× |
| Uint64 | 4,484 | 1,705 | 2.63× |
| Apache Builds (minified) | 12,188 | 4,541 | 2.68× |
| Canada (minified) | 1,411 | 1,144 | 1.23× |
| Apache Builds (prettified) | 12,575 | 5,056 | 2.49× |
| Canada (prettified) | 4,090 | 3,097 | 1.32× |
| CitmCatalog (minified) | 8,672 | 4,651 | 1.86× |
| CitmCatalog (prettified) | 13,055 | 7,341 | 1.78× |
| Discord (minified) | 11,952 | 4,613 | 2.59× |
| Discord (prettified) | 12,391 | 4,554 | 2.72× |
| Github Events (minified) | 12,326 | 6,307 | 1.95× |
| Google Maps Response (minified) | 9,856 | 3,925 | 2.51× |
| Github Events (prettified) | 11,099 | 3,947 | 2.81× |
| Google Maps Response (prettified) | 15,531 | 5,715 | 2.72× |
| Instruments (minified) | 14,017 | 4,573 | 3.07× |
| Instruments (prettified) | 14,001 | 5,118 | 2.74× |
| Marine IK Reverse (minified) | 818 | 726 | 1.13× |
| Marine IK Reverse (prettified) | 3,155 | 3,323 | 0.95× |
| Marine IK (minified) | 814 | 735 | 1.11× |
| Marine IK (prettified) | 3,628 | 3,332 | 1.09× |
| Random (minified) | 11,460 | 3,260 | 3.52× |
| Random (prettified) | 12,467 | 4,359 | 2.86× |
| Twitter (minified) | 16,038 | 4,847 | 3.31× |
| Twitter (prettified) | 13,386 | 3,143 | 4.26× |
| Apache Builds Small (minified) | 10,947 | 4,083 | 2.68× |
| Canada Small (minified) | 1,652 | 1,172 | 1.41× |
| Apache Builds Small (prettified) | 11,469 | 4,549 | 2.52× |
| Canada Small (prettified) | 4,783 | 3,160 | 1.51× |
| CitmCatalog Small (minified) | 7,797 | 4,133 | 1.89× |
| CitmCatalog Small (prettified) | 8,736 | 5,252 | 1.66× |
| Discord Small (minified) | 10,654 | 4,495 | 2.37× |
| Discord Small (prettified) | 8,933 | 4,120 | 2.17× |
| Github Events Small (minified) | 12,804 | 5,710 | 2.24× |
| Google Maps Response Small (minified) | 9,722 | 3,913 | 2.48× |
| Github Events Small (prettified) | 11,732 | 4,305 | 2.73× |
| Google Maps Response Small (prettified) | 15,377 | 5,273 | 2.92× |
| Instruments Small (minified) | 12,009 | 4,945 | 2.43× |
| Instruments Small (prettified) | 11,400 | 4,840 (n/c) | 2.36× |
| Marine IK Reverse Small (minified) | 1,139 | 962 | 1.18× |
| Marine IK Reverse Small (prettified) | 4,229 | 3,312 | 1.28× |
| Marine IK Small (minified) | 1,075 | 1,004 | 1.07× |
| Marine IK Small (prettified) | 4,557 | 3,384 | 1.35× |
| Random Small (minified) | 11,582 | 3,873 | 2.99× |
| Random Small (prettified) | 13,359 | 4,302 | 3.10× |
| Twitter Small (minified) | 12,478 | 5,040 | 2.48× |
| Twitter Small (prettified) | 9,809 | 5,024 | 1.95× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,183 | 1,780 | 1,454 | 1.79× |
| Prettify | 3,904 | 2,538 | 1,140 | 1.54× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,259 | 903 | 4.72× |
| Double | 897 | 615 | 1.46× |
| Int64 | 5,154 | 2,208 | 2.33× |
| String | 11,624 | 4,541 | 2.56× |
| Uint64 | 4,503 | 2,226 | 2.02× |
| Apache Builds (minified) | 16,776 | 6,319 | 2.65× |
| Canada (minified) | 1,967 | 996 | 1.97× |
| Apache Builds (prettified) | 18,981 | 5,796 | 3.28× |
| Canada (prettified) | 5,107 | 3,952 | 1.29× |
| CitmCatalog (minified) | 10,473 | 5,740 | 1.82× |
| CitmCatalog (prettified) | 14,900 | 7,093 | 2.10× |
| Discord (minified) | 14,803 | 5,532 | 2.68× |
| Discord (prettified) | 16,538 | 4,557 | 3.63× |
| Github Events (minified) | 16,449 | 6,040 | 2.72× |
| Google Maps Response (minified) | 12,369 | 4,367 | 2.83× |
| Github Events (prettified) | 16,400 | 5,127 | 3.20× |
| Google Maps Response (prettified) | 16,240 | 5,647 | 2.88× |
| Instruments (minified) | 18,582 | 5,298 | 3.51× |
| Instruments (prettified) | 20,616 | 5,592 | 3.69× |
| Marine IK Reverse (minified) | 1,168 | 651 | 1.79× |
| Marine IK Reverse (prettified) | 5,051 | 3,991 | 1.27× |
| Marine IK (minified) | 1,159 | 658 | 1.76× |
| Marine IK (prettified) | 5,035 | 4,033 | 1.25× |
| Random (minified) | 14,774 | 3,923 | 3.77× |
| Random (prettified) | 15,935 | 4,493 | 3.55× |
| Twitter (minified) | 19,735 | 7,020 | 2.81× |
| Twitter (prettified) | 18,311 | 5,819 | 3.15× |
| Apache Builds Small (minified) | 11,971 | 3,657 | 3.27× |
| Canada Small (minified) | 1,671 | 1,011 | 1.65× |
| Apache Builds Small (prettified) | 13,282 | 3,867 | 3.43× |
| Canada Small (prettified) | 4,854 | 4,028 | 1.20× |
| CitmCatalog Small (minified) | 8,698 | 3,264 | 2.67× |
| CitmCatalog Small (prettified) | 11,053 | 3,554 | 3.11× |
| Discord Small (minified) | 11,564 | 2,970 | 3.89× |
| Discord Small (prettified) | 12,497 | 3,009 | 4.15× |
| Github Events Small (minified) | 11,974 | 3,894 | 3.07× |
| Google Maps Response Small (minified) | 11,380 | 4,037 | 2.82× |
| Github Events Small (prettified) | 12,735 | 3,962 | 3.21× |
| Google Maps Response Small (prettified) | 17,582 | 5,470 | 3.21× |
| Instruments Small (minified) | 13,691 | 2,751 | 4.98× |
| Instruments Small (prettified) | 15,782 | 2,682 | 5.89× |
| Marine IK Reverse Small (minified) | 1,520 | 855 | 1.78× |
| Marine IK Reverse Small (prettified) | 5,476 | 3,881 | 1.41× |
| Marine IK Small (minified) | 1,512 | 855 | 1.77× |
| Marine IK Small (prettified) | 5,377 | 3,636 | 1.48× |
| Random Small (minified) | 14,196 | 3,495 | 4.06× |
| Random Small (prettified) | 17,125 | 4,578 | 3.74× |
| Twitter Small (minified) | 10,512 | 4,420 | 2.38× |
| Twitter Small (prettified) | 11,508 | 3,576 | 3.22× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,707 | 2,649 | 1,618 | 1.40× |
| Prettify | 4,606 | 3,514 | 2,282 | 1.31× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,243 | 685 | 3,142 | 1.03× |
| Double | 826 | 650 | 649 | 1.27× |
| Int64 | 5,464 | 2,742 | 3,986 | 1.37× |
| String | 11,280 | 5,909 | 9,223 | 1.22× |
| Uint64 | 6,438 | 2,714 | 3,917 | 1.64× |
| Apache Builds (minified) | 20,553 | 9,304 | 13,488 | 1.52× |
| Canada (minified) | 2,097 | 1,534 | 1,048 | 1.37× |
| Apache Builds (prettified) | 23,431 | 8,077 | 627 | 2.90× |
| Canada (prettified) | 6,075 | 3,919 | 384 | 1.55× |
| CitmCatalog (minified) | 12,473 | 7,203 | 11,162 | 1.12× |
| CitmCatalog (prettified) | 20,184 | 7,902 | 570 | 2.55× |
| Discord (minified) | 15,419 | 7,226 | 13,320 | 1.16× |
| Discord (prettified) | 20,624 | 7,150 | 559 | 2.88× |
| Github Events (minified) | 15,031 | 8,566 | 11,090 | 1.36× |
| Google Maps Response (minified) | 16,822 | 5,173 | 10,406 | 1.62× |
| Github Events (prettified) | 18,397 | 6,038 | 637 | 3.05× |
| Google Maps Response (prettified) | 21,472 | 7,380 | 528 | 2.91× |
| Instruments (minified) | 20,205 | 6,414 | 15,550 | 1.30× |
| Instruments (prettified) | 26,592 | 6,301 | 466 | 4.22× |
| Marine IK Reverse (minified) | 1,300 | 980 | 884 | 1.33× |
| Marine IK Reverse (prettified) | 6,321 | 4,019 | 306 | 1.57× |
| Marine IK (minified) | 1,297 | 1,002 | 891 | 1.29× |
| Marine IK (prettified) | 6,246 | 4,088 | 342 | 1.53× |
| Random (minified) | 17,938 | 5,201 | 11,278 | 1.59× |
| Random (prettified) | 20,541 | 5,721 | 534 | 3.59× |
| Twitter (minified) | 17,950 | 9,052 | 11,040 | 1.63× |
| Twitter (prettified) | 21,157 | 6,567 | 524 | 3.22× |
| Apache Builds Small (minified) | 14,151 | 5,225 | 8,109 | 1.75× |
| Canada Small (minified) | 1,980 | 1,659 | 1,072 | 1.19× |
| Apache Builds Small (prettified) | 18,305 | 4,640 | 540 | 3.95× |
| Canada Small (prettified) | 5,533 | 3,930 | 412 | 1.41× |
| CitmCatalog Small (minified) | 8,575 | 4,104 | 6,553 | 1.31× |
| CitmCatalog Small (prettified) | 16,566 | 3,471 | 502 | 4.77× |
| Discord Small (minified) | 11,431 | 4,816 | 8,295 | 1.38× |
| Discord Small (prettified) | 16,306 | 3,961 | 517 | 4.12× |
| Github Events Small (minified) | 10,079 | 6,078 | 7,388 | 1.36× |
| Google Maps Response Small (minified) | 14,231 | 4,111 | 8,638 | 1.65× |
| Github Events Small (prettified) | 14,399 | 4,550 | 528 | 3.16× |
| Google Maps Response Small (prettified) | 30,756 | 6,957 | 523 | 4.42× |
| Instruments Small (minified) | 13,147 | 3,943 | 8,584 | 1.53× |
| Instruments Small (prettified) | 23,730 | 3,420 | 509 | 6.94× |
| Marine IK Reverse Small (minified) | 1,489 | 1,310 | 1,531 | 0.97× |
| Marine IK Reverse Small (prettified) | 6,116 | 3,581 | 514 | 1.71× |
| Marine IK Small (minified) | 1,446 | 1,327 | 1,524 | 0.95× |
| Marine IK Small (prettified) | 6,594 | 3,909 | 530 | 1.69× |
| Random Small (minified) | 16,331 | 4,992 | 8,965 | 1.82× |
| Random Small (prettified) | 27,584 | 5,288 | 531 | 5.22× |
| Twitter Small (minified) | 10,850 | 6,696 | 7,441 | 1.46× |
| Twitter Small (prettified) | 14,136 | 4,236 | 553 | 3.34× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,221 | 2,549 | 1,868 | 1.26× |
| Prettify | 4,158 | 3,165 | 2,473 | 1.31× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,643 | 833 | 2,337 | 1.56× |
| Double | 882 | 627 | 598 | 1.41× |
| Int64 | 4,981 | 2,085 | 3,254 | 1.53× |
| String | 9,325 | 4,658 | 11,102 | 0.84× |
| Uint64 | 4,493 | 2,750 | 3,065 | 1.47× |
| Apache Builds (minified) | 12,218 | 6,294 | 9,226 | 1.32× |
| Canada (minified) | 2,229 | 1,689 | 868 | 1.32× |
| Apache Builds (prettified) | 12,864 | 4,743 | 338 | 2.71× |
| Canada (prettified) | 7,994 | 3,739 | 250 (n/c) | 2.14× |
| CitmCatalog (minified) | 8,776 | 2,300 | 7,069 | 1.24× |
| CitmCatalog (prettified) | 17,731 | 4,255 | 310 | 4.17× |
| Discord (minified) | 12,271 | 3,065 | 9,892 | 1.24× |
| Discord (prettified) | 13,176 | 2,369 | 319 | 5.56× |
| Github Events (minified) | 15,597 | 4,191 | 11,458 | 1.36× |
| Google Maps Response (minified) | 10,882 | 1,858 | 8,035 | 1.35× |
| Github Events (prettified) | 15,840 | 3,866 | 392 | 4.10× |
| Google Maps Response (prettified) | 19,572 | 2,936 | 302 | 6.67× |
| Instruments (minified) | 15,431 | 2,156 | 8,285 | 1.86× |
| Instruments (prettified) | 17,484 | 2,550 | 339 | 6.86× |
| Marine IK Reverse (minified) | 1,299 | 943 | 747 | 1.38× |
| Marine IK Reverse (prettified) | 6,528 | 2,931 | 264 | 2.23× |
| Marine IK (minified) | 1,306 | 868 | 726 | 1.50× |
| Marine IK (prettified) | 6,617 | 2,900 | 283 | 2.28× |
| Random (minified) | 11,651 | 2,367 | 8,164 | 1.43× |
| Random (prettified) | 14,871 | 2,757 | 339 | 5.39× |
| Twitter (minified) | 12,653 | 3,547 | 8,651 | 1.46× |
| Twitter (prettified) | 16,252 | 4,516 | 351 | 3.60× |
| Apache Builds Small (minified) | 9,178 | 3,195 | 6,746 (n/c) | 1.36× |
| Canada Small (minified) | 1,932 | 1,767 | 795 | 1.09× |
| Apache Builds Small (prettified) | 11,718 | 3,592 | 326 | 3.26× |
| Canada Small (prettified) | 7,157 | 3,075 | 255 | 2.33× |
| CitmCatalog Small (minified) | 8,737 | 1,794 | 8,103 | 1.08× |
| CitmCatalog Small (prettified) | 17,366 | 2,864 | 259 | 6.06× |
| Discord Small (minified) | 12,606 | 2,596 | 9,324 | 1.35× |
| Discord Small (prettified) | 13,585 | 2,549 | 308 | 5.33× |
| Github Events Small (minified) | 14,751 (n/c) | 3,164 (n/c) | 9,147 | 1.61× |
| Google Maps Response Small (minified) | 11,383 | 1,989 | 8,761 | 1.30× |
| Github Events Small (prettified) | 13,505 | 3,154 | 349 | 4.28× |
| Google Maps Response Small (prettified) | 19,977 | 2,913 | 304 | 6.86× |
| Instruments Small (minified) | 13,807 | 2,094 | 6,931 | 1.99× |
| Instruments Small (prettified) | 17,983 (n/c) | 2,098 | 303 | 8.57× |
| Marine IK Reverse Small (minified) | 1,868 | 1,108 | 1,589 | 1.18× |
| Marine IK Reverse Small (prettified) | 8,300 | 3,404 | 325 | 2.44× |
| Marine IK Small (minified) | 1,762 | 995 | 1,604 | 1.10× |
| Marine IK Small (prettified) | 9,237 | 3,521 | 291 | 2.62× |
| Random Small (minified) | 11,398 | 2,228 | 8,436 (n/c) | 1.35× |
| Random Small (prettified) | 15,052 | 2,754 | 287 | 5.47× |
| Twitter Small (minified) | 13,548 | 3,783 | 9,330 | 1.45× |
| Twitter Small (prettified) | 16,082 (n/c) | 4,062 | 368 | 3.96× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,242 | 1,486 | 1,233 | 1.51× |
| Prettify | 2,856 | 2,222 | 1,347 | 1.29× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,265 | 673 | 1.88× |
| Double | 662 | 593 | 1.12× |
| Int64 | 3,400 | 2,047 | 1.66× |
| String | 7,624 | 3,993 | 1.91× |
| Uint64 | 3,104 | 2,237 | 1.39× |
| Apache Builds (minified) | 12,408 | 4,263 | 2.91× |
| Canada (minified) | 2,118 | 1,791 | 1.18× |
| Apache Builds (prettified) | 11,265 | 3,456 | 3.26× |
| Canada (prettified) | 6,579 | 3,472 | 1.90× |
| CitmCatalog (minified) | 7,025 | 2,223 | 3.16× |
| CitmCatalog (prettified) | 13,389 | 4,581 | 2.92× |
| Discord (minified) | 10,700 | 3,335 | 3.21× |
| Discord (prettified) | 13,787 | 3,059 | 4.51× |
| Github Events (minified) | 12,745 | 6,185 | 2.06× |
| Google Maps Response (minified) | 10,241 | 2,621 | 3.91× |
| Github Events (prettified) | 13,461 | 4,399 | 3.06× |
| Google Maps Response (prettified) | 14,435 | 4,015 | 3.60× |
| Instruments (minified) | 11,964 | 1,986 | 6.03× |
| Instruments (prettified) | 14,339 | 3,285 | 4.37× |
| Marine IK Reverse (minified) | 1,206 | 734 | 1.64× |
| Marine IK Reverse (prettified) | 5,243 | 3,153 | 1.66× |
| Marine IK (minified) | 1,391 | 826 | 1.68× |
| Marine IK (prettified) | 5,182 | 3,184 | 1.63× |
| Random (minified) | 10,152 | 2,735 | 3.71× |
| Random (prettified) | 12,804 | 3,627 | 3.53× |
| Twitter (minified) | 12,614 | 4,781 | 2.64× |
| Twitter (prettified) | 11,520 | 4,266 | 2.70× |
| Apache Builds Small (minified) | 10,118 (n/c) | 3,268 | 3.10× |
| Canada Small (minified) | 2,617 | 1,737 | 1.51× |
| Apache Builds Small (prettified) | 10,348 | 3,702 | 2.80× |
| Canada Small (prettified) | 6,827 | 3,602 | 1.90× |
| CitmCatalog Small (minified) | 9,108 | 2,029 | 4.49× |
| CitmCatalog Small (prettified) | 12,555 | 2,333 | 5.38× |
| Discord Small (minified) | 10,922 | 3,333 | 3.28× |
| Discord Small (prettified) | 13,266 | 3,565 | 3.72× |
| Github Events Small (minified) | 11,630 | 4,719 | 2.46× |
| Google Maps Response Small (minified) | 9,900 | 2,341 | 4.23× |
| Github Events Small (prettified) | 13,738 | 3,874 | 3.55× |
| Google Maps Response Small (prettified) | 11,275 | 3,260 | 3.46× |
| Instruments Small (minified) | 15,376 | 1,654 | 9.30× |
| Instruments Small (prettified) | 16,570 | 3,039 | 5.45× |
| Marine IK Reverse Small (minified) | 1,840 | 997 | 1.85× |
| Marine IK Reverse Small (prettified) | 5,236 | 3,502 | 1.50× |
| Marine IK Small (minified) | 1,906 | 892 | 2.14× |
| Marine IK Small (prettified) | 5,385 | 3,929 | 1.37× |
| Random Small (minified) | 10,852 | 3,151 | 3.44× |
| Random Small (prettified) | 12,737 | 3,495 | 3.64× |
| Twitter Small (minified) | 13,386 | 4,279 | 3.13× |
| Twitter Small (prettified) | 13,210 | 3,484 | 3.79× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,521 | 1,646 | 1,070 | 1.53× |
| Prettify | 2,793 | 2,336 | 1,430 | 1.20× |

### Losses and ties

The eight freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,451 vs Jsonifier's 3,891 MB/s (0.87×). Reused, Jsonifier wins by 3.31× (16,038 vs 4,847).
- **Prettify, Windows / MSVC.** Glaze runs 2,555 vs 2,406 MB/s (0.94×), with simdjson On Demand at 1,452. All three converged. Reused, Jsonifier wins by 1.54× (3,904 vs 2,538).
- **Double, Linux / GCC.** The reflection writer runs 356 vs 335 MB/s (0.94×), with Glaze at 319, though Jsonifier is faster per call (see *Ranking*). Reused, Jsonifier wins by 1.27× over Glaze (826 vs 650), with the reflection writer at 649.
- **Int64, Linux / GCC.** The reflection writer runs 997 vs 948 MB/s (0.95×), with Glaze at 931. All three write the same 3,862 bytes, so this is a per-call loss too. Reused, Jsonifier wins by 1.37× over the reflection writer (5,464 vs 3,986).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,507 vs 1,451 MB/s (0.96×), with Glaze at 1,194. It also wins reused, 1,531 vs 1,489 MB/s (0.97×), with Glaze at 1,310.
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,539 vs 1,418 MB/s (0.92×), with Glaze at 1,236. It also wins reused, 1,524 vs 1,446 MB/s (0.95×), with Glaze at 1,327.
- **Double, macOS / GCC.** The reflection writer runs 311 vs 243 MB/s (0.78×), with Glaze at 211, and here it is faster per call as well (see *Ranking*). Reused, Jsonifier wins by 1.41× over Glaze (882 vs 627), with the reflection writer at 598.
- **Int64, macOS / GCC.** The reflection writer runs 727 vs 657 MB/s (0.90×), with Glaze at 455. Reused, Jsonifier wins by 1.53× over the reflection writer (4,981 vs 3,254).

The freshly allocated tie is against the reflection writer on macOS / GCC: on minified CitmCatalog Small Jsonifier runs 8,223 vs its 7,545 MB/s (1.09×, with RSEs of 3.6% and 3.4%), Glaze at 1,802, and Jsonifier wins it reused by 1.08× (8,737 vs 8,103).

Two reused losses have no freshly allocated counterpart. Prettified Marine IK Reverse on Windows / MSVC wins freshly allocated by 1.25× (2,207 vs Glaze's 1,759) and loses to Glaze reused (3,155 vs 3,323 MB/s). String on macOS / GCC wins freshly allocated by 1.17× over the reflection writer (1,269 vs 1,089) and loses to it reused (9,325 vs 11,102 MB/s).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.11–2.52× and prettified documents by 16–61×; reused, by 1.12–2.57× and 16–65×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 41 of the 44 Small document tests freshly allocated and 42 reused. The losses are minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, in both runs, and minified CitmCatalog Small on macOS / GCC is a tie freshly allocated. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.26–1.79× freshly allocated and 1.26–1.79× reused. It wins Prettify on all five reused, by 1.20–1.54×, and on four of five freshly allocated, by 1.31–1.42×; the exception is Windows / MSVC, where Glaze runs 2,555 vs 2,406 MB/s (0.94×). Glaze is the next-fastest library in every case.

## AVX-512 builds

The results above come from AVX2 and NEON builds. The same 51 write tests also ran on three AVX-512 builds, Linux / Clang, Linux / GCC and Windows / MSVC, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor. As on AVX2, simdjson's reflection writer runs only on the Linux / GCC build.

Freshly allocated, Jsonifier was fastest in 150 of the 153 write tests, tied 1 and lost 2. The two losses are the same two that it loses on the AVX2 Windows / MSVC build: minified Twitter (Glaze 4,758 vs 3,769 MB/s, 0.79×) and Prettify (Glaze 3,372 vs 2,608 MB/s, 0.77×, with simdjson On Demand at 1,408). The tie is prettified Marine IK Reverse Small on Windows / MSVC against Glaze (3,192 vs 3,182 MB/s). Reused, it was fastest in 151 and tied 2: prettified Marine IK Reverse on Windows / MSVC (3,330 vs Glaze's 3,309 MB/s) and String on Linux / GCC against the reflection writer (8,921 vs 8,925 MB/s). Both freshly allocated losses become wins reused: minified Twitter by 2.51× (13,396 vs 5,342 MB/s) and Prettify by 1.36× (4,472 vs 3,278). Linux / Clang wins all 51 tests in both runs, and Linux / GCC wins all 51 freshly allocated. Freshly allocated, prettified full-size documents lead Glaze by 1.28–9.88× and prettified Small documents by 1.00–6.71×; reused, by 1.01–9.77× and 1.29–7.77×. On Linux / GCC, freshly allocated prettified documents lead the reflection writer by 14.93–79.75× full-size and 12.93–54.75× Small. Minify leads Glaze by 1.58× on Linux / Clang, 1.79× on Linux / GCC and 1.97× on Windows / MSVC freshly allocated, and by 1.57×, 1.59× and 2.00× reused.

### AVX-512 method

These results come from the Json-Performance branch `benchmarking-avx512-main`, with the same commits as above (Jsonifier [79b3e67](https://github.com/nihilai-collective/jsonifier/commit/79b3e67) [5], Glaze [a53ddd59](https://github.com/stephenberry/glaze/commit/a53ddd59) [6], simdjson [36c5b3cfc](https://github.com/simdjson/simdjson/commit/36c5b3cfc) [7], BenchmarkSuite [71afc62](https://github.com/nihilai-collective/benchmarksuite/commit/71afc62) [8]), run November 17, 2026. The tests, sampling, ranking and convergence limits (RSE under 5%, mean shift under 2.5%) are the same as for the x86 builds above.

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | AMD EPYC 9V45 (cloud VM) | Linux 7.0.0-1019-azure | AVX-512 | AVX2 / SSE4.1 | icelake |
| Linux / GCC 16.2 | AMD EPYC 9V45 (cloud VM) | Linux 7.0.0-1019-azure | AVX-512 | AVX2 / SSE4.1 | icelake |
| Windows / MSVC 19.51 | AMD EPYC 9V45 (cloud VM) | Windows 10.0.26100 | AVX-512 | AVX2 / SSE4.1 | icelake |

Glaze uses AVX512BW for UTF-8 validation and structural skipping on these builds, but its string escaper stays on AVX2 and its float writer on SSE4.1. The runners are shared virtual machines, and the CPU, operating-system images and MSVC version (19.51, against 19.52 above) all differ from the AVX2 builds, so an AVX-512 result here cannot be compared with the matching AVX2 result to isolate the instruction set. Each comparison below is between libraries within one build.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 51 | 48 | 1 | 2 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| **Total** | **153** | **150** | **1** | **2** |

### Windows / MSVC 19.51 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,091 | 78 | 14.01× |
| Double | 451 | 132 | 3.41× |
| Int64 | 421 | 350 | 1.20× |
| String | 1,226 | 803 | 1.53× |
| Uint64 | 425 | 334 | 1.27× |
| Apache Builds (minified) | 12,405 | 4,714 | 2.63× |
| Canada (minified) | 1,183 | 774 | 1.53× |
| Apache Builds (prettified) | 12,178 | 3,988 | 3.05× |
| Canada (prettified) | 2,718 | 1,133 | 2.40× |
| CitmCatalog (minified) | 7,662 | 3,154 | 2.43× |
| CitmCatalog (prettified) | 3,749 | 1,963 | 1.91× |
| Discord (minified) | 10,818 | 3,021 | 3.58× |
| Discord (prettified) | 11,215 | 2,995 | 3.74× |
| Github Events (minified) | 11,054 | 4,719 | 2.34× |
| Google Maps Response (minified) | 8,980 | 2,690 | 3.34× |
| Github Events (prettified) | 11,055 | 4,289 | 2.58× |
| Google Maps Response (prettified) | 13,586 | 3,764 | 3.61× |
| Instruments (minified) | 14,073 | 2,985 | 4.71× |
| Instruments (prettified) | 14,158 | 3,523 | 4.02× |
| Marine IK Reverse (minified) | 814 | 533 | 1.53× |
| Marine IK Reverse (prettified) | 2,154 (n/c) | 1,684 | 1.28× |
| Marine IK (minified) | 844 | 572 | 1.48× |
| Marine IK (prettified) | 2,477 | 1,234 | 2.01× |
| Random (minified) | 3,205 | 2,620 | 1.22× |
| Random (prettified) | 3,500 | 1,844 | 1.90× |
| Twitter (minified) | 3,769 | 4,758 | 0.79× |
| Twitter (prettified) | 3,905 | 2,343 | 1.67× |
| Apache Builds Small (minified) | 9,393 | 3,273 | 2.87× |
| Canada Small (minified) | 1,733 | 1,021 | 1.70× |
| Apache Builds Small (prettified) | 10,273 | 3,240 | 3.17× |
| Canada Small (prettified) | 4,619 | 2,810 | 1.64× |
| CitmCatalog Small (minified) | 5,311 | 2,577 | 2.06× |
| CitmCatalog Small (prettified) | 7,897 | 3,439 | 2.30× |
| Discord Small (minified) | 8,125 | 2,936 | 2.77× |
| Discord Small (prettified) | 7,052 | 2,932 | 2.41× |
| Github Events Small (minified) | 10,220 | 3,457 | 2.96× |
| Google Maps Response Small (minified) | 8,550 | 2,629 | 3.25× |
| Github Events Small (prettified) | 10,516 | 3,352 | 3.14× |
| Google Maps Response Small (prettified) | 12,212 | 3,460 | 3.53× |
| Instruments Small (minified) | 11,491 | 2,726 | 4.22× |
| Instruments Small (prettified) | 11,061 | 3,395 | 3.26× |
| Marine IK Reverse Small (minified) | 1,388 | 819 | 1.70× |
| Marine IK Reverse Small (prettified) | 3,192 | 3,182 | 1.00× |
| Marine IK Small (minified) | 1,364 | 824 | 1.66× |
| Marine IK Small (prettified) | 4,843 | 3,275 | 1.48× |
| Random Small (minified) | 8,214 | 2,795 | 2.94× |
| Random Small (prettified) | 10,616 | 3,131 | 3.39× |
| Twitter Small (minified) | 7,479 | 3,891 | 1.92× |
| Twitter Small (prettified) | 7,070 | 3,550 | 1.99× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,454 | 1,755 | 1,316 | 1.97× |
| Prettify | 2,608 | 3,372 | 1,408 | 0.77× |

### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,171 | 187 | 11.62× |
| Double | 289 | 208 | 1.39× |
| Int64 | 817 | 769 | 1.06× |
| String | 3,959 | 1,847 | 2.14× |
| Uint64 | 866 | 782 | 1.11× |
| Apache Builds (minified) | 17,482 | 5,148 | 3.40× |
| Canada (minified) | 1,853 | 922 | 2.01× |
| Apache Builds (prettified) | 18,540 | 5,660 | 3.28× |
| Canada (prettified) | 5,142 | 2,689 | 1.91× |
| CitmCatalog (minified) | 9,980 | 3,664 | 2.72× |
| CitmCatalog (prettified) | 16,460 | 4,788 | 3.44× |
| Discord (minified) | 15,200 | 3,609 | 4.21× |
| Discord (prettified) | 17,128 | 3,137 | 5.46× |
| Github Events (minified) | 16,115 | 5,550 | 2.90× |
| Google Maps Response (minified) | 11,689 | 2,684 | 4.35× |
| Github Events (prettified) | 16,690 | 4,895 | 3.41× |
| Google Maps Response (prettified) | 17,599 | 4,891 | 3.60× |
| Instruments (minified) | 19,789 | 3,651 | 5.42× |
| Instruments (prettified) | 23,342 | 3,728 | 6.26× |
| Marine IK Reverse (minified) | 1,161 | 616 | 1.89× |
| Marine IK Reverse (prettified) | 5,047 | 3,120 | 1.62× |
| Marine IK (minified) | 1,150 | 593 | 1.94× |
| Marine IK (prettified) | 4,853 | 2,815 | 1.72× |
| Random (minified) | 12,949 | 2,898 | 4.47× |
| Random (prettified) | 17,130 | 3,643 | 4.70× |
| Twitter (minified) | 17,510 | 5,467 | 3.20× |
| Twitter (prettified) | 15,685 | 4,623 | 3.39× |
| Apache Builds Small (minified) | 12,516 | 3,842 | 3.26× |
| Canada Small (minified) | 1,989 | 1,003 | 1.98× |
| Apache Builds Small (prettified) | 13,286 | 4,244 | 3.13× |
| Canada Small (prettified) | 5,463 | 3,114 | 1.75× |
| CitmCatalog Small (minified) | 11,039 | 2,655 | 4.16× |
| CitmCatalog Small (prettified) | 11,848 | 4,092 | 2.90× |
| Discord Small (minified) | 12,148 | 3,491 | 3.48× |
| Discord Small (prettified) | 12,950 | 3,153 | 4.11× |
| Github Events Small (minified) | 12,954 | 4,396 | 2.95× |
| Google Maps Response Small (minified) | 11,304 | 2,642 | 4.28× |
| Github Events Small (prettified) | 14,902 | 4,234 | 3.52× |
| Google Maps Response Small (prettified) | 17,356 | 5,085 | 3.41× |
| Instruments Small (minified) | 17,716 | 3,626 | 4.89× |
| Instruments Small (prettified) | 17,652 | 3,443 | 5.13× |
| Marine IK Reverse Small (minified) | 1,500 | 815 | 1.84× |
| Marine IK Reverse Small (prettified) | 5,451 | 3,614 | 1.51× |
| Marine IK Small (minified) | 1,430 | 798 | 1.79× |
| Marine IK Small (prettified) | 5,578 | 3,639 | 1.53× |
| Random Small (minified) | 13,249 | 3,146 | 4.21× |
| Random Small (prettified) | 17,109 | 3,800 | 4.50× |
| Twitter Small (minified) | 11,401 | 4,874 | 2.34× |
| Twitter Small (prettified) | 10,064 | 3,716 | 2.71× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,952 | 2,507 | 1,687 | 1.58× |
| Prettify | 5,579 | 3,820 | 2,253 | 1.46× |

### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,015 | 200 | 259 | 7.79× |
| Double | 345 | 266 | 315 | 1.10× |
| Int64 | 953 | 776 | 891 | 1.07× |
| String | 3,930 | 1,932 | 3,808 | 1.03× |
| Uint64 | 923 | 786 | 882 | 1.05× |
| Apache Builds (minified) | 26,345 | 6,280 | 14,755 | 1.79× |
| Canada (minified) | 1,771 | 1,289 | 876 | 1.37× |
| Apache Builds (prettified) | 34,481 | 5,918 | 602 | 5.83× |
| Canada (prettified) | 5,612 | 3,127 | 376 | 1.79× |
| CitmCatalog (minified) | 10,439 | 3,693 | 8,504 | 1.23× |
| CitmCatalog (prettified) | 27,254 | 5,334 | 554 | 5.11× |
| Discord (minified) | 15,615 | 5,255 | 13,466 | 1.16× |
| Discord (prettified) | 24,344 | 4,924 | 522 | 4.94× |
| Github Events (minified) | 14,487 | 6,551 | 10,988 | 1.32× |
| Google Maps Response (minified) | 15,510 | 3,730 | 12,054 | 1.29× |
| Github Events (prettified) | 18,594 | 5,238 | 658 | 3.55× |
| Google Maps Response (prettified) | 32,352 | 5,646 | 585 | 5.73× |
| Instruments (minified) | 23,912 | 2,536 | 14,630 | 1.63× |
| Instruments (prettified) | 39,634 | 4,010 | 497 | 9.88× |
| Marine IK Reverse (minified) | 1,119 | 907 | 715 | 1.23× |
| Marine IK Reverse (prettified) | 6,133 | 3,751 (n/c) | 192 | 1.64× |
| Marine IK (minified) | 1,139 | 898 | 733 | 1.27× |
| Marine IK (prettified) | 5,927 | 3,159 | 371 | 1.88× |
| Random (minified) | 14,516 | 3,366 | 10,089 | 1.44× |
| Random (prettified) | 24,799 | 3,945 | 579 | 6.29× |
| Twitter (minified) | 12,538 | 5,549 | 9,391 | 1.34× |
| Twitter (prettified) | 17,676 | 4,175 | 567 | 4.23× |
| Apache Builds Small (minified) | 14,501 | 4,576 | 9,093 | 1.59× |
| Canada Small (minified) | 1,845 | 1,412 | 929 | 1.31× |
| Apache Builds Small (prettified) | 17,494 | 4,265 | 570 | 4.10× |
| Canada Small (prettified) | 5,669 | 3,294 | 403 | 1.72× |
| CitmCatalog Small (minified) | 7,579 | 2,594 | 6,463 | 1.17× |
| CitmCatalog Small (prettified) | 14,253 | 3,715 | 535 | 3.84× |
| Discord Small (minified) | 10,448 | 3,406 | 7,623 | 1.37× |
| Discord Small (prettified) | 14,358 | 3,908 | 524 | 3.67× |
| Github Events Small (minified) | 13,095 | 5,459 | 9,412 | 1.39× |
| Google Maps Response Small (minified) | 13,694 | 3,639 | 10,900 | 1.26× |
| Github Events Small (prettified) | 16,286 | 2,857 | 570 | 5.70× |
| Google Maps Response Small (prettified) | 31,984 | 5,747 | 584 | 5.57× |
| Instruments Small (minified) | 15,438 | 1,980 | 9,050 | 1.71× |
| Instruments Small (prettified) | 22,464 | 3,346 | 517 | 6.71× |
| Marine IK Reverse Small (minified) | 1,499 | 1,032 | 1,293 | 1.16× |
| Marine IK Reverse Small (prettified) | 7,656 | 3,794 | 534 | 2.02× |
| Marine IK Small (minified) | 1,569 | 1,095 | 1,285 | 1.22× |
| Marine IK Small (prettified) | 7,188 | 3,714 | 556 | 1.94× |
| Random Small (minified) | 17,441 | 3,757 | 11,467 | 1.52× |
| Random Small (prettified) | 27,101 | 4,448 | 575 | 6.09× |
| Twitter Small (minified) | 12,471 | 5,108 | 8,979 | 1.39× |
| Twitter Small (prettified) | 16,749 | 3,798 | 553 | 4.41× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 4,450 | 2,482 | 1,888 | 1.79× |
| Prettify | 5,280 | 3,312 | 2,653 | 1.59× |

### Reused output strings

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 51 | 50 | 1 | 0 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 50 | 1 | 0 |
| **Total** | **153** | **151** | **2** | **0** |

#### Windows / MSVC 19.51 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,501 | 276 | 5.44× |
| Double | 692 | 355 | 1.95× |
| Int64 | 3,658 | 1,099 | 3.33× |
| String | 7,592 | 2,483 | 3.06× |
| Uint64 | 3,429 | 1,046 | 3.28× |
| Apache Builds (minified) | 12,616 | 5,158 | 2.45× |
| Canada (minified) | 1,409 | 1,089 | 1.29× |
| Apache Builds (prettified) | 12,254 | 4,255 | 2.88× |
| Canada (prettified) | 4,193 | 3,117 | 1.35× |
| CitmCatalog (minified) | 7,646 | 3,318 | 2.30× |
| CitmCatalog (prettified) | 12,039 | 5,909 | 2.04× |
| Discord (minified) | 11,519 | 3,259 | 3.53× |
| Discord (prettified) | 11,608 | 3,256 | 3.57× |
| Github Events (minified) | 11,720 | 5,343 | 2.19× |
| Google Maps Response (minified) | 9,610 | 3,244 | 2.96× |
| Github Events (prettified) | 11,794 | 4,869 | 2.42× |
| Google Maps Response (prettified) | 14,295 | 4,198 | 3.41× |
| Instruments (minified) | 14,499 | 3,124 | 4.64× |
| Instruments (prettified) | 14,461 | 3,775 | 3.83× |
| Marine IK Reverse (minified) | 934 | 689 | 1.36× |
| Marine IK Reverse (prettified) | 3,330 | 3,309 | 1.01× |
| Marine IK (minified) | 943 | 700 | 1.35× |
| Marine IK (prettified) | 3,978 | 3,292 | 1.21× |
| Random (minified) | 9,849 | 2,821 | 3.49× |
| Random (prettified) | 11,281 | 3,365 | 3.35× |
| Twitter (minified) | 13,396 | 5,342 | 2.51× |
| Twitter (prettified) | 10,844 | 5,006 | 2.17× |
| Apache Builds Small (minified) | 10,534 | 3,826 | 2.75× |
| Canada Small (minified) | 1,758 | 1,121 | 1.57× |
| Apache Builds Small (prettified) | 11,685 | 3,690 | 3.17× |
| Canada Small (prettified) | 4,914 | 3,134 | 1.57× |
| CitmCatalog Small (minified) | 7,680 | 2,920 | 2.63× |
| CitmCatalog Small (prettified) | 8,818 | 4,398 | 2.01× |
| Discord Small (minified) | 9,339 | 3,577 | 2.61× |
| Discord Small (prettified) | 8,698 | 3,318 | 2.62× |
| Github Events Small (minified) | 12,419 | 4,275 | 2.91× |
| Google Maps Response Small (minified) | 8,555 | 3,116 | 2.75× |
| Github Events Small (prettified) | 13,282 | 3,930 | 3.38× |
| Google Maps Response Small (prettified) | 13,909 | 4,002 | 3.48× |
| Instruments Small (minified) | 13,564 | 3,245 | 4.18× |
| Instruments Small (prettified) | 11,985 | 3,681 | 3.26× |
| Marine IK Reverse Small (minified) | 1,466 | 865 | 1.70× |
| Marine IK Reverse Small (prettified) | 5,164 | 3,719 | 1.39× |
| Marine IK Small (minified) | 1,444 | 878 | 1.65× |
| Marine IK Small (prettified) | 5,152 | 3,807 | 1.35× |
| Random Small (minified) | 10,807 | 3,269 | 3.31× |
| Random Small (prettified) | 12,464 | 3,817 | 3.27× |
| Twitter Small (minified) | 9,111 | 4,613 | 1.97× |
| Twitter Small (prettified) | 7,563 | 4,334 | 1.75× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,496 | 1,751 | 1,320 | 2.00× |
| Prettify | 4,472 | 3,278 | 1,463 | 1.36× |

#### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 3,761 | 900 | 4.18× |
| Double | 808 | 529 | 1.53× |
| Int64 | 4,356 | 2,337 | 1.86× |
| String | 12,707 | 4,995 | 2.54× |
| Uint64 | 4,728 | 2,378 | 1.99× |
| Apache Builds (minified) | 17,776 | 5,621 | 3.16× |
| Canada (minified) | 1,819 | 933 | 1.95× |
| Apache Builds (prettified) | 18,601 | 6,178 | 3.01× |
| Canada (prettified) | 5,208 | 3,243 | 1.61× |
| CitmCatalog (minified) | 9,887 | 3,891 | 2.54× |
| CitmCatalog (prettified) | 16,228 | 5,809 | 2.79× |
| Discord (minified) | 15,110 | 3,792 | 3.98× |
| Discord (prettified) | 17,877 | 3,371 | 5.30× |
| Github Events (minified) | 15,952 | 6,236 | 2.56× |
| Google Maps Response (minified) | 11,605 | 2,958 | 3.92× |
| Github Events (prettified) | 16,718 | 5,314 | 3.15× |
| Google Maps Response (prettified) | 18,072 | 5,327 | 3.39× |
| Instruments (minified) | 20,281 | 3,818 | 5.31× |
| Instruments (prettified) | 23,951 | 3,976 | 6.02× |
| Marine IK Reverse (minified) | 1,180 | 623 | 1.89× |
| Marine IK Reverse (prettified) | 4,973 | 3,559 | 1.40× |
| Marine IK (minified) | 1,139 | 619 | 1.84× |
| Marine IK (prettified) | 4,869 | 3,581 | 1.36× |
| Random (minified) | 12,986 | 3,107 | 4.18× |
| Random (prettified) | 17,694 | 3,929 | 4.50× |
| Twitter (minified) | 17,383 | 6,025 | 2.89× |
| Twitter (prettified) | 15,830 | 5,295 | 2.99× |
| Apache Builds Small (minified) | 14,017 | 4,359 | 3.22× |
| Canada Small (minified) | 1,933 | 1,036 | 1.87× |
| Apache Builds Small (prettified) | 14,293 | 4,758 | 3.00× |
| Canada Small (prettified) | 5,617 | 3,454 | 1.63× |
| CitmCatalog Small (minified) | 11,583 | 2,863 | 4.05× |
| CitmCatalog Small (prettified) | 13,706 | 4,595 | 2.98× |
| Discord Small (minified) | 14,269 | 3,957 | 3.61× |
| Discord Small (prettified) | 14,980 | 3,566 | 4.20× |
| Github Events Small (minified) | 15,302 | 5,171 | 2.96× |
| Google Maps Response Small (minified) | 11,946 | 2,831 | 4.22× |
| Github Events Small (prettified) | 17,810 | 4,853 | 3.67× |
| Google Maps Response Small (prettified) | 18,704 | 5,410 | 3.46× |
| Instruments Small (minified) | 19,882 | 3,944 | 5.04× |
| Instruments Small (prettified) | 20,832 | 4,202 | 4.96× |
| Marine IK Reverse Small (minified) | 1,446 | 812 | 1.78× |
| Marine IK Reverse Small (prettified) | 5,204 | 4,038 | 1.29× |
| Marine IK Small (minified) | 1,472 | 836 | 1.76× |
| Marine IK Small (prettified) | 5,523 | 4,195 | 1.32× |
| Random Small (minified) | 15,114 | 3,467 | 4.36× |
| Random Small (prettified) | 17,871 | 4,266 | 4.19× |
| Twitter Small (minified) | 14,998 | 5,336 | 2.81× |
| Twitter Small (prettified) | 11,889 | 4,241 | 2.80× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,888 | 2,469 | 1,686 | 1.57× |
| Prettify | 5,590 | 3,875 | 2,214 | 1.44× |

#### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,173 | 480 | 2,207 | 1.44× |
| Double | 814 | 523 | 568 | 1.43× |
| Int64 | 4,713 | 1,701 | 3,172 | 1.49× |
| String | 8,921 | 3,872 | 8,925 | 1.00× |
| Uint64 | 4,733 | 1,847 | 3,189 | 1.48× |
| Apache Builds (minified) | 26,471 | 6,776 | 15,312 | 1.73× |
| Canada (minified) | 1,770 | 1,331 | 863 | 1.33× |
| Apache Builds (prettified) | 34,668 | 6,417 | 599 | 5.40× |
| Canada (prettified) | 5,672 | 3,404 | 374 | 1.67× |
| CitmCatalog (minified) | 10,533 | 4,071 | 8,456 | 1.25× |
| CitmCatalog (prettified) | 27,083 | 6,215 | 569 | 4.36× |
| Discord (minified) | 15,780 | 5,889 | 13,756 | 1.15× |
| Discord (prettified) | 24,986 | 5,514 | 532 | 4.53× |
| Github Events (minified) | 14,270 | 7,744 | 10,998 | 1.30× |
| Google Maps Response (minified) | 16,206 | 4,277 | 12,324 | 1.31× |
| Github Events (prettified) | 18,790 | 6,095 | 657 | 3.08× |
| Google Maps Response (prettified) | 33,895 | 6,456 | 559 | 5.25× |
| Instruments (minified) | 25,551 | 2,651 | 15,101 | 1.69× |
| Instruments (prettified) | 40,685 | 4,166 | 508 | 9.77× |
| Marine IK Reverse (minified) | 1,127 | 906 | 728 | 1.24× |
| Marine IK Reverse (prettified) | 5,728 | 3,793 | 245 (n/c) | 1.51× |
| Marine IK (minified) | 1,142 | 928 | 734 | 1.23× |
| Marine IK (prettified) | 5,934 | 3,785 | 287 | 1.57× |
| Random (minified) | 14,455 | 3,571 | 10,646 | 1.36× |
| Random (prettified) | 24,903 | 4,164 | 581 | 5.98× |
| Twitter (minified) | 12,671 | 6,022 | 9,475 | 1.34× |
| Twitter (prettified) | 17,726 | 4,839 | 564 | 3.66× |
| Apache Builds Small (minified) | 16,299 | 4,914 | 10,596 | 1.54× |
| Canada Small (minified) | 1,928 | 1,509 | 967 | 1.28× |
| Apache Builds Small (prettified) | 20,800 | 4,740 | 558 | 4.39× |
| Canada Small (prettified) | 5,846 | 3,609 | 427 | 1.62× |
| CitmCatalog Small (minified) | 9,094 | 2,893 | 7,098 | 1.28× |
| CitmCatalog Small (prettified) | 16,298 | 4,316 | 538 | 3.78× |
| Discord Small (minified) | 12,082 | 3,895 | 9,308 | 1.30× |
| Discord Small (prettified) | 17,396 | 4,360 | 522 | 3.99× |
| Github Events Small (minified) | 14,414 | 6,038 | 11,953 | 1.21× |
| Google Maps Response Small (minified) | 15,151 | 3,829 | 11,057 | 1.37× |
| Github Events Small (prettified) | 19,836 | 5,268 | 556 | 3.77× |
| Google Maps Response Small (prettified) | 34,072 | 6,335 | 569 | 5.38× |
| Instruments Small (minified) | 19,684 | 2,197 | 9,577 | 2.06× |
| Instruments Small (prettified) | 27,699 | 3,563 | 522 | 7.77× |
| Marine IK Reverse Small (minified) | 1,526 | 1,077 | 1,324 | 1.15× |
| Marine IK Reverse Small (prettified) | 7,704 | 4,160 | 552 | 1.85× |
| Marine IK Small (minified) | 1,586 | 1,147 | 1,374 | 1.15× |
| Marine IK Small (prettified) | 7,385 | 4,117 | 524 | 1.79× |
| Random Small (minified) | 17,983 | 4,225 | 12,462 | 1.44× |
| Random Small (prettified) | 30,423 | 4,919 | 588 | 6.18× |
| Twitter Small (minified) | 14,019 | 5,595 | 11,102 | 1.26× |
| Twitter Small (prettified) | 19,550 | 4,621 | 541 | 4.23× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 4,192 | 2,638 | 1,913 | 1.59× |
| Prettify | 5,203 | 3,453 | 2,678 | 1.51× |

## Questions from external review

An outside review of a draft raised these concerns, answered against the code as it stands.

### Can an overstore run past the buffer?

No, because every overstore lands inside the string's own size, not past it. `serializeJson` resizes the buffer to the full bound plus 64 bytes before writing, so the write pass works inside memory the string already owns. No write ever reaches capacity or the allocator's edge, and ASan sees only in-bounds writes. The one deliberate out-of-bounds access is a read: the page-safe short-string load reads up to 16 bytes past the source string, and under ASan it runs in a `JSONIFIER_NO_SANITIZE_ADDRESS` helper (see *Strings*).

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,269 MB/s on macOS / GCC (1.21× Glaze's 1,045, and ahead of the reflection writer's 1,089) and 1,273 MB/s on macOS / Clang (1.24× Glaze's 1,028). The AVX2 builds run the same test at 1,164–4,302 MB/s and lead Glaze by 1.31× (Windows / MSVC), 2.24× (Linux / Clang) and 2.03× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.91× on macOS / Clang (7,624 vs 3,993 MB/s) and 2.00× on macOS / GCC (9,325 vs 4,658), against 1.91–2.56× on the AVX2 builds. On macOS / GCC the reflection writer runs String reused at 11,102 MB/s, ahead of Jsonifier.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: freshly allocated the M2 Pro leads over Glaze are 1.21–1.24× against 1.31–2.24× on AVX2, and reused they rise to 1.91–2.00×, inside the AVX2 range of 1.91–2.56×. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

By the ranking, on both GCC builds freshly allocated; per call, only on macOS / GCC. The reflection writer leads in MB/s, 356 vs 335 on Linux / GCC (0.94×) and 311 vs 243 on macOS / GCC (0.78×), and both count as losses, but it writes 1,997 bytes against Jsonifier's 1,811. Per call, Jsonifier is faster on Linux / GCC (5,154 vs 5,354 ns) and slower on macOS / GCC (7,118 vs 6,133 ns). Reused, Jsonifier leads it on both: 826 vs 649 MB/s on Linux / GCC (1.27×) and 882 vs 598 MB/s on macOS / GCC (1.47×). Jsonifier beats Glaze on Double on all five builds in both runs.

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

This paper is dedicated to Sonny Moore (Skrillex), for showing what it looks like when someone's vocation is also their resting state.

## References

1. Nihilai Collective Corp. "CAFBERIHT." Nihilai Collective Corp, 2026. https://nihilai-collective.net/cafberiht
2. memcpy-decomp benchmark harness. https://github.com/nihilai-collective/memcpy-decomp
3. Nihilai Collective Corp. "Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier." Nihilai Collective Corp, 2026.
4. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
5. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
6. Glaze. https://github.com/stephenberry/glaze
7. simdjson. https://github.com/simdjson/simdjson
8. BenchmarkSuite. https://github.com/nihilai-collective/benchmarksuite
