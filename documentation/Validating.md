# Validating

`validateJson` checks whether a JSON document is well-formed against RFC8259 without extracting any values. Use it for pre-flight validation, cheap structural checks, or when you want to confirm JSON is well-formed without needing a schema.

## The Basics

```cpp
jsonifier::jsonifier_core<> parser;

std::string json = R"({"name":"Concert","id":42})";
bool valid = parser.validateJson(json);
```

`validateJson` returns `true` if the input is valid JSON, `false` otherwise. Like [minifying](Minifying.md) and [prettifying](Prettifying.md), it operates on JSON strings directly — no `jsonifier::core<T>` involved, no typed objects.

## What Gets Checked

Validation walks the entire document and enforces every structural rule in RFC8259:

- **Object structure** — matched `{` / `}`, keys must be strings, `:` after every key, `,` between members, no trailing comma
- **Array structure** — matched `[` / `]`, `,` between elements, no trailing comma
- **String contents** — valid escape sequences (`\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`, `\uXXXX`), no unescaped control characters, valid surrogate pairs, and full UTF-8 correctness of raw multi-byte content
- **Number grammar** — optional sign, digits, optional decimal fraction (with required digits after `.`), optional exponent (`e` or `E`, optional sign, required digits), no leading zeros except for zero itself
- **Literal correctness** — `true`, `false`, `null` spelled exactly
- **Nothing extra** — the entire input must be consumed; trailing garbage after the root value is rejected

UTF-8 validation is enabled by default in `validateJson` — you don't need to opt in. See [UTF-8 Validation](UTF8_Validation.md) for details on how the UTF-8 validator works.

## How It Works

Validation uses the same stage-1 structural scanner that powers [Partial Reading](PartialReading.md), [Minifying](Minifying.md), and [Prettifying](Prettifying.md). Stage-1 scans the input once and builds the structural tape. Stage-2 then walks the tape recursively — for each structural position, it dispatches on the leading character (`{`, `[`, `"`, digit, `t`/`f`, `n`) and validates that value's grammar.

Because validation only walks structure and character classes without extracting values into typed destinations, it's faster than a full `parseJson` for the same input.

## Error Reporting

When validation fails, details land in `parser.getErrors()` — the same mechanism as parsing:

```cpp
if (!parser.validateJson(json)) {
    for (auto& error : parser.getErrors()) {
        std::cout << error << std::endl;
    }
}
```

Validation errors use the `validate_statuses` enum. See [Error Handling](Errors.md) for the full breakdown of status codes and error record fields.

## When to Use `validateJson` vs. `parseJson`

Both `parseJson` and `validateJson` will catch structurally invalid JSON — `parseJson` validates as a side effect of extracting values. Choose based on what you're doing:

**Use `validateJson` when:**

- You need to accept-or-reject JSON without having a schema for it
- You want a cheap pre-flight check before deciding whether to commit to a full parse (for example, quarantining suspect input to a slow path)
- You're building a JSON linter or diagnostic tool
- You're validating input at a system boundary (API gateway, message router) where downstream code handles the actual parsing

**Use `parseJson` when:**

- You have a schema and want the parsed data anyway — the extra validation is free at that point
- You need per-field error reporting, not just "valid or not"

## Full Example

```cpp
#include <jsonifier>
#include <iostream>

int32_t main() {
    jsonifier::jsonifier_core<> parser;

    std::string good = R"({"name":"Concert","id":42,"topicIds":[1,2,3]})";
    std::string bad  = R"({"name":"Concert","id":42,})";

    std::cout << "good is valid: " << std::boolalpha << parser.validateJson(good) << std::endl;

    if (!parser.validateJson(bad)) {
        std::cout << "bad failed validation:" << std::endl;
        for (auto& error : parser.getErrors()) {
            std::cout << error << std::endl;
        }
    }

    return 0;
}
```

## What's Next

- **[UTF-8 Validation](UTF8_Validation.md)** — details on the UTF-8 validator that `validateJson` uses under the hood
- **[Error Handling](Errors.md)** — full breakdown of `validate_statuses` and the `error` type
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — when you need typed values, not just a valid/invalid answer

---