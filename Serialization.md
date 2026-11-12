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

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md).

## Performance results

Jsonifier was fastest in 248 of the 255 write tests on five builds, tied 1 and lost 6. With the output string reused across iterations it was fastest in 253 of 255, tied 1 and lost 1. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other four are against simdjson's reflection writer, which exists only on GCC: Double, minified Marine IK Small and minified Marine IK Reverse Small on Linux / GCC, and Double on macOS / GCC. The one freshly allocated tie is Uint64 on macOS / GCC, against the reflection writer. Reused, the one loss is minified Marine IK Small on Linux / GCC and the one tie is String on macOS / GCC, both against the reflection writer. Windows / MSVC wins all 51 tests reused, and Linux / Clang and macOS / Clang win all 51 in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.38–6.73× across the five builds and the reflection writer by 15–58× on the two GCC builds; Small documents lead them by 1.35–6.83× and 12.87–54.53×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 12, 2026, with Jsonifier [55d73f0](https://github.com/nihilai-collective/jsonifier/commit/55d73f0), Glaze [7ef5e1ab](https://github.com/stephenberry/glaze/commit/7ef5e1ab), simdjson [05da81d45](https://github.com/simdjson/simdjson/commit/05da81d45) and BenchmarkSuite [0aaa9ac](https://github.com/nihilai-collective/benchmarksuite/commit/0aaa9ac) on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md). `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-42 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-42 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M2 Pro (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.44 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size. Ratios are computed from unrounded throughput, so one recomputed from the rounded values shown can differ in the last digit.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M2 Pro. Non-converged results are included in the rankings and in the win, tie and loss counts. Each one is marked n/c in the tables below, and a test with a non-converged result should be read with reduced statistical confidence.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. A tie is therefore usually at the top, and both ties in this sweep are. Because the verdict is a significance test and not a ratio threshold, a lead of about 1% with tight samples counts as a win: on Linux / GCC, Int64 freshly allocated (939 vs 934 MB/s, RSEs 0.11% and 0.27%) and Bool reused (3,215 vs 3,178, 0.39% and 0.42%) are wins of 1.01× against the reflection writer. Both ties are against the same writer on macOS / GCC and sit within 1%: Uint64 freshly allocated (767 vs Jsonifier's 762 MB/s, RSEs 0.7% and 0.3%) and String reused (9,175 vs 9,149, 0.6% and 1.2%).

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 0 | 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 49 | 1 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **248** | **1** | **6** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,201 | 91 | 13.19× |
| Double | 445 | 142 | 3.13× |
| Int64 | 475 | 360 | 1.32× |
| String | 1,399 | 912 | 1.53× |
| Uint64 | 499 | 374 | 1.33× |
| Apache Builds (minified) | 12,928 | 4,645 | 2.78× |
| Canada (minified) | 1,231 | 800 | 1.54× |
| Apache Builds (prettified) | 12,252 | 5,193 | 2.36× |
| Canada (prettified) | 2,688 | 1,399 | 1.92× |
| CitmCatalog (minified) | 8,865 | 4,188 | 2.12× |
| CitmCatalog (prettified) | 4,295 | 1,913 | 2.25× |
| Discord (minified) | 10,418 | 4,356 | 2.39× |
| Discord (prettified) | 11,968 | 4,190 | 2.86× |
| Github Events (minified) | 12,270 | 5,443 | 2.25× |
| Google Maps Response (minified) | 9,339 | 3,499 | 2.67× |
| Github Events (prettified) | 11,555 (n/c) | 3,790 | 3.05× |
| Google Maps Response (prettified) | 13,298 | 5,406 | 2.46× |
| Instruments (minified) | 12,606 | 4,215 (n/c) | 2.99× |
| Instruments (prettified) | 14,394 | 4,304 | 3.34× |
| Marine IK Reverse (minified) | 827 | 641 | 1.29× |
| Marine IK Reverse (prettified) | 2,621 | 1,687 | 1.55× |
| Marine IK (minified) | 800 | 579 | 1.38× |
| Marine IK (prettified) | 2,678 | 1,188 | 2.25× |
| Random (minified) | 3,612 | 3,075 | 1.17× |
| Random (prettified) | 4,174 | 2,268 | 1.84× |
| Twitter (minified) | 4,053 | 4,824 | 0.84× |
| Twitter (prettified) | 3,946 | 1,860 | 2.12× |
| Apache Builds Small (minified) | 8,163 | 3,641 | 2.24× |
| Canada Small (minified) | 1,580 | 1,200 | 1.32× |
| Apache Builds Small (prettified) | 11,560 | 3,903 | 2.96× |
| Canada Small (prettified) | 4,218 | 2,970 | 1.42× |
| CitmCatalog Small (minified) | 6,869 | 3,288 | 2.09× |
| CitmCatalog Small (prettified) | 9,503 | 4,326 | 2.20× |
| Discord Small (minified) | 9,503 | 3,961 | 2.40× |
| Discord Small (prettified) | 7,971 | 3,761 | 2.12× |
| Github Events Small (minified) | 10,135 | 3,127 | 3.24× |
| Google Maps Response Small (minified) | 8,544 | 3,484 | 2.45× |
| Github Events Small (prettified) | 10,657 | 4,118 | 2.59× |
| Google Maps Response Small (prettified) | 13,962 | 5,091 | 2.74× |
| Instruments Small (minified) | 10,224 | 3,223 (n/c) | 3.17× |
| Instruments Small (prettified) | 10,332 | 3,876 | 2.67× |
| Marine IK Reverse Small (minified) | 1,380 | 908 | 1.52× |
| Marine IK Reverse Small (prettified) | 4,950 | 3,017 | 1.64× |
| Marine IK Small (minified) | 1,415 | 935 | 1.51× |
| Marine IK Small (prettified) | 4,965 | 2,948 | 1.68× |
| Random Small (minified) | 9,054 | 3,600 | 2.51× |
| Random Small (prettified) | 11,681 | 3,788 | 3.08× |
| Twitter Small (minified) | 8,435 | 4,683 | 1.80× |
| Twitter Small (prettified) | 8,147 | 3,235 (n/c) | 2.52× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,825 | 1,922 | 1,315 | 1.47× |
| Prettify | 2,037 | 2,446 | 1,354 | 0.83× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,319 | 216 | 10.76× |
| Double | 342 | 297 | 1.15× |
| Int64 | 924 | 901 | 1.02× |
| String | 4,568 | 1,897 | 2.41× |
| Uint64 | 946 | 832 | 1.14× |
| Apache Builds (minified) | 16,605 | 5,338 | 3.11× |
| Canada (minified) | 1,946 | 960 | 2.03× |
| Apache Builds (prettified) | 18,846 | 5,513 | 3.42× |
| Canada (prettified) | 5,220 | 3,230 | 1.62× |
| CitmCatalog (minified) | 10,402 | 5,012 | 2.08× |
| CitmCatalog (prettified) | 14,967 | 5,320 | 2.81× |
| Discord (minified) | 14,184 | 5,037 | 2.82× |
| Discord (prettified) | 15,967 | 4,194 | 3.81× |
| Github Events (minified) | 16,367 | 5,107 | 3.20× |
| Google Maps Response (minified) | 11,782 | 3,795 | 3.10× |
| Github Events (prettified) | 16,716 | 4,816 | 3.47× |
| Google Maps Response (prettified) | 15,331 | 5,273 | 2.91× |
| Instruments (minified) | 18,551 | 4,819 | 3.85× |
| Instruments (prettified) | 20,668 | 4,871 | 4.24× |
| Marine IK Reverse (minified) | 1,169 | 639 | 1.83× |
| Marine IK Reverse (prettified) | 5,076 | 3,675 | 1.38× |
| Marine IK (minified) | 1,157 | 630 | 1.84× |
| Marine IK (prettified) | 5,010 | 2,920 | 1.72× |
| Random (minified) | 14,991 | 3,599 | 4.17× |
| Random (prettified) | 16,180 | 4,022 | 4.02× |
| Twitter (minified) | 19,364 | 6,459 | 3.00× |
| Twitter (prettified) | 18,205 | 4,387 | 4.15× |
| Apache Builds Small (minified) | 10,356 | 3,388 | 3.06× |
| Canada Small (minified) | 1,908 | 968 | 1.97× |
| Apache Builds Small (prettified) | 11,434 | 3,730 | 3.07× |
| Canada Small (prettified) | 5,053 | 3,485 | 1.45× |
| CitmCatalog Small (minified) | 7,451 | 2,285 | 3.26× |
| CitmCatalog Small (prettified) | 10,309 | 3,497 | 2.95× |
| Discord Small (minified) | 10,140 | 3,169 | 3.20× |
| Discord Small (prettified) | 11,762 | 2,756 | 4.27× |
| Github Events Small (minified) | 9,361 | 4,125 | 2.27× |
| Google Maps Response Small (minified) | 10,509 | 3,472 | 3.03× |
| Github Events Small (prettified) | 10,604 | 3,858 | 2.75× |
| Google Maps Response Small (prettified) | 15,239 | 4,859 | 3.14× |
| Instruments Small (minified) | 12,206 | 2,522 | 4.84× |
| Instruments Small (prettified) | 15,136 | 2,499 | 6.06× |
| Marine IK Reverse Small (minified) | 1,398 | 819 | 1.71× |
| Marine IK Reverse Small (prettified) | 4,882 | 3,604 | 1.35× |
| Marine IK Small (minified) | 1,439 | 820 | 1.75× |
| Marine IK Small (prettified) | 5,126 | 3,638 | 1.41× |
| Random Small (minified) | 13,073 | 3,674 | 3.56× |
| Random Small (prettified) | 15,827 | 3,975 | 3.98× |
| Twitter Small (minified) | 8,987 | 4,460 | 2.02× |
| Twitter Small (prettified) | 9,333 | 3,087 | 3.02× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,611 | 2,629 | 1,721 | 1.37× |
| Prettify | 4,507 | 3,490 | 2,226 | 1.29× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,724 | 195 | 254 | 6.78× |
| Double | 337 | 317 | 345 | 0.97× |
| Int64 | 939 | 902 | 934 | 1.01× |
| String | 4,229 | 2,083 | 3,818 | 1.11× |
| Uint64 | 927 | 855 | 894 | 1.04× |
| Apache Builds (minified) | 20,089 | 7,940 | 13,366 | 1.50× |
| Canada (minified) | 2,087 | 1,426 | 1,031 | 1.46× |
| Apache Builds (prettified) | 22,999 | 7,286 | 629 | 3.16× |
| Canada (prettified) | 6,023 | 3,242 | 389 | 1.86× |
| CitmCatalog (minified) | 12,386 | 6,004 | 11,184 | 1.11× |
| CitmCatalog (prettified) | 19,823 | 5,751 | 566 | 3.45× |
| Discord (minified) | 15,098 | 6,310 | 12,976 | 1.16× |
| Discord (prettified) | 19,908 | 5,941 | 531 | 3.35× |
| Github Events (minified) | 14,392 | 7,159 | 10,958 | 1.31× |
| Google Maps Response (minified) | 15,624 | 4,474 | 9,758 | 1.60× |
| Github Events (prettified) | 18,457 | 5,072 | 624 | 3.64× |
| Google Maps Response (prettified) | 20,803 | 6,522 | 522 | 3.19× |
| Instruments (minified) | 19,667 | 5,244 | 15,517 | 1.27× |
| Instruments (prettified) | 25,603 | 5,294 | 480 | 4.84× |
| Marine IK Reverse (minified) | 1,258 | 972 | 885 | 1.30× |
| Marine IK Reverse (prettified) | 6,276 | 3,624 | 266 | 1.73× |
| Marine IK (minified) | 1,258 | 947 | 884 | 1.33× |
| Marine IK (prettified) | 6,300 | 3,073 | 392 | 2.05× |
| Random (minified) | 17,461 | 4,567 | 11,320 | 1.54× |
| Random (prettified) | 21,005 | 4,899 | 543 | 4.29× |
| Twitter (minified) | 16,993 | 7,665 | 10,915 | 1.56× |
| Twitter (prettified) | 20,357 | 4,909 | 517 | 4.15× |
| Apache Builds Small (minified) | 11,904 | 4,572 | 7,118 | 1.67× |
| Canada Small (minified) | 2,060 | 1,567 | 1,085 | 1.31× |
| Apache Builds Small (prettified) | 13,475 | 4,105 | 550 | 3.28× |
| Canada Small (prettified) | 5,496 | 3,641 | 415 | 1.51× |
| CitmCatalog Small (minified) | 7,586 | 3,562 | 6,208 | 1.22× |
| CitmCatalog Small (prettified) | 14,868 | 3,097 | 502 | 4.80× |
| Discord Small (minified) | 10,115 | 3,996 | 8,084 | 1.25× |
| Discord Small (prettified) | 14,916 | 3,166 | 509 | 4.71× |
| Github Events Small (minified) | 8,198 | 5,273 | 6,298 | 1.30× |
| Google Maps Response Small (minified) | 13,064 | 3,871 | 8,039 | 1.63× |
| Github Events Small (prettified) | 11,395 | 3,572 | 531 | 3.19× |
| Google Maps Response Small (prettified) | 24,471 | 5,713 | 524 | 4.28× |
| Instruments Small (minified) | 12,368 | 4,010 | 8,178 | 1.51× |
| Instruments Small (prettified) | 21,534 | 3,500 | 523 | 6.15× |
| Marine IK Reverse Small (minified) | 1,443 | 1,197 | 1,551 | 0.93× |
| Marine IK Reverse Small (prettified) | 6,598 | 3,470 | 513 | 1.90× |
| Marine IK Small (minified) | 1,420 | 1,190 | 1,528 | 0.93× |
| Marine IK Small (prettified) | 7,125 | 3,348 | 532 | 2.13× |
| Random Small (minified) | 14,582 | 4,294 | 8,367 | 1.74× |
| Random Small (prettified) | 23,826 | 4,400 | 539 | 5.41× |
| Twitter Small (minified) | 8,923 | 5,465 | 6,674 | 1.34× |
| Twitter Small (prettified) | 12,260 | 3,741 | 558 | 3.28× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,189 | 2,497 | 1,833 | 1.28× |
| Prettify | 4,187 | 3,169 | 2,428 | 1.32× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,830 | 133 | 229 | 8.00× |
| Double | 241 | 173 | 313 | 0.77× |
| Int64 | 788 | 493 | 729 | 1.08× |
| String | 1,346 | 1,155 | 1,083 | 1.17× |
| Uint64 | 762 | 529 | 767 | 0.99× |
| Apache Builds (minified) | 11,562 | 3,937 | 9,711 | 1.19× |
| Canada (minified) | 2,508 | 1,717 | 841 | 1.46× |
| Apache Builds (prettified) | 14,049 | 4,676 | 388 | 3.00× |
| Canada (prettified) | 7,203 | 3,633 | 261 | 1.98× |
| CitmCatalog (minified) | 7,758 | 2,004 | 6,786 | 1.14× |
| CitmCatalog (prettified) | 17,197 | 3,652 | 329 (n/c) | 4.71× |
| Discord (minified) | 11,804 | 2,558 | 8,027 | 1.47× |
| Discord (prettified) | 13,741 | 2,283 | 291 | 6.02× |
| Github Events (minified) | 15,595 | 3,578 | 11,307 | 1.38× |
| Google Maps Response (minified) | 11,283 | 1,825 | 7,589 | 1.49× |
| Github Events (prettified) | 14,289 | 3,099 | 398 | 4.61× |
| Google Maps Response (prettified) | 18,684 | 2,778 | 320 | 6.73× |
| Instruments (minified) | 12,715 | 2,098 | 8,487 | 1.50× |
| Instruments (prettified) | 16,008 | 2,427 | 335 | 6.60× |
| Marine IK Reverse (minified) | 1,429 | 931 | 820 | 1.54× |
| Marine IK Reverse (prettified) | 7,099 | 3,347 | 311 | 2.12× |
| Marine IK (minified) | 1,422 | 926 | 830 | 1.53× |
| Marine IK (prettified) | 7,134 | 2,984 | 317 (n/c) | 2.39× |
| Random (minified) | 12,572 | 2,402 | 8,573 | 1.47× |
| Random (prettified) | 15,858 | 2,752 | 354 | 5.76× |
| Twitter (minified) | 16,246 | 4,108 | 12,169 | 1.34× |
| Twitter (prettified) | 17,554 | 4,041 | 428 | 4.34× |
| Apache Builds Small (minified) | 9,228 | 2,608 | 6,175 | 1.49× |
| Canada Small (minified) | 2,290 | 1,352 | 749 | 1.69× |
| Apache Builds Small (prettified) | 10,487 | 3,692 | 348 | 2.84× |
| Canada Small (prettified) | 7,884 | 3,311 | 302 | 2.38× |
| CitmCatalog Small (minified) | 8,943 | 1,954 | 7,754 | 1.15× |
| CitmCatalog Small (prettified) | 16,888 | 2,670 | 346 | 6.33× |
| Discord Small (minified) | 12,488 | 2,718 | 9,361 | 1.33× |
| Discord Small (prettified) | 13,842 | 2,443 | 358 | 5.67× |
| Github Events Small (minified) | 11,532 | 3,092 | 10,350 | 1.11× |
| Google Maps Response Small (minified) | 9,638 | 1,551 | 7,665 | 1.26× |
| Github Events Small (prettified) | 13,721 | 2,925 | 394 | 4.69× |
| Google Maps Response Small (prettified) | 15,427 | 2,259 | 283 | 6.83× |
| Instruments Small (minified) | 11,970 | 1,499 | 6,075 | 1.97× |
| Instruments Small (prettified) | 14,333 | 2,204 | 319 | 6.50× |
| Marine IK Reverse Small (minified) | 2,033 | 1,098 | 1,458 | 1.39× |
| Marine IK Reverse Small (prettified) | 7,917 | 2,983 | 274 | 2.65× |
| Marine IK Small (minified) | 1,823 | 982 | 1,536 | 1.19× |
| Marine IK Small (prettified) | 7,272 | 3,019 | 299 | 2.41× |
| Random Small (minified) | 10,388 | 1,945 | 8,196 | 1.27× |
| Random Small (prettified) | 13,774 | 2,146 | 297 | 6.42× |
| Twitter Small (minified) | 11,816 | 3,504 | 8,286 | 1.43× |
| Twitter Small (prettified) | 13,615 | 2,918 | 355 | 4.67× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,984 | 1,440 | 997 | 1.38× |
| Prettify | 2,719 | 1,993 | 1,451 | 1.36× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 946 | 124 | 7.64× |
| Double | 262 | 212 | 1.24× |
| Int64 | 2,745 | 507 | 5.41× |
| String | 1,294 | 1,057 | 1.22× |
| Uint64 | 2,785 | 486 | 5.73× |
| Apache Builds (minified) | 10,676 | 3,253 | 3.28× |
| Canada (minified) | 2,666 | 1,907 | 1.40× |
| Apache Builds (prettified) | 11,459 | 2,928 | 3.91× |
| Canada (prettified) | 7,372 | 3,373 | 2.19× |
| CitmCatalog (minified) | 8,230 | 2,221 | 3.71× |
| CitmCatalog (prettified) | 14,653 | 4,227 | 3.47× |
| Discord (minified) | 12,388 | 3,329 | 3.72× |
| Discord (prettified) | 12,846 | 2,624 | 4.90× |
| Github Events (minified) | 15,014 | 4,863 | 3.09× |
| Google Maps Response (minified) | 10,262 | 2,362 | 4.35× |
| Github Events (prettified) | 14,256 | 4,059 | 3.51× |
| Google Maps Response (prettified) | 14,165 | 3,841 | 3.69× |
| Instruments (minified) | 13,641 | 2,343 | 5.82× |
| Instruments (prettified) | 13,753 | 2,616 | 5.26× |
| Marine IK Reverse (minified) | 1,572 | 927 | 1.70× |
| Marine IK Reverse (prettified) | 4,855 | 3,097 (n/c) | 1.57× |
| Marine IK (minified) | 1,364 | 756 | 1.80× |
| Marine IK (prettified) | 4,726 | 2,617 | 1.81× |
| Random (minified) | 9,955 | 2,387 | 4.17× |
| Random (prettified) | 10,475 | 2,774 | 3.78× |
| Twitter (minified) | 12,744 | 4,062 (n/c) | 3.14× |
| Twitter (prettified) | 15,816 | 4,142 | 3.82× |
| Apache Builds Small (minified) | 8,452 | 3,161 | 2.67× |
| Canada Small (minified) | 2,782 | 1,628 | 1.71× |
| Apache Builds Small (prettified) | 8,504 | 3,252 | 2.61× |
| Canada Small (prettified) | 7,088 | 3,379 | 2.10× |
| CitmCatalog Small (minified) | 6,804 | 1,990 | 3.42× |
| CitmCatalog Small (prettified) | 12,211 | 2,949 | 4.14× |
| Discord Small (minified) | 11,852 | 3,097 | 3.83× |
| Discord Small (prettified) | 12,452 | 3,022 | 4.12× |
| Github Events Small (minified) | 9,923 | 3,545 | 2.80× |
| Google Maps Response Small (minified) | 9,268 | 2,203 | 4.21× |
| Github Events Small (prettified) | 10,427 | 3,212 | 3.25× |
| Google Maps Response Small (prettified) | 14,127 | 3,626 | 3.90× |
| Instruments Small (minified) | 13,032 | 1,988 | 6.56× |
| Instruments Small (prettified) | 10,657 | 2,975 | 3.58× |
| Marine IK Reverse Small (minified) | 1,570 | 937 | 1.68× |
| Marine IK Reverse Small (prettified) | 5,126 | 3,099 | 1.65× |
| Marine IK Small (minified) | 1,880 | 1,001 | 1.88× |
| Marine IK Small (prettified) | 5,568 | 2,859 | 1.95× |
| Random Small (minified) | 9,327 | 2,470 | 3.78× |
| Random Small (prettified) | 10,474 | 2,874 | 3.64× |
| Twitter Small (minified) | 9,565 | 3,224 | 2.97× |
| Twitter Small (prettified) | 11,314 | 2,728 | 4.15× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,529 | 1,533 | 1,146 | 1.65× |
| Prettify | 2,945 | 2,401 | 1,448 | 1.23× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 253 of the 255 write tests, ties 1 and loses 1. On Linux / GCC the reflection writer wins minified Marine IK Small (1,535 vs 1,519 MB/s, 0.99×). On macOS / GCC it ties String, at 9,175 vs Jsonifier's 9,149 MB/s.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 1 | 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **253** | **1** | **1** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,765 | 255 | 6.92× |
| Double | 629 | 316 | 1.99× |
| Int64 | 3,939 | 983 | 4.01× |
| String | 8,177 | 2,362 | 3.46× |
| Uint64 | 3,975 | 1,000 | 3.98× |
| Apache Builds (minified) | 13,446 | 5,242 | 2.57× |
| Canada (minified) | 1,542 | 1,130 | 1.36× |
| Apache Builds (prettified) | 13,620 | 5,706 | 2.39× |
| Canada (prettified) | 4,234 | 3,134 | 1.35× |
| CitmCatalog (minified) | 8,705 | 4,591 | 1.90× |
| CitmCatalog (prettified) | 13,689 | 7,543 | 1.81× |
| Discord (minified) | 11,058 | 4,992 | 2.22× |
| Discord (prettified) | 12,593 | 5,224 | 2.41× |
| Github Events (minified) | 12,270 | 6,089 | 2.01× |
| Google Maps Response (minified) | 10,754 | 4,059 | 2.65× |
| Github Events (prettified) | 12,144 | 4,414 | 2.75× |
| Google Maps Response (prettified) | 15,300 | 5,933 | 2.58× |
| Instruments (minified) | 12,895 | 4,663 | 2.77× |
| Instruments (prettified) | 15,079 | 5,436 | 2.77× |
| Marine IK Reverse (minified) | 939 | 738 | 1.27× |
| Marine IK Reverse (prettified) | 3,977 | 3,310 | 1.20× |
| Marine IK (minified) | 950 | 750 | 1.27× |
| Marine IK (prettified) | 4,065 | 3,355 | 1.21× |
| Random (minified) | 11,082 | 3,260 | 3.40× |
| Random (prettified) | 13,061 | 4,613 | 2.83× |
| Twitter (minified) | 15,474 | 5,585 | 2.77× |
| Twitter (prettified) | 14,462 | 3,792 | 3.81× |
| Apache Builds Small (minified) | 11,756 | 4,254 | 2.76× |
| Canada Small (minified) | 1,815 | 1,202 | 1.51× |
| Apache Builds Small (prettified) | 12,332 | 4,670 | 2.64× |
| Canada Small (prettified) | 4,377 | 3,334 | 1.31× |
| CitmCatalog Small (minified) | 8,080 | 4,129 | 1.96× |
| CitmCatalog Small (prettified) | 10,704 | 5,249 | 2.04× |
| Discord Small (minified) | 10,815 | 4,055 | 2.67× |
| Discord Small (prettified) | 9,766 | 4,271 | 2.29× |
| Github Events Small (minified) | 14,452 | 5,790 | 2.50× |
| Google Maps Response Small (minified) | 10,066 | 3,995 | 2.52× |
| Github Events Small (prettified) | 13,267 | 5,628 | 2.36× |
| Google Maps Response Small (prettified) | 17,076 | 6,741 (n/c) | 2.53× |
| Instruments Small (minified) | 11,417 | 3,859 | 2.96× |
| Instruments Small (prettified) | 12,500 | 4,288 | 2.91× |
| Marine IK Reverse Small (minified) | 1,498 | 1,017 | 1.47× |
| Marine IK Reverse Small (prettified) | 5,206 | 3,434 | 1.52× |
| Marine IK Small (minified) | 1,466 | 1,014 | 1.45× |
| Marine IK Small (prettified) | 5,163 | 3,246 | 1.59× |
| Random Small (minified) | 11,347 | 4,403 | 2.58× |
| Random Small (prettified) | 14,772 | 5,630 | 2.62× |
| Twitter Small (minified) | 12,170 | 5,632 | 2.16× |
| Twitter Small (prettified) | 11,696 | 2,864 (n/c) | 4.08× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,027 | 1,938 | 1,313 | 1.56× |
| Prettify | 3,240 | 2,441 | 1,425 | 1.33× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,506 | 896 | 5.03× |
| Double | 895 | 636 | 1.41× |
| Int64 | 5,147 | 2,203 | 2.34× |
| String | 11,980 | 4,553 | 2.63× |
| Uint64 | 4,463 | 2,194 | 2.03× |
| Apache Builds (minified) | 16,946 | 6,137 | 2.76× |
| Canada (minified) | 1,942 | 998 | 1.94× |
| Apache Builds (prettified) | 19,081 | 6,074 | 3.14× |
| Canada (prettified) | 5,165 | 3,921 | 1.32× |
| CitmCatalog (minified) | 10,313 | 5,713 | 1.81× |
| CitmCatalog (prettified) | 15,301 | 7,091 | 2.16× |
| Discord (minified) | 14,436 | 5,611 | 2.57× |
| Discord (prettified) | 15,933 | 4,769 | 3.34× |
| Github Events (minified) | 16,920 | 6,251 | 2.71× |
| Google Maps Response (minified) | 12,367 | 4,286 | 2.89× |
| Github Events (prettified) | 16,787 | 5,577 | 3.01× |
| Google Maps Response (prettified) | 15,967 | 5,826 | 2.74× |
| Instruments (minified) | 18,611 | 5,427 | 3.43× |
| Instruments (prettified) | 20,296 | 5,622 | 3.61× |
| Marine IK Reverse (minified) | 1,183 | 634 | 1.87× |
| Marine IK Reverse (prettified) | 5,056 | 4,057 | 1.25× |
| Marine IK (minified) | 1,160 | 658 | 1.76× |
| Marine IK (prettified) | 5,012 | 4,070 | 1.23× |
| Random (minified) | 14,814 | 3,907 | 3.79× |
| Random (prettified) | 16,180 | 4,619 | 3.50× |
| Twitter (minified) | 19,250 | 7,251 | 2.65× |
| Twitter (prettified) | 18,161 | 5,209 | 3.49× |
| Apache Builds Small (minified) | 12,194 | 3,669 | 3.32× |
| Canada Small (minified) | 1,913 | 1,000 | 1.91× |
| Apache Builds Small (prettified) | 13,451 | 3,985 | 3.38× |
| Canada Small (prettified) | 5,334 | 3,576 | 1.49× |
| CitmCatalog Small (minified) | 8,912 | 2,649 | 3.36× |
| CitmCatalog Small (prettified) | 11,216 | 3,876 | 2.89× |
| Discord Small (minified) | 11,880 | 3,562 | 3.34× |
| Discord Small (prettified) | 12,919 | 3,048 | 4.24× |
| Github Events Small (minified) | 12,640 | 4,509 | 2.80× |
| Google Maps Response Small (minified) | 11,591 | 3,710 | 3.12× |
| Github Events Small (prettified) | 13,074 | 4,260 | 3.07× |
| Google Maps Response Small (prettified) | 17,095 | 5,628 | 3.04× |
| Instruments Small (minified) | 14,369 | 2,657 | 5.41× |
| Instruments Small (prettified) | 15,993 | 3,058 | 5.23× |
| Marine IK Reverse Small (minified) | 1,438 | 856 | 1.68× |
| Marine IK Reverse Small (prettified) | 5,046 | 3,940 | 1.28× |
| Marine IK Small (minified) | 1,520 | 865 | 1.76× |
| Marine IK Small (prettified) | 5,269 | 3,985 | 1.32× |
| Random Small (minified) | 13,941 | 4,146 | 3.36× |
| Random Small (prettified) | 16,593 | 4,577 | 3.63× |
| Twitter Small (minified) | 10,663 | 4,753 | 2.24× |
| Twitter Small (prettified) | 10,931 | 3,677 | 2.97× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,703 | 2,660 | 1,721 | 1.39× |
| Prettify | 4,554 | 3,477 | 2,220 | 1.31× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,215 | 687 | 3,178 | 1.01× |
| Double | 784 | 735 | 662 | 1.07× |
| Int64 | 5,343 | 2,664 | 4,052 | 1.32× |
| String | 10,929 | 5,917 | 9,301 | 1.18× |
| Uint64 | 6,376 | 2,653 | 4,128 | 1.54× |
| Apache Builds (minified) | 20,468 | 9,299 | 13,342 | 1.53× |
| Canada (minified) | 2,106 | 1,509 | 1,027 | 1.40× |
| Apache Builds (prettified) | 23,194 | 7,984 | 614 | 2.91× |
| Canada (prettified) | 6,014 | 3,860 | 384 | 1.56× |
| CitmCatalog (minified) | 12,439 | 7,137 | 11,004 | 1.13× |
| CitmCatalog (prettified) | 20,344 | 7,835 | 567 | 2.60× |
| Discord (minified) | 15,267 | 7,191 | 12,998 | 1.17× |
| Discord (prettified) | 20,241 | 6,999 | 524 | 2.89× |
| Github Events (minified) | 14,794 | 8,417 | 11,333 | 1.31× |
| Google Maps Response (minified) | 16,802 | 5,149 | 9,895 | 1.70× |
| Github Events (prettified) | 18,203 | 5,816 | 638 | 3.13× |
| Google Maps Response (prettified) | 21,822 | 7,128 | 532 | 3.06× |
| Instruments (minified) | 20,132 | 6,007 | 15,663 | 1.29× |
| Instruments (prettified) | 25,865 | 6,281 | 474 | 4.12× |
| Marine IK Reverse (minified) | 1,261 | 966 | 876 | 1.31× |
| Marine IK Reverse (prettified) | 6,307 | 3,926 | 307 | 1.61× |
| Marine IK (minified) | 1,257 | 990 | 880 | 1.27× |
| Marine IK (prettified) | 6,343 | 4,002 | 328 | 1.59× |
| Random (minified) | 17,541 | 5,127 | 11,343 | 1.55× |
| Random (prettified) | 21,111 | 5,698 | 548 | 3.70× |
| Twitter (minified) | 17,598 | 9,034 | 10,976 | 1.60× |
| Twitter (prettified) | 20,248 | 6,070 | 519 | 3.34× |
| Apache Builds Small (minified) | 13,856 | 5,148 | 8,061 | 1.72× |
| Canada Small (minified) | 2,110 | 1,534 | 1,090 | 1.38× |
| Apache Builds Small (prettified) | 18,162 | 4,582 | 545 | 3.96× |
| Canada Small (prettified) | 5,648 | 4,217 | 418 | 1.34× |
| CitmCatalog Small (minified) | 8,459 | 3,979 | 6,717 | 1.26× |
| CitmCatalog Small (prettified) | 16,836 | 2,992 | 503 | 5.63× |
| Discord Small (minified) | 11,385 | 4,841 | 8,495 | 1.34× |
| Discord Small (prettified) | 15,789 | 3,510 | 518 | 4.50× |
| Github Events Small (minified) | 9,906 | 5,923 | 7,485 | 1.32× |
| Google Maps Response Small (minified) | 14,504 | 4,183 | 8,505 | 1.71× |
| Github Events Small (prettified) | 13,588 | 4,475 | 526 | 3.04× |
| Google Maps Response Small (prettified) | 29,848 | 7,012 | 521 | 4.26× |
| Instruments Small (minified) | 14,043 | 4,580 | 8,777 | 1.60× |
| Instruments Small (prettified) | 24,396 | 3,758 | 517 | 6.49× |
| Marine IK Reverse Small (minified) | 1,484 | 1,251 | 1,398 | 1.06× |
| Marine IK Reverse Small (prettified) | 6,836 | 3,899 | 513 | 1.75× |
| Marine IK Small (minified) | 1,519 | 1,177 | 1,535 | 0.99× |
| Marine IK Small (prettified) | 7,118 | 3,666 | 534 | 1.94× |
| Random Small (minified) | 16,131 | 4,947 | 8,922 | 1.81× |
| Random Small (prettified) | 28,210 | 5,421 | 541 | 5.20× |
| Twitter Small (minified) | 10,566 | 6,605 | 7,543 | 1.40× |
| Twitter Small (prettified) | 14,249 | 4,267 | 549 | 3.34× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,169 | 2,495 | 1,811 | 1.27× |
| Prettify | 4,181 | 3,153 | 2,412 | 1.33× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,759 | 873 | 2,731 | 1.38× |
| Double | 762 | 710 | 668 | 1.07× |
| Int64 | 4,595 | 2,512 | 3,594 | 1.28× |
| String | 9,149 | 5,252 | 9,175 | 1.00× |
| Uint64 | 5,035 | 2,570 | 3,224 | 1.56× |
| Apache Builds (minified) | 11,097 | 5,183 | 7,945 | 1.40× |
| Canada (minified) | 2,379 | 1,847 | 723 | 1.29× |
| Apache Builds (prettified) | 14,455 | 5,437 | 390 | 2.66× |
| Canada (prettified) | 7,167 | 4,115 | 262 | 1.74× |
| CitmCatalog (minified) | 8,325 | 1,935 | 6,218 | 1.34× |
| CitmCatalog (prettified) | 18,978 | 4,289 | 288 | 4.42× |
| Discord (minified) | 11,963 | 3,071 | 8,311 | 1.44× |
| Discord (prettified) | 14,208 | 2,587 | 339 | 5.49× |
| Github Events (minified) | 16,800 | 4,401 | 10,920 | 1.54× |
| Google Maps Response (minified) | 10,287 | 2,032 | 7,983 | 1.29× |
| Github Events (prettified) | 14,605 | 3,903 | 332 | 3.74× |
| Google Maps Response (prettified) | 20,164 | 3,130 | 340 | 6.44× |
| Instruments (minified) | 12,742 | 2,261 | 8,691 | 1.47× |
| Instruments (prettified) | 18,496 | 2,670 | 354 | 6.93× |
| Marine IK Reverse (minified) | 1,426 | 946 | 752 | 1.51× |
| Marine IK Reverse (prettified) | 7,087 | 3,440 | 315 | 2.06× |
| Marine IK (minified) | 1,436 | 937 | 827 | 1.53× |
| Marine IK (prettified) | 7,152 | 3,425 | 318 | 2.09× |
| Random (minified) | 12,633 | 2,536 | 8,483 | 1.49× |
| Random (prettified) | 16,037 | 2,978 | 354 | 5.38× |
| Twitter (minified) | 16,409 | 4,525 | 12,151 | 1.35× |
| Twitter (prettified) | 17,867 | 4,672 | 370 | 3.82× |
| Apache Builds Small (minified) | 10,221 | 2,697 | 7,188 (n/c) | 1.42× |
| Canada Small (minified) | 2,208 | 1,684 | 862 | 1.31× |
| Apache Builds Small (prettified) | 12,216 | 4,414 | 338 | 2.77× |
| Canada Small (prettified) | 8,072 | 3,249 | 300 | 2.48× |
| CitmCatalog Small (minified) | 9,716 | 2,178 | 8,475 | 1.15× |
| CitmCatalog Small (prettified) | 18,861 | 2,868 | 348 | 6.58× |
| Discord Small (minified) | 13,949 | 2,871 | 10,205 | 1.37× |
| Discord Small (prettified) | 15,128 | 2,664 | 358 | 5.68× |
| Github Events Small (minified) | 15,169 | 3,773 | 11,976 (n/c) | 1.27× |
| Google Maps Response Small (minified) | 10,403 | 1,697 | 7,669 | 1.36× |
| Github Events Small (prettified) | 16,231 | 3,282 | 396 | 4.95× |
| Google Maps Response Small (prettified) | 18,310 | 2,295 | 274 | 7.98× |
| Instruments Small (minified) | 14,956 | 1,736 | 6,681 | 2.24× |
| Instruments Small (prettified) | 17,529 | 2,392 | 341 | 7.33× |
| Marine IK Reverse Small (minified) | 1,864 | 1,114 | 1,584 | 1.18× |
| Marine IK Reverse Small (prettified) | 7,860 | 3,563 | 310 | 2.21× |
| Marine IK Small (minified) | 1,942 | 1,058 | 1,641 | 1.18× |
| Marine IK Small (prettified) | 8,637 | 3,534 | 282 | 2.44× |
| Random Small (minified) | 11,424 (n/c) | 2,093 | 9,650 | 1.18× |
| Random Small (prettified) | 13,508 | 2,765 | 332 | 4.89× |
| Twitter Small (minified) | 13,417 | 3,778 | 9,652 | 1.39× |
| Twitter Small (prettified) | 13,144 | 3,372 | 368 | 3.90× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,991 | 1,407 | 981 | 1.42× |
| Prettify | 2,808 | 2,388 | 1,446 | 1.18× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,180 | 776 | 1.52× |
| Double | 754 | 680 | 1.11× |
| Int64 | 3,167 | 2,030 | 1.56× |
| String | 7,354 | 4,017 | 1.83× |
| Uint64 | 3,156 | 2,156 | 1.46× |
| Apache Builds (minified) | 10,965 | 3,511 | 3.12× |
| Canada (minified) | 2,966 | 1,991 | 1.49× |
| Apache Builds (prettified) | 9,731 | 3,274 | 2.97× |
| Canada (prettified) | 7,454 | 4,033 | 1.85× |
| CitmCatalog (minified) | 8,263 | 2,355 | 3.51× |
| CitmCatalog (prettified) | 14,753 | 4,939 | 2.99× |
| Discord (minified) | 13,256 | 3,908 | 3.39× |
| Discord (prettified) | 12,417 | 3,279 | 3.79× |
| Github Events (minified) | 16,016 | 6,198 | 2.58× |
| Google Maps Response (minified) | 10,628 | 2,629 | 4.04× |
| Github Events (prettified) | 15,392 | 5,233 | 2.94× |
| Google Maps Response (prettified) | 15,468 | 4,373 | 3.54× |
| Instruments (minified) | 14,197 | 2,548 | 5.57× |
| Instruments (prettified) | 16,308 | 3,411 | 4.78× |
| Marine IK Reverse (minified) | 1,575 | 927 | 1.70× |
| Marine IK Reverse (prettified) | 5,086 | 3,015 | 1.69× |
| Marine IK (minified) | 1,382 | 763 | 1.81× |
| Marine IK (prettified) | 5,100 | 2,956 | 1.73× |
| Random (minified) | 10,601 | 2,551 | 4.15× |
| Random (prettified) | 10,618 | 3,046 | 3.49× |
| Twitter (minified) | 14,710 | 4,243 | 3.47× |
| Twitter (prettified) | 16,010 | 4,188 | 3.82× |
| Apache Builds Small (minified) | 9,619 | 3,971 | 2.42× |
| Canada Small (minified) | 2,887 | 1,903 | 1.52× |
| Apache Builds Small (prettified) | 11,012 | 3,609 | 3.05× |
| Canada Small (prettified) | 7,409 | 3,885 | 1.91× |
| CitmCatalog Small (minified) | 9,503 | 2,183 | 4.35× |
| CitmCatalog Small (prettified) | 13,433 | 3,266 | 4.11× |
| Discord Small (minified) | 13,110 | 4,025 | 3.26× |
| Discord Small (prettified) | 13,727 | 3,553 | 3.86× |
| Github Events Small (minified) | 13,503 | 4,921 | 2.74× |
| Google Maps Response Small (minified) | 10,120 | 2,632 | 3.85× |
| Github Events Small (prettified) | 13,966 | 4,218 | 3.31× |
| Google Maps Response Small (prettified) | 13,286 | 3,877 | 3.43× |
| Instruments Small (minified) | 14,773 | 2,330 | 6.34× |
| Instruments Small (prettified) | 15,582 | 3,566 | 4.37× |
| Marine IK Reverse Small (minified) | 1,938 | 1,116 | 1.74× |
| Marine IK Reverse Small (prettified) | 5,589 | 3,634 | 1.54× |
| Marine IK Small (minified) | 2,115 | 1,071 | 1.97× |
| Marine IK Small (prettified) | 5,490 | 3,525 | 1.56× |
| Random Small (minified) | 10,424 | 3,252 | 3.21× |
| Random Small (prettified) | 12,591 | 3,598 | 3.50× |
| Twitter Small (minified) | 12,385 | 4,083 | 3.03× |
| Twitter Small (prettified) | 11,584 | 3,926 | 2.95× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,447 | 1,576 | 1,206 | 1.55× |
| Prettify | 3,122 | 2,414 | 1,423 | 1.29× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,824 vs Jsonifier's 4,053 MB/s (0.84×). Reused, Jsonifier wins by 2.77× (15,474 vs 5,585).
- **Prettify, Windows / MSVC.** Glaze runs 2,446 vs 2,037 MB/s (0.83×), with simdjson On Demand at 1,354. Reused, Jsonifier wins by 1.33× (3,240 vs 2,441).
- **Double, Linux / GCC.** The reflection writer runs 345 vs 337 MB/s (0.97×), with Glaze at 317. Reused, Jsonifier wins by 1.07× over Glaze (784 vs 735), with the reflection writer at 662.
- **Marine IK Small (minified), Linux / GCC.** The reflection writer runs 1,528 vs 1,420 MB/s (0.93×), with Glaze at 1,190. It also wins reused, 1,535 vs 1,519 MB/s (0.99×).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,551 vs 1,443 MB/s (0.93×), with Glaze at 1,197. Reused, Jsonifier wins by 1.06× (1,484 vs 1,398).
- **Double, macOS / GCC.** The reflection writer runs 313 vs 241 MB/s (0.77×), with Glaze at 173. Reused, Jsonifier wins by 1.07× over Glaze (762 vs 710), with the reflection writer at 668.

