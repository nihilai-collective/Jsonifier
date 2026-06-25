# Prettifying

Prettifying reformats a JSON document with indentation and newlines, making it easy to read. It's the inverse of [minifying](Minifying.md) — same input, human-readable output.

## The Basics

```cpp
jsonifier::jsonifier_core<> parser;

std::string minified = R"({"name":"Concert","id":42})";

std::string pretty;
parser.prettifyJson(minified, pretty);
```

The result:

```json
{
   "name": "Concert",
   "id": 42
}
```

Like [minifying](Minifying.md), prettifying operates on JSON strings directly. No `jsonifier::core<T>` involved, no typed objects — just JSON string in, JSON string out.

## Two API Forms

The two-argument form writes into a destination buffer you provide and returns `bool`:

```cpp
std::string pretty;
bool ok = parser.prettifyJson(input, pretty);
```

The single-argument form returns a freshly allocated string:

```cpp
auto pretty = parser.prettifyJson(input);
```

Both forms use the parser's internal scratch buffer for the work.

## Prettify Options

Indentation is controlled by `jsonifier::prettify_options`, passed as a template argument:

```cpp
parser.prettifyJson<jsonifier::prettify_options{
    .indentSize = 2,
    .indentChar = ' '
}>(input, output);
```

The full set:

| Option | Default | Purpose |
|--------|---------|---------|
| `indentSize` | `3` | Number of `indentChar` characters per indent level. |
| `indentChar` | `' '` | Character used for indentation. Set to `'\t'` for tab-based indenting. |

The template argument is evaluated at compile time — different option sets produce different specialized code paths.

Example with tab indentation:

```cpp
parser.prettifyJson<jsonifier::prettify_options{
    .indentSize = 1,
    .indentChar = '\t'
}>(input, output);
```

## How It Works

Prettifying uses the same stage-1 structural scanner that powers [Partial Reading](PartialReading.md) and [Minifying](Minifying.md). Stage-1 scans the input once and builds a tape of pointers to structural characters and values. Stage-2 then walks the tape, emitting each token with appropriate whitespace:

- **Structural characters** (`{`, `}`, `[`, `]`) get newlines and indentation around them
- **Commas** are followed by a newline and the current indent
- **Colons** are followed by a single indent character (usually a space) — `"key": value`
- **Strings, numbers, booleans, and nulls** are emitted verbatim

Empty objects (`{}`) and empty arrays (`[]`) are kept on a single line rather than being split across three lines.

## Error Handling

Prettifying reports errors through `parser.getErrors()`:

```cpp
if (!parser.prettifyJson(input, output)) {
    for (auto& error : parser.getErrors()) {
        std::cout << "Prettify error: " << error << std::endl;
    }
}
```

Prettifier-specific error statuses live in a `prettify_statuses` enum, covering cases like empty input and unexpected structural characters. See [Error Handling](Errors.md) for the full breakdown.

## What Stays the Same

Prettifying preserves everything semantically meaningful:

- **All keys and values** — nothing is dropped
- **Key order** — fields stay in the order they appear in the input
- **String contents** — including all internal whitespace, escapes, and Unicode
- **Number precision** — the number token is copied verbatim

## Round-Tripping With Minifying

Prettifying and minifying are inverses. You can freely round-trip:

```cpp
std::string pretty;
parser.prettifyJson(minified_input, pretty);

std::string minified_again;
parser.minifyJson(pretty, minified_again);
```

`minified_again` will be byte-identical to `minified_input` (for valid JSON input).

## Full Example

```cpp
#include <jsonifier>
#include <iostream>

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string minified = R"({"events":{"42":{"id":42,"name":"Concert","topicIds":[1,2,3]}},"schema-version":"1.0"})";

    std::string pretty;
    if (!parser.prettifyJson<jsonifier::prettify_options{ .indentSize = 2 }>(minified, pretty)) {
        for (auto& error : parser.getErrors()) {
            std::cout << "Prettify error: " << error << std::endl;
        }
        return 1;
    }

    std::cout << pretty << std::endl;
    return 0;
}
```

Output:

```json
{
  "events": {
    "42": {
      "id": 42,
      "name": "Concert",
      "topicIds": [
        1,
        2,
        3
      ]
    }
  },
  "schema-version": "1.0"
}
```

## Prettifying vs. `serializeJson` With `prettify = true`

Both produce pretty-printed output, but they operate on different inputs — and use different option types.

- **`prettifyJson`** — takes a JSON string and produces a pretty-printed JSON string. Uses `jsonifier::prettify_options`. No typed object involved.
- **`serializeJson<serialize_options{ .prettify = true }>`** — takes a typed C++ object (via your registered `jsonifier::core<T>`) and produces pretty-printed JSON. Uses `jsonifier::serialize_options`.

If you have a typed object and want pretty output, use `serializeJson` — going through `prettifyJson` afterward would be an unnecessary round-trip.

Note that `prettify_options` and `serialize_options` are separate types with overlapping fields. Both have `indentSize` and `indentChar`, but only `serialize_options` has `prettify` (the toggle) and `indent` (starting depth). Standalone prettifying always prettifies from indent depth zero.

## What's Next

- **[Minifying](Minifying.md)** — the inverse operation, stripping whitespace
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — for producing pretty JSON directly from typed C++ objects
- **[Error Handling](Errors.md)** — full breakdown of error reporting including prettifier errors

---