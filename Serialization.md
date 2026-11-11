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
| Compile-time 2 / 4 / 8 bytes of constant data | 2 or 4 bytes: one immediate store, no load (`mov dword ptr [rdi], imm32`); 8 bytes: one `movabs` of the constant into a register, then one `mov` store (a single `mov qword ptr [rdi], imm32` only when the value fits a sign-extended 32-bit immediate) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers across every member write of the object.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Runtime lengths stay short.** Copies whose length is only known at run time, such as the runs that Minify and Prettify carry over, go through `jsonifierMemcpy` (`utility.hpp`). It sorts the length into a power-of-2 bucket `b`, where `b ≤ length < 2b`, and copies the first `b` bytes and the last `b` bytes. The two copies overlap, so any length from 2 to 255 bytes takes one fixed-size copy pair (a single byte takes one store), with no loop and no tail. Buckets wider than the vector register are split into register-width moves: the 128-byte bucket is 8 moves on AVX2, 4 on AVX-512 and 16 on SSE or NEON. Only lengths of 256 bytes or more reach `std::memcpy`. `jsonifierMemcpyUpTo<maxBytes>` takes a compile-time upper bound and leaves out every bucket above it, so a caller whose length is bounded never pays for the cases it cannot hit.
- **The bucket is chosen by a compare tree, not a jump table.** `dispatchBuckets` is a balanced binary tree of `length < 2^k` tests generated at compile time, so the bucket is found in at most four conditional branches on the length itself, with no `lzcnt` and no indirect jump. A switch on `std::bit_width(length)` compiled to a jump table on GCC and lost to glibc's `memcpy`; the tree compiles to the same branch-only shape on GCC and Clang, and in the [memcpy-decomp](https://github.com/nihilai-collective/memcpy-decomp) harness it beats `std::memcpy` on every range tested, from 1–15 to 1–129 bytes, on all five platforms.
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

On the Bool test this writer leads Glaze by 7.86–14.51× freshly allocated on all five builds.

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

With the string reused, Jsonifier leads the fastest other library on Int64 and Uint64 on every build, by 1.29× to 4.14×.

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

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md).

## Performance results

Jsonifier was fastest in 246 of the 255 write tests on five builds, tied 3 and lost 6. With the output string reused across iterations it was fastest in 251 of 255, tied 1 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other four are against simdjson's reflection writer, which exists only on GCC: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC, and Double and Uint64 on macOS / GCC. The three freshly allocated ties are all against the reflection writer on macOS / GCC: Int64, minified Github Events Small and minified Twitter Small. Reused, all three losses are against the reflection writer: minified Marine IK Small and Marine IK Reverse Small on Linux / GCC and String on macOS / GCC. The one reused tie is minified Github Events Small on macOS / GCC, against the reflection writer. Linux / Clang and macOS / Clang win all 51 tests in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.31–8.55× across the five builds and the reflection writer by 15–56× on the two GCC builds; Small documents lead them by 1.34–7.34× and 12.40–63.24×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 11, 2026, with Jsonifier [b2f5e1d](https://github.com/nihilai-collective/jsonifier/commit/b2f5e1d), Glaze [82831644](https://github.com/stephenberry/glaze/commit/82831644), simdjson [f0c2d2ac2](https://github.com/simdjson/simdjson/commit/f0c2d2ac2) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md). `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. A tie is therefore usually at the top, and every tie in this sweep is. Because the verdict is a significance test and not a ratio threshold, a narrow lead with tight samples counts as a win while a wider one with noisy samples can tie. On Linux / GCC, Double (348 vs 345 MB/s, RSEs 0.05% and 0.11%) and Int64 (941 vs 923, 0.07% and 0.06%) freshly allocated, and Bool reused (3,214 vs 3,172, 0.13% and 0.18%), are wins of 1.01–1.02× against the reflection writer, while minified Github Events Small on macOS / GCC ties at 1.05× in both runs, with RSEs of 3.0% and 4.1% freshly allocated and 6.0% and 4.1% reused.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 46 | 3 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **246** | **3** | **6** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.51 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,283 | 93 | 13.80× |
| Double | 478 | 148 | 3.23× |
| Int64 | 516 | 391 | 1.32× |
| String | 1,470 | 935 | 1.57× |
| Uint64 | 517 | 398 | 1.30× |
| Apache Builds (minified) | 12,584 | 4,393 | 2.86× |
| Canada (minified) | 1,255 | 922 | 1.36× |
| Apache Builds (prettified) | 13,162 | 4,974 | 2.65× |
| Canada (prettified) | 2,990 | 1,441 | 2.08× |
| CitmCatalog (minified) | 8,844 | 4,420 | 2.00× |
| CitmCatalog (prettified) | 4,596 | 1,985 | 2.32× |
| Discord (minified) | 11,136 | 4,156 | 2.68× |
| Discord (prettified) | 12,179 | 4,015 | 3.03× |
| Github Events (minified) | 12,028 | 5,157 | 2.33× |
| Google Maps Response (minified) | 9,100 | 3,536 | 2.57× |
| Github Events (prettified) | 11,754 | 3,158 | 3.72× |
| Google Maps Response (prettified) | 15,510 | 4,936 | 3.14× |
| Instruments (minified) | 13,728 | 4,110 | 3.34× |
| Instruments (prettified) | 15,701 | 3,900 | 4.03× |
| Marine IK Reverse (minified) | 889 | 713 | 1.25× |
| Marine IK Reverse (prettified) | 2,487 | 1,902 | 1.31× |
| Marine IK (minified) | 899 | 637 | 1.41× |
| Marine IK (prettified) | 2,780 | 1,320 | 2.11× |
| Random (minified) | 3,759 | 3,194 | 1.18× |
| Random (prettified) | 4,168 | 2,251 | 1.85× |
| Twitter (minified) | 4,151 | 4,944 | 0.84× |
| Twitter (prettified) | 4,379 | 1,798 | 2.44× |
| Apache Builds Small (minified) | 9,416 | 3,637 | 2.59× |
| Canada Small (minified) | 1,891 | 1,252 | 1.51× |
| Apache Builds Small (prettified) | 10,786 | 4,045 | 2.67× |
| Canada Small (prettified) | 5,084 | 3,118 | 1.63× |
| CitmCatalog Small (minified) | 7,063 | 3,133 | 2.25× |
| CitmCatalog Small (prettified) | 9,406 | 4,085 | 2.30× |
| Discord Small (minified) | 9,571 | 3,404 | 2.81× |
| Discord Small (prettified) | 8,800 | 3,208 | 2.74× |
| Github Events Small (minified) | 9,916 | 4,141 | 2.39× |
| Google Maps Response Small (minified) | 8,920 | 3,606 | 2.47× |
| Github Events Small (prettified) | 10,653 | 4,012 | 2.66× |
| Google Maps Response Small (prettified) | 14,589 | 4,670 | 3.12× |
| Instruments Small (minified) | 10,576 | 3,432 (n/c) | 3.08× |
| Instruments Small (prettified) | 11,703 | 3,004 | 3.90× |
| Marine IK Reverse Small (minified) | 1,497 | 962 | 1.55× |
| Marine IK Reverse Small (prettified) | 5,072 | 2,842 | 1.78× |
| Marine IK Small (minified) | 1,499 | 975 | 1.54× |
| Marine IK Small (prettified) | 5,145 | 2,906 | 1.77× |
| Random Small (minified) | 9,192 | 3,501 | 2.63× |
| Random Small (prettified) | 11,493 | 4,024 | 2.86× |
| Twitter Small (minified) | 10,475 | 4,574 | 2.29× |
| Twitter Small (prettified) | 8,940 | 3,498 (n/c) | 2.56× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,074 | 1,870 | 1,379 | 1.64× |
| Prettify | 2,404 | 2,779 | 1,407 | 0.86× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,350 | 216 | 10.86× |
| Double | 332 | 309 | 1.08× |
| Int64 | 903 | 860 | 1.05× |
| String | 4,443 | 1,843 | 2.41× |
| Uint64 | 933 | 895 | 1.04× |
| Apache Builds (minified) | 16,479 | 5,338 | 3.09× |
| Canada (minified) | 1,903 | 961 | 1.98× |
| Apache Builds (prettified) | 18,813 | 5,250 | 3.58× |
| Canada (prettified) | 5,185 | 3,312 | 1.57× |
| CitmCatalog (minified) | 9,999 | 5,003 | 2.00× |
| CitmCatalog (prettified) | 14,936 | 5,289 | 2.82× |
| Discord (minified) | 14,544 | 4,871 | 2.99× |
| Discord (prettified) | 16,133 | 4,115 | 3.92× |
| Github Events (minified) | 16,516 | 5,001 | 3.30× |
| Google Maps Response (minified) | 11,761 | 3,864 | 3.04× |
| Github Events (prettified) | 16,434 | 4,646 | 3.54× |
| Google Maps Response (prettified) | 15,250 | 5,146 | 2.96× |
| Instruments (minified) | 18,464 | 4,468 | 4.13× |
| Instruments (prettified) | 20,525 | 4,923 | 4.17× |
| Marine IK Reverse (minified) | 1,186 | 637 | 1.86× |
| Marine IK Reverse (prettified) | 5,074 | 3,624 | 1.40× |
| Marine IK (minified) | 1,228 | 631 | 1.94× |
| Marine IK (prettified) | 4,981 | 2,899 | 1.72× |
| Random (minified) | 14,689 | 3,462 | 4.24× |
| Random (prettified) | 16,026 | 3,864 | 4.15× |
| Twitter (minified) | 19,404 | 6,030 | 3.22× |
| Twitter (prettified) | 18,664 | 4,580 | 4.08× |
| Apache Builds Small (minified) | 9,685 | 3,378 | 2.87× |
| Canada Small (minified) | 1,802 | 957 | 1.88× |
| Apache Builds Small (prettified) | 11,561 | 3,564 | 3.24× |
| Canada Small (prettified) | 4,528 | 3,389 | 1.34× |
| CitmCatalog Small (minified) | 7,455 | 2,872 | 2.60× |
| CitmCatalog Small (prettified) | 10,399 | 3,477 | 2.99× |
| Discord Small (minified) | 9,975 | 3,006 | 3.32× |
| Discord Small (prettified) | 10,738 | 2,289 | 4.69× |
| Github Events Small (minified) | 10,074 | 4,070 | 2.48× |
| Google Maps Response Small (minified) | 10,762 | 3,698 | 2.91× |
| Github Events Small (prettified) | 10,679 | 3,331 | 3.21× |
| Google Maps Response Small (prettified) | 15,572 | 4,678 | 3.33× |
| Instruments Small (minified) | 12,300 | 2,382 | 5.16× |
| Instruments Small (prettified) | 14,724 | 2,662 | 5.53× |
| Marine IK Reverse Small (minified) | 1,488 | 822 | 1.81× |
| Marine IK Reverse Small (prettified) | 5,117 | 3,354 | 1.53× |
| Marine IK Small (minified) | 1,603 | 814 | 1.97× |
| Marine IK Small (prettified) | 5,259 | 3,144 | 1.67× |
| Random Small (minified) | 12,871 | 3,351 | 3.84× |
| Random Small (prettified) | 15,338 | 3,873 | 3.96× |
| Twitter Small (minified) | 8,826 | 4,065 | 2.17× |
| Twitter Small (prettified) | 9,356 | 3,534 | 2.65× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,631 | 2,381 | 1,742 | 1.52× |
| Prettify | 4,542 | 3,659 | 2,370 | 1.24× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,971 | 198 | 254 | 7.75× |
| Double | 348 | 306 | 345 | 1.01× |
| Int64 | 941 | 873 | 923 | 1.02× |
| String | 4,297 | 2,069 | 3,864 | 1.11× |
| Uint64 | 945 | 849 | 886 | 1.07× |
| Apache Builds (minified) | 20,171 | 7,970 | 13,566 | 1.49× |
| Canada (minified) | 2,123 | 1,439 | 1,047 | 1.48× |
| Apache Builds (prettified) | 22,882 | 7,269 | 643 | 3.15× |
| Canada (prettified) | 6,086 | 3,225 | 396 | 1.89× |
| CitmCatalog (minified) | 12,703 | 6,076 | 11,092 | 1.15× |
| CitmCatalog (prettified) | 20,109 | 5,710 | 570 | 3.52× |
| Discord (minified) | 14,939 | 6,204 | 13,117 | 1.14× |
| Discord (prettified) | 19,142 | 5,929 | 556 | 3.23× |
| Github Events (minified) | 14,544 | 7,428 | 11,025 | 1.32× |
| Google Maps Response (minified) | 15,823 | 4,527 | 9,600 | 1.65× |
| Github Events (prettified) | 18,772 | 4,752 | 621 | 3.95× |
| Google Maps Response (prettified) | 21,342 | 6,531 | 524 | 3.27× |
| Instruments (minified) | 20,116 | 5,780 | 15,517 | 1.30× |
| Instruments (prettified) | 26,012 | 5,420 | 475 | 4.80× |
| Marine IK Reverse (minified) | 1,288 | 986 | 891 | 1.31× |
| Marine IK Reverse (prettified) | 6,301 | 3,656 | 272 | 1.72× |
| Marine IK (minified) | 1,190 (n/c) | 922 | 885 | 1.29× |
| Marine IK (prettified) | 6,393 | 3,153 | 399 | 2.03× |
| Random (minified) | 17,694 | 4,605 | 11,468 | 1.54× |
| Random (prettified) | 21,454 | 4,923 | 533 | 4.36× |
| Twitter (minified) | 17,569 | 7,799 | 11,048 | 1.59× |
| Twitter (prettified) | 20,693 | 4,903 | 529 | 4.22× |
| Apache Builds Small (minified) | 12,165 | 4,621 | 7,269 | 1.67× |
| Canada Small (minified) | 2,004 | 1,574 | 1,063 | 1.27× |
| Apache Builds Small (prettified) | 14,034 | 4,134 | 548 | 3.39× |
| Canada Small (prettified) | 5,919 | 3,498 | 417 | 1.69× |
| CitmCatalog Small (minified) | 7,515 | 3,346 | 6,080 | 1.24× |
| CitmCatalog Small (prettified) | 14,896 | 3,232 | 499 | 4.61× |
| Discord Small (minified) | 10,049 | 4,073 | 7,686 | 1.31× |
| Discord Small (prettified) | 14,219 | 3,217 | 516 | 4.42× |
| Github Events Small (minified) | 8,027 | 5,035 | 6,410 | 1.25× |
| Google Maps Response Small (minified) | 13,247 | 3,902 | 8,321 | 1.59× |
| Github Events Small (prettified) | 10,935 | 3,632 | 548 | 3.01× |
| Google Maps Response Small (prettified) | 24,503 | 5,790 | 514 | 4.23× |
| Instruments Small (minified) | 12,582 | 4,037 | 8,047 | 1.56× |
| Instruments Small (prettified) | 21,545 | 3,110 | 527 | 6.93× |
| Marine IK Reverse Small (minified) | 1,383 | 1,268 | 1,554 | 0.89× |
| Marine IK Reverse Small (prettified) | 6,732 | 3,300 | 543 | 2.04× |
| Marine IK Small (minified) | 1,396 | 1,206 | 1,555 | 0.90× |
| Marine IK Small (prettified) | 6,598 | 3,537 | 531 | 1.87× |
| Random Small (minified) | 13,264 | 4,185 | 8,113 | 1.63× |
| Random Small (prettified) | 24,592 | 4,123 | 527 | 5.96× |
| Twitter Small (minified) | 9,045 | 5,794 | 6,851 | 1.32× |
| Twitter Small (prettified) | 12,405 | 3,538 | 560 | 3.51× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,404 | 2,552 | 1,914 | 1.33× |
| Prettify | 4,067 | 3,232 | 2,517 | 1.26× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,928 | 133 | 231 | 8.34× |
| Double | 293 | 218 | 310 | 0.94× |
| Int64 | 821 | 567 | 826 | 0.99× |
| String | 1,486 | 1,149 | 1,337 | 1.11× |
| Uint64 | 733 | 489 | 758 | 0.97× |
| Apache Builds (minified) | 10,276 | 4,139 | 8,448 | 1.22× |
| Canada (minified) | 2,090 | 1,545 | 743 | 1.35× |
| Apache Builds (prettified) | 11,824 | 4,077 | 336 | 2.90× |
| Canada (prettified) | 6,570 | 3,258 | 264 | 2.02× |
| CitmCatalog (minified) | 7,448 | 1,920 | 6,542 | 1.14× |
| CitmCatalog (prettified) | 16,663 | 3,208 | 320 (n/c) | 5.19× |
| Discord (minified) | 11,931 | 2,533 | 9,159 | 1.30× |
| Discord (prettified) | 13,733 | 1,971 | 330 | 6.97× |
| Github Events (minified) | 13,578 | 3,904 | 8,944 | 1.52× |
| Google Maps Response (minified) | 10,141 | 1,766 | 8,114 | 1.25× |
| Github Events (prettified) | 15,799 | 2,685 | 384 | 5.88× |
| Google Maps Response (prettified) | 18,058 | 2,966 | 333 | 6.09× |
| Instruments (minified) | 13,886 | 1,631 | 8,489 | 1.64× |
| Instruments (prettified) | 16,778 | 1,962 | 298 | 8.55× |
| Marine IK Reverse (minified) | 1,259 | 950 | 744 | 1.33× |
| Marine IK Reverse (prettified) | 6,266 | 3,058 | 295 (n/c) | 2.05× |
| Marine IK (minified) | 1,408 | 924 | 823 | 1.52× |
| Marine IK (prettified) | 7,008 | 2,993 | 318 | 2.34× |
| Random (minified) | 11,432 | 2,174 | 7,740 | 1.48× |
| Random (prettified) | 12,903 | 2,344 | 330 | 5.50× |
| Twitter (minified) | 15,339 | 3,869 | 11,024 | 1.39× |
| Twitter (prettified) | 15,833 | 3,829 | 341 | 4.14× |
| Apache Builds Small (minified) | 9,042 | 2,821 | 7,095 | 1.27× |
| Canada Small (minified) | 2,256 | 1,638 | 899 | 1.38× |
| Apache Builds Small (prettified) | 9,618 | 3,766 | 354 | 2.55× |
| Canada Small (prettified) | 6,995 | 2,844 | 270 | 2.46× |
| CitmCatalog Small (minified) | 8,186 | 1,778 | 7,192 | 1.14× |
| CitmCatalog Small (prettified) | 16,983 | 2,559 | 311 | 6.64× |
| Discord Small (minified) | 10,698 | 2,511 | 9,264 | 1.15× |
| Discord Small (prettified) | 13,486 | 2,230 | 320 | 6.05× |
| Github Events Small (minified) | 10,362 (n/c) | 2,978 | 9,873 | 1.05× |
| Google Maps Response Small (minified) | 9,353 | 1,836 | 8,133 | 1.15× |
| Github Events Small (prettified) | 13,808 | 2,814 | 355 | 4.91× |
| Google Maps Response Small (prettified) | 20,046 | 2,741 | 317 | 7.31× |
| Instruments Small (minified) | 14,599 | 1,978 | 6,431 | 2.27× |
| Instruments Small (prettified) | 17,256 | 2,386 | 344 | 7.23× |
| Marine IK Reverse Small (minified) | 1,791 | 986 | 1,680 | 1.07× |
| Marine IK Reverse Small (prettified) | 7,915 | 3,169 | 284 | 2.50× |
| Marine IK Small (minified) | 1,836 | 879 | 1,576 | 1.17× |
| Marine IK Small (prettified) | 8,798 | 3,168 | 294 | 2.78× |
| Random Small (minified) | 11,463 | 1,991 | 8,981 | 1.28× |
| Random Small (prettified) | 14,618 | 1,991 | 303 | 7.34× |
| Twitter Small (minified) | 9,964 | 3,618 | 9,388 | 1.06× |
| Twitter Small (prettified) | 14,688 | 2,487 | 406 | 5.90× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,435 | 1,709 | 1,082 | 1.42× |
| Prettify | 2,737 | 2,245 | 1,311 | 1.22× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,028 | 131 | 7.86× |
| Double | 290 | 224 | 1.29× |
| Int64 | 2,234 | 547 | 4.08× |
| String | 1,002 | 841 | 1.19× |
| Uint64 | 2,283 | 409 | 5.58× |
| Apache Builds (minified) | 8,849 | 3,248 | 2.72× |
| Canada (minified) | 3,001 | 1,465 | 2.05× |
| Apache Builds (prettified) | 10,371 | 2,831 | 3.66× |
| Canada (prettified) | 6,800 | 3,402 | 2.00× |
| CitmCatalog (minified) | 8,252 | 2,058 | 4.01× |
| CitmCatalog (prettified) | 12,290 | 4,240 | 2.90× |
| Discord (minified) | 10,801 | 2,930 | 3.69× |
| Discord (prettified) | 13,425 | 2,352 | 5.71× |
| Github Events (minified) | 14,433 | 4,654 | 3.10× |
| Google Maps Response (minified) | 9,858 | 2,250 | 4.38× |
| Github Events (prettified) | 12,892 | 3,457 | 3.73× |
| Google Maps Response (prettified) | 11,803 | 3,232 | 3.65× |
| Instruments (minified) | 11,333 | 1,958 | 5.79× |
| Instruments (prettified) | 13,372 | 2,396 | 5.58× |
| Marine IK Reverse (minified) | 1,296 | 666 | 1.95× |
| Marine IK Reverse (prettified) | 4,842 | 2,605 | 1.86× |
| Marine IK (minified) | 1,240 | 805 | 1.54× |
| Marine IK (prettified) | 4,120 | 2,332 | 1.77× |
| Random (minified) | 9,017 | 2,275 | 3.96× |
| Random (prettified) | 10,572 | 2,888 | 3.66× |
| Twitter (minified) | 15,556 | 4,526 | 3.44× |
| Twitter (prettified) | 12,397 | 4,141 | 2.99× |
| Apache Builds Small (minified) | 7,491 | 2,855 | 2.62× |
| Canada Small (minified) | 2,579 | 1,414 | 1.82× |
| Apache Builds Small (prettified) | 9,094 | 3,110 | 2.92× |
| Canada Small (prettified) | 6,599 | 3,252 | 2.03× |
| CitmCatalog Small (minified) | 6,931 | 1,887 | 3.67× |
| CitmCatalog Small (prettified) | 11,191 | 2,607 | 4.29× |
| Discord Small (minified) | 11,361 | 2,974 | 3.82× |
| Discord Small (prettified) | 12,460 | 2,903 | 4.29× |
| Github Events Small (minified) | 9,459 | 3,435 | 2.75× |
| Google Maps Response Small (minified) | 8,783 | 2,218 | 3.96× |
| Github Events Small (prettified) | 10,018 | 2,644 | 3.79× |
| Google Maps Response Small (prettified) | 13,573 | 3,454 | 3.93× |
| Instruments Small (minified) | 12,647 | 2,008 | 6.30× |
| Instruments Small (prettified) | 14,225 | 3,086 | 4.61× |
| Marine IK Reverse Small (minified) | 1,948 | 1,033 | 1.89× |
| Marine IK Reverse Small (prettified) | 5,511 | 3,164 | 1.74× |
| Marine IK Small (minified) | 1,938 | 1,018 | 1.90× |
| Marine IK Small (prettified) | 5,496 | 3,183 | 1.73× |
| Random Small (minified) | 9,569 | 2,632 | 3.64× |
| Random Small (prettified) | 10,512 | 3,171 | 3.32× |
| Twitter Small (minified) | 9,974 | 3,673 | 2.72× |
| Twitter Small (prettified) | 11,813 | 3,351 | 3.53× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,004 | 1,467 | 976 | 1.37× |
| Prettify | 3,005 | 2,316 | 1,626 | 1.30× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 251 of the 255 write tests, ties 1 and loses 3. On Linux / GCC the reflection writer wins minified Marine IK Small (1,545 vs 1,417 MB/s, 0.92×) and Marine IK Reverse Small (1,558 vs 1,441 MB/s, 0.92×). On macOS / GCC it wins String (11,175 vs 10,245 MB/s, 0.92×) and ties minified Github Events Small, where Jsonifier leads 10,658 vs 10,147 MB/s with RSEs of 6.0% and 4.1%.

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
| Bool | 1,641 | 296 | 5.55× |
| Double | 770 | 388 | 1.98× |
| Int64 | 4,179 | 1,064 | 3.93× |
| String | 7,805 | 2,427 | 3.22× |
| Uint64 | 4,450 | 1,075 | 4.14× |
| Apache Builds (minified) | 12,179 | 5,061 | 2.41× |
| Canada (minified) | 1,589 | 1,295 | 1.23× |
| Apache Builds (prettified) | 12,993 | 5,579 | 2.33× |
| Canada (prettified) | 4,748 | 3,333 | 1.42× |
| CitmCatalog (minified) | 8,876 | 4,913 | 1.81× |
| CitmCatalog (prettified) | 13,890 | 7,495 | 1.85× |
| Discord (minified) | 11,624 | 4,758 | 2.44× |
| Discord (prettified) | 13,154 | 4,512 | 2.92× |
| Github Events (minified) | 12,408 | 6,519 | 1.90× |
| Google Maps Response (minified) | 10,602 | 4,004 | 2.65× |
| Github Events (prettified) | 11,984 | 3,421 | 3.50× |
| Google Maps Response (prettified) | 16,964 | 5,762 | 2.94× |
| Instruments (minified) | 14,074 | 4,706 | 2.99× |
| Instruments (prettified) | 16,188 | 4,554 | 3.55× |
| Marine IK Reverse (minified) | 1,025 | 787 | 1.30× |
| Marine IK Reverse (prettified) | 3,623 | 3,526 | 1.03× |
| Marine IK (minified) | 1,030 | 802 | 1.28× |
| Marine IK (prettified) | 4,291 | 3,590 | 1.20× |
| Random (minified) | 11,258 | 3,565 | 3.16× |
| Random (prettified) | 12,620 | 4,506 | 2.80× |
| Twitter (minified) | 16,115 | 5,883 | 2.74× |
| Twitter (prettified) | 14,525 | 3,295 | 4.41× |
| Apache Builds Small (minified) | 11,523 | 4,219 | 2.73× |
| Canada Small (minified) | 1,932 | 1,365 | 1.42× |
| Apache Builds Small (prettified) | 12,429 | 4,771 | 2.60× |
| Canada Small (prettified) | 5,309 | 3,482 | 1.52× |
| CitmCatalog Small (minified) | 7,896 | 4,044 | 1.95× |
| CitmCatalog Small (prettified) | 10,050 | 4,431 | 2.27× |
| Discord Small (minified) | 10,977 | 4,668 | 2.35× |
| Discord Small (prettified) | 9,774 | 3,654 | 2.68× |
| Github Events Small (minified) | 13,241 | 5,599 | 2.37× |
| Google Maps Response Small (minified) | 9,805 | 4,107 | 2.39× |
| Github Events Small (prettified) | 12,960 | 4,857 | 2.67× |
| Google Maps Response Small (prettified) | 16,987 | 6,287 | 2.70× |
| Instruments Small (minified) | 11,132 | 4,099 | 2.72× |
| Instruments Small (prettified) | 12,431 | 4,317 | 2.88× |
| Marine IK Reverse Small (minified) | 1,607 | 1,030 | 1.56× |
| Marine IK Reverse Small (prettified) | 5,295 | 3,334 | 1.59× |
| Marine IK Small (minified) | 1,592 | 1,033 | 1.54× |
| Marine IK Small (prettified) | 5,338 | 3,387 | 1.58× |
| Random Small (minified) | 11,639 | 4,309 | 2.70× |
| Random Small (prettified) | 13,815 | 5,186 | 2.66× |
| Twitter Small (minified) | 12,494 | 5,820 | 2.15× |
| Twitter Small (prettified) | 10,938 | 4,523 | 2.42× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,080 | 1,875 | 1,378 | 1.64× |
| Prettify | 3,712 | 2,812 | 1,438 | 1.32× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,550 | 425 | 10.72× |
| Double | 884 | 714 | 1.24× |
| Int64 | 5,341 | 2,174 | 2.46× |
| String | 11,572 | 4,578 | 2.53× |
| Uint64 | 4,637 | 2,208 | 2.10× |
| Apache Builds (minified) | 16,535 | 6,084 | 2.72× |
| Canada (minified) | 1,902 | 998 | 1.91× |
| Apache Builds (prettified) | 19,216 | 5,728 | 3.35× |
| Canada (prettified) | 5,158 | 3,949 | 1.31× |
| CitmCatalog (minified) | 9,907 | 5,752 | 1.72× |
| CitmCatalog (prettified) | 15,527 | 7,028 | 2.21× |
| Discord (minified) | 14,921 | 5,399 | 2.76× |
| Discord (prettified) | 16,298 | 4,535 | 3.59× |
| Github Events (minified) | 17,040 | 5,758 | 2.96× |
| Google Maps Response (minified) | 12,420 | 4,143 | 3.00× |
| Github Events (prettified) | 16,453 | 5,354 | 3.07× |
| Google Maps Response (prettified) | 15,958 | 5,656 | 2.82× |
| Instruments (minified) | 18,296 | 5,215 | 3.51× |
| Instruments (prettified) | 20,307 | 5,671 | 3.58× |
| Marine IK Reverse (minified) | 1,199 | 638 | 1.88× |
| Marine IK Reverse (prettified) | 5,043 | 4,058 | 1.24× |
| Marine IK (minified) | 1,215 | 660 | 1.84× |
| Marine IK (prettified) | 5,039 | 4,098 | 1.23× |
| Random (minified) | 14,876 | 3,856 | 3.86× |
| Random (prettified) | 16,007 | 4,436 | 3.61× |
| Twitter (minified) | 19,162 | 6,927 | 2.77× |
| Twitter (prettified) | 18,520 | 5,437 | 3.41× |
| Apache Builds Small (minified) | 11,812 | 3,712 | 3.18× |
| Canada Small (minified) | 1,823 | 1,007 | 1.81× |
| Apache Builds Small (prettified) | 13,458 | 3,782 | 3.56× |
| Canada Small (prettified) | 4,660 | 3,805 | 1.22× |
| CitmCatalog Small (minified) | 8,372 | 3,303 | 2.53× |
| CitmCatalog Small (prettified) | 11,126 | 3,299 | 3.37× |
| Discord Small (minified) | 11,607 | 3,479 | 3.34× |
| Discord Small (prettified) | 11,991 | 2,939 | 4.08× |
| Github Events Small (minified) | 12,240 | 4,404 | 2.78× |
| Google Maps Response Small (minified) | 11,664 | 3,825 | 3.05× |
| Github Events Small (prettified) | 12,972 | 3,804 | 3.41× |
| Google Maps Response Small (prettified) | 17,183 | 5,341 | 3.22× |
| Instruments Small (minified) | 14,477 | 2,837 | 5.10× |
| Instruments Small (prettified) | 15,872 | 2,815 | 5.64× |
| Marine IK Reverse Small (minified) | 1,537 | 874 | 1.76× |
| Marine IK Reverse Small (prettified) | 5,213 | 4,010 | 1.30× |
| Marine IK Small (minified) | 1,670 | 795 | 2.10× |
| Marine IK Small (prettified) | 5,367 | 3,698 | 1.45× |
| Random Small (minified) | 14,250 | 3,935 | 3.62× |
| Random Small (prettified) | 17,049 | 4,448 | 3.83× |
| Twitter Small (minified) | 11,106 | 4,340 | 2.56× |
| Twitter Small (prettified) | 10,560 | 3,911 | 2.70× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,645 | 2,397 | 1,741 | 1.52× |
| Prettify | 4,543 | 3,699 | 2,365 | 1.23× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,214 | 669 | 3,172 | 1.01× |
| Double | 891 | 721 | 653 | 1.23× |
| Int64 | 5,480 | 2,742 | 4,021 | 1.36× |
| String | 11,113 | 5,809 | 9,471 | 1.17× |
| Uint64 | 6,415 | 2,718 | 4,107 | 1.56× |
| Apache Builds (minified) | 20,378 | 9,366 | 13,639 | 1.49× |
| Canada (minified) | 2,128 | 1,530 | 1,042 | 1.39× |
| Apache Builds (prettified) | 23,175 | 8,103 | 624 | 2.86× |
| Canada (prettified) | 6,108 | 3,911 | 389 | 1.56× |
| CitmCatalog (minified) | 12,750 | 7,099 | 11,368 | 1.12× |
| CitmCatalog (prettified) | 20,279 | 7,987 | 569 | 2.54× |
| Discord (minified) | 15,138 | 7,232 | 13,068 | 1.16× |
| Discord (prettified) | 19,124 | 7,163 | 559 | 2.67× |
| Github Events (minified) | 14,765 | 8,743 | 11,125 | 1.33× |
| Google Maps Response (minified) | 17,191 | 5,061 | 10,015 | 1.72× |
| Github Events (prettified) | 18,209 | 5,774 | 638 | 3.15× |
| Google Maps Response (prettified) | 22,417 | 7,464 | 522 | 3.00× |
| Instruments (minified) | 20,176 | 6,536 | 15,584 | 1.29× |
| Instruments (prettified) | 26,130 | 6,159 | 458 | 4.24× |
| Marine IK Reverse (minified) | 1,288 | 983 | 880 | 1.31× |
| Marine IK Reverse (prettified) | 6,326 | 3,950 | 307 | 1.60× |
| Marine IK (minified) | 1,258 | 950 | 888 | 1.32× |
| Marine IK (prettified) | 5,801 | 3,792 | 337 | 1.53× |
| Random (minified) | 17,751 | 5,185 | 11,494 | 1.54× |
| Random (prettified) | 20,408 | 5,751 | 529 | 3.55× |
| Twitter (minified) | 17,537 | 8,726 | 11,079 | 1.58× |
| Twitter (prettified) | 20,761 | 6,096 | 529 | 3.41× |
| Apache Builds Small (minified) | 13,644 | 5,121 | 8,100 | 1.68× |
| Canada Small (minified) | 2,097 | 1,692 | 1,088 | 1.24× |
| Apache Builds Small (prettified) | 18,688 | 4,554 | 546 | 4.10× |
| Canada Small (prettified) | 6,162 | 4,002 | 410 | 1.54× |
| CitmCatalog Small (minified) | 8,496 | 3,968 | 6,640 | 1.28× |
| CitmCatalog Small (prettified) | 16,706 | 3,630 | 494 | 4.60× |
| Discord Small (minified) | 11,491 | 4,837 | 8,559 | 1.34× |
| Discord Small (prettified) | 15,500 | 3,227 | 519 | 4.80× |
| Github Events Small (minified) | 10,486 | 5,943 | 7,502 | 1.40× |
| Google Maps Response Small (minified) | 14,552 | 4,235 | 8,799 | 1.65× |
| Github Events Small (prettified) | 14,201 | 4,428 | 538 | 3.21× |
| Google Maps Response Small (prettified) | 28,929 | 7,162 | 517 | 4.04× |
| Instruments Small (minified) | 14,187 | 3,750 | 8,822 | 1.61× |
| Instruments Small (prettified) | 23,893 | 3,469 | 525 | 6.89× |
| Marine IK Reverse Small (minified) | 1,441 | 1,332 | 1,558 | 0.92× |
| Marine IK Reverse Small (prettified) | 6,787 | 3,737 | 543 | 1.82× |
| Marine IK Small (minified) | 1,417 | 1,320 | 1,545 | 0.92× |
| Marine IK Small (prettified) | 6,696 | 3,862 | 532 | 1.73× |
| Random Small (minified) | 16,477 | 4,929 | 8,993 | 1.83× |
| Random Small (prettified) | 27,799 | 5,405 | 533 | 5.14× |
| Twitter Small (minified) | 10,455 | 6,576 | 7,380 | 1.42× |
| Twitter Small (prettified) | 14,413 | 4,211 | 562 | 3.42× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,410 | 2,534 | 1,885 | 1.35× |
| Prettify | 3,979 | 3,253 | 2,487 | 1.22× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,602 | 860 | 2,758 | 1.31× |
| Double | 916 | 712 | 747 | 1.23× |
| Int64 | 5,437 | 2,552 | 3,628 | 1.50× |
| String | 10,245 | 4,980 | 11,175 | 0.92× |
| Uint64 | 5,100 | 2,664 | 3,415 | 1.49× |
| Apache Builds (minified) | 12,595 | 5,071 | 8,723 | 1.44× |
| Canada (minified) | 2,107 | 1,603 | 749 | 1.31× |
| Apache Builds (prettified) | 13,759 | 4,604 | 343 | 2.99× |
| Canada (prettified) | 6,511 | 3,263 | 251 | 2.00× |
| CitmCatalog (minified) | 7,865 | 2,004 | 6,418 | 1.23× |
| CitmCatalog (prettified) | 15,908 | 3,682 | 301 | 4.32× |
| Discord (minified) | 12,612 | 2,968 | 8,994 | 1.40× |
| Discord (prettified) | 14,918 | 2,429 | 345 | 6.14× |
| Github Events (minified) | 15,162 | 3,943 | 10,890 | 1.39× |
| Google Maps Response (minified) | 11,265 | 1,895 | 8,480 | 1.33× |
| Github Events (prettified) | 15,572 | 3,991 | 419 | 3.90× |
| Google Maps Response (prettified) | 22,492 | 3,296 | 309 | 6.82× |
| Instruments (minified) | 14,497 | 1,801 | 7,613 | 1.90× |
| Instruments (prettified) | 17,888 | 2,023 | 294 | 8.84× |
| Marine IK Reverse (minified) | 1,402 | 840 | 678 | 1.67× |
| Marine IK Reverse (prettified) | 6,581 | 3,117 | 289 | 2.11× |
| Marine IK (minified) | 1,400 | 949 | 834 | 1.47× |
| Marine IK (prettified) | 7,068 | 3,446 | 312 | 2.05× |
| Random (minified) | 11,028 | 2,317 | 8,150 | 1.35× |
| Random (prettified) | 14,122 | 2,753 | 329 | 5.13× |
| Twitter (minified) | 13,975 | 4,155 | 10,687 | 1.31× |
| Twitter (prettified) | 14,884 | 2,163 | 418 | 6.88× |
| Apache Builds Small (minified) | 10,626 | 3,275 | 6,926 | 1.53× |
| Canada Small (minified) | 2,491 | 1,908 | 912 | 1.31× |
| Apache Builds Small (prettified) | 12,170 | 4,523 | 326 | 2.69× |
| Canada Small (prettified) | 7,323 | 3,715 | 284 | 1.97× |
| CitmCatalog Small (minified) | 9,172 | 2,014 | 7,680 | 1.19× |
| CitmCatalog Small (prettified) | 18,322 | 2,353 | 348 | 7.79× |
| Discord Small (minified) | 13,185 | 2,673 | 8,343 (n/c) | 1.58× |
| Discord Small (prettified) | 13,982 | 2,333 | 313 | 5.99× |
| Github Events Small (minified) | 10,658 | 3,196 | 10,147 | 1.05× |
| Google Maps Response Small (minified) | 10,708 | 1,811 | 7,981 | 1.34× |
| Github Events Small (prettified) | 15,817 | 3,117 | 349 | 5.07× |
| Google Maps Response Small (prettified) | 20,204 | 3,048 | 292 | 6.63× |
| Instruments Small (minified) | 15,868 | 2,262 | 7,834 | 2.03× |
| Instruments Small (prettified) | 19,234 | 2,196 | 334 | 8.76× |
| Marine IK Reverse Small (minified) | 1,739 | 1,106 | 1,694 | 1.03× |
| Marine IK Reverse Small (prettified) | 8,108 | 3,635 | 307 | 2.23× |
| Marine IK Small (minified) | 1,822 | 1,175 | 1,552 | 1.17× |
| Marine IK Small (prettified) | 9,498 | 3,843 | 325 | 2.47× |
| Random Small (minified) | 11,907 | 2,327 | 9,120 | 1.31× |
| Random Small (prettified) | 13,521 | 2,588 | 312 | 5.22× |
| Twitter Small (minified) | 12,780 | 2,830 | 8,114 (n/c) | 1.58× |
| Twitter Small (prettified) | 14,419 | 1,795 | 407 | 8.03× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,005 | 1,713 | 1,032 | 1.17× |
| Prettify | 3,186 | 2,413 | 1,268 | 1.32× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,267 | 774 | 1.64× |
| Double | 899 | 708 | 1.27× |
| Int64 | 3,328 | 1,834 | 1.81× |
| String | 7,588 | 3,344 | 2.27× |
| Uint64 | 2,459 | 1,908 | 1.29× |
| Apache Builds (minified) | 9,561 | 3,755 | 2.55× |
| Canada (minified) | 2,998 | 1,826 | 1.64× |
| Apache Builds (prettified) | 11,496 | 3,600 | 3.19× |
| Canada (prettified) | 6,821 | 3,766 | 1.81× |
| CitmCatalog (minified) | 8,303 | 2,082 | 3.99× |
| CitmCatalog (prettified) | 12,152 | 4,342 | 2.80× |
| Discord (minified) | 11,166 | 3,279 | 3.41× |
| Discord (prettified) | 11,126 | 3,017 | 3.69× |
| Github Events (minified) | 15,427 | 5,956 | 2.59× |
| Google Maps Response (minified) | 8,397 | 2,525 | 3.33× |
| Github Events (prettified) | 15,476 | 4,309 | 3.59× |
| Google Maps Response (prettified) | 12,153 | 3,567 | 3.41× |
| Instruments (minified) | 11,422 | 2,144 | 5.33× |
| Instruments (prettified) | 13,269 | 3,015 | 4.40× |
| Marine IK Reverse (minified) | 1,190 | 697 | 1.71× |
| Marine IK Reverse (prettified) | 5,324 | 2,879 | 1.85× |
| Marine IK (minified) | 1,212 | 713 | 1.70× |
| Marine IK (prettified) | 4,686 | 2,612 (n/c) | 1.79× |
| Random (minified) | 9,086 | 2,359 | 3.85× |
| Random (prettified) | 13,705 | 3,086 | 4.44× |
| Twitter (minified) | 15,700 | 5,048 | 3.11× |
| Twitter (prettified) | 14,022 | 4,742 | 2.96× |
| Apache Builds Small (minified) | 9,385 | 3,031 | 3.10× |
| Canada Small (minified) | 2,613 | 1,540 | 1.70× |
| Apache Builds Small (prettified) | 10,278 | 3,537 | 2.91× |
| Canada Small (prettified) | 6,825 | 3,736 | 1.83× |
| CitmCatalog Small (minified) | 9,458 | 2,068 | 4.57× |
| CitmCatalog Small (prettified) | 12,814 | 3,102 | 4.13× |
| Discord Small (minified) | 12,352 | 3,838 | 3.22× |
| Discord Small (prettified) | 13,261 | 3,457 | 3.84× |
| Github Events Small (minified) | 13,004 | 4,661 | 2.79× |
| Google Maps Response Small (minified) | 9,791 | 2,490 | 3.93× |
| Github Events Small (prettified) | 13,473 | 4,126 | 3.27× |
| Google Maps Response Small (prettified) | 14,958 | 4,092 | 3.66× |
| Instruments Small (minified) | 14,747 | 2,238 | 6.59× |
| Instruments Small (prettified) | 15,956 | 3,553 | 4.49× |
| Marine IK Reverse Small (minified) | 2,016 | 1,119 | 1.80× |
| Marine IK Reverse Small (prettified) | 5,794 | 3,770 | 1.54× |
| Marine IK Small (minified) | 1,993 | 1,118 | 1.78× |
| Marine IK Small (prettified) | 5,820 | 3,795 | 1.53× |
| Random Small (minified) | 10,531 | 3,033 | 3.47× |
| Random Small (prettified) | 12,606 | 3,618 | 3.48× |
| Twitter Small (minified) | 10,780 | 3,576 | 3.01× |
| Twitter Small (prettified) | 13,356 | 4,333 | 3.08× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,061 | 1,746 | 992 | 1.18× |
| Prettify | 2,900 | 2,522 | 1,632 | 1.15× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,944 vs Jsonifier's 4,151 MB/s (0.84×). Reused, Jsonifier wins by 2.74× (16,115 vs 5,883).
- **Prettify, Windows / MSVC.** Glaze runs 2,779 vs 2,404 MB/s (0.86×), with simdjson On Demand at 1,407. Reused, Jsonifier wins by 1.32× (3,712 vs 2,812).
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,555 vs 1,396 MB/s (0.90×), with Glaze at 1,206. It also wins reused, 1,545 vs 1,417 MB/s (0.92×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,554 vs 1,383 MB/s (0.89×), with Glaze at 1,268. It also wins reused, 1,558 vs 1,441 MB/s (0.92×).
- **Double, macOS / GCC.** The reflection writer runs 310 vs 293 MB/s (0.94×), with Glaze at 218. Reused, Jsonifier wins by 1.23× (916 vs 747), with Glaze at 712.
- **Uint64, macOS / GCC.** The reflection writer runs 758 vs 733 MB/s (0.97×), with Glaze at 489. Reused, Jsonifier wins by 1.49× (5,100 vs 3,415).

The three freshly allocated ties are all against the reflection writer on macOS / GCC: Int64 (826 vs 821 MB/s, Glaze at 567), which Jsonifier wins reused by 1.50× (5,437 vs 3,628); minified Twitter Small (9,964 vs 9,388 MB/s, RSEs of 7.0% and 4.3%), which it wins reused by 1.58× (12,780 vs 8,114); and minified Github Events Small (10,362 vs 9,873 MB/s, with Jsonifier's result n/c), which stays a tie reused.

One reused non-win has no freshly allocated counterpart: String on macOS / GCC, which wins freshly allocated (1,486 MB/s against the reflection writer's 1,337 and Glaze's 1,149) and loses reused (0.92×).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.14–2.81× and prettified documents by 15–56×; reused, by 1.12–2.81× and 16–73×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 40 of the 44 Small document tests freshly allocated and 41 reused. The non-wins are minified Marine IK Small and Marine IK Reverse Small on Linux / GCC in both runs, minified Github Events Small on macOS / GCC in both runs (a tie), and minified Twitter Small on macOS / GCC freshly allocated (a tie). Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.33–1.64× freshly allocated and 1.17–1.64× reused. It wins Prettify on all five reused, by 1.15–1.32×, and on four of five freshly allocated, by 1.22–1.30×; the exception is Windows / MSVC, where Glaze runs 2,779 vs 2,404 MB/s (0.86×). Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,486 MB/s on macOS / GCC (1.29× Glaze's 1,149, ahead of the reflection writer's 1,337) and 1,002 MB/s on macOS / Clang (1.19× Glaze's 841). The AVX2 builds run the same test at 1,470–4,443 MB/s and lead Glaze by 1.57× (Windows / MSVC), 2.41× (Linux / Clang) and 2.08× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 2.27× on macOS / Clang (7,588 vs 3,344 MB/s) and 2.06× on macOS / GCC (10,245 vs 4,980), against 1.91–3.22× on the AVX2 builds. On macOS / GCC the reflection writer is ahead reused, at 11,175 MB/s (0.92×), a significant loss.

So most of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: reused, both M2 Pro leads over Glaze sit inside the AVX2 range. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated on macOS / GCC, yes: the reflection writer runs 310 vs 293 MB/s (0.94×), a significant loss. Freshly allocated on Linux / GCC, Jsonifier edges it, 348 vs 345 MB/s (1.01×). Reused, Jsonifier leads on both: 891 vs 653 MB/s on Linux / GCC (1.36×) and 916 vs 747 MB/s on macOS / GCC (1.23×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