The one freshly allocated tie is Uint64 on macOS / GCC, against the reflection writer (767 vs 762 MB/s, Glaze at 529), which Jsonifier wins reused by 1.56× (5,035 vs 3,224).

One reused non-win has no freshly allocated counterpart: String on macOS / GCC, which wins freshly allocated (1,346 MB/s against Glaze's 1,155 and the reflection writer's 1,083) and ties the reflection writer reused (9,149 vs 9,175 MB/s).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.11–2.98× and prettified documents by 15–58×; reused, by 1.13–3.29× and 16–66×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 42 of the 44 Small document tests freshly allocated and 43 reused. The non-wins are minified Marine IK Small on Linux / GCC in both runs and minified Marine IK Reverse Small on Linux / GCC freshly allocated. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.28–1.65× freshly allocated and 1.27–1.56× reused. It wins Prettify on all five reused, by 1.18–1.33×, and on four of five freshly allocated, by 1.23–1.36×; the exception is Windows / MSVC, where Glaze runs 2,446 vs 2,037 MB/s (0.83×). Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,346 MB/s on macOS / GCC (1.17× Glaze's 1,155, ahead of the reflection writer's 1,083) and 1,294 MB/s on macOS / Clang (1.22× Glaze's 1,057). The AVX2 builds run the same test at 1,399–4,568 MB/s and lead Glaze by 1.53× (Windows / MSVC), 2.41× (Linux / Clang) and 2.03× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.83× on macOS / Clang (7,354 vs 4,017 MB/s) and 1.74× on macOS / GCC (9,149 vs 5,252), against 1.85–3.46× on the AVX2 builds. On macOS / GCC the reflection writer ties Jsonifier reused, at 9,175 MB/s.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: freshly allocated the M2 Pro leads over Glaze are 1.17–1.22× against 1.53–2.41× on AVX2, and reused they rise to 1.74–1.83×, just under the AVX2 range of 1.85–3.46×. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated, yes, on both: the reflection writer runs 313 vs 241 MB/s on macOS / GCC (0.77×) and 345 vs 337 MB/s on Linux / GCC (0.97×), both significant losses. Reused, Jsonifier leads it on both: 784 vs 662 MB/s on Linux / GCC (1.18×) and 762 vs 668 MB/s on macOS / GCC (1.14×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
