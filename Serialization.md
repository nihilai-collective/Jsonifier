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

Jsonifier was fastest in 247 of the 255 write tests on five builds, tied 2 and lost 6. With the output string reused across iterations it was fastest in 252 of 255 and lost 3. Each build runs 51 write tests: five single-value tests, eleven corpus documents in minified and prettified form, the same eleven documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins Minify on every build in both runs, and Prettify on every build except freshly allocated Windows / MSVC. Freshly allocated, two losses are to Glaze: minified Twitter and Prettify on Windows / MSVC. The other four are against simdjson's reflection writer, which exists only on GCC: Int64 and minified Marine IK Reverse Small on Linux / GCC, and Double and Uint64 on macOS / GCC. The two freshly allocated ties are also against the reflection writer: minified Marine IK Small on Linux / GCC and Int64 on macOS / GCC. Reused, one loss is to Glaze, prettified Marine IK Reverse on Windows / MSVC, and two are to the reflection writer: minified Marine IK Reverse Small on Linux / GCC and String on macOS / GCC. Linux / Clang and macOS / Clang win all 51 tests in both runs. Prettified documents are where the lead is widest. Freshly allocated, full-size documents lead Glaze by 1.28–8.28× across the five builds and the reflection writer by 16–63× on the two GCC builds; Small documents lead them by 1.38–8.12× and 12.20–62.08×.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 13, 2026, with Jsonifier [4919934](https://github.com/nihilai-collective/jsonifier/commit/4919934), Glaze [232e9f50](https://github.com/stephenberry/glaze/commit/232e9f50), simdjson [449f38e63](https://github.com/simdjson/simdjson/commit/449f38e63) and BenchmarkSuite [27950a9](https://github.com/nihilai-collective/benchmarksuite/commit/27950a9) on every build. Sampling and tie rules match those in [*Two Stages, On Demand*](https://github.com/nihilai-collective/jsonifier/blob/main/Two-Stages.md). `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, so its prettified results are a small fraction of its minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

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

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at its rank, and as a loss when another library ranks above it and none is tied with it. A tie is therefore usually at the top, and both ties in this sweep are. Because the verdict is a significance test and not a ratio threshold, a lead of about 1% with tight samples counts as a win: on Linux / GCC, minified Marine IK Small reused (1,435 vs 1,421 MB/s, RSEs 0.15% and 0.04%) is a win of 1.01× against the reflection writer. The test runs on latency, while MB/s credits each library its own output size, so the two can disagree where output sizes differ. Both ties are against the reflection writer. Int64 on macOS / GCC sits within 2% (675 vs Jsonifier's 661 MB/s, RSEs 0.7% and 0.8%). Minified Marine IK Small on Linux / GCC reads as 0.94× in MB/s (1,511 vs 1,415), but the reflection writer emits 4,918 bytes against Jsonifier's 4,604, and the two run at 3,105 and 3,104 ns per call.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M2 Pro numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 49 | 0 | 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 48 | 1 | 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 48 | 1 | 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **247** | **2** | **6** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,126 | 103 | 10.94× |
| Double | 348 | 161 | 2.15× |
| Int64 | 510 | 445 | 1.15× |
| String | 1,440 | 1,043 | 1.38× |
| Uint64 | 523 | 440 | 1.19× |
| Apache Builds (minified) | 12,044 | 4,042 | 2.98× |
| Canada (minified) | 1,171 | 832 | 1.41× |
| Apache Builds (prettified) | 12,107 | 4,566 | 2.65× |
| Canada (prettified) | 2,742 | 1,375 | 1.99× |
| CitmCatalog (minified) | 8,677 | 4,193 | 2.07× |
| CitmCatalog (prettified) | 4,167 | 1,890 | 2.20× |
| Discord (minified) | 11,336 | 4,202 | 2.70× |
| Discord (prettified) | 11,571 | 3,818 | 3.03× |
| Github Events (minified) | 11,692 | 5,237 | 2.23× |
| Google Maps Response (minified) | 9,471 | 3,372 | 2.81× |
| Github Events (prettified) | 11,224 | 3,145 | 3.57× |
| Google Maps Response (prettified) | 14,862 | 5,272 | 2.82× |
| Instruments (minified) | 13,526 | 4,153 | 3.26× |
| Instruments (prettified) | 13,488 | 4,330 | 3.12× |
| Marine IK Reverse (minified) | 726 | 662 | 1.10× |
| Marine IK Reverse (prettified) | 2,172 | 1,700 (n/c) | 1.28× |
| Marine IK (minified) | 712 | 582 | 1.22× |
| Marine IK (prettified) | 2,406 | 1,271 | 1.89× |
| Random (minified) | 3,617 | 3,028 | 1.19× |
| Random (prettified) | 3,869 | 2,152 | 1.80× |
| Twitter (minified) | 3,989 | 4,958 | 0.80× |
| Twitter (prettified) | 2,560 (n/c) | 890 (n/c) | 2.88× |
| Apache Builds Small (minified) | 9,504 | 3,588 | 2.65× |
| Canada Small (minified) | 1,502 | 1,116 | 1.35× |
| Apache Builds Small (prettified) | 10,380 | 3,908 | 2.66× |
| Canada Small (prettified) | 4,472 | 2,921 | 1.53× |
| CitmCatalog Small (minified) | 7,125 | 3,313 | 2.15× |
| CitmCatalog Small (prettified) | 7,975 | 4,031 | 1.98× |
| Discord Small (minified) | 9,578 | 3,714 | 2.58× |
| Discord Small (prettified) | 8,174 | 3,277 | 2.49× |
| Github Events Small (minified) | 9,785 | 4,308 | 2.27× |
| Google Maps Response Small (minified) | 9,064 | 3,407 | 2.66× |
| Github Events Small (prettified) | 7,980 | 3,029 | 2.64× |
| Google Maps Response Small (prettified) | 12,719 | 4,525 | 2.81× |
| Instruments Small (minified) | 10,683 | 3,832 | 2.79× |
| Instruments Small (prettified) | 10,516 | 3,389 | 3.10× |
| Marine IK Reverse Small (minified) | 1,110 | 882 | 1.26× |
| Marine IK Reverse Small (prettified) | 4,148 | 2,998 | 1.38× |
| Marine IK Small (minified) | 1,062 | 916 | 1.16× |
| Marine IK Small (prettified) | 4,371 | 2,973 | 1.47× |
| Random Small (minified) | 8,850 | 3,405 | 2.60× |
| Random Small (prettified) | 11,063 | 3,915 | 2.83× |
| Twitter Small (minified) | 10,632 | 4,895 | 2.17× |
| Twitter Small (prettified) | 7,389 | 2,513 | 2.94× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,531 | 1,815 | 1,416 | 1.39× |
| Prettify | 1,086 (n/c) | 2,887 (n/c) | 629 (n/c) | 0.38× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,240 | 212 | 10.59× |
| Double | 335 | 296 | 1.13× |
| Int64 | 903 | 888 | 1.02× |
| String | 4,238 | 1,952 | 2.17× |
| Uint64 | 942 | 878 | 1.07× |
| Apache Builds (minified) | 16,312 | 5,641 | 2.89× |
| Canada (minified) | 1,915 | 957 | 2.00× |
| Apache Builds (prettified) | 18,919 | 5,236 | 3.61× |
| Canada (prettified) | 5,197 | 3,283 | 1.58× |
| CitmCatalog (minified) | 9,913 | 4,961 | 2.00× |
| CitmCatalog (prettified) | 15,345 | 5,196 | 2.95× |
| Discord (minified) | 14,454 | 4,933 | 2.93× |
| Discord (prettified) | 16,441 | 4,161 | 3.95× |
| Github Events (minified) | 16,414 | 5,173 | 3.17× |
| Google Maps Response (minified) | 11,459 | 3,691 | 3.10× |
| Github Events (prettified) | 16,251 | 4,762 | 3.41× |
| Google Maps Response (prettified) | 15,698 | 5,162 | 3.04× |
| Instruments (minified) | 18,176 | 4,615 | 3.94× |
| Instruments (prettified) | 20,238 | 4,845 | 4.18× |
| Marine IK Reverse (minified) | 1,162 | 641 | 1.81× |
| Marine IK Reverse (prettified) | 5,009 | 3,669 | 1.37× |
| Marine IK (minified) | 1,164 | 634 | 1.84× |
| Marine IK (prettified) | 4,975 | 2,920 | 1.70× |
| Random (minified) | 14,527 | 3,629 | 4.00× |
| Random (prettified) | 16,094 | 3,889 | 4.14× |
| Twitter (minified) | 19,960 | 6,500 | 3.07× |
| Twitter (prettified) | 18,009 | 4,481 | 4.02× |
| Apache Builds Small (minified) | 10,276 | 3,462 | 2.97× |
| Canada Small (minified) | 1,845 | 950 | 1.94× |
| Apache Builds Small (prettified) | 10,116 | 3,584 | 2.82× |
| Canada Small (prettified) | 5,031 | 3,506 | 1.44× |
| CitmCatalog Small (minified) | 7,262 | 2,645 | 2.75× |
| CitmCatalog Small (prettified) | 10,258 | 3,500 | 2.93× |
| Discord Small (minified) | 9,417 | 2,710 | 3.47× |
| Discord Small (prettified) | 11,502 | 2,702 | 4.26× |
| Github Events Small (minified) | 9,584 | 3,927 | 2.44× |
| Google Maps Response Small (minified) | 10,435 | 3,583 | 2.91× |
| Github Events Small (prettified) | 10,347 | 3,511 | 2.95× |
| Google Maps Response Small (prettified) | 15,458 | 4,719 | 3.28× |
| Instruments Small (minified) | 11,979 | 2,288 | 5.24× |
| Instruments Small (prettified) | 14,168 | 2,633 | 5.38× |
| Marine IK Reverse Small (minified) | 1,449 | 766 | 1.89× |
| Marine IK Reverse Small (prettified) | 5,100 | 3,654 | 1.40× |
| Marine IK Small (minified) | 1,440 | 805 | 1.79× |
| Marine IK Small (prettified) | 5,246 | 3,492 | 1.50× |
| Random Small (minified) | 12,510 | 3,345 | 3.74× |
| Random Small (prettified) | 15,245 | 3,866 | 3.94× |
| Twitter Small (minified) | 8,560 | 4,423 | 1.94× |
| Twitter Small (prettified) | 9,249 | 3,185 | 2.90× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,622 | 2,642 | 1,731 | 1.37× |
| Prettify | 4,583 | 3,464 | 2,293 | 1.32× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,722 | 197 | 254 | 6.79× |
| Double | 349 | 322 | 349 | 1.00× |
| Int64 | 954 | 958 | 963 | 0.99× |
| String | 4,292 | 2,129 | 4,056 | 1.06× |
| Uint64 | 950 | 862 | 884 | 1.07× |
| Apache Builds (minified) | 20,141 | 7,804 | 13,229 | 1.52× |
| Canada (minified) | 2,127 | 1,440 | 1,038 | 1.48× |
| Apache Builds (prettified) | 23,551 | 7,253 | 624 | 3.25× |
| Canada (prettified) | 6,041 | 3,253 | 273 | 1.86× |
| CitmCatalog (minified) | 12,441 | 5,897 | 11,306 | 1.10× |
| CitmCatalog (prettified) | 20,066 | 5,715 | 578 | 3.51× |
| Discord (minified) | 15,141 | 6,344 | 13,207 | 1.15× |
| Discord (prettified) | 20,274 | 5,919 | 550 | 3.43× |
| Github Events (minified) | 14,656 | 7,097 | 11,029 | 1.33× |
| Google Maps Response (minified) | 15,522 | 4,488 | 10,201 | 1.52× |
| Github Events (prettified) | 18,483 | 5,385 | 626 | 3.43× |
| Google Maps Response (prettified) | 20,936 | 6,586 | 528 | 3.18× |
| Instruments (minified) | 20,339 | 5,622 | 15,568 | 1.31× |
| Instruments (prettified) | 26,325 | 5,514 | 476 | 4.77× |
| Marine IK Reverse (minified) | 1,289 | 984 | 883 | 1.31× |
| Marine IK Reverse (prettified) | 6,340 | 3,663 | 268 | 1.73× |
| Marine IK (minified) | 1,269 | 958 | 890 | 1.32× |
| Marine IK (prettified) | 6,376 | 3,061 | 388 | 2.08× |
| Random (minified) | 17,629 | 4,577 | 11,425 | 1.54× |
| Random (prettified) | 21,320 | 4,949 | 537 | 4.31× |
| Twitter (minified) | 17,892 | 7,634 | 10,918 | 1.64× |
| Twitter (prettified) | 21,207 | 5,115 | 519 | 4.15× |
| Apache Builds Small (minified) | 11,978 | 4,382 | 7,073 | 1.69× |
| Canada Small (minified) | 2,070 | 1,469 | 1,044 | 1.41× |
| Apache Builds Small (prettified) | 13,055 | 4,059 | 549 | 3.22× |
| Canada Small (prettified) | 5,369 | 3,690 | 410 | 1.46× |
| CitmCatalog Small (minified) | 7,768 | 3,330 | 6,077 | 1.28× |
| CitmCatalog Small (prettified) | 14,925 | 3,012 | 506 | 4.96× |
| Discord Small (minified) | 9,321 | 3,946 | 7,487 | 1.24× |
| Discord Small (prettified) | 14,641 | 3,282 | 517 | 4.46× |
| Github Events Small (minified) | 8,508 | 5,167 | 6,733 | 1.26× |
| Google Maps Response Small (minified) | 12,841 | 3,850 | 8,035 | 1.60× |
| Github Events Small (prettified) | 11,198 | 3,572 | 543 | 3.13× |
| Google Maps Response Small (prettified) | 23,658 | 5,782 | 515 | 4.09× |
| Instruments Small (minified) | 11,880 | 3,868 | 7,863 | 1.51× |
| Instruments Small (prettified) | 21,139 | 3,305 | 524 | 6.40× |
| Marine IK Reverse Small (minified) | 1,271 | 1,230 | 1,527 | 0.83× |
| Marine IK Reverse Small (prettified) | 6,319 | 3,376 | 518 | 1.87× |
| Marine IK Small (minified) | 1,415 | 1,194 | 1,511 | 0.94× |
| Marine IK Small (prettified) | 6,627 | 3,417 | 528 | 1.94× |
| Random Small (minified) | 14,804 | 4,103 | 8,187 | 1.81× |
| Random Small (prettified) | 24,182 | 4,417 | 537 | 5.48× |
| Twitter Small (minified) | 8,871 | 5,390 | 6,551 | 1.35× |
| Twitter Small (prettified) | 11,931 | 3,785 | 554 | 3.15× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,277 | 2,533 | 1,908 | 1.29× |
| Prettify | 4,169 | 3,172 | 2,463 | 1.31× |

### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,934 | 133 | 230 | 8.42× |
| Double | 284 | 221 | 319 | 0.89× |
| Int64 | 661 | 486 | 675 | 0.98× |
| String | 1,210 | 963 | 1,185 | 1.02× |
| Uint64 | 699 | 529 | 720 | 0.97× |
| Apache Builds (minified) | 10,960 | 5,285 | 9,168 | 1.20× |
| Canada (minified) | 2,073 | 1,804 | 807 | 1.15× |
| Apache Builds (prettified) | 13,407 | 4,038 | 350 | 3.32× |
| Canada (prettified) | 7,997 | 3,638 | 296 (n/c) | 2.20× |
| CitmCatalog (minified) | 8,750 | 2,226 | 6,888 | 1.27× |
| CitmCatalog (prettified) | 18,957 | 3,511 | 307 | 5.40× |
| Discord (minified) | 11,741 | 2,673 | 9,237 | 1.27× |
| Discord (prettified) | 12,933 | 2,219 | 329 | 5.83× |
| Github Events (minified) | 13,371 | 3,060 | 9,815 | 1.36× |
| Google Maps Response (minified) | 9,236 | 1,684 | 7,804 (n/c) | 1.18× |
| Github Events (prettified) | 14,731 | 2,697 | 357 | 5.46× |
| Google Maps Response (prettified) | 15,673 | 2,561 | 247 | 6.12× |
| Instruments (minified) | 14,002 | 2,094 | 7,969 | 1.76× |
| Instruments (prettified) | 16,769 | 2,024 | 306 | 8.28× |
| Marine IK Reverse (minified) | 1,431 | 854 | 816 | 1.68× |
| Marine IK Reverse (prettified) | 7,015 | 3,336 | 313 | 2.10× |
| Marine IK (minified) | 1,231 | 872 | 700 | 1.41× |
| Marine IK (prettified) | 7,140 | 2,991 | 317 | 2.39× |
| Random (minified) | 11,504 | 2,184 | 7,670 | 1.50× |
| Random (prettified) | 14,212 | 2,274 | 325 | 6.25× |
| Twitter (minified) | 14,896 | 3,717 | 11,229 | 1.33× |
| Twitter (prettified) | 16,062 | 3,457 | 392 | 4.65× |
| Apache Builds Small (minified) | 8,246 | 2,858 | 6,726 | 1.23× |
| Canada Small (minified) | 2,386 | 1,582 | 873 | 1.51× |
| Apache Builds Small (prettified) | 10,130 | 3,247 | 331 | 3.12× |
| Canada Small (prettified) | 7,324 | 3,003 | 266 | 2.44× |
| CitmCatalog Small (minified) | 8,285 | 1,779 | 7,284 | 1.14× |
| CitmCatalog Small (prettified) | 15,313 (n/c) | 2,086 | 282 | 7.34× |
| Discord Small (minified) | 9,650 | 2,401 | 6,996 | 1.38× |
| Discord Small (prettified) | 11,438 | 2,196 | 334 | 5.21× |
| Github Events Small (minified) | 11,680 | 3,027 | 10,231 | 1.14× |
| Google Maps Response Small (minified) | 7,682 | 1,882 | 6,222 | 1.23× |
| Github Events Small (prettified) | 13,783 | 3,039 | 400 | 4.54× |
| Google Maps Response Small (prettified) | 18,459 | 2,273 | 297 | 8.12× |
| Instruments Small (minified) | 13,240 | 1,644 | 7,058 | 1.88× |
| Instruments Small (prettified) | 15,253 | 2,183 | 319 | 6.99× |
| Marine IK Reverse Small (minified) | 1,987 | 1,082 | 1,662 | 1.20× |
| Marine IK Reverse Small (prettified) | 8,684 | 3,329 | 323 | 2.61× |
| Marine IK Small (minified) | 1,857 | 1,079 | 1,691 | 1.10× |
| Marine IK Small (prettified) | 8,350 | 3,300 | 325 | 2.53× |
| Random Small (minified) | 12,429 | 2,219 | 9,541 | 1.30× |
| Random Small (prettified) | 15,601 | 2,584 | 343 | 6.04× |
| Twitter Small (minified) | 12,594 | 3,516 | 9,366 | 1.34× |
| Twitter Small (prettified) | 14,704 | 3,340 | 421 | 4.40× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,441 | 1,703 | 1,221 | 1.43× |
| Prettify | 3,327 | 2,199 | 1,349 | 1.51× |

### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 888 | 106 | 8.40× |
| Double | 267 | 217 | 1.23× |
| Int64 | 2,094 | 421 | 4.97× |
| String | 1,147 | 934 | 1.23× |
| Uint64 | 2,709 | 464 | 5.84× |
| Apache Builds (minified) | 11,416 | 3,263 | 3.50× |
| Canada (minified) | 2,560 | 1,704 | 1.50× |
| Apache Builds (prettified) | 10,220 | 3,094 | 3.30× |
| Canada (prettified) | 7,016 | 3,323 (n/c) | 2.11× |
| CitmCatalog (minified) | 6,969 | 1,911 | 3.65× |
| CitmCatalog (prettified) | 12,459 | 3,571 | 3.49× |
| Discord (minified) | 11,615 | 2,832 | 4.10× |
| Discord (prettified) | 11,657 | 2,330 | 5.00× |
| Github Events (minified) | 11,217 | 4,052 | 2.77× |
| Google Maps Response (minified) | 9,571 | 2,147 | 4.46× |
| Github Events (prettified) | 13,156 | 3,442 | 3.82× |
| Google Maps Response (prettified) | 13,174 | 3,457 | 3.81× |
| Instruments (minified) | 11,695 | 2,136 | 5.48× |
| Instruments (prettified) | 11,703 | 2,303 | 5.08× |
| Marine IK Reverse (minified) | 1,564 | 774 (n/c) | 2.02× |
| Marine IK Reverse (prettified) | 5,617 | 3,295 | 1.70× |
| Marine IK (minified) | 1,423 | 828 | 1.72× |
| Marine IK (prettified) | 5,177 | 3,010 | 1.72× |
| Random (minified) | 10,132 | 2,513 | 4.03× |
| Random (prettified) | 10,562 | 2,991 | 3.53× |
| Twitter (minified) | 11,977 | 3,698 | 3.24× |
| Twitter (prettified) | 13,976 | 3,203 | 4.36× |
| Apache Builds Small (minified) | 6,105 | 2,409 | 2.53× |
| Canada Small (minified) | 2,593 | 1,120 | 2.31× |
| Apache Builds Small (prettified) | 7,353 | 2,751 | 2.67× |
| Canada Small (prettified) | 6,719 | 3,162 | 2.13× |
| CitmCatalog Small (minified) | 6,173 | 1,627 | 3.79× |
| CitmCatalog Small (prettified) | 8,930 | 2,654 | 3.36× |
| Discord Small (minified) | 11,103 | 2,365 | 4.69× |
| Discord Small (prettified) | 12,200 | 2,341 | 5.21× |
| Github Events Small (minified) | 7,315 | 3,271 | 2.24× |
| Google Maps Response Small (minified) | 9,145 | 2,020 | 4.53× |
| Github Events Small (prettified) | 8,435 | 3,056 | 2.76× |
| Google Maps Response Small (prettified) | 13,197 | 3,301 | 4.00× |
| Instruments Small (minified) | 13,271 | 1,629 | 8.14× |
| Instruments Small (prettified) | 14,403 | 2,217 | 6.50× |
| Marine IK Reverse Small (minified) | 1,424 | 842 | 1.69× |
| Marine IK Reverse Small (prettified) | 5,812 | 3,068 | 1.89× |
| Marine IK Small (minified) | 1,631 | 752 | 2.17× |
| Marine IK Small (prettified) | 4,773 | 2,364 | 2.02× |
| Random Small (minified) | 7,595 | 2,431 | 3.12× |
| Random Small (prettified) | 10,062 | 2,705 | 3.72× |
| Twitter Small (minified) | 10,158 | 3,518 | 2.89× |
| Twitter Small (prettified) | 10,873 | 2,996 | 3.63× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,983 | 1,626 | 1,133 | 1.22× |
| Prettify | 3,005 | 2,246 | 1,388 | 1.34× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 252 of the 255 write tests and loses 3. On Windows / MSVC Glaze wins prettified Marine IK Reverse (2,922 vs 2,625 MB/s, 0.90×). On Linux / GCC the reflection writer wins minified Marine IK Reverse Small (1,545 vs 1,333 MB/s, 0.86×), and on macOS / GCC it wins String (10,584 vs 9,120 MB/s, 0.86×).

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 51 | 51 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 51 | 50 | 0 | 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 51 | 50 | 0 | 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 51 | 51 | 0 | 0 |
| **Total** | **255** | **252** | **0** | **3** |

#### Windows / MSVC 19.52 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,649 | 551 | 2.99× |
| Double | 548 | 429 | 1.28× |
| Int64 | 3,895 | 1,686 | 2.31× |
| String | 8,234 | 3,460 | 2.38× |
| Uint64 | 4,547 | 1,702 | 2.67× |
| Apache Builds (minified) | 11,997 | 4,532 | 2.65× |
| Canada (minified) | 1,419 | 1,143 | 1.24× |
| Apache Builds (prettified) | 12,512 | 4,968 | 2.52× |
| Canada (prettified) | 4,085 | 3,115 | 1.31× |
| CitmCatalog (minified) | 8,772 | 4,583 | 1.91× |
| CitmCatalog (prettified) | 12,998 | 7,081 | 1.84× |
| Discord (minified) | 12,020 | 4,443 | 2.71× |
| Discord (prettified) | 12,306 | 4,403 | 2.79× |
| Github Events (minified) | 12,189 | 6,208 | 1.96× |
| Google Maps Response (minified) | 10,240 | 3,864 | 2.65× |
| Github Events (prettified) | 11,068 | 3,413 | 3.24× |
| Google Maps Response (prettified) | 15,750 | 5,920 | 2.66× |
| Instruments (minified) | 13,987 | 4,882 | 2.87× |
| Instruments (prettified) | 13,799 | 4,788 | 2.88× |
| Marine IK Reverse (minified) | 808 | 723 | 1.12× |
| Marine IK Reverse (prettified) | 2,625 | 2,922 | 0.90× |
| Marine IK (minified) | 798 | 739 | 1.08× |
| Marine IK (prettified) | 3,387 | 3,260 | 1.04× |
| Random (minified) | 11,118 | 3,232 | 3.44× |
| Random (prettified) | 12,694 | 4,353 | 2.92× |
| Twitter (minified) | 15,760 | 5,588 | 2.82× |
| Twitter (prettified) | 13,390 | 3,185 | 4.20× |
| Apache Builds Small (minified) | 11,258 | 4,123 | 2.73× |
| Canada Small (minified) | 1,506 | 1,232 | 1.22× |
| Apache Builds Small (prettified) | 11,605 | 4,513 | 2.57× |
| Canada Small (prettified) | 4,738 | 3,346 | 1.42× |
| CitmCatalog Small (minified) | 7,875 | 4,220 | 1.87× |
| CitmCatalog Small (prettified) | 8,549 | 4,993 | 1.71× |
| Discord Small (minified) | 10,532 | 4,617 | 2.28× |
| Discord Small (prettified) | 8,931 | 3,268 (n/c) | 2.73× |
| Github Events Small (minified) | 12,331 | 5,139 | 2.40× |
| Google Maps Response Small (minified) | 9,369 | 3,803 | 2.46× |
| Github Events Small (prettified) | 11,756 | 4,921 | 2.39× |
| Google Maps Response Small (prettified) | 15,182 | 6,181 | 2.46× |
| Instruments Small (minified) | 12,284 | 4,408 (n/c) | 2.79× |
| Instruments Small (prettified) | 11,345 | 3,638 (n/c) | 3.12× |
| Marine IK Reverse Small (minified) | 1,140 | 977 | 1.17× |
| Marine IK Reverse Small (prettified) | 4,310 | 3,496 | 1.23× |
| Marine IK Small (minified) | 1,100 | 993 | 1.11× |
| Marine IK Small (prettified) | 4,489 | 3,359 | 1.34× |
| Random Small (minified) | 11,528 | 4,095 | 2.81× |
| Random Small (prettified) | 13,552 | 4,642 | 2.92× |
| Twitter Small (minified) | 12,583 | 6,044 | 2.08× |
| Twitter Small (prettified) | 9,437 | 3,291 | 2.87× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,620 | 1,795 | 1,424 | 1.46× |
| Prettify | 3,508 | 2,885 | 1,399 | 1.22× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,670 | 854 | 5.47× |
| Double | 875 | 673 | 1.30× |
| Int64 | 5,090 | 2,120 | 2.40× |
| String | 11,542 | 4,495 | 2.57× |
| Uint64 | 4,145 | 2,177 | 1.90× |
| Apache Builds (minified) | 16,688 | 6,331 | 2.64× |
| Canada (minified) | 1,920 | 999 | 1.92× |
| Apache Builds (prettified) | 18,722 | 5,825 | 3.21× |
| Canada (prettified) | 5,146 | 3,932 | 1.31× |
| CitmCatalog (minified) | 10,013 | 5,700 | 1.76× |
| CitmCatalog (prettified) | 15,027 | 6,945 | 2.16× |
| Discord (minified) | 14,972 | 5,465 | 2.74× |
| Discord (prettified) | 16,476 | 4,447 | 3.70× |
| Github Events (minified) | 16,775 | 6,023 | 2.79× |
| Google Maps Response (minified) | 12,053 | 4,277 | 2.82× |
| Github Events (prettified) | 16,803 | 4,997 | 3.36× |
| Google Maps Response (prettified) | 16,316 | 5,631 | 2.90× |
| Instruments (minified) | 18,665 | 5,017 | 3.72× |
| Instruments (prettified) | 20,459 | 5,572 | 3.67× |
| Marine IK Reverse (minified) | 1,163 | 640 | 1.82× |
| Marine IK Reverse (prettified) | 4,958 | 4,031 | 1.23× |
| Marine IK (minified) | 1,173 | 658 | 1.78× |
| Marine IK (prettified) | 4,983 | 4,075 | 1.22× |
| Random (minified) | 14,488 | 3,940 | 3.68× |
| Random (prettified) | 16,091 | 4,469 | 3.60× |
| Twitter (minified) | 19,862 | 7,120 | 2.79× |
| Twitter (prettified) | 18,004 | 5,433 | 3.31× |
| Apache Builds Small (minified) | 12,162 | 3,851 | 3.16× |
| Canada Small (minified) | 1,905 | 1,007 | 1.89× |
| Apache Builds Small (prettified) | 13,179 | 3,850 | 3.42× |
| Canada Small (prettified) | 5,362 | 3,925 | 1.37× |
| CitmCatalog Small (minified) | 8,307 | 3,066 | 2.71× |
| CitmCatalog Small (prettified) | 11,219 | 3,778 | 2.97× |
| Discord Small (minified) | 11,844 | 3,203 | 3.70× |
| Discord Small (prettified) | 12,636 | 2,918 | 4.33× |
| Github Events Small (minified) | 12,452 | 4,233 | 2.94× |
| Google Maps Response Small (minified) | 11,403 | 4,073 | 2.80× |
| Github Events Small (prettified) | 12,420 | 3,862 | 3.22× |
| Google Maps Response Small (prettified) | 17,314 | 5,441 | 3.18× |
| Instruments Small (minified) | 14,184 | 2,591 | 5.47× |
| Instruments Small (prettified) | 15,912 | 2,900 | 5.49× |
| Marine IK Reverse Small (minified) | 1,492 | 824 | 1.81× |
| Marine IK Reverse Small (prettified) | 5,097 | 3,951 | 1.29× |
| Marine IK Small (minified) | 1,482 | 822 | 1.80× |
| Marine IK Small (prettified) | 5,328 | 3,922 | 1.36× |
| Random Small (minified) | 14,416 | 3,785 | 3.81× |
| Random Small (prettified) | 17,104 | 4,618 | 3.70× |
| Twitter Small (minified) | 10,120 | 4,521 | 2.24× |
| Twitter Small (prettified) | 10,559 | 3,700 | 2.85× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,675 | 2,638 | 1,702 | 1.39× |
| Prettify | 4,534 | 3,499 | 2,275 | 1.30× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,245 | 678 | 2,943 | 1.10× |
| Double | 870 | 707 | 636 | 1.23× |
| Int64 | 5,290 | 2,699 | 4,007 | 1.32× |
| String | 10,993 | 5,897 | 9,207 | 1.19× |
| Uint64 | 6,289 | 2,597 | 4,134 | 1.52× |
| Apache Builds (minified) | 20,493 | 9,390 | 13,634 | 1.50× |
| Canada (minified) | 2,126 | 1,531 | 1,039 | 1.39× |
| Apache Builds (prettified) | 23,429 | 8,179 | 617 | 2.86× |
| Canada (prettified) | 6,081 | 3,898 | 392 | 1.56× |
| CitmCatalog (minified) | 12,538 | 7,191 | 11,415 | 1.10× |
| CitmCatalog (prettified) | 20,213 | 7,807 | 563 | 2.59× |
| Discord (minified) | 15,363 | 7,148 | 13,340 | 1.15× |
| Discord (prettified) | 20,592 | 7,185 | 558 | 2.87× |
| Github Events (minified) | 15,145 | 8,622 | 11,131 | 1.36× |
| Google Maps Response (minified) | 16,910 | 5,125 | 10,412 | 1.62× |
| Github Events (prettified) | 18,256 | 5,989 | 640 | 3.05× |
| Google Maps Response (prettified) | 21,680 | 7,381 | 533 | 2.94× |
| Instruments (minified) | 20,608 | 6,465 | 15,160 | 1.36× |
| Instruments (prettified) | 26,066 | 6,271 | 467 | 4.16× |
| Marine IK Reverse (minified) | 1,288 | 985 | 881 | 1.31× |
| Marine IK Reverse (prettified) | 6,300 | 4,000 | 303 | 1.58× |
| Marine IK (minified) | 1,273 | 1,004 | 881 | 1.27× |
| Marine IK (prettified) | 6,309 | 4,075 | 335 | 1.55× |
| Random (minified) | 17,697 | 5,193 | 11,341 | 1.56× |
| Random (prettified) | 21,829 | 5,723 | 542 | 3.81× |
| Twitter (minified) | 17,668 | 8,616 | 10,857 | 1.63× |
| Twitter (prettified) | 21,246 | 6,604 | 520 | 3.22× |
| Apache Builds Small (minified) | 13,390 | 5,255 | 8,142 | 1.64× |
| Canada Small (minified) | 2,107 | 1,614 | 1,050 | 1.31× |
| Apache Builds Small (prettified) | 17,968 | 4,511 | 542 | 3.98× |
| Canada Small (prettified) | 6,566 | 4,115 | 410 | 1.60× |
| CitmCatalog Small (minified) | 8,296 | 4,010 | 6,551 | 1.27× |
| CitmCatalog Small (prettified) | 16,508 | 3,397 | 506 | 4.86× |
| Discord Small (minified) | 10,617 | 4,610 | 8,368 | 1.27× |
| Discord Small (prettified) | 15,624 | 3,877 | 515 | 4.03× |
| Github Events Small (minified) | 10,080 | 5,923 | 7,587 | 1.33× |
| Google Maps Response Small (minified) | 14,219 | 4,269 | 8,585 | 1.66× |
| Github Events Small (prettified) | 13,922 | 4,481 | 530 | 3.11× |
| Google Maps Response Small (prettified) | 29,079 | 7,002 | 514 | 4.15× |
| Instruments Small (minified) | 13,732 | 3,681 | 8,214 | 1.67× |
| Instruments Small (prettified) | 23,764 | 3,454 | 523 | 6.88× |
| Marine IK Reverse Small (minified) | 1,333 | 1,345 | 1,545 | 0.86× |
| Marine IK Reverse Small (prettified) | 6,435 | 3,439 | 520 | 1.87× |
| Marine IK Small (minified) | 1,435 | 1,206 | 1,421 | 1.01× |
| Marine IK Small (prettified) | 6,722 | 3,593 | 531 | 1.87× |
| Random Small (minified) | 16,403 | 4,748 | 8,821 | 1.86× |
| Random Small (prettified) | 27,620 | 5,363 | 537 | 5.15× |
| Twitter Small (minified) | 10,079 | 6,235 | 7,418 | 1.36× |
| Twitter Small (prettified) | 14,192 | 4,186 | 553 | 3.39× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,314 | 2,530 | 1,876 | 1.31× |
| Prettify | 4,206 | 3,168 | 2,473 | 1.33× |

#### macOS / GCC 16.2 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,603 | 847 | 2,800 | 1.29× |
| Double | 925 | 708 | 645 | 1.31× |
| Int64 | 4,612 | 2,634 | 2,832 | 1.63× |
| String | 9,120 | 5,134 | 10,584 | 0.86× |
| Uint64 | 5,072 | 2,256 | 3,059 | 1.66× |
| Apache Builds (minified) | 13,379 | 6,652 | 10,014 | 1.34× |
| Canada (minified) | 2,158 | 1,570 | 857 | 1.37× |
| Apache Builds (prettified) | 14,400 | 4,848 | 318 | 2.97× |
| Canada (prettified) | 7,972 | 4,120 | 312 | 1.94× |
| CitmCatalog (minified) | 8,782 | 2,246 | 6,355 | 1.38× |
| CitmCatalog (prettified) | 19,078 | 4,322 | 319 (n/c) | 4.41× |
| Discord (minified) | 12,576 | 3,029 | 9,620 | 1.31× |
| Discord (prettified) | 13,640 | 2,578 | 306 | 5.29× |
| Github Events (minified) | 14,337 | 3,989 | 10,404 | 1.38× |
| Google Maps Response (minified) | 10,178 | 1,708 | 7,623 | 1.34× |
| Github Events (prettified) | 12,985 | 3,548 | 396 | 3.66× |
| Google Maps Response (prettified) | 21,692 | 3,320 | 319 | 6.53× |
| Instruments (minified) | 12,067 | 2,202 | 6,990 | 1.73× |
| Instruments (prettified) | 16,804 | 2,491 | 304 | 6.74× |
| Marine IK Reverse (minified) | 1,434 | 942 | 718 | 1.52× |
| Marine IK Reverse (prettified) | 7,032 | 3,461 | 313 | 2.03× |
| Marine IK (minified) | 1,365 | 843 | 733 | 1.62× |
| Marine IK (prettified) | 7,134 | 3,443 | 316 | 2.07× |
| Random (minified) | 11,431 | 2,365 | 7,767 | 1.47× |
| Random (prettified) | 14,904 | 2,736 | 317 | 5.45× |
| Twitter (minified) | 14,858 | 4,156 | 11,182 | 1.33× |
| Twitter (prettified) | 15,665 | 4,249 | 392 | 3.69× |
| Apache Builds Small (minified) | 10,212 | 3,070 | 6,760 | 1.51× |
| Canada Small (minified) | 2,408 | 1,911 | 868 | 1.26× |
| Apache Builds Small (prettified) | 11,150 | 4,133 | 325 | 2.70× |
| Canada Small (prettified) | 7,423 | 3,417 | 287 | 2.17× |
| CitmCatalog Small (minified) | 9,208 | 1,953 | 7,504 | 1.23× |
| CitmCatalog Small (prettified) | 14,638 | 2,491 | 312 | 5.88× |
| Discord Small (minified) | 11,673 | 2,396 | 8,510 | 1.37× |
| Discord Small (prettified) | 14,558 | 2,127 | 336 | 6.84× |
| Github Events Small (minified) | 15,150 | 3,721 | 11,096 | 1.37× |
| Google Maps Response Small (minified) | 9,732 | 1,739 | 8,829 | 1.10× |
| Github Events Small (prettified) | 16,739 | 3,387 | 401 | 4.94× |
| Google Maps Response Small (prettified) | 17,679 | 3,190 | 320 | 5.54× |
| Instruments Small (minified) | 15,859 | 2,039 | 8,740 | 1.81× |
| Instruments Small (prettified) | 18,172 | 2,397 | 335 | 7.58× |
| Marine IK Reverse Small (minified) | 2,025 | 1,159 | 1,680 | 1.21× |
| Marine IK Reverse Small (prettified) | 8,329 | 3,743 | 323 | 2.23× |
| Marine IK Small (minified) | 2,037 | 1,170 | 1,726 | 1.18× |
| Marine IK Small (prettified) | 9,365 | 3,519 | 323 | 2.66× |
| Random Small (minified) | 13,414 | 2,555 | 9,007 | 1.49× |
| Random Small (prettified) | 16,485 | 2,989 | 346 | 5.52× |
| Twitter Small (minified) | 14,679 | 3,978 | 10,416 | 1.41× |
| Twitter Small (prettified) | 17,861 | 3,993 | 421 | 4.47× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,479 | 1,716 | 1,232 | 1.45× |
| Prettify | 3,332 | 2,204 | 1,385 | 1.51× |

#### macOS / Clang 23.1 (Apple M2 Pro, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,051 | 655 | 1.60× |
| Double | 689 | 565 | 1.22× |
| Int64 | 3,281 | 1,580 | 2.08× |
| String | 7,565 | 4,152 | 1.82× |
| Uint64 | 3,295 | 2,150 | 1.53× |
| Apache Builds (minified) | 11,295 | 3,985 | 2.83× |
| Canada (minified) | 2,791 | 1,870 | 1.49× |
| Apache Builds (prettified) | 10,614 | 3,604 | 2.95× |
| Canada (prettified) | 6,228 | 3,641 | 1.71× |
| CitmCatalog (minified) | 6,729 | 1,945 | 3.46× |
| CitmCatalog (prettified) | 12,839 | 4,247 | 3.02× |
| Discord (minified) | 11,162 | 3,655 | 3.05× |
| Discord (prettified) | 12,783 | 3,170 | 4.03× |
| Github Events (minified) | 15,070 | 4,796 | 3.14× |
| Google Maps Response (minified) | 9,657 | 2,517 | 3.84× |
| Github Events (prettified) | 11,567 | 4,658 | 2.48× |
| Google Maps Response (prettified) | 14,046 | 3,757 | 3.74× |
| Instruments (minified) | 10,904 | 2,200 | 4.96× |
| Instruments (prettified) | 12,606 | 2,956 | 4.27× |
| Marine IK Reverse (minified) | 1,562 | 928 | 1.68× |
| Marine IK Reverse (prettified) | 5,609 | 3,468 | 1.62× |
| Marine IK (minified) | 1,428 | 842 | 1.70× |
| Marine IK (prettified) | 5,187 | 3,162 | 1.64× |
| Random (minified) | 10,176 | 2,708 | 3.76× |
| Random (prettified) | 12,362 | 3,299 | 3.75× |
| Twitter (minified) | 13,509 | 4,313 | 3.13× |
| Twitter (prettified) | 11,955 | 3,738 | 3.20× |
| Apache Builds Small (minified) | 7,128 | 2,783 | 2.56× |
| Canada Small (minified) | 2,895 | 1,408 | 2.06× |
| Apache Builds Small (prettified) | 8,218 | 2,577 | 3.19× |
| Canada Small (prettified) | 7,228 | 3,304 | 2.19× |
| CitmCatalog Small (minified) | 6,930 | 1,483 | 4.67× |
| CitmCatalog Small (prettified) | 9,232 | 3,021 | 3.06× |
| Discord Small (minified) | 9,169 | 3,743 | 2.45× |
| Discord Small (prettified) | 12,854 | 2,760 | 4.66× |
| Github Events Small (minified) | 11,000 | 4,361 | 2.52× |
| Google Maps Response Small (minified) | 7,102 | 2,583 | 2.75× |
| Github Events Small (prettified) | 10,508 | 3,742 | 2.81× |
| Google Maps Response Small (prettified) | 13,902 | 3,902 | 3.56× |
| Instruments Small (minified) | 14,313 | 2,177 | 6.58× |
| Instruments Small (prettified) | 12,034 | 3,459 | 3.48× |
| Marine IK Reverse Small (minified) | 1,474 | 940 | 1.57× |
| Marine IK Reverse Small (prettified) | 5,589 | 3,431 | 1.63× |
| Marine IK Small (minified) | 1,642 | 1,040 | 1.58× |
| Marine IK Small (prettified) | 4,714 | 3,181 | 1.48× |
| Random Small (minified) | 9,008 | 2,876 | 3.13× |
| Random Small (prettified) | 12,430 | 3,505 | 3.55× |
| Twitter Small (minified) | 12,303 | 3,902 | 3.15× |
| Twitter Small (prettified) | 14,458 | 4,100 | 3.53× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,003 | 1,427 | 1,111 | 1.40× |
| Prettify | 2,812 | 2,222 | 1,381 | 1.27× |

### Losses and ties

The six freshly allocated losses:

- **Twitter (minified), Windows / MSVC.** Glaze runs 4,958 vs Jsonifier's 3,989 MB/s (0.80×). Reused, Jsonifier wins by 2.82× (15,760 vs 5,588).
- **Prettify, Windows / MSVC.** Glaze runs 2,887 vs 1,086 MB/s (0.38×), with simdjson On Demand at 629. None of the three converged, and Jsonifier's RSE is 17.9%. Reused, Jsonifier wins by 1.22× (3,508 vs 2,885).
- **Int64, Linux / GCC.** The reflection writer runs 963 vs 954 MB/s (0.99×), with Glaze also ahead at 958. Reused, Jsonifier wins by 1.32× over the reflection writer (5,290 vs 4,007).
- **Marine IK Reverse Small (minified), Linux / GCC.** The reflection writer runs 1,527 vs 1,271 MB/s (0.83×), with Glaze at 1,230. It also wins reused, 1,545 vs 1,333 MB/s (0.86×), with Glaze at 1,345.
- **Double, macOS / GCC.** The reflection writer runs 319 vs 284 MB/s (0.89×), with Glaze at 221. Reused, Jsonifier wins by 1.31× over Glaze (925 vs 708), with the reflection writer at 645.
- **Uint64, macOS / GCC.** The reflection writer runs 720 vs 699 MB/s (0.97×), with Glaze at 529. Reused, Jsonifier wins by 1.66× (5,072 vs 3,059).

The two freshly allocated ties are both against the reflection writer. Minified Marine IK Small on Linux / GCC (1,511 vs 1,415 MB/s, Glaze at 1,194) runs at the same latency for both, and Jsonifier wins it reused by 1.01× (1,435 vs 1,421). Int64 on macOS / GCC (675 vs 661 MB/s, Glaze at 486) Jsonifier wins reused by 1.63× (4,612 vs 2,832).

Two reused non-wins have no freshly allocated counterpart. Prettified Marine IK Reverse on Windows / MSVC wins freshly allocated by 1.28× (2,172 vs Glaze's 1,700, which did not converge) and loses to Glaze reused (2,625 vs 2,922 MB/s). String on macOS / GCC wins freshly allocated (1,210 MB/s against the reflection writer's 1,185 and Glaze's 963) and loses to the reflection writer reused (9,120 vs 10,584 MB/s).

Every full-size document is ahead of the reflection writer on both GCC builds in both runs. Freshly allocated, minified documents lead it by 1.10–2.57× and prettified documents by 16–63×; reused, by 1.10–2.52× and 16–68×. The Small documents mostly follow: on the two GCC builds Jsonifier beats the reflection writer on 42 of the 44 Small document tests freshly allocated and 43 reused. The non-wins are minified Marine IK Reverse Small on Linux / GCC in both runs and minified Marine IK Small on Linux / GCC freshly allocated, a tie. Why the reflection writer edges ahead on these has not been profiled.

**Reformatting.** Jsonifier wins Minify on all five builds in both runs, by 1.22–1.43× freshly allocated and 1.31–1.46× reused. It wins Prettify on all five reused, by 1.22–1.51×, and on four of five freshly allocated, by 1.31–1.51×; the exception is Windows / MSVC, where Glaze runs 2,887 vs 1,086 MB/s (0.38×) and none of the three libraries converged. Glaze is the next-fastest library in every case.

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

The gap is widest freshly allocated. Freshly allocated, Jsonifier runs String at 1,210 MB/s on macOS / GCC (1.26× Glaze's 963, ahead of the reflection writer's 1,185) and 1,147 MB/s on macOS / Clang (1.23× Glaze's 934). The AVX2 builds run the same test at 1,440–4,292 MB/s and lead Glaze by 1.38× (Windows / MSVC), 2.17× (Linux / Clang) and 2.02× (Linux / GCC). With the string reused the M2 Pro lead over Glaze is 1.82× on macOS / Clang (7,565 vs 4,152 MB/s) and 1.78× on macOS / GCC (9,120 vs 5,134), against 1.86–2.57× on the AVX2 builds. On macOS / GCC the reflection writer wins String reused, at 10,584 MB/s.

So part of the M2 Pro shortfall sits in the allocation and release of the output string, which the reused run removes: freshly allocated the M2 Pro leads over Glaze are 1.23–1.26× against 1.38–2.17× on AVX2, and reused they rise to 1.78–1.82×, just under the AVX2 range of 1.86–2.57×. That is an inference from the two runs, not a profile. Two features of the general loop are candidates for what remains:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

Freshly allocated, on macOS / GCC only: the reflection writer runs 319 vs 284 MB/s there (0.89×), a significant loss. On Linux / GCC the two read 349 MB/s each, but the reflection writer's 1,997-byte output against Jsonifier's 1,811 hides the gap: Jsonifier takes 4,948 ns per call against 5,457, and the test counts it a win. Reused, Jsonifier leads it on both: 870 vs 636 MB/s on Linux / GCC (1.37×) and 925 vs 645 MB/s on macOS / GCC (1.43×). Jsonifier beats Glaze on Double on all five builds in both runs.

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
