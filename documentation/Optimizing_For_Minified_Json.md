# Optimizing For Minified JSON

When your JSON input has no whitespace between tokens — no indentation, no newlines, no padding spaces around colons or commas — you can tell the parser about it with `parse_options{ .minified = true }` and unlock a significantly faster code path.

## The Idea

Most JSON parsers spend a real fraction of their time on whitespace: detecting it, skipping it, deciding whether the byte they're looking at is meaningful or just formatting. For JSON that's already minified — the typical shape of machine-to-machine JSON, API responses, log lines, wire-format messages — that work is pure overhead. The tokens are packed tight and there's nothing to skip.

Jsonifier's `minified` flag lets the compiler eliminate whitespace handling from every parsing path at compile time. Not "we skip whitespace faster when there isn't any" — the whitespace-handling code paths don't exist in the generated binary at all.

## Turning It On

```cpp
parser.parseJson<jsonifier::parse_options{ .minified = true }>(data, json);
```

`minified` is a template parameter, so the code path changes at compile time. There's no runtime branch cost either way.

## What Actually Changes

The optimization applies to every parsing path — both the default scalar walker and the two-stage structural pipeline used by `partialRead = true`:

**The value iterator drops all whitespace-skip calls.** Both the scalar iterator (which walks the raw JSON string directly) and the structural iterator (which walks the pre-scanned tape) insert a `skipWhitespace()` call at every value boundary, before every colon, before every comma, and after every closing bracket by default. With `minified = true`, every one of those calls is `if constexpr`-eliminated. The iterator advances directly from one structural token to the next with no intermediate work — fewer branches, tighter inner loops, better instruction cache behavior.

**Stage-1's tape build skips whitespace tracking (when the tape is being built).** When you're using `partialRead = true`, stage-1's SIMD scanner tracks whitespace positions along with structural characters, because the tape needs that information for stage-2 to skip whitespace efficiently. With `minified = true`, that whitespace-tracking pass is eliminated. Stage-1 only marks true structural characters — fewer SIMD comparisons, fewer memory writes, faster tape construction.

Whether you're on the scalar path or the two-stage path, `minified = true` shortens the code path from raw bytes to typed values.

## When It Applies

**Turn it on when your input is guaranteed to be minified.** Common cases:

- **API responses from services that minify by default** — most modern JSON APIs return minified output
- **Log lines** — one JSON object per line, no internal whitespace
- **Wire-format messages** — anything designed for machine consumption
- **JSON produced by Jsonifier's own `serializeJson`** — the default output is minified
- **JSON that's been through `minifyJson`** — the whole point is that it's guaranteed whitespace-free

**Leave it off when your input might have whitespace.** Common cases:

- **User-authored JSON** — humans indent and pad; assume whitespace
- **Config files** — usually formatted for readability
- **JSON from unknown sources** — if you don't control the producer, don't assume the shape

## ⚠️ The Correctness Requirement

`minified = true` is a **promise you're making to the parser**. If you pass a JSON document that contains whitespace with this flag on, the parser will misinterpret the whitespace as unexpected tokens — the resulting errors won't corrupt your data, but you'll get parse failures on inputs that would have succeeded with the flag off.

If you're not sure whether your input is minified, either:

- **Leave the flag off** (the default) — a small performance cost for correctness on unknown inputs
- **Pre-minify with `minifyJson`** if you'd rather pay the minification cost once and then use the fast path repeatedly

## Interaction With Other Options

- **`knownOrder`** — Combining `minified = true` with `knownOrder = true` unlocks the fastest parsing path Jsonifier offers. When both are set (and `partialRead` is off), object parsing uses compile-time-generated string literals for each field including the surrounding punctuation, letting the parser check the next expected field with a single `memcmp` against the raw stream. See [Known Order Parsing](Known_Order.md).
- **`partialRead`** — Fully compatible. Stage-1's tape is still built (needed for partial mode), just without the whitespace-tracking work.
- **`validateUtf8`** — Fully compatible. UTF-8 validation runs alongside string parsing regardless of the `minified` flag.
- **`nullTerminated`** — Independent knob; see the [Serializing & Parsing](Usage_Serializing_Parsing.md#-the-nullterminated-footgun) warning.

## Example: Dispatching on Whether Input Is Minified

If your input source is mixed — sometimes minified, sometimes not — you can't have the compiler pick a single code path. But you can dispatch at runtime based on a cheap check:

```cpp
bool looks_minified(std::string_view json) {
    for (char c : json) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
    }
    return true;
}

if (looks_minified(json)) {
    parser.parseJson<jsonifier::parse_options{ .minified = true }>(data, json);
} else {
    parser.parseJson(data, json);
}
```

The runtime check adds a full scan of the input, so this only pays off when the fast-path wins are big and the dispatch cost is amortized across a lot of subsequent parsing work. For most workloads, picking one flag statically based on your input source is simpler and faster overall.

## Full Example

```cpp
#include <jsonifier>
#include <iostream>

struct event {
    int64_t id{};
    std::string name{};
    std::vector<int64_t> topicIds{};
};

template<> struct jsonifier::core<event> {
    using value_type = event;
    static constexpr auto parseValue = createValue
        &value_type::id,
        &value_type::name,
        &value_type::topicIds>();
};

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string minified_json = R"({"id":42,"name":"Concert","topicIds":[1,2,3]})";

    event e;
    if (parser.parseJson<jsonifier::parse_options{ .minified = true }>(e, minified_json)) {
        std::cout << e.name << ": " << e.id << std::endl;
    }

    return 0;
}
```

## What's Next

- **[Known Order Parsing](Known_Order.md)** — the other big performance flag, especially effective when combined with `minified`
- **[Minifying](Minifying.md)** — how to convert whitespace-bearing JSON into a form suitable for the fast path
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the full `parse_options` reference

---