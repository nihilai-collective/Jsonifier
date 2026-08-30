# UTF-8 Validation

Jsonifier ships with a high-performance SIMD UTF-8 validator that catches malformed byte sequences before they enter your program. It's available three ways: automatically inside `validateJson`, opt-in during parsing via `parse_options{ .validateUtf8 = true }`, and as a standalone `jsonifier::validateUtf8` function for validating arbitrary byte buffers.

## Why UTF-8 Validation Matters

Malformed UTF-8 is a real-world security and correctness issue:

- **Overlong encodings** can smuggle characters past filters (e.g. representing `/` as a multi-byte sequence to bypass path checks)
- **Invalid continuation sequences** produce corrupted output that later stages misinterpret
- **Encodings that decode to surrogate code points** (U+D800–U+DFFF) violate Unicode and can crash downstream text handlers
- **Truncated multi-byte sequences** at the end of an input can cause silent data loss or read-past-buffer bugs

If your JSON is coming from an untrusted source — user uploads, external APIs, network messages — UTF-8 validation is a cheap safety net compared to what malformed sequences can do downstream.

## Three Ways to Use It

### Automatic (inside `validateJson`)

`validateJson` enables UTF-8 validation on string content internally. If you validate a JSON document with `validateJson`, string content is checked for UTF-8 correctness automatically — you don't opt in.

```cpp
bool valid = parser.validateJson(json);
```

### Opt-In (during `parseJson`)

When you're extracting typed data, turn UTF-8 validation on via `parse_options`:

```cpp
parser.parseJson<jsonifier::parse_options{ .validateUtf8 = true }>(data, json);
```

This adds the validator to the string-parsing SIMD path. Non-string JSON bytes (structural characters, numbers, keywords) are validated against JSON grammar during normal parsing, which implicitly guarantees they're 7-bit ASCII — so UTF-8 validation only needs to cover string content. If any string in the document contains malformed UTF-8, `parseJson` returns `false` and the error lands in `parser.getErrors()`.

### Standalone (arbitrary byte buffers)

For validating byte sequences that aren't JSON at all — file contents, network buffers, existing string data, protocol payloads — call `jsonifier::validateUtf8` directly:

```cpp
#include <jsonifier>

const uint8_t* buffer = /* your bytes */;
uint64_t length = /* buffer length */;

bool valid = jsonifier::validateUtf8(buffer, length);
```

Unlike the JSON-integrated version (which only validates string content because the parser guarantees non-string bytes are ASCII), the standalone function **validates the entire byte range** as UTF-8. Every byte in the input is checked. No parser instance needed, no JSON assumptions.

## What UTF-8 Validation Catches (vs. Doesn't)

Understanding what changes when the `validateUtf8` flag is on vs. off during `parseJson` is important — because the parser catches a lot of malformed JSON even with UTF-8 validation off, and only certain classes of error require the validator.

**Caught with the flag OFF (always, as part of normal parsing):**

- JSON structural violations (missing brackets, commas, colons, etc.)
- Invalid escape sequences (`\z`, malformed `\u` sequences, etc.)
- Unescaped control characters (raw U+0000–U+001F inside strings)
- Non-ASCII bytes outside of string content (JSON grammar requires all structural bytes to be 7-bit ASCII)

**Only caught with the flag ON:**

- Invalid UTF-8 lead-byte / continuation-byte patterns inside string content
- Overlong encodings (e.g. encoding `/` as `0xC0 0xAF` instead of `0x2F`)
- Continuation bytes without a preceding lead byte
- Lead bytes without enough following continuation bytes
- Encodings that decode to surrogate code points (U+D800–U+DFFF)
- Truncated multi-byte sequences at buffer end

If the flag is off and a JSON string contains malformed UTF-8, the parse succeeds and the malformed bytes end up in your destination `std::string` verbatim. Whether that's acceptable depends on what your downstream code does with the string.

## Performance

When integrated into JSON parsing, UTF-8 validation scopes to **string content only**. Non-string bytes are validated against JSON grammar during normal parsing, which implicitly guarantees they're 7-bit ASCII — so there's no additional UTF-8 work outside string values.

This means UTF-8 validation cost during parsing is proportional to **how much string content** is in the document, not the total document size. A JSON document that's mostly numbers, booleans, and structural characters pays almost nothing even with the flag on. A document that's mostly long string values pays more.

When running standalone via `jsonifier::validateUtf8`, every byte in the input is validated — cost is proportional to the full buffer size.

In both modes, the validator has an ASCII fast-path: if all bytes in a SIMD register are in the ASCII range (high bit clear), the register is validated in a single comparison and the rest of the validation work is skipped. Real-world English or code-heavy content sees near-zero overhead. Multi-byte-heavy content (CJK, emoji, non-Latin scripts) pays the full validation cost, but the SIMD validator's throughput on multi-byte content is still measured in gigabytes per second on modern hardware.

## Under the Hood: Cross-Width Register Validation

Jsonifier's UTF-8 validator is based on the [Keiser-Lemire algorithm](https://arxiv.org/abs/2010.03090) — a lookup-table approach where each byte's role (lead, continuation, illegal patterns) is classified via SIMD table lookups, and errors are accumulated into an error register that's OR'd across the whole input. If the error register is non-zero at the end, the input is invalid.

The core problem in SIMD UTF-8 validation is that codepoints can be up to 4 bytes long, and a codepoint can start near the end of one SIMD register and continue into the next. Naive per-register validation misses these boundary-crossing codepoints or produces false errors when it can't see the lead byte from the previous register.

Jsonifier's validator solves this with a `utf8_validation_state` struct that carries state across register loads:

- **`previousInputBlock`** — the previous register's bytes, kept so lookback operations (`prev1`, `prev2`, `prev3`) can reach across the boundary
- **`previousIncomplete`** — a persistent bit-tracked state indicating whether the previous register ended with an incomplete codepoint (a lead byte in the last 1–3 positions with continuations expected in the next register)
- **`error`** — an accumulated error register, OR'd across all validated blocks

For each register, the validator:

1. **Fast-path check** — if `nonAsciiPresent` is false (all high bits clear), just update `previousIncomplete` state and skip the rest.
2. **Cross-boundary lookback** — concatenate `previousInputBlock` with the current register to form `prev1`, so continuation bytes at the start of the current register can see the lead byte at the end of the previous one.
3. **Byte classification** — classify each byte's role using the Keiser-Lemire lookup tables, with the boundary handled by the concat above.
4. **Multi-byte length checks** — verify that 3-byte and 4-byte lead bytes are followed by the correct number of continuation bytes, using `prev2` and `prev3` position checks.
5. **State update** — save the current register as `previousInputBlock`, and update `previousIncomplete` based on whether the last 4 bytes of this register indicate a codepoint spilling into the next register.
6. **Finalize** — when the buffer ends, OR `previousIncomplete` into `error`, since ending with an unfinished codepoint is itself invalid.

The whole design is templated on the SIMD width, so the same `utf8_validation_state` shape works uniformly across SSE2 (16-byte registers), AVX2 (32-byte), AVX-512 (64-byte), and NEON (16-byte) — only the register operations differ per architecture. This is the "cross-SIMD-width validation state carry" that lets Jsonifier deploy the same validator logic across every supported target with no algorithmic changes.

For arbitrary-length strings that don't fit exactly into a whole number of SIMD registers, the tail is handled by copying the remaining bytes into a zero-padded temporary register-sized buffer and validating that. Zero bytes are valid UTF-8, so the padding doesn't introduce false errors.

## Full Example

```cpp
#include <jsonifier>
#include <iostream>

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string json_valid = R"({"name":"Concert 🎵","id":42})";
    std::string json_bad_utf8;
    json_bad_utf8 = R"({"name":")";
    json_bad_utf8.push_back(static_cast<char>(0xC0));
    json_bad_utf8.push_back(static_cast<char>(0xAF));
    json_bad_utf8 += R"(","id":42})";

    struct event { std::string name; int64_t id; };
    event e;

    if (parser.parseJson<jsonifier::parse_options{ .validateUtf8 = true }>(e, json_valid)) {
        std::cout << "Valid JSON parsed: " << e.name << std::endl;
    }

    if (!parser.parseJson<jsonifier::parse_options{ .validateUtf8 = true }>(e, json_bad_utf8)) {
        std::cout << "Malformed UTF-8 caught:" << std::endl;
        for (auto& err : parser.getErrors()) {
            std::cout << err << std::endl;
        }
    }

    const uint8_t* raw_bytes = std::bit_cast<const uint8_t*>(json_valid.data());
    bool raw_is_valid = jsonifier::validateUtf8(raw_bytes, json_valid.size());
    std::cout << "Raw byte buffer valid UTF-8: " << std::boolalpha << raw_is_valid << std::endl;

    return 0;
}
```

The `event` registration is omitted from this snippet — see [Reflection](Reflection.md) for the setup.

## When to Enable UTF-8 Validation

**Untrusted input:** always on. The cost is small on ASCII-heavy content and the safety benefit is significant.

**Semi-trusted input:** judgment call. Data from your own services that you know is well-formed may not need the validation cost, but consider whether "your own services" includes anything that transits systems you don't fully control.

**Trusted internal-only input:** off is fine. If you're parsing your own serialized output that was already produced by Jsonifier or another correctness-guaranteeing library, the validation is redundant.

## What's Next

- **[Validating](Validating.md)** — the pure structural-check function that includes UTF-8 validation automatically
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the full `parse_options` reference including the `validateUtf8` flag
- **[Error Handling](Errors.md)** — how UTF-8 errors are reported through `parser.getErrors()`

---