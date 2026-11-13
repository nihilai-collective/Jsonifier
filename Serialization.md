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

On the Bool test this writer leads Glaze by 7.64–13.80× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.28× to 4.01×.

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

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3].

## Performance results

Jsonifier was fastest in 247 of the 255 write tests on five builds, tied 2 and lost 6. With the output string reused across iterations it was fastest in 252 of 255, tied 1 and lost 2. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other four are against simdjson's reflection writer, which exists only on GCC: Double, minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, and Double on macOS / GCC. The two freshly allocated ties are also against the reflection writer: Int64 and Uint64 on macOS / GCC. Reused, the one loss to Glaze is prettified Marine IK Reverse on Windows / MSVC; against the reflection writer, minified Marine IK Reverse Small on Linux / GCC is a loss and minified Marine IK Small on Linux / GCC a tie. Linux / Clang and macOS / Clang win all 51 tests in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.23–6.83× across the five builds and the reflection writer by 16–59× on the two GCC builds; Small documents lead them by 1.25–7.34× and 11.99–64.36×. Three AVX-512 builds are reported separately, in *AVX-512 builds* below: there Jsonifier was fastest in 150 of 153 write tests freshly allocated and 152 of 153 reused.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) [4] sweep published November 14, 2026, with Jsonifier [d4727ec](https://github.com/nihilai-collective/jsonifier/commit/d4727ec) [5], Glaze [a53ddd59](https://github.com/stephenberry/glaze/commit/a53ddd59) [6], simdjson [449f38e63](https://github.com/simdjson/simdjson/commit/449f38e63) [7] and BenchmarkSuite [27950a9](https://github.com/nihilai-collective/benchmarksuite/commit/27950a9) [8] on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md) [3]. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**Ranking.** Libraries are ordered by throughput. Variance is Bessel-corrected, and two neighbouring libraries tie when Welch's t-test (two-sided, p < 0.05) on their per-call times cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. All three ties in this sweep are at the top. Because the verdict is a significance test and not a ratio threshold, a lead of about 1% with tight samples is decisive: on Linux / GCC, Int64 freshly allocated (969 vs the reflection writer's 963 MB/s) is a win of 1.01×. Because the order follows MB/s, which credits each library its own output size, a library that is faster per call can still rank below one that writes more bytes. Double on both GCC builds is such a case: the reflection writer emits 1,997 bytes against Jsonifier's 1,811 and leads in MB/s (353 vs 337 on Linux / GCC, 259 vs 251 on macOS / GCC), while Jsonifier takes less time per call (5,128 vs 5,396 ns on Linux / GCC, 6,879 vs 7,354 ns on macOS / GCC). Both count as losses.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 48 | 2 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **247** | **2** | **6** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,131 | 103 | 11.01× |
| Double | 349 | 160 | 2.18× |
| Int64 | 506 | 436 | 1.16× |
| String | 1,402 | 1,046 | 1.34× |
| Uint64 | 516 | 429 | 1.20× |
| Apache Builds (minified) | 11,941 | 4,035 | 2.96× |
| Canada (minified) | 1,178 | 844 | 1.40× |
| Apache Builds (prettified) | 12,468 | 4,482 | 2.78× |
| Canada (prettified) | 2,689 | 1,386 | 1.94× |
| CitmCatalog (minified) | 8,609 | 4,226 | 2.04× |
| CitmCatalog (prettified) | 4,458 | 1,927 | 2.31× |
| Discord (minified) | 11,278 | 4,172 | 2.70× |
| Discord (prettified) | 11,537 | 4,048 | 2.85× |
| Github Events (minified) | 11,532 | 5,211 | 2.21× |
| Google Maps Response (minified) | 9,253 | 3,356 | 2.76× |
| Github Events (prettified) | 10,978 | 3,404 | 3.23× |
| Google Maps Response (prettified) | 14,329 | 5,187 | 2.76× |
| Instruments (minified) | 13,510 | 3,953 | 3.42× |
| Instruments (prettified) | 13,648 | 4,406 | 3.10× |
| Marine IK Reverse (minified) | 731 | 667 | 1.10× |
| Marine IK Reverse (prettified) | 2,223 | 1,813 | 1.23× |
| Marine IK (minified) | 725 | 588 | 1.23× |
| Marine IK (prettified) | 2,432 | 1,272 | 1.91× |
| Random (minified) | 3,816 | 2,996 | 1.27× |
| Random (prettified) | 4,104 | 2,127 | 1.93× |
| Twitter (minified) | 3,721 | 4,581 | 0.81× |
| Twitter (prettified) | 4,284 | 1,732 | 2.47× |
| Apache Builds Small (minified) | 9,765 | 3,593 | 2.72× |
| Canada Small (minified) | 1,601 | 1,119 | 1.43× |
| Apache Builds Small (prettified) | 10,194 | 3,917 | 2.60× |
| Canada Small (prettified) | 4,385 | 2,808 | 1.56× |
| CitmCatalog Small (minified) | 7,200 | 3,278 | 2.20× |
| CitmCatalog Small (prettified) | 7,954 | 4,214 | 1.89× |
| Discord Small (minified) | 9,474 | 3,623 | 2.61× |
| Discord Small (prettified) | 8,153 | 3,466 | 2.35× |
| Github Events Small (minified) | 9,644 | 2,951 | 3.27× |
| Google Maps Response Small (minified) | 9,045 | 3,396 | 2.66× |
| Github Events Small (prettified) | 10,164 | 3,897 | 2.61× |
| Google Maps Response Small (prettified) | 12,782 | 4,627 | 2.76× |
| Instruments Small (minified) | 10,740 | 3,339 | 3.22× |
| Instruments Small (prettified) | 10,346 | 3,310 | 3.13× |
| Marine IK Reverse Small (minified) | 1,104 | 903 | 1.22× |
| Marine IK Reverse Small (prettified) | 4,187 | 3,017 | 1.39× |
| Marine IK Small (minified) | 1,081 | 922 | 1.17× |
| Marine IK Small (prettified) | 4,320 | 3,008 | 1.44× |
| Random Small (minified) | 9,188 | 3,397 | 2.70× |
| Random Small (prettified) | 10,981 | 3,815 | 2.88× |
| Twitter Small (minified) | 10,514 | 4,568 | 2.30× |
| Twitter Small (prettified) | 8,046 | 3,317 | 2.43× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,852 | 1,825 | 1,439 | 1.56× |
| Prettify | 2,077 | 2,700 | 1,325 | 0.77× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,249 | 216 | 10.43× |
| Double | 340 | 305 | 1.11× |
| Int64 | 898 | 892 | 1.01× |
| String | 4,127 | 1,938 | 2.13× |
| Uint64 | 945 | 878 | 1.08× |
| Apache Builds (minified) | 16,402 | 5,648 | 2.90× |
| Canada (minified) | 1,939 | 958 | 2.02× |
| Apache Builds (prettified) | 18,986 | 5,266 | 3.61× |
| Canada (prettified) | 4,931 | 3,180 | 1.55× |
| CitmCatalog (minified) | 9,993 | 4,988 | 2.00× |
| CitmCatalog (prettified) | 15,355 | 5,196 | 2.96× |
| Discord (minified) | 14,480 | 4,936 | 2.93× |
| Discord (prettified) | 16,044 | 4,339 | 3.70× |
| Github Events (minified) | 16,546 | 5,336 | 3.10× |
| Google Maps Response (minified) | 11,742 | 3,734 | 3.14× |
| Github Events (prettified) | 16,327 | 4,645 | 3.51× |
| Google Maps Response (prettified) | 15,701 | 5,208 | 3.01× |
| Instruments (minified) | 18,260 | 4,887 | 3.74× |
| Instruments (prettified) | 20,241 | 4,952 | 4.09× |
| Marine IK Reverse (minified) | 1,153 | 641 | 1.80× |
| Marine IK Reverse (prettified) | 5,035 | 3,655 | 1.38× |
| Marine IK (minified) | 1,130 | 629 | 1.80× |
| Marine IK (prettified) | 5,015 | 2,880 | 1.74× |
| Random (minified) | 14,668 | 3,667 | 4.00× |
| Random (prettified) | 15,831 | 3,925 | 4.03× |
| Twitter (minified) | 19,477 | 6,220 | 3.13× |
| Twitter (prettified) | 18,159 | 4,478 | 4.06× |
| Apache Builds Small (minified) | 10,775 | 3,453 | 3.12× |
| Canada Small (minified) | 1,835 | 974 | 1.88× |
| Apache Builds Small (prettified) | 10,542 | 3,514 | 3.00× |
| Canada Small (prettified) | 4,484 | 3,599 | 1.25× |
| CitmCatalog Small (minified) | 7,479 | 2,765 | 2.70× |
| CitmCatalog Small (prettified) | 10,359 | 3,492 | 2.97× |
| Discord Small (minified) | 9,730 | 2,789 | 3.49× |
| Discord Small (prettified) | 11,128 | 2,851 | 3.90× |
| Github Events Small (minified) | 9,369 | 3,950 | 2.37× |
| Google Maps Response Small (minified) | 10,683 | 3,651 | 2.93× |
| Github Events Small (prettified) | 10,594 | 3,645 | 2.91× |
| Google Maps Response Small (prettified) | 15,690 | 4,791 | 3.28× |
| Instruments Small (minified) | 12,273 | 2,255 | 5.44× |
| Instruments Small (prettified) | 14,389 | 2,634 | 5.46× |
| Marine IK Reverse Small (minified) | 1,338 | 791 | 1.69× |
| Marine IK Reverse Small (prettified) | 5,290 | 3,650 | 1.45× |
| Marine IK Small (minified) | 1,491 | 818 | 1.82× |
| Marine IK Small (prettified) | 5,151 | 3,543 | 1.45× |
| Random Small (minified) | 13,186 | 3,469 | 3.80× |
| Random Small (prettified) | 14,819 | 3,988 | 3.72× |
| Twitter Small (minified) | 8,626 | 4,356 | 1.98× |
| Twitter Small (prettified) | 9,503 | 3,157 | 3.01× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,674 | 2,654 | 1,764 | 1.38× |
| Prettify | 4,616 | 3,528 | 2,314 | 1.31× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,709 | 200 | 250 | 6.84× |
| Double | 337 | 326 | 353 | 0.95× |
| Int64 | 969 | 940 | 963 | 1.01× |
| String | 4,323 | 2,202 | 4,195 | 1.03× |
| Uint64 | 948 | 861 | 896 | 1.06× |
| Apache Builds (minified) | 20,046 | 7,757 | 13,444 | 1.49× |
| Canada (minified) | 2,032 | 1,428 | 1,020 | 1.42× |
| Apache Builds (prettified) | 23,688 | 7,290 | 637 | 3.25× |
| Canada (prettified) | 6,057 | 3,236 | 265 | 1.87× |
| CitmCatalog (minified) | 12,450 | 5,935 | 11,129 | 1.12× |
| CitmCatalog (prettified) | 20,054 | 5,750 | 556 | 3.49× |
| Discord (minified) | 14,889 | 6,277 | 13,028 | 1.14× |
| Discord (prettified) | 20,448 | 5,947 | 558 | 3.44× |
| Github Events (minified) | 14,809 | 7,003 | 10,984 | 1.35× |
| Google Maps Response (minified) | 15,748 | 4,580 | 9,955 | 1.58× |
| Github Events (prettified) | 18,610 | 5,183 | 626 | 3.59× |
| Google Maps Response (prettified) | 21,155 | 6,677 | 533 | 3.17× |
| Instruments (minified) | 20,268 | 5,638 | 15,583 | 1.30× |
| Instruments (prettified) | 26,011 | 5,513 | 472 | 4.72× |
| Marine IK Reverse (minified) | 1,282 | 982 | 884 | 1.30× |
| Marine IK Reverse (prettified) | 6,386 | 3,654 | 273 | 1.75× |
| Marine IK (minified) | 1,272 | 960 | 890 | 1.32× |
| Marine IK (prettified) | 6,296 | 3,152 | 387 | 2.00× |
| Random (minified) | 17,737 | 4,602 | 11,477 | 1.55× |
| Random (prettified) | 21,490 | 4,844 | 525 | 4.44× |
| Twitter (minified) | 17,544 | 7,766 | 11,122 | 1.58× |
| Twitter (prettified) | 20,970 | 5,079 | 515 | 4.13× |
| Apache Builds Small (minified) | 12,339 | 4,556 | 7,230 | 1.71× |
| Canada Small (minified) | 2,090 | 1,473 | 1,086 | 1.42× |
| Apache Builds Small (prettified) | 14,676 | 4,118 | 548 | 3.56× |
| Canada Small (prettified) | 6,079 | 3,645 | 414 | 1.67× |
| CitmCatalog Small (minified) | 7,568 | 3,402 | 6,085 | 1.24× |
| CitmCatalog Small (prettified) | 15,132 | 3,029 | 488 | 5.00× |
| Discord Small (minified) | 10,225 | 3,770 | 7,830 | 1.31× |
| Discord Small (prettified) | 14,880 | 3,435 | 524 | 4.33× |
| Github Events Small (minified) | 8,630 | 5,355 | 6,710 | 1.29× |
| Google Maps Response Small (minified) | 13,391 | 3,813 | 8,273 | 1.62× |
| Github Events Small (prettified) | 11,332 | 3,740 | 556 | 3.03× |
| Google Maps Response Small (prettified) | 23,767 | 5,860 | 522 | 4.06× |
| Instruments Small (minified) | 12,188 | 3,909 | 8,326 | 1.46× |
| Instruments Small (prettified) | 21,007 | 2,973 | 523 | 7.07× |
| Marine IK Reverse Small (minified) | 1,413 | 1,250 | 1,522 | 0.93× |
| Marine IK Reverse Small (prettified) | 6,320 | 3,137 | 527 | 2.01× |
| Marine IK Small (minified) | 1,388 | 1,220 | 1,522 | 0.91× |
| Marine IK Small (prettified) | 6,477 | 3,252 | 527 | 1.99× |
| Random Small (minified) | 13,448 | 4,235 | 8,088 | 1.66× |
| Random Small (prettified) | 24,797 | 4,517 | 516 | 5.49× |
| Twitter Small (minified) | 8,941 | 5,788 | 6,737 | 1.33× |
| Twitter Small (prettified) | 12,089 | 3,648 | 550 | 3.31× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,286 | 2,494 | 1,878 | 1.32× |
| Prettify | 4,171 | 3,218 | 2,430 | 1.30× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,758 | 103 | 212 | 8.31× |
| Double | 251 | 167 | 259 | 0.97× |
| Int64 | 634 | 467 | 652 | 0.97× |
| String | 1,371 | 920 (n/c) | 1,125 | 1.22× |
| Uint64 | 769 | 536 | 763 | 1.01× |
| Apache Builds (minified) | 13,319 | 4,574 | 7,542 | 1.77× |
| Canada (minified) | 2,199 | 1,589 | 769 | 1.38× |
| Apache Builds (prettified) | 12,974 | 3,979 | 337 | 3.26× |
| Canada (prettified) | 6,625 | 3,121 | 267 | 2.12× |
| CitmCatalog (minified) | 7,642 | 2,017 | 7,073 | 1.08× |
| CitmCatalog (prettified) | 16,617 | 3,634 | 307 | 4.57× |
| Discord (minified) | 10,146 | 2,476 | 9,312 | 1.09× |
| Discord (prettified) | 13,497 | 2,283 | 333 | 5.91× |
| Github Events (minified) | 14,918 | 3,538 | 10,962 | 1.36× |
| Google Maps Response (minified) | 10,220 | 1,727 | 7,912 | 1.29× |
| Github Events (prettified) | 15,255 | 3,165 | 402 | 4.82× |
| Google Maps Response (prettified) | 18,380 | 2,720 | 309 | 6.76× |
| Instruments (minified) | 13,998 | 1,793 | 7,760 | 1.80× |
| Instruments (prettified) | 12,863 | 1,882 | 310 | 6.83× |
| Marine IK Reverse (minified) | 1,161 | 863 | 744 | 1.35× |
| Marine IK Reverse (prettified) | 6,421 | 2,904 | 306 (n/c) | 2.21× |
| Marine IK (minified) | 1,315 | 793 | 719 | 1.66× |
| Marine IK (prettified) | 6,222 (n/c) | 2,558 | 272 (n/c) | 2.43× |
| Random (minified) | 11,451 | 2,171 | 7,909 | 1.45× |
| Random (prettified) | 14,614 | 2,516 | 320 | 5.81× |
| Twitter (minified) | 16,338 | 3,735 | 12,108 | 1.35× |
| Twitter (prettified) | 17,646 | 3,514 | 420 | 5.02× |
| Apache Builds Small (minified) | 9,339 | 2,874 | 6,001 | 1.56× |
| Canada Small (minified) | 2,084 | 1,658 | 911 | 1.26× |
| Apache Builds Small (prettified) | 10,386 | 3,672 | 345 | 2.83× |
| Canada Small (prettified) | 7,195 | 2,886 | 287 | 2.49× |
| CitmCatalog Small (minified) | 8,531 | 1,851 | 7,424 | 1.15× |
| CitmCatalog Small (prettified) | 15,936 | 2,447 | 324 | 6.51× |
| Discord Small (minified) | 10,008 | 2,599 | 6,936 | 1.44× |
| Discord Small (prettified) | 12,396 | 2,271 | 271 | 5.46× |
| Github Events Small (minified) | 11,202 | 2,945 | 9,973 | 1.12× |
| Google Maps Response Small (minified) | 10,083 | 1,767 | 8,047 | 1.25× |
| Github Events Small (prettified) | 14,164 | 2,758 | 377 | 5.14× |
| Google Maps Response Small (prettified) | 19,847 | 2,705 | 308 | 7.34× |
| Instruments Small (minified) | 14,024 | 1,911 | 7,672 | 1.83× |
| Instruments Small (prettified) | 15,667 (n/c) | 2,313 | 330 | 6.77× |
| Marine IK Reverse Small (minified) | 1,899 | 1,029 | 1,608 | 1.18× |
| Marine IK Reverse Small (prettified) | 7,698 | 2,960 | 288 | 2.60× |
| Marine IK Small (minified) | 1,885 | 994 | 1,718 | 1.10× |
| Marine IK Small (prettified) | 7,126 | 2,961 | 265 | 2.41× |
| Random Small (minified) | 12,124 | 1,949 | 8,673 | 1.40× |
| Random Small (prettified) | 13,172 | 2,458 | 307 | 5.36× |
| Twitter Small (minified) | 12,350 | 3,165 | 9,440 | 1.31× |
| Twitter Small (prettified) | 11,757 (n/c) | 2,985 | 361 | 3.94× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,039 | 1,425 | 1,107 | 1.43× |
| Prettify | 2,663 | 2,088 | 1,651 | 1.28× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,019 | 132 | 7.70× |
| Double | 301 | 223 | 1.35× |
| Int64 | 3,026 | 551 | 5.49× |
| String | 1,294 | 1,139 | 1.14× |
| Uint64 | 2,945 | 557 | 5.28× |
| Apache Builds (minified) | 12,205 | 3,745 | 3.26× |
| Canada (minified) | 2,954 | 1,884 | 1.57× |
| Apache Builds (prettified) | 12,096 | 3,564 | 3.39× |
| Canada (prettified) | 7,387 | 3,551 | 2.08× |
| CitmCatalog (minified) | 8,249 | 2,219 | 3.72× |
| CitmCatalog (prettified) | 14,788 | 4,224 | 3.50× |
| Discord (minified) | 12,630 | 3,362 | 3.76× |
| Discord (prettified) | 13,355 | 2,991 | 4.46× |
| Github Events (minified) | 14,942 | 4,866 | 3.07× |
| Google Maps Response (minified) | 10,230 | 2,354 | 4.35× |
| Github Events (prettified) | 14,125 | 3,976 | 3.55× |
| Google Maps Response (prettified) | 14,161 | 3,820 | 3.71× |
| Instruments (minified) | 13,804 | 2,346 | 5.88× |
| Instruments (prettified) | 16,114 | 3,215 | 5.01× |
| Marine IK Reverse (minified) | 1,555 | 922 | 1.69× |
| Marine IK Reverse (prettified) | 4,561 | 2,642 (n/c) | 1.73× |
| Marine IK (minified) | 1,562 | 896 | 1.74× |
| Marine IK (prettified) | 5,631 | 3,000 | 1.88× |
| Random (minified) | 11,105 | 2,755 | 4.03× |
| Random (prettified) | 11,642 | 3,288 | 3.54× |
| Twitter (minified) | 15,480 | 4,524 | 3.42× |
| Twitter (prettified) | 15,788 | 4,084 | 3.87× |
| Apache Builds Small (minified) | 9,241 | 2,953 | 3.13× |
| Canada Small (minified) | 2,785 | 1,606 | 1.73× |
| Apache Builds Small (prettified) | 9,839 | 3,258 | 3.02× |
| Canada Small (prettified) | 7,022 | 3,418 | 2.05× |
| CitmCatalog Small (minified) | 7,867 | 1,961 | 4.01× |
| CitmCatalog Small (prettified) | 12,066 | 2,855 | 4.23× |
| Discord Small (minified) | 11,696 | 3,111 | 3.76× |
| Discord Small (prettified) | 12,349 | 3,072 | 4.02× |
| Github Events Small (minified) | 11,020 | 3,526 | 3.13× |
| Google Maps Response Small (minified) | 9,604 | 2,314 | 4.15× |
| Github Events Small (prettified) | 10,382 | 3,233 | 3.21× |
| Google Maps Response Small (prettified) | 14,143 | 3,593 | 3.94× |
| Instruments Small (minified) | 13,546 | 1,995 | 6.79× |
| Instruments Small (prettified) | 14,446 | 3,213 | 4.50× |
| Marine IK Reverse Small (minified) | 2,014 | 934 | 2.16× |
| Marine IK Reverse Small (prettified) | 5,746 | 3,296 | 1.74× |
| Marine IK Small (minified) | 1,996 | 1,059 | 1.88× |
| Marine IK Small (prettified) | 5,777 | 3,318 | 1.74× |
| Random Small (minified) | 10,346 | 2,778 | 3.72× |
| Random Small (prettified) | 11,336 | 3,115 | 3.64× |
| Twitter Small (minified) | 10,161 | 3,655 | 2.78× |
| Twitter Small (prettified) | 11,584 | 3,248 | 3.57× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,292 | 1,744 | 1,250 | 1.31× |
| Prettify | 3,267 | 2,515 | 1,630 | 1.30× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 252 of the 255 write tests, ties 1 and loses 2. On Windows / MSVC Glaze wins prettified Marine IK Reverse (3,081 vs 2,854 MB/s, 0.93×). On Linux / GCC the reflection writer wins minified Marine IK Reverse Small (1,539 vs 1,470 MB/s, 0.95×) and ties minified Marine IK Small (1,548 vs 1,449 MB/s).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 1 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **252** | **1** | **2** |

#### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,648 | 553 | 2.98× |
| Double | 552 | 437 | 1.26× |
| Int64 | 3,916 | 1,570 | 2.49× |
| String | 8,545 | 3,327 | 2.57× |
| Uint64 | 4,492 | 1,555 | 2.89× |
| Apache Builds (minified) | 12,664 | 4,536 | 2.79× |
| Canada (minified) | 1,421 | 1,139 | 1.25× |
| Apache Builds (prettified) | 12,617 | 5,013 | 2.52× |
| Canada (prettified) | 4,084 | 3,130 | 1.30× |
| CitmCatalog (minified) | 8,565 | 4,572 | 1.87× |
| CitmCatalog (prettified) | 13,076 | 7,191 | 1.82× |
| Discord (minified) | 11,745 | 4,715 | 2.49× |
| Discord (prettified) | 12,117 | 4,393 | 2.76× |
| Github Events (minified) | 12,214 | 6,291 | 1.94× |
| Google Maps Response (minified) | 9,795 | 3,853 | 2.54× |
| Github Events (prettified) | 11,209 | 3,676 | 3.05× |
| Google Maps Response (prettified) | 15,528 | 5,619 | 2.76× |
| Instruments (minified) | 14,023 | 4,410 | 3.18× |
| Instruments (prettified) | 13,752 | 5,117 | 2.69× |
| Marine IK Reverse (minified) | 825 | 726 | 1.14× |
| Marine IK Reverse (prettified) | 2,854 | 3,081 | 0.93× |
| Marine IK (minified) | 806 | 738 | 1.09× |
| Marine IK (prettified) | 3,496 | 3,334 | 1.05× |
| Random (minified) | 11,282 | 3,223 | 3.50× |
| Random (prettified) | 12,814 | 4,362 | 2.94× |
| Twitter (minified) | 15,906 | 5,629 | 2.83× |
| Twitter (prettified) | 13,417 | 3,181 | 4.22× |
| Apache Builds Small (minified) | 11,185 | 4,160 | 2.69× |
| Canada Small (minified) | 1,654 | 1,242 | 1.33× |
| Apache Builds Small (prettified) | 11,520 | 4,455 | 2.59× |
| Canada Small (prettified) | 4,594 | 3,127 | 1.47× |
| CitmCatalog Small (minified) | 8,022 | 4,225 | 1.90× |
| CitmCatalog Small (prettified) | 8,556 | 5,261 | 1.63× |
| Discord Small (minified) | 10,459 | 4,529 | 2.31× |
| Discord Small (prettified) | 9,060 | 4,268 | 2.12× |
| Github Events Small (minified) | 11,871 | 4,968 | 2.39× |
| Google Maps Response Small (minified) | 9,279 | 3,871 | 2.40× |
| Github Events Small (prettified) | 11,780 | 5,181 | 2.27× |
| Google Maps Response Small (prettified) | 15,347 | 6,010 | 2.55× |
| Instruments Small (minified) | 12,451 | 5,079 | 2.45× |
| Instruments Small (prettified) | 11,847 | 4,202 (n/c) | 2.82× |
| Marine IK Reverse Small (minified) | 1,125 | 986 | 1.14× |
| Marine IK Reverse Small (prettified) | 4,356 | 3,479 | 1.25× |
| Marine IK Small (minified) | 1,118 | 981 | 1.14× |
| Marine IK Small (prettified) | 4,570 | 3,479 | 1.31× |
| Random Small (minified) | 11,517 | 4,122 | 2.79× |
| Random Small (prettified) | 13,350 | 5,057 | 2.64× |
| Twitter Small (minified) | 12,698 | 5,965 | 2.13× |
| Twitter Small (prettified) | 9,373 | 3,733 (n/c) | 2.51× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,864 | 1,813 | 1,442 | 1.58× |
| Prettify | 3,566 | 2,889 | 1,461 | 1.23× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,599 | 882 | 5.22× |
| Double | 896 | 614 | 1.46× |
| Int64 | 5,109 | 2,155 | 2.37× |
| String | 11,809 | 4,535 | 2.60× |
| Uint64 | 4,339 | 2,201 | 1.97× |
| Apache Builds (minified) | 16,712 | 6,319 | 2.64× |
| Canada (minified) | 1,909 | 999 | 1.91× |
| Apache Builds (prettified) | 19,098 | 5,814 | 3.29× |
| Canada (prettified) | 5,082 | 3,913 | 1.30× |
| CitmCatalog (minified) | 10,005 | 5,762 | 1.74× |
| CitmCatalog (prettified) | 14,678 | 6,880 (n/c) | 2.13× |
| Discord (minified) | 14,940 | 5,423 | 2.75× |
| Discord (prettified) | 16,306 | 4,490 | 3.63× |
| Github Events (minified) | 16,780 | 5,963 | 2.81× |
| Google Maps Response (minified) | 12,458 | 4,240 | 2.94× |
| Github Events (prettified) | 16,421 | 5,205 | 3.15× |
| Google Maps Response (prettified) | 16,249 | 5,679 | 2.86× |
| Instruments (minified) | 18,686 | 5,099 | 3.66× |
| Instruments (prettified) | 20,555 | 5,761 | 3.57× |
| Marine IK Reverse (minified) | 1,132 | 642 | 1.76× |
| Marine IK Reverse (prettified) | 4,914 | 3,998 | 1.23× |
| Marine IK (minified) | 1,154 | 656 | 1.76× |
| Marine IK (prettified) | 5,024 | 4,075 | 1.23× |
| Random (minified) | 14,726 | 4,004 | 3.68× |
| Random (prettified) | 15,890 | 4,492 | 3.54× |
| Twitter (minified) | 19,567 | 6,896 | 2.84× |
| Twitter (prettified) | 18,199 | 5,459 | 3.33× |
| Apache Builds Small (minified) | 12,445 | 3,504 | 3.55× |
| Canada Small (minified) | 1,903 | 1,006 | 1.89× |
| Apache Builds Small (prettified) | 13,103 | 3,839 | 3.41× |
| Canada Small (prettified) | 4,706 | 3,817 | 1.23× |
| CitmCatalog Small (minified) | 8,436 | 3,054 | 2.76× |
| CitmCatalog Small (prettified) | 11,303 | 3,783 | 2.99× |
| Discord Small (minified) | 11,669 | 3,182 | 3.67× |
| Discord Small (prettified) | 12,751 | 2,998 | 4.25× |
| Github Events Small (minified) | 12,431 | 4,364 | 2.85× |
| Google Maps Response Small (minified) | 11,612 | 3,992 | 2.91× |
| Github Events Small (prettified) | 12,299 | 3,981 | 3.09× |
| Google Maps Response Small (prettified) | 17,602 | 5,362 | 3.28× |
| Instruments Small (minified) | 14,417 | 2,602 | 5.54× |
| Instruments Small (prettified) | 16,040 | 2,721 | 5.89× |
| Marine IK Reverse Small (minified) | 1,396 | 827 | 1.69× |
| Marine IK Reverse Small (prettified) | 5,353 | 3,995 | 1.34× |
| Marine IK Small (minified) | 1,570 | 832 | 1.89× |
| Marine IK Small (prettified) | 5,300 | 3,989 | 1.33× |
| Random Small (minified) | 14,638 | 3,853 | 3.80× |
| Random Small (prettified) | 16,909 | 4,610 | 3.67× |
| Twitter Small (minified) | 10,109 | 4,433 | 2.28× |
| Twitter Small (prettified) | 10,728 | 3,794 | 2.83× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,721 | 2,659 | 1,737 | 1.40× |
| Prettify | 4,606 | 3,539 | 2,292 | 1.30× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,222 | 675 | 2,955 | 1.09× |
| Double | 878 | 706 | 650 | 1.24× |
| Int64 | 5,186 | 2,727 | 4,002 | 1.30× |
| String | 11,024 | 5,811 | 9,526 | 1.16× |
| Uint64 | 6,353 | 2,682 | 4,127 | 1.54× |
| Apache Builds (minified) | 20,109 | 9,344 | 13,586 | 1.48× |
| Canada (minified) | 2,064 | 1,518 | 1,037 | 1.36× |
| Apache Builds (prettified) | 23,208 | 8,198 | 627 | 2.83× |
| Canada (prettified) | 6,063 | 3,925 | 388 | 1.54× |
| CitmCatalog (minified) | 12,500 | 7,168 | 11,366 | 1.10× |
| CitmCatalog (prettified) | 20,118 | 7,933 | 553 | 2.54× |
| Discord (minified) | 15,183 | 7,182 | 13,345 | 1.14× |
| Discord (prettified) | 20,741 | 7,122 | 561 | 2.91× |
| Github Events (minified) | 15,032 | 8,582 | 11,312 | 1.33× |
| Google Maps Response (minified) | 16,965 | 5,051 | 10,131 | 1.67× |
| Github Events (prettified) | 18,554 | 6,225 | 639 | 2.98× |
| Google Maps Response (prettified) | 21,908 | 7,382 | 530 | 2.97× |
| Instruments (minified) | 20,500 | 6,577 | 15,531 | 1.32× |
| Instruments (prettified) | 26,583 | 6,349 | 476 | 4.19× |
| Marine IK Reverse (minified) | 1,280 | 987 | 884 | 1.30× |
| Marine IK Reverse (prettified) | 6,382 | 4,058 | 309 | 1.57× |
| Marine IK (minified) | 1,270 | 1,007 | 890 | 1.26× |
| Marine IK (prettified) | 6,316 | 4,077 | 334 | 1.55× |
| Random (minified) | 17,827 | 5,207 | 11,372 | 1.57× |
| Random (prettified) | 21,902 | 5,529 | 518 | 3.96× |
| Twitter (minified) | 17,565 | 8,762 | 10,884 | 1.61× |
| Twitter (prettified) | 20,989 | 6,544 | 511 | 3.21× |
| Apache Builds Small (minified) | 13,724 | 5,244 | 8,123 | 1.69× |
| Canada Small (minified) | 2,114 | 1,623 | 1,092 | 1.30× |
| Apache Builds Small (prettified) | 18,444 | 4,563 | 545 | 4.04× |
| Canada Small (prettified) | 6,303 | 4,138 | 414 | 1.52× |
| CitmCatalog Small (minified) | 8,128 | 4,086 | 6,605 | 1.23× |
| CitmCatalog Small (prettified) | 16,355 | 3,375 | 504 | 4.85× |
| Discord Small (minified) | 11,295 | 4,755 | 8,484 | 1.33× |
| Discord Small (prettified) | 15,979 | 3,923 | 525 | 4.07× |
| Github Events Small (minified) | 10,106 | 6,001 | 7,663 | 1.32× |
| Google Maps Response Small (minified) | 14,332 | 4,195 | 8,694 | 1.65× |
| Github Events Small (prettified) | 14,325 | 4,509 | 535 | 3.18× |
| Google Maps Response Small (prettified) | 29,839 | 7,120 | 519 | 4.19× |
| Instruments Small (minified) | 13,548 | 4,593 | 8,769 | 1.54× |
| Instruments Small (prettified) | 24,152 | 2,964 | 526 | 8.15× |
| Marine IK Reverse Small (minified) | 1,470 | 1,354 | 1,539 | 0.95× |
| Marine IK Reverse Small (prettified) | 6,438 | 3,661 | 529 | 1.76× |
| Marine IK Small (minified) | 1,449 | 1,243 | 1,548 | 0.94× |
| Marine IK Small (prettified) | 6,647 | 3,566 | 534 | 1.86× |
| Random Small (minified) | 16,608 | 4,882 | 9,089 | 1.83× |
| Random Small (prettified) | 28,376 | 5,355 | 516 | 5.30× |
| Twitter Small (minified) | 10,293 | 6,727 | 7,564 | 1.36× |
| Twitter Small (prettified) | 14,341 | 4,249 | 554 | 3.38× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,302 | 2,495 | 1,856 | 1.32× |
| Prettify | 4,167 | 3,180 | 2,406 | 1.31× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,217 | 635 | 2,380 | 1.35× |
| Double | 791 | 577 | 669 | 1.18× |
| Int64 | 5,070 | 1,977 | 3,080 | 1.65× |
| String | 9,346 | 4,487 | 8,571 | 1.09× |
| Uint64 | 5,107 | 2,101 | 3,077 | 1.66× |
| Apache Builds (minified) | 9,579 | 5,152 | 8,623 | 1.11× |
| Canada (minified) | 2,225 | 1,556 | 724 | 1.43× |
| Apache Builds (prettified) | 13,242 | 4,794 | 347 | 2.76× |
| Canada (prettified) | 6,797 | 3,555 | 275 | 1.91× |
| CitmCatalog (minified) | 8,008 | 1,816 | 7,310 | 1.10× |
| CitmCatalog (prettified) | 17,475 | 4,210 | 325 | 4.15× |
| Discord (minified) | 12,275 | 2,969 | 9,522 | 1.29× |
| Discord (prettified) | 12,441 | 2,528 | 334 | 4.92× |
| Github Events (minified) | 16,024 | 4,363 | 11,464 | 1.40× |
| Google Maps Response (minified) | 10,632 | 1,865 | 8,159 | 1.30× |
| Github Events (prettified) | 16,200 | 3,960 | 405 | 4.09× |
| Google Maps Response (prettified) | 20,712 | 3,056 | 312 | 6.78× |
| Instruments (minified) | 14,302 | 2,047 | 7,895 | 1.81× |
| Instruments (prettified) | 16,353 | 2,406 | 290 | 6.80× |
| Marine IK Reverse (minified) | 1,258 | 858 | 721 | 1.47× |
| Marine IK Reverse (prettified) | 6,285 | 2,862 | 303 | 2.20× |
| Marine IK (minified) | 1,282 | 846 | 694 | 1.52× |
| Marine IK (prettified) | 6,011 | 3,133 | 272 | 1.92× |
| Random (minified) | 11,536 | 2,322 | 7,791 | 1.48× |
| Random (prettified) | 14,781 | 2,680 | 319 | 5.51× |
| Twitter (minified) | 16,458 | 4,110 | 12,229 | 1.35× |
| Twitter (prettified) | 17,836 | 4,642 | 396 | 3.84× |
| Apache Builds Small (minified) | 10,631 | 3,096 | 6,943 | 1.53× |
| Canada Small (minified) | 2,099 | 1,934 | 914 | 1.09× |
| Apache Builds Small (prettified) | 11,870 | 4,420 | 349 | 2.69× |
| Canada Small (prettified) | 7,346 | 3,688 | 262 | 1.99× |
| CitmCatalog Small (minified) | 9,470 | 2,077 | 8,157 | 1.16× |
| CitmCatalog Small (prettified) | 17,342 | 2,755 | 315 | 6.29× |
| Discord Small (minified) | 11,434 | 2,290 | 9,040 | 1.26× |
| Discord Small (prettified) | 13,965 | 2,428 | 332 | 5.75× |
| Github Events Small (minified) | 14,293 | 3,615 | 11,324 | 1.26× |
| Google Maps Response Small (minified) | 10,775 | 1,923 | 8,437 | 1.28× |
| Github Events Small (prettified) | 16,164 | 3,255 | 383 | 4.97× |
| Google Maps Response Small (prettified) | 21,252 | 3,043 | 312 | 6.98× |
| Instruments Small (minified) | 15,256 | 2,181 | 8,351 | 1.83× |
| Instruments Small (prettified) | 17,997 | 2,299 | 328 | 7.83× |
| Marine IK Reverse Small (minified) | 1,931 | 1,081 | 1,622 | 1.19× |
| Marine IK Reverse Small (prettified) | 7,047 | 3,137 | 305 | 2.25× |
| Marine IK Small (minified) | 1,890 | 1,091 | 1,574 | 1.20× |
| Marine IK Small (prettified) | 7,921 | 3,554 | 288 | 2.23× |
| Random Small (minified) | 12,286 | 2,303 | 8,226 (n/c) | 1.49× |
| Random Small (prettified) | 14,261 | 2,745 | 302 | 5.20× |
| Twitter Small (minified) | 11,092 | 3,208 (n/c) | 9,748 | 1.14× |
| Twitter Small (prettified) | 14,557 | 3,499 | 333 | 4.16× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,110 | 1,303 | 1,000 | 1.62× |
| Prettify | 2,837 | 2,195 | 1,644 | 1.29× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,266 | 766 | 1.65× |
| Double | 886 | 707 | 1.25× |
| Int64 | 3,246 | 2,180 | 1.49× |
| String | 7,889 | 4,297 | 1.84× |
| Uint64 | 3,458 | 2,248 | 1.54× |
| Apache Builds (minified) | 12,643 | 4,420 | 2.86× |
| Canada (minified) | 2,953 | 1,972 | 1.50× |
| Apache Builds (prettified) | 12,389 | 3,924 | 3.16× |
| Canada (prettified) | 7,338 | 4,017 | 1.83× |
| CitmCatalog (minified) | 8,256 | 2,343 | 3.52× |
| CitmCatalog (prettified) | 14,790 | 4,913 | 3.01× |
| Discord (minified) | 13,276 | 3,909 | 3.40× |
| Discord (prettified) | 13,714 | 3,426 | 4.00× |
| Github Events (minified) | 16,038 | 6,122 | 2.62× |
| Google Maps Response (minified) | 10,525 | 2,628 | 4.00× |
| Github Events (prettified) | 15,499 | 5,210 | 2.98× |
| Google Maps Response (prettified) | 15,541 | 4,191 | 3.71× |
| Instruments (minified) | 14,489 | 2,540 | 5.70× |
| Instruments (prettified) | 16,580 | 3,639 | 4.56× |
| Marine IK Reverse (minified) | 1,562 | 922 | 1.69× |
| Marine IK Reverse (prettified) | 4,433 | 2,688 | 1.65× |
| Marine IK (minified) | 1,564 | 904 | 1.73× |
| Marine IK (prettified) | 5,611 | 3,434 | 1.63× |
| Random (minified) | 11,211 | 2,962 | 3.79× |
| Random (prettified) | 13,453 | 3,597 | 3.74× |
| Twitter (minified) | 15,564 | 5,022 | 3.10× |
| Twitter (prettified) | 15,851 | 4,745 | 3.34× |
| Apache Builds Small (minified) | 10,104 | 3,788 | 2.67× |
| Canada Small (minified) | 2,896 | 1,855 | 1.56× |
| Apache Builds Small (prettified) | 10,511 | 3,895 | 2.70× |
| Canada Small (prettified) | 7,337 | 3,907 | 1.88× |
| CitmCatalog Small (minified) | 9,528 | 2,029 | 4.69× |
| CitmCatalog Small (prettified) | 13,317 | 3,241 | 4.11× |
| Discord Small (minified) | 13,340 | 4,025 | 3.31× |
| Discord Small (prettified) | 13,830 | 3,597 | 3.85× |
| Github Events Small (minified) | 13,527 | 4,948 | 2.73× |
| Google Maps Response Small (minified) | 10,179 | 2,612 | 3.90× |
| Github Events Small (prettified) | 14,512 | 4,392 | 3.30× |
| Google Maps Response Small (prettified) | 14,984 | 4,339 | 3.45× |
| Instruments Small (minified) | 15,380 | 2,357 | 6.52× |
| Instruments Small (prettified) | 16,662 | 3,723 | 4.48× |
| Marine IK Reverse Small (minified) | 2,062 | 1,173 | 1.76× |
| Marine IK Reverse Small (prettified) | 5,993 | 3,931 | 1.52× |
| Marine IK Small (minified) | 2,066 | 1,172 | 1.76× |
| Marine IK Small (prettified) | 6,075 | 3,955 | 1.54× |
| Random Small (minified) | 11,337 | 3,401 | 3.33× |
| Random Small (prettified) | 13,686 | 3,925 | 3.49× |
| Twitter Small (minified) | 13,413 | 4,573 | 2.93× |
| Twitter Small (prettified) | 14,361 | 4,453 | 3.23× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,274 | 1,752 | 1,263 | 1.30× |
| Prettify | 3,269 | 2,542 | 1,638 | 1.29× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,581 vs Jsonifier's 3,721 MB/s (0.81×). Reused, Jsonifier wins by 2.83× (15,906 vs 5,629).
- **Prettify, Windows / MSVC.** Glaze runs 2,700 vs 2,077 MB/s (0.77×), with simdjson On Demand at 1,325. All three converged. Reused, Jsonifier wins by 1.23× (3,566 vs 2,889).
- **Double, Linux / GCC.** The reflection writer runs 353 vs 337 MB/s (0.95×), with Glaze at 326, though Jsonifier is faster per call (see *Ranking*). Reused, Jsonifier wins by 1.24× over Glaze (878 vs 706), with the reflection writer at 650.
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,522 vs 1,413 MB/s (0.93×), with Glaze at 1,250. It also wins reused, 1,539 vs 1,470 MB/s (0.95×), with Glaze at 1,354.
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,522 vs 1,388 MB/s (0.91×), with Glaze at 1,220. Reused, the two tie (1,548 vs 1,449 MB/s), with Glaze at 1,243.
- **Double, macOS / GCC.** The reflection writer runs 259 vs 251 MB/s (0.97×), with Glaze at 167, though Jsonifier is faster per call (see *Ranking*). Reused, Jsonifier wins by 1.18× over the reflection writer (791 vs 669), with Glaze at 577.

The two freshly allocated ties are both against the reflection writer on macOS / GCC. On Int64 it runs 652 vs Jsonifier's 634 MB/s (Glaze at 467), and Jsonifier wins reused by 1.65× (5,070 vs 3,080). On Uint64 Jsonifier runs 769 vs its 763 MB/s (Glaze at 536), and wins reused by 1.66× (5,107 vs 3,077).

One reused non-win has no freshly allocated counterpart. Prettified Marine IK Reverse on Windows / MSVC wins freshly allocated by 1.23× (2,223 vs Glaze's 1,813) and loses to Glaze reused (2,854 vs 3,081 MB/s).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.08–2.86× and prettified documents by 16–59×; reused, by 1.10–3.08× and 16–66×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 42 of the 44 Small document tests in both runs. The non-wins are minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC: both losses freshly allocated, and a tie and a loss reused. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.31–1.56× freshly allocated and 1.30–1.62× reused. It wins Prettify on all five reused, by 1.23–1.31×, and on four of five freshly allocated, by 1.28–1.31×; the exception is Windows / MSVC, where Glaze runs 2,700 vs 2,077 MB/s (0.77×). Glaze is the next-fastest library in every case.

## AVX-512 builds

The results above come from AVX2 and NEON builds. The same 51 write tests also ran on three AVX-512 builds, Linux / Clang, Linux / GCC and Windows / MSVC, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor. As on AVX2, simdjson's reflection writer runs only on the Linux / GCC build.

Freshly allocated, Jsonifier was fastest in 150 of the 153 write tests, tied 1 and lost 2. The two losses are the same two that it loses on the AVX2 Windows / MSVC build: minified Twitter (Glaze 4,570 vs 3,085 MB/s, 0.68×) and Prettify (Glaze 3,240 vs 2,463 MB/s, 0.76×, with simdjson On Demand at 1,492). The tie is String on Linux / GCC against the reflection writer (3,968 vs 3,966 MB/s). Reused, it was fastest in 152 and tied 1, prettified Marine IK Reverse on Windows / MSVC (3,218 vs Glaze's 3,168 MB/s). Both freshly allocated losses become wins reused: minified Twitter by 2.48× (12,638 vs 5,096 MB/s) and Prettify by 1.40× (4,575 vs 3,273). Linux / Clang wins all 51 tests in both runs, and Linux / GCC wins all 51 reused. Freshly allocated, prettified full-size documents lead Glaze by 1.28–10.22× and prettified Small documents by 1.39–6.98×; reused, by 1.02–9.81× and 1.29–7.64×. On Linux / GCC, freshly allocated prettified documents lead the reflection writer by 15.00–78.42× full-size and 13.68–48.88× Small. Minify leads Glaze by 1.42× on Linux / Clang, 1.70× on Linux / GCC and 1.98× on Windows / MSVC freshly allocated, and by 1.46×, 1.61× and 2.02× reused.

### AVX-512 method

These results come from the Json-Performance branch `benchmarking-avx512-main`, with the same commits as above (Jsonifier [d4727ec](https://github.com/nihilai-collective/jsonifier/commit/d4727ec) [5], Glaze [a53ddd59](https://github.com/stephenberry/glaze/commit/a53ddd59) [6], simdjson [449f38e63](https://github.com/simdjson/simdjson/commit/449f38e63) [7], BenchmarkSuite [27950a9](https://github.com/nihilai-collective/benchmarksuite/commit/27950a9) [8]), run November 15, 2026. The tests, sampling, ranking and convergence limits (RSE under 5%, mean shift under 2.5%) are the same as for the x86 builds above.

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | AMD EPYC 9V45 (cloud VM) | Linux 7.0.0-1019-azure | AVX-512 | AVX2 / SSE4.1 | icelake |
| Linux / GCC 16.2 | AMD EPYC 9V45 (cloud VM) | Linux 7.0.0-1019-azure | AVX-512 | AVX2 / SSE4.1 | icelake |
| Windows / MSVC 19.51 | AMD EPYC 9V45 (cloud VM) | Windows 10.0.26100 | AVX-512 | AVX2 / SSE4.1 | icelake |

Glaze uses AVX512BW for UTF-8 validation and structural skipping on these builds, but its string escaper stays on AVX2 and its float writer on SSE4.1. The runners are shared virtual machines, and the CPU, operating-system images and MSVC version (19.51, against 19.52 above) all differ from the AVX2 builds, so an AVX-512 result here cannot be compared with the matching AVX2 result to isolate the instruction set. Each comparison below is between libraries within one build.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 50 | 1 | 0 |
| **Total** | **153** | **150** | **1** | **2** |

### Windows / MSVC 19.51 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,081 | 79 | 13.68× |
| Double | 445 | 130 | 3.42× |
| Int64 | 426 | 335 | 1.27× |
| String | 1,239 | 800 | 1.55× |
| Uint64 | 443 | 333 | 1.33× |
| Apache Builds (minified) | 12,168 | 4,816 | 2.53× |
| Canada (minified) | 1,146 | 788 | 1.45× |
| Apache Builds (prettified) | 12,117 | 4,007 | 3.02× |
| Canada (prettified) | 2,627 | 1,269 | 2.07× |
| CitmCatalog (minified) | 7,599 | 3,129 | 2.43× |
| CitmCatalog (prettified) | 3,671 | 1,934 | 1.90× |
| Discord (minified) | 10,719 | 3,030 | 3.54× |
| Discord (prettified) | 11,360 | 3,023 | 3.76× |
| Github Events (minified) | 11,077 | 4,707 | 2.35× |
| Google Maps Response (minified) | 8,709 | 2,710 | 3.21× |
| Github Events (prettified) | 11,342 | 4,295 | 2.64× |
| Google Maps Response (prettified) | 13,509 | 3,690 | 3.66× |
| Instruments (minified) | 13,718 | 2,963 | 4.63× |
| Instruments (prettified) | 13,462 | 3,270 | 4.12× |
| Marine IK Reverse (minified) | 790 | 566 | 1.40× |
| Marine IK Reverse (prettified) | 2,144 | 1,674 | 1.28× |
| Marine IK (minified) | 811 | 558 | 1.45× |
| Marine IK (prettified) | 2,532 (n/c) | 1,163 | 2.18× |
| Random (minified) | 2,848 | 2,636 | 1.08× |
| Random (prettified) | 3,396 | 2,117 | 1.60× |
| Twitter (minified) | 3,085 | 4,570 | 0.68× |
| Twitter (prettified) | 3,578 (n/c) | 2,659 | 1.35× |
| Apache Builds Small (minified) | 9,534 | 3,359 | 2.84× |
| Canada Small (minified) | 1,732 | 1,060 | 1.63× |
| Apache Builds Small (prettified) | 10,115 | 3,266 | 3.10× |
| Canada Small (prettified) | 4,719 | 2,880 | 1.64× |
| CitmCatalog Small (minified) | 6,922 | 2,628 | 2.63× |
| CitmCatalog Small (prettified) | 6,559 | 3,297 | 1.99× |
| Discord Small (minified) | 8,071 | 2,894 | 2.79× |
| Discord Small (prettified) | 7,416 | 2,819 | 2.63× |
| Github Events Small (minified) | 9,661 | 3,351 | 2.88× |
| Google Maps Response Small (minified) | 8,541 | 2,714 | 3.15× |
| Github Events Small (prettified) | 10,512 | 3,281 | 3.20× |
| Google Maps Response Small (prettified) | 12,066 | 3,537 | 3.41× |
| Instruments Small (minified) | 12,362 | 2,610 | 4.74× |
| Instruments Small (prettified) | 10,502 | 3,274 | 3.21× |
| Marine IK Reverse Small (minified) | 1,387 | 861 | 1.61× |
| Marine IK Reverse Small (prettified) | 5,012 | 3,139 | 1.60× |
| Marine IK Small (minified) | 1,377 | 549 | 2.51× |
| Marine IK Small (prettified) | 4,914 | 3,192 | 1.54× |
| Random Small (minified) | 8,598 | 2,602 | 3.30× |
| Random Small (prettified) | 10,472 | 3,137 | 3.34× |
| Twitter Small (minified) | 5,752 | 3,952 | 1.46× |
| Twitter Small (prettified) | 7,501 | 3,592 | 2.09× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,570 | 1,802 | 1,307 | 1.98× |
| Prettify | 2,463 | 3,240 | 1,492 | 0.76× |

### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,221 | 194 | 11.46× |
| Double | 310 | 267 | 1.16× |
| Int64 | 876 | 769 | 1.14× |
| String | 4,146 | 1,963 | 2.11× |
| Uint64 | 868 | 785 | 1.11× |
| Apache Builds (minified) | 17,144 | 5,277 | 3.25× |
| Canada (minified) | 1,922 | 918 | 2.09× |
| Apache Builds (prettified) | 18,037 | 5,797 | 3.11× |
| Canada (prettified) | 5,269 | 3,076 | 1.71× |
| CitmCatalog (minified) | 10,119 | 3,646 | 2.78× |
| CitmCatalog (prettified) | 16,428 | 4,805 | 3.42× |
| Discord (minified) | 14,850 | 3,671 | 4.05× |
| Discord (prettified) | 18,474 | 3,305 | 5.59× |
| Github Events (minified) | 16,342 | 5,595 | 2.92× |
| Google Maps Response (minified) | 12,293 | 3,076 | 4.00× |
| Github Events (prettified) | 16,716 | 4,768 | 3.51× |
| Google Maps Response (prettified) | 17,431 | 5,123 | 3.40× |
| Instruments (minified) | 20,213 | 3,603 | 5.61× |
| Instruments (prettified) | 23,556 | 3,987 | 5.91× |
| Marine IK Reverse (minified) | 1,151 | 625 | 1.84× |
| Marine IK Reverse (prettified) | 4,961 | 3,281 | 1.51× |
| Marine IK (minified) | 1,181 | 619 | 1.91× |
| Marine IK (prettified) | 5,176 | 3,165 | 1.64× |
| Random (minified) | 12,877 | 2,969 | 4.34× |
| Random (prettified) | 17,013 | 3,442 | 4.94× |
| Twitter (minified) | 17,031 | 5,276 | 3.23× |
| Twitter (prettified) | 15,645 | 4,351 | 3.60× |
| Apache Builds Small (minified) | 11,322 | 3,723 | 3.04× |
| Canada Small (minified) | 1,841 | 927 | 1.99× |
| Apache Builds Small (prettified) | 13,211 | 3,976 | 3.32× |
| Canada Small (prettified) | 5,279 | 3,102 | 1.70× |
| CitmCatalog Small (minified) | 10,869 | 2,542 | 4.28× |
| CitmCatalog Small (prettified) | 11,403 | 3,715 | 3.07× |
| Discord Small (minified) | 12,488 | 3,539 | 3.53× |
| Discord Small (prettified) | 12,848 | 3,230 | 3.98× |
| Github Events Small (minified) | 13,711 | 4,471 | 3.07× |
| Google Maps Response Small (minified) | 11,184 | 2,650 | 4.22× |
| Github Events Small (prettified) | 14,365 | 4,135 | 3.47× |
| Google Maps Response Small (prettified) | 17,141 | 4,883 | 3.51× |
| Instruments Small (minified) | 17,886 | 3,563 | 5.02× |
| Instruments Small (prettified) | 18,727 | 3,936 | 4.76× |
| Marine IK Reverse Small (minified) | 1,495 | 822 | 1.82× |
| Marine IK Reverse Small (prettified) | 5,598 | 3,737 | 1.50× |
| Marine IK Small (minified) | 1,458 | 810 | 1.80× |
| Marine IK Small (prettified) | 5,154 | 3,701 | 1.39× |
| Random Small (minified) | 13,767 | 3,202 | 4.30× |
| Random Small (prettified) | 18,034 | 3,979 | 4.53× |
| Twitter Small (minified) | 11,601 | 5,003 | 2.32× |
| Twitter Small (prettified) | 10,060 | 3,882 | 2.59× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,737 | 2,632 | 1,755 | 1.42× |
| Prettify | 5,286 | 3,652 | 2,397 | 1.45× |

### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,107 | 210 | 269 | 7.85× |
| Double | 361 | 293 | 332 | 1.09× |
| Int64 | 990 | 901 | 904 | 1.09× |
| String | 3,968 | 2,409 | 3,966 | 1.00× |
| Uint64 | 943 | 912 | 925 | 1.02× |
| Apache Builds (minified) | 27,181 | 6,372 | 15,558 | 1.75× |
| Canada (minified) | 1,918 | 1,398 | 915 | 1.37× |
| Apache Builds (prettified) | 36,441 | 6,089 | 644 | 5.98× |
| Canada (prettified) | 5,991 | 3,346 | 399 | 1.79× |
| CitmCatalog (minified) | 11,348 | 3,760 | 9,627 | 1.18× |
| CitmCatalog (prettified) | 28,701 | 5,479 | 610 | 5.24× |
| Discord (minified) | 16,725 | 5,694 | 14,570 | 1.15× |
| Discord (prettified) | 25,046 | 5,232 | 545 | 4.79× |
| Github Events (minified) | 15,055 | 7,093 | 10,907 | 1.38× |
| Google Maps Response (minified) | 16,178 | 3,995 | 12,017 | 1.35× |
| Github Events (prettified) | 19,234 | 5,526 | 700 | 3.48× |
| Google Maps Response (prettified) | 31,962 | 5,759 | 594 | 5.55× |
| Instruments (minified) | 25,578 | 2,648 | 8,235 | 3.11× |
| Instruments (prettified) | 39,104 | 3,827 | 499 | 10.22× |
| Marine IK Reverse (minified) | 1,187 | 908 | 770 | 1.31× |
| Marine IK Reverse (prettified) | 6,197 | 3,813 | 205 | 1.63× |
| Marine IK (minified) | 1,200 | 946 | 761 | 1.27× |
| Marine IK (prettified) | 6,197 | 3,402 | 357 (n/c) | 1.82× |
| Random (minified) | 15,533 | 3,628 | 10,529 | 1.48× |
| Random (prettified) | 24,995 | 3,918 | 589 | 6.38× |
| Twitter (minified) | 14,639 | 5,860 | 10,651 | 1.37× |
| Twitter (prettified) | 19,180 | 4,612 | 601 | 4.16× |
| Apache Builds Small (minified) | 14,424 | 4,472 | 9,735 | 1.48× |
| Canada Small (minified) | 1,958 | 1,510 | 961 | 1.30× |
| Apache Builds Small (prettified) | 19,971 | 4,604 | 594 | 4.34× |
| Canada Small (prettified) | 5,938 | 3,456 | 429 | 1.72× |
| CitmCatalog Small (minified) | 9,307 | 2,591 | 7,201 | 1.29× |
| CitmCatalog Small (prettified) | 15,463 | 4,163 | 574 | 3.71× |
| Discord Small (minified) | 11,856 | 4,088 | 8,725 | 1.36× |
| Discord Small (prettified) | 15,391 | 4,262 | 558 | 3.61× |
| Github Events Small (minified) | 14,399 | 6,082 | 11,788 | 1.22× |
| Google Maps Response Small (minified) | 14,025 | 3,766 | 10,812 | 1.30× |
| Github Events Small (prettified) | 17,614 | 4,645 | 611 | 3.79× |
| Google Maps Response Small (prettified) | 29,238 | 5,573 | 598 | 5.25× |
| Instruments Small (minified) | 14,844 | 2,050 | 7,550 | 1.97× |
| Instruments Small (prettified) | 25,007 | 3,584 | 541 | 6.98× |
| Marine IK Reverse Small (minified) | 1,538 | 1,062 | 1,348 | 1.14× |
| Marine IK Reverse Small (prettified) | 7,724 | 3,821 | 558 | 2.02× |
| Marine IK Small (minified) | 1,550 | 1,071 | 1,333 | 1.16× |
| Marine IK Small (prettified) | 7,680 | 3,797 | 561 | 2.02× |
| Random Small (minified) | 15,992 | 3,890 | 11,800 | 1.36× |
| Random Small (prettified) | 26,976 | 4,389 | 590 | 6.15× |
| Twitter Small (minified) | 13,874 | 5,391 | 8,960 | 1.55× |
| Twitter Small (prettified) | 16,185 | 4,071 | 620 | 3.98× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 4,565 | 2,680 | 1,966 | 1.70× |
| Prettify | 5,304 | 3,404 | 2,739 | 1.56× |

### Reused output strings

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 51 | 50 | 1 | 0 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 51 | 51 | 0 | 0 |
| **Total** | **153** | **152** | **1** | **0** |

#### Windows / MSVC 19.51 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,565 | 253 | 6.18× |
| Double | 710 | 357 | 1.99× |
| Int64 | 3,849 | 1,134 | 3.39× |
| String | 7,960 | 2,713 | 2.93× |
| Uint64 | 2,886 | 1,073 | 2.69× |
| Apache Builds (minified) | 12,646 | 5,516 | 2.29× |
| Canada (minified) | 1,391 | 1,103 | 1.26× |
| Apache Builds (prettified) | 11,988 | 4,257 | 2.82× |
| Canada (prettified) | 3,996 | 3,030 | 1.32× |
| CitmCatalog (minified) | 7,563 | 3,416 | 2.21× |
| CitmCatalog (prettified) | 12,159 | 5,844 | 2.08× |
| Discord (minified) | 11,592 | 3,336 | 3.48× |
| Discord (prettified) | 11,732 | 3,307 | 3.55× |
| Github Events (minified) | 11,666 | 5,327 | 2.19× |
| Google Maps Response (minified) | 9,184 | 3,090 | 2.97× |
| Github Events (prettified) | 11,715 | 4,923 | 2.38× |
| Google Maps Response (prettified) | 14,168 | 4,124 | 3.44× |
| Instruments (minified) | 14,274 | 3,113 | 4.59× |
| Instruments (prettified) | 13,479 | 3,523 | 3.83× |
| Marine IK Reverse (minified) | 883 | 673 | 1.31× |
| Marine IK Reverse (prettified) | 3,218 | 3,168 | 1.02× |
| Marine IK (minified) | 913 | 678 | 1.35× |
| Marine IK (prettified) | 3,975 | 3,149 | 1.26× |
| Random (minified) | 10,006 | 2,823 | 3.54× |
| Random (prettified) | 11,711 | 3,518 | 3.33× |
| Twitter (minified) | 12,638 | 5,096 | 2.48× |
| Twitter (prettified) | 10,970 | 5,150 | 2.13× |
| Apache Builds Small (minified) | 10,527 | 3,945 | 2.67× |
| Canada Small (minified) | 1,765 | 1,149 | 1.54× |
| Apache Builds Small (prettified) | 11,746 | 3,756 | 3.13× |
| Canada Small (prettified) | 4,963 | 3,259 | 1.52× |
| CitmCatalog Small (minified) | 8,234 | 3,056 | 2.69× |
| CitmCatalog Small (prettified) | 8,494 | 4,142 | 2.05× |
| Discord Small (minified) | 9,429 | 3,450 | 2.73× |
| Discord Small (prettified) | 8,138 | 3,240 | 2.51× |
| Github Events Small (minified) | 11,583 | 4,130 | 2.80× |
| Google Maps Response Small (minified) | 9,120 | 3,120 | 2.92× |
| Github Events Small (prettified) | 13,069 | 4,026 | 3.25× |
| Google Maps Response Small (prettified) | 14,086 | 4,146 | 3.40× |
| Instruments Small (minified) | 13,827 | 3,199 | 4.32× |
| Instruments Small (prettified) | 11,165 | 3,753 | 2.98× |
| Marine IK Reverse Small (minified) | 1,428 | 897 | 1.59× |
| Marine IK Reverse Small (prettified) | 5,300 | 3,692 | 1.44× |
| Marine IK Small (minified) | 1,467 | 887 | 1.65× |
| Marine IK Small (prettified) | 5,164 | 3,596 | 1.44× |
| Random Small (minified) | 10,716 | 3,172 | 3.38× |
| Random Small (prettified) | 12,164 | 3,776 | 3.22× |
| Twitter Small (minified) | 9,661 | 4,764 | 2.03× |
| Twitter Small (prettified) | 7,873 | 4,591 | 1.72× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,688 | 1,822 | 1,300 | 2.02× |
| Prettify | 4,575 | 3,273 | 1,464 | 1.40× |

#### Linux / Clang 24.0 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 3,915 | 948 | 4.13× |
| Double | 822 | 541 | 1.52× |
| Int64 | 4,450 | 2,366 | 1.88× |
| String | 11,459 | 5,171 | 2.22× |
| Uint64 | 4,836 | 2,544 | 1.90× |
| Apache Builds (minified) | 17,359 | 5,814 | 2.99× |
| Canada (minified) | 1,967 | 952 | 2.07× |
| Apache Builds (prettified) | 18,224 | 6,229 | 2.93× |
| Canada (prettified) | 5,349 | 3,277 | 1.63× |
| CitmCatalog (minified) | 10,116 | 4,060 | 2.49× |
| CitmCatalog (prettified) | 16,305 | 5,831 | 2.80× |
| Discord (minified) | 15,150 | 3,946 | 3.84× |
| Discord (prettified) | 18,670 | 3,533 | 5.28× |
| Github Events (minified) | 16,742 | 6,238 | 2.68× |
| Google Maps Response (minified) | 12,546 | 3,317 | 3.78× |
| Github Events (prettified) | 16,830 | 5,367 | 3.14× |
| Google Maps Response (prettified) | 18,136 | 5,348 | 3.39× |
| Instruments (minified) | 20,270 | 3,798 | 5.34× |
| Instruments (prettified) | 23,643 | 4,245 | 5.57× |
| Marine IK Reverse (minified) | 1,156 | 621 | 1.86× |
| Marine IK Reverse (prettified) | 5,034 | 3,652 | 1.38× |
| Marine IK (minified) | 1,188 | 631 | 1.88× |
| Marine IK (prettified) | 5,153 | 3,868 | 1.33× |
| Random (minified) | 12,864 | 3,168 | 4.06× |
| Random (prettified) | 17,039 | 3,771 | 4.52× |
| Twitter (minified) | 17,171 | 6,106 | 2.81× |
| Twitter (prettified) | 15,477 | 5,087 | 3.04× |
| Apache Builds Small (minified) | 12,662 | 3,978 | 3.18× |
| Canada Small (minified) | 1,926 | 967 | 1.99× |
| Apache Builds Small (prettified) | 14,517 | 4,357 | 3.33× |
| Canada Small (prettified) | 5,408 | 3,369 | 1.61× |
| CitmCatalog Small (minified) | 11,674 | 2,713 | 4.30× |
| CitmCatalog Small (prettified) | 13,523 | 4,099 | 3.30× |
| Discord Small (minified) | 14,068 | 4,042 | 3.48× |
| Discord Small (prettified) | 15,050 | 3,537 | 4.26× |
| Github Events Small (minified) | 16,120 | 4,912 | 3.28× |
| Google Maps Response Small (minified) | 12,184 | 2,838 | 4.29× |
| Github Events Small (prettified) | 16,683 | 4,640 | 3.60× |
| Google Maps Response Small (prettified) | 18,169 | 5,446 | 3.34× |
| Instruments Small (minified) | 20,143 | 3,862 | 5.22× |
| Instruments Small (prettified) | 22,135 | 4,261 | 5.19× |
| Marine IK Reverse Small (minified) | 1,506 | 812 | 1.85× |
| Marine IK Reverse Small (prettified) | 5,677 | 4,060 | 1.40× |
| Marine IK Small (minified) | 1,522 | 835 | 1.82× |
| Marine IK Small (prettified) | 5,252 | 4,085 | 1.29× |
| Random Small (minified) | 15,361 | 3,534 | 4.35× |
| Random Small (prettified) | 19,214 | 4,226 | 4.55× |
| Twitter Small (minified) | 15,963 | 5,455 | 2.93× |
| Twitter Small (prettified) | 11,715 | 4,472 | 2.62× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,740 | 2,568 | 1,780 | 1.46× |
| Prettify | 5,506 | 3,764 | 2,447 | 1.46× |

#### Linux / GCC 16.2 (EPYC 9V45, AVX-512)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,315 | 1,130 | 2,538 | 1.31× |
| Double | 852 | 597 | 593 | 1.43× |
| Int64 | 4,909 | 2,689 | 3,324 | 1.48× |
| String | 9,453 | 6,832 | 9,196 | 1.03× |
| Uint64 | 4,886 | 3,177 | 3,266 | 1.50× |
| Apache Builds (minified) | 27,239 | 7,098 | 15,388 | 1.77× |
| Canada (minified) | 1,919 | 1,428 | 908 | 1.34× |
| Apache Builds (prettified) | 36,747 | 6,612 | 647 | 5.56× |
| Canada (prettified) | 5,965 | 3,590 | 387 | 1.66× |
| CitmCatalog (minified) | 11,364 | 4,206 | 9,645 | 1.18× |
| CitmCatalog (prettified) | 28,706 | 6,644 | 609 | 4.32× |
| Discord (minified) | 17,209 | 6,270 | 14,732 | 1.17× |
| Discord (prettified) | 25,059 | 5,829 | 561 | 4.30× |
| Github Events (minified) | 15,190 | 8,103 | 11,010 | 1.38× |
| Google Maps Response (minified) | 16,908 | 4,360 | 12,862 | 1.31× |
| Github Events (prettified) | 19,627 | 6,232 | 701 | 3.15× |
| Google Maps Response (prettified) | 34,163 | 6,345 | 596 | 5.38× |
| Instruments (minified) | 26,085 | 2,772 | 10,842 | 2.41× |
| Instruments (prettified) | 40,285 | 4,106 | 510 | 9.81× |
| Marine IK Reverse (minified) | 1,175 | 958 | 761 | 1.23× |
| Marine IK Reverse (prettified) | 6,194 | 3,917 | 265 | 1.58× |
| Marine IK (minified) | 1,199 | 971 | 769 | 1.23× |
| Marine IK (prettified) | 6,206 | 3,887 | 313 | 1.60× |
| Random (minified) | 15,514 | 4,041 | 10,580 | 1.47× |
| Random (prettified) | 24,978 | 4,433 | 588 | 5.63× |
| Twitter (minified) | 14,689 | 6,658 | 10,705 | 1.37× |
| Twitter (prettified) | 19,300 | 5,335 | 602 | 3.62× |
| Apache Builds Small (minified) | 16,425 | 5,112 | 10,626 | 1.55× |
| Canada Small (minified) | 1,993 | 1,597 | 967 | 1.25× |
| Apache Builds Small (prettified) | 21,630 | 4,925 | 592 | 4.39× |
| Canada Small (prettified) | 6,195 | 2,076 (n/c) | 427 | 2.98× |
| CitmCatalog Small (minified) | 9,840 | 2,851 | 8,387 | 1.17× |
| CitmCatalog Small (prettified) | 17,128 | 4,592 | 578 | 3.73× |
| Discord Small (minified) | 12,858 | 4,850 | 9,857 | 1.30× |
| Discord Small (prettified) | 18,483 | 4,767 | 559 | 3.88× |
| Github Events Small (minified) | 16,105 | 6,953 | 13,210 | 1.22× |
| Google Maps Response Small (minified) | 15,078 | 4,038 | 11,876 | 1.27× |
| Github Events Small (prettified) | 22,390 | 5,770 | 612 | 3.88× |
| Google Maps Response Small (prettified) | 32,552 | 6,362 | 592 | 5.12× |
| Instruments Small (minified) | 17,521 | 2,218 | 8,432 | 2.08× |
| Instruments Small (prettified) | 29,482 | 3,858 | 537 | 7.64× |
| Marine IK Reverse Small (minified) | 1,582 | 1,130 | 1,347 | 1.17× |
| Marine IK Reverse Small (prettified) | 7,821 | 4,232 | 557 | 1.85× |
| Marine IK Small (minified) | 1,591 | 1,123 | 1,370 | 1.16× |
| Marine IK Small (prettified) | 7,820 | 4,131 | 560 | 1.89× |
| Random Small (minified) | 17,894 | 4,298 | 12,992 | 1.38× |
| Random Small (prettified) | 29,276 | 5,019 | 581 | 5.83× |
| Twitter Small (minified) | 15,043 | 5,707 | 10,939 | 1.38× |
| Twitter Small (prettified) | 19,287 | 4,765 | 621 | 4.05× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 4,322 | 2,690 | 1,969 | 1.61× |
| Prettify | 5,360 | 3,393 | 2,557 | 1.58× |

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,371 MB/s on macOS / GCC (1.49× Glaze's 920, which did not converge, and ahead of the reflection writer's 1,125) and 1,294 MB/s on macOS / Clang (1.14× Glaze's 1,139). The AVX2 builds run the same test at 1,402–4,323 MB/s and lead Glaze by 1.34× (Windows / MSVC), 2.13× (Linux / Clang) and 1.96× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.84× on macOS / Clang (7,889 vs 4,297 MB/s) and 2.08× on macOS / GCC (9,346 vs 4,487), against 1.90–2.60× on the AVX2 builds. On macOS / GCC the reflection writer runs String reused at 8,571 MB/s, behind Jsonifier.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: freshly allocated the M2 Pro leads over Glaze are 1.14–1.49× against 1.34–2.13× on AVX2, and reused they rise to 1.84–2.08×, close to the AVX2 range of 1.90–2.60×. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

By the ranking, on both GCC builds freshly allocated; per call, on neither. The reflection writer leads in MB/s, 353 vs 337 on Linux / GCC (0.95×) and 259 vs 251 on macOS / GCC (0.97×), and both count as losses, but it writes 1,997 bytes against Jsonifier's 1,811, and Jsonifier takes less time per call on both (5,128 vs 5,396 ns and 6,879 vs 7,354 ns). Reused, Jsonifier leads it on both: 878 vs 650 MB/s on Linux / GCC (1.35×) and 791 vs 669 MB/s on macOS / GCC (1.18×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
