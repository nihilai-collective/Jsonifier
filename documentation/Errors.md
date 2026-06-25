# Error Handling

Every Jsonifier operation reports failures through a unified error mechanism: the operation returns `false`, and detailed error records are pushed onto `parser.getErrors()`. This page covers everything the error type exposes and every status code you might encounter.

## The Basic Pattern

Every parser method that can fail returns `bool`. Errors accumulate on the parser's error list, which you inspect after the fact:

```cpp
if (!parser.parseJson(data, json)) {
    for (auto& error : parser.getErrors()) {
        std::cout << error << std::endl;
    }
}
```

`parser.getErrors()` returns `std::vector<jsonifier::error>&` — a reference to the internal list. The list is cleared at the start of each parse, minify, prettify, and validate call, so inspect it before invoking the parser again.

## What an `error` Contains

Each `error` record carries enough information to diagnose exactly what went wrong and where. Streaming an `error` to `std::ostream` produces something like:

```
Error of Type: missing_comma, at global index: 47, on line: 3, at local index: 12
Here's some of the string's values: '"' 'i' 'd' '"' ':' ...
The Values: "id": 42 "name"
In file: /path/to/jsonifier/parse.hpp, at: 218:24, in function: reject().
```

Breaking that down:

- **`Error of Type:`** — the specific status enum value (`missing_comma`, `invalid_string_characters`, etc.) as a human-readable string
- **`global index`** — the byte offset in your input where the error was detected
- **`line`** — the line number within your input (1-indexed, counted by newlines)
- **`local index`** — the column within that line
- **`Here's some of the string's values:`** — up to 32 bytes of context starting at the error position, byte-by-byte with escape sequences printed (`\n`, `\t`, `\x00`, etc.) so you can see exactly what was there — including invisible or non-printable characters
- **`The Values:`** — up to 64 bytes of raw context (with tabs normalized to spaces for readability)
- **`In file: ... at: ..., in function: ...`** — the source location **inside Jsonifier's own code** where the error was reported. This is Jsonifier's internal file/line/function, not your code — useful for reporting bugs or understanding which parser path detected the issue.

## Accessing Fields Programmatically

If you want to route errors into your own logging or telemetry system rather than printing them, the `error` type exposes conversion operators:

```cpp
for (auto& err : parser.getErrors()) {
    uint64_t raw_code = err;
    jsonifier::parse_statuses parse_code = err;
    bool is_error = err;

    if (parse_code == jsonifier::parse_statuses::missing_comma) {
        // handle this specific case
    }
}
```

- **`operator uint64_t`** — the raw status code as an integer
- **`operator parse_statuses`** — static_cast directly to the parse-status enum (useful for switching on parse errors)
- **`operator bool`** — `true` if this is a real error (non-success)
- **`operator==`** — compares two errors by type and index

For the full formatted string, call `err.reportError()` — this returns the same output that streaming to `ostream` produces.

## The Five Status Classes

Every error belongs to one of five status classes, corresponding to the operation that produced it:

| Class | Operation | Status Enum |
|-------|-----------|-------------|
| `parsing` | `parseJson` | `parse_statuses` |
| `serializing` | `serializeJson` | `serialize_statuses` |
| `minifying` | `minifyJson` | `minify_statuses` |
| `prettifying` | `prettifyJson` | `prettify_statuses` |
| `validating` | `validateJson` | `validate_statuses` |

Each class has its own status enum. The status names below are what you'll see in the `Error of Type:` field.

### Parse Statuses

Produced by `parseJson`:

| Status | Meaning |
|--------|---------|
| `success` | No error |
| `missing_key_start` | Expected a `"` opening an object key, got something else |
| `missing_object_start` | Expected `{`, got something else |
| `missing_object_end` | Expected `}`, got something else |
| `missing_array_start` | Expected `[`, got something else |
| `missing_array_end` | Expected `]`, got something else |
| `invalid_string_characters` | String contained bytes that aren't valid in a JSON string (bad escape, invalid Unicode, illegal control character, etc.) |
| `missing_colon` | Expected `:` between an object key and value, got something else |
| `missing_comma` | Expected `,` between elements, got something else |
| `invalid_number_value` | Number token doesn't parse as a valid JSON number |
| `invalid_null_value` | Expected `null`, got a different token |
| `invalid_bool_value` | Expected `true` or `false`, got a different token |
| `no_input` | Input buffer was empty |
| `unfinished_input` | Input ended before the JSON value was complete |
| `unexpected_string_end` | String or container ended prematurely |
| `unexpected_end_of_input` | Ran off the end of the buffer while inside a value |
| `exceeded_max_depth` | JSON nesting exceeded the `maxDepth` parse option (default 1024) |
| `unexpected_token` | Token at this position doesn't fit any type the parser expected |
| `illegal_control_character` | Raw control character (U+0000–U+001F) inside a string without escaping |

### Validate Statuses

Produced by `validateJson`:

| Status | Meaning |
|--------|---------|
| `success` | No error |
| `missing_object_start` / `missing_object_end` / `missing_array_start` / `missing_array_end` | Structural characters missing where expected |
| `invalid_string_characters` | String violates RFC8259 string rules |
| `missing_colon` / `missing_comma` | Delimiter missing |
| `invalid_number_value` / `invalid_null_value` / `invalid_bool_value` | Malformed literal |
| `invalid_escape_characters` | Escape sequence isn't valid (bad `\u` sequence, unknown escape letter, etc.) |
| `missing_comma_or_closing_brace` | Inside an object or array, expected either `,` or the closing bracket |
| `no_input` | Input was empty |

See [Validating](Validating.md) for details on when to reach for `validateJson` vs. relying on `parseJson`'s built-in validation.

### Minify Statuses

Produced by `minifyJson`:

| Status | Meaning |
|--------|---------|
| `success` | No error |
| `no_input` | Input was empty |
| `invalid_string_length` | String bounds don't match structural indices — usually indicates malformed input |
| `invalid_number_value` | Number token doesn't parse |
| `incorrect_structural_index` | Structural scanner produced an unexpected token — usually indicates malformed input |

### Prettify Statuses

Produced by `prettifyJson`:

| Status | Meaning |
|--------|---------|
| `success` | No error |
| `no_input` | Input was empty |
| `exceeded_max_depth` | JSON nesting exceeded the internal depth stack |
| `incorrect_structural_index` | Structural scanner produced an unexpected token |

### Serialize Status

Produced by `serializeJson`. Only one value:

| Status | Meaning |
|--------|---------|
| `success` | No error |

Serialization can't fail on input — it consumes a valid C++ object and produces valid JSON. The `serialize_statuses` enum exists for consistency with the other operations, but you won't see errors in `getErrors()` after a `serializeJson` call.

## Multiple Errors Per Call

The parser can accumulate multiple errors from a single call. When one error is detected, the parser aborts the current value and unwinds — but partial state left behind can trigger follow-on errors as the unwind progresses. In practice the first error in the list is usually the actionable one; later errors are consequences.

For diagnostic output, printing all errors gives the most context. For programmatic branching, checking just the first is usually enough.

## Error Handling in Production

A reasonable pattern for production code:

```cpp
if (!parser.parseJson(data, json)) {
    auto& errors = parser.getErrors();
    if (!errors.empty()) {
        const auto& first = errors.front();
        jsonifier::parse_statuses status = first;

        switch (status) {
            case jsonifier::parse_statuses::no_input:
                return handle_empty_input();
            case jsonifier::parse_statuses::exceeded_max_depth:
                return handle_adversarial_input();
            default:
                log_parse_error(first.reportError());
                return handle_malformed_input();
        }
    }
}
```

The first error's status is enough for high-level branching. The full `reportError()` string is what you want in logs.

## What's Next

- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the operations that produce these errors
- **[Validating](Validating.md)** — when to use `validateJson` for pre-flight checking
- **[Minifying](Minifying.md)** and **[Prettifying](Prettifying.md)** — the string-transform operations and their error modes

---