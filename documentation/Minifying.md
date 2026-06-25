# Minifying

Minifying strips all non-essential whitespace from a JSON document, producing the smallest valid representation. It's useful for reducing payload size before transmission or storage.

## The Basics

```cpp
jsonifier::jsonifier_core<> parser;

std::string pretty = R"({
    "name": "Concert",
    "id": 42
})";

std::string minified;
parser.minifyJson(pretty, minified);
```

The result: `{"name":"Concert","id":42}` — same JSON, no wasted bytes.

Unlike `parseJson` and `serializeJson`, minifying doesn't involve your registered types at all. It's a pure JSON string-to-JSON string transformation. You can minify any valid JSON document without touching `jsonifier::core<T>`.

## Two API Forms

The two-argument form writes into a destination buffer you provide and returns `bool`:

```cpp
std::string minified;
bool ok = parser.minifyJson(input, minified);
```

The single-argument form returns a freshly allocated string:

```cpp
auto minified = parser.minifyJson(input);
```

Both forms use the parser's internal scratch buffer for the work, then produce the output. The single-argument form allocates a new string for the result; the two-argument form fills in the buffer you passed. Use whichever fits your ownership model.

## How It Works

Minifying uses the same stage-1 structural scanner that powers [Partial Reading](PartialReading.md). Stage-1 scans the input once and builds a tape of pointers to every structural character (`{ } [ ] " , :`) and every value. Minifying then walks that tape, emitting each structural character directly and copying string and number values verbatim — with any whitespace between them stripped out.

Booleans and nulls are normalized to canonical form (`true`, `false`, `null`) rather than copied byte-for-byte from the source. In practice this changes nothing because valid JSON already uses those exact literals, but if your input has any oddities the output will always be canonical.

## Error Handling

Minifying reports errors through the same `parser.getErrors()` mechanism as parsing:

```cpp
if (!parser.minifyJson(input, output)) {
    for (auto& error : parser.getErrors()) {
        std::cout << "Minify error: " << error << std::endl;
    }
}
```

Minifier-specific error statuses live in a `minify_statuses` enum, covering cases like empty input, malformed strings, invalid numbers, and unexpected structural characters. See [Error Handling](Errors.md) for the full breakdown.

## What Stays the Same

Minifying preserves everything semantically meaningful:

- **All keys and values** — nothing is dropped
- **Key order** — fields stay in the order they appear in the input
- **String contents** — including all internal whitespace, escapes, and Unicode
- **Number precision** — the number token is copied verbatim, so `1.0`, `1`, and `1e0` stay distinct

## What Gets Stripped

- **Whitespace between structural characters** — spaces, tabs, newlines, carriage returns
- **Indentation** — regardless of style or depth
- **Trailing whitespace** — inside numbers and after values

## Round-Tripping With Prettifying

Minifying is the inverse of prettifying. You can freely round-trip:

```cpp
std::string pretty_output;
parser.prettifyJson(minified_input, pretty_output);

std::string minified_output;
parser.minifyJson(pretty_output, minified_output);
```

The final `minified_output` will be byte-identical to `minified_input` (assuming valid JSON in).

## Full Example

```cpp
#include <jsonifier>
#include <iostream>

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string pretty = R"({
        "events": {
            "42": {
                "id": 42,
                "name": "Concert",
                "topicIds": [1, 2, 3]
            }
        },
        "schema-version": "1.0"
    })";

    std::string minified;
    if (!parser.minifyJson(pretty, minified)) {
        for (auto& error : parser.getErrors()) {
            std::cout << "Minify error: " << error << std::endl;
        }
        return 1;
    }

    std::cout << minified << std::endl;
    return 0;
}
```

## When to Use Minifying vs. `serializeJson` With Defaults

Both produce minified output, but they operate on different inputs:

- **`minifyJson`** — takes a JSON string and produces a minified JSON string. No typed object involved. Use when you're passing JSON through your system without needing to interpret it.
- **`serializeJson`** — takes a typed C++ object (via your registered `jsonifier::core<T>`) and produces JSON. Use when you're producing JSON from your own data.

If you have a typed object and want minified output, use `serializeJson` — going through `minifyJson` afterward would just be a round-trip through the parser for no reason.

## What's Next

- **[Prettifying](Prettifying.md)** — the inverse operation, adding indentation and newlines for human readability
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — for producing JSON directly from typed C++ objects
- **[Error Handling](Errors.md)** — full breakdown of error reporting including minifier errors
