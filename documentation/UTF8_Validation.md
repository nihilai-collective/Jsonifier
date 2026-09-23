# UTF-8 Validation

Jsonifier ships with a high-performance SIMD UTF-8 validator that catches malformed byte sequences before they enter your program. It runs automatically on every string during `parseJson` and inside `validateJson`, and it is also available as a standalone `jsonifier::validateUtf8` function for validating arbitrary byte buffers. There is no option to turn it off.

## Why UTF-8 Validation Matters

Malformed UTF-8 is a real-world security and correctness issue:

- **Overlong encodings** can smuggle characters past filters (e.g. representing `/` as a multi-byte sequence to bypass path checks)
- **Invalid continuation sequences** produce corrupted output that later stages misinterpret
- **Encodings that decode to surrogate code points** (U+D800–U+DFFF) violate Unicode and can crash downstream text handlers
- **Truncated multi-byte sequences** at the end of an input can cause silent data loss or read-past-buffer bugs

Jsonifier treats UTF-8 validation as part of what it means to parse JSON correctly, so it is always on.

## Where It Runs

### Inside `parseJson`

Every string the parser materializes is validated as it is unescaped:

```cpp
parser.parseJson(data, json);
```

The validator is fused into the string-parsing SIMD path. Non-string JSON bytes (structural characters, numbers, keywords) are validated against JSON grammar during normal parsing, which implicitly guarantees they're 7-bit ASCII — so UTF-8 validation only needs to cover string content. If any string in the document contains malformed UTF-8, `parseJson` returns `false` and the error lands in `parser.getErrors()`.

### Inside `validateJson`

`validateJson` runs the same string validation, so a structurally valid document with malformed UTF-8 in a string is reported as invalid:

```cpp
bool valid = parser.validateJson(json);
```

### Standalone (arbitrary byte buffers)

For validating byte sequences that aren't JSON at all — file contents, network buffers, existing string data, protocol payloads — call `jsonifier::validateUtf8` directly:

```cpp
#include <jsonifier>

const uint8_t* buffer = /* your bytes */;
uint64_t length = /* buffer length */;

bool valid = jsonifier::validateUtf8(buffer, length);
```

Unlike the JSON-integrated version (which only validates string content because the parser guarantees non-string bytes are ASCII), the standalone function **validates the entire byte range** as UTF-8. Every byte in the input is checked. No parser instance needed, no JSON assumptions.

## What Gets Caught, and by What

Most malformed JSON is rejected by the parser's grammar checks; the UTF-8 validator covers what grammar alone cannot.

**Caught by normal parsing:**

- JSON structural violations (missing brackets, commas, colons, etc.)
- Invalid escape sequences (`\z`, malformed `\u` sequences, etc.)
- Unescaped control characters (raw U+0000–U+001F inside strings)
- Non-ASCII bytes outside of string content (JSON grammar requires all structural bytes to be 7-bit ASCII)

**Caught by the UTF-8 validator:**

- Invalid UTF-8 lead-byte / continuation-byte patterns inside string content
- Overlong encodings (e.g. encoding `/` as `0xC0 0xAF` instead of `0x2F`)
- Continuation bytes without a preceding lead byte
- Lead bytes without enough following continuation bytes
- Encodings that decode to surrogate code points (U+D800–U+DFFF)
- Truncated multi-byte sequences at buffer end

## Performance

When integrated into JSON parsing, UTF-8 validation scopes to **string content only**. Non-string bytes are validated against JSON grammar during normal parsing, which implicitly guarantees they're 7-bit ASCII — so there's no additional UTF-8 work outside string values.

This means UTF-8 validation cost during parsing is proportional to **how much string content** is in the document, not the total document size. A JSON document that's mostly numbers, booleans, and structural characters pays almost nothing for it. A document that's mostly long string values pays more.

When running standalone via `jsonifier::validateUtf8`, every byte in the input is validated — cost is proportional to the full buffer size.

In both modes, the validator has an ASCII fast-path: if all bytes in a SIMD register are in the ASCII range (high bit clear), the register is validated in a single comparison and the rest of the validation work is skipped. Real-world English or code-heavy content sees near-zero overhead. Multi-byte-heavy content (CJK, emoji, non-Latin scripts) pays the full validation cost, but the SIMD validator's throughput on multi-byte content is still measured in gigabytes per second on modern hardware.

## Under the Hood: Cross-Width Register Validation

Jsonifier's UTF-8 validator is based on the [Keiser-Lemire algorithm](https://arxiv.org/abs/2010.03090) — a lookup-table approach where each byte's role (lead, continuation, illegal patterns) is classified via SIMD table lookups, and errors are accumulated into an error register that's OR'd across the whole input. If the error register is non-zero at the end, the input is invalid.

The core problem in SIMD UTF-8 validation is that codepoints can be up to 4 bytes long, and a codepoint can start near the end of one SIMD register and continue into the next. Naive per-register validation misses these boundary-crossing codepoints or produces false errors when it can't see the lead byte from the previous register.

Jsonifier's validators solve this by carrying three registers from one load to the next:

- **`prevInput`** — the previous register's bytes, so the byte-shifted lookbacks (`opPrev<15>`, `opPrev<14>`, `opPrev<13>`: one, two, and three bytes back) can reach across the boundary
- **`incompleteRegister`** (`prevIncomplete` in the block validator) — nonzero when the previous register ended with an unfinished codepoint, i.e. a lead byte in its last 1–3 positions still waiting for continuation bytes
- **`error`** — an accumulated error register, OR'd across everything validated so far

For each register, the validator:

1. **ASCII fast path** — if every byte has its high bit clear, OR the pending incomplete carry into `error` (an unfinished codepoint followed by ASCII is invalid), save the register as `prevInput`, clear the carry, and skip the rest.
2. **Byte classification** — classify each byte against the one-byte lookback (`opPrev<15>`) using the three Keiser-Lemire nibble tables (`byte1HighTable`, `byte1LowTable`, `byte2HighTable`).
3. **Multi-byte length checks** — use the two- and three-byte lookbacks (`opPrev<14>`, `opPrev<13>`) to verify that 3- and 4-byte lead bytes are followed by the right number of continuation bytes.
4. **State update** — save the register as `prevInput`, and recompute the incomplete carry with a saturating subtract against the `isIncompleteMax` thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0).
5. **Finalize** — the input is invalid if either `error` or the incomplete carry is nonzero, since ending on an unfinished codepoint is itself invalid.

There are two validators built on this algorithm. The standalone `jsonifier::validateUtf8` uses `utf8_checker`, which works in 64-byte blocks and takes its ASCII fast path per block. The parser uses `utf8_register_validator`, which works one register at a time inside the string-scanning loop, at whatever width that loop is using: 16 bytes on SSE, NEON, and SVE2, 32 on AVX2, and 64 on AVX-512.

On AVX2 and AVX-512 the string loop cascades from its widest register down to 16 bytes, so the carry has to survive a change of register width. The small `utf8_validation_state` struct handles this: it stores the last three bytes of the previous register (`prevBytes[3]`), an incomplete flag, and a sticky error bit. Each width flushes into it on exit, and the next width's validator reseeds its registers from it. Three bytes are enough because the lookbacks never reach further than that.

Input that doesn't fill a whole register is copied into a register-sized scratch buffer padded with plain ASCII and validated through the same path: spaces (0x20) in the string scanner and `A` (0x41) in the standalone validator. ASCII padding is always valid UTF-8, so it can't create false errors, while a truncated codepoint just before the padding is still caught by the incomplete carry.

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

    if (parser.parseJson(e, json_valid)) {
        std::cout << "Valid JSON parsed: " << e.name << std::endl;
    }

    if (!parser.parseJson(e, json_bad_utf8)) {
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

## What's Next

- **[Validating](Validating.md)** — the pure structural-check function that includes UTF-8 validation automatically
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the full `parse_options` reference
- **[Error Handling](Errors.md)** — how UTF-8 errors are reported through `parser.getErrors()`

---