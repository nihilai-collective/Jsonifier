# Serializing & Parsing

This page covers everything you can do with `parseJson` and `serializeJson`. If you haven't registered your types yet, see [Reflection](Reflection.md) first.

## The Parser

All operations go through a `jsonifier::jsonifier_core<>` instance:

```cpp
jsonifier::jsonifier_core<> parser;
```

Create it once and reuse it across many operations. The instance holds internal scratch buffers and the error list, so reusing it avoids repeated allocation.

## Parsing

The basic form takes a destination object and a source buffer:

```cpp
catalog data;
std::string json = R"({"events":{}, "schema-version":"1.0"})";
bool ok = parser.parseJson(data, json);
```

`parseJson` returns `true` on success, `false` if any errors were encountered. The destination is populated in place — whatever was there before gets overwritten.

Both arguments are flexible. The destination can be any registered type, and the source can be any contiguous buffer of characters (`std::string`, `std::string_view`, `std::vector<char>`, raw pointers, etc.).

### Parse Options

Every option lives in `jsonifier::parse_options`, passed as a template argument:

```cpp
parser.parseJson<jsonifier::parse_options{ .partialRead = true }>(data, json);
```

The full set:

| Option | Default | Purpose |
|--------|---------|---------|
| `partialRead` | `false` | Switches to a two-stage parser architecture where structural characters are pre-scanned into a tape before values are extracted. Enables handling of unordered or partial JSON structures. See [Partial Reading](PartialReading.md). |
| `knownOrder` | `false` | Enables adaptive memoization of field ordering, so repeated parses of the same shape hit a fast path. See [Known Order Parsing](Known_Order.md). |
| `minified` | `false` | Tells the parser the input has no whitespace, eliminating all whitespace-skipping logic in the value walker. See [Optimizing For Minified JSON](Optimizing_For_Minified_Json.md). |
| `validateUtf8` | `false` | Turns on UTF-8 validation during string parsing. See [UTF-8 Validation](UTF8_Validation.md). |
| `nullTerminated` | `true` | Whether the input buffer has a trailing null byte. **See the warning below.** |
| `maxDepth` | `1024` | Maximum JSON nesting depth. Enforced at runtime — inputs exceeding this depth are rejected with `parse_statuses::exceeded_max_depth`. Guards against stack exhaustion on adversarial input. |

Options compose — you can turn any combination on simultaneously:

```cpp
parser.parseJson<jsonifier::parse_options{
    .partialRead = true,
    .knownOrder = false,
    .minified = true,
    .validateUtf8 = true
}>(data, json);
```

The template argument is evaluated at compile time, so different option sets produce different specialized code paths. There's no runtime branch cost for options you don't use.

### ⚠️ The `nullTerminated` Footgun

`nullTerminated` defaults to **`true`**. When it's `true`, the parser uses the null byte at the end of your buffer as a stop sentinel and skips explicit `iter < endIter` bounds checks on every character access. This is faster, but it's only correct if your buffer actually has a null byte at `buffer.data() + buffer.size()`.

- **`std::string`** — safe. Its underlying storage is guaranteed to be null-terminated since C++11.
- **`std::string_view` into a string literal or `std::string`** — safe for the same reason.
- **`std::vector<char>`** — **not safe by default.** No trailing null guarantee.
- **Raw `char*` from a file read, socket, or arbitrary source** — **only safe if you know it's null-terminated.**

If your source doesn't have a trailing null, set `nullTerminated = false` explicitly:

```cpp
parser.parseJson<jsonifier::parse_options{ .nullTerminated = false }>(data, my_char_buffer);
```

Passing a non-null-terminated buffer with `nullTerminated = true` is **undefined behavior** — the parser will read past the buffer end, likely producing a crash, silent data corruption, or a security vulnerability. When in doubt, set it to `false`.

### Handling Errors

`parseJson` returns `false` on failure. Detailed error information lives in `parser.getErrors()`:

```cpp
if (!parser.parseJson(data, json)) {
    for (auto& error : parser.getErrors()) {
        std::cout << "Parse error: " << error << std::endl;
    }
}
```

Errors carry source location, the specific `parse_statuses` enum value that fired, and enough context to diagnose the input. See [Error Handling](Errors.md) for the full breakdown.

The error list is reused across parse calls — `parseJson` clears it at the start of each call, so you should inspect errors before invoking the parser again.

## Serializing

The basic form takes a source object and a destination buffer:

```cpp
std::string output;
parser.serializeJson(data, output);
```

The destination is resized to fit the output. Any prior contents are overwritten.

### Serialize Without a Buffer

There's a second form that returns a `string_view` into the parser's internal buffer, skipping the copy into a user-provided destination:

```cpp
auto view = parser.serializeJson(data);
std::cout << view << std::endl;
```

This is faster when you just need to inspect or write the JSON somewhere immediately. **The returned view is only valid until the next `parseJson` or `serializeJson` call on the same parser instance** — the internal buffer gets reused. If you need the JSON to outlive the next parser operation, use the buffer form instead.

### Serialize Options

Options live in `jsonifier::serialize_options`:

```cpp
parser.serializeJson<jsonifier::serialize_options{ .prettify = true }>(data, output);
```

The full set:

| Option | Default | Purpose |
|--------|---------|---------|
| `prettify` | `false` | Emit human-readable JSON with newlines and indentation instead of compact output. |
| `indentSize` | `3` | Number of `indentChar` characters per indent level when `prettify` is on. |
| `indentChar` | `' '` | Character used for indentation. Set to `'\t'` for tab-based indenting. |
| `indent` | `0` | Starting indent depth. Useful when serializing a fragment into a larger pre-indented document. |

Like parse options, these are template arguments — the compiler generates a specialized serializer for each option set with no runtime branching.

Example with tab indentation:

```cpp
parser.serializeJson<jsonifier::serialize_options{
    .prettify = true,
    .indentSize = 1,
    .indentChar = '\t'
}>(data, output);
```

### Pretty vs. Minified

By default, serialization is minified — no whitespace, smallest possible output. Turn on `prettify` for output intended for humans:

```cpp
parser.serializeJson(data, minified_output);

parser.serializeJson<jsonifier::serialize_options{ .prettify = true }>(data, pretty_output);
```

For standalone pretty-printing or minification of existing JSON strings (without going through a typed object), see [Prettifying](Prettifying.md) and [Minifying](Minifying.md).

## Reusing the Parser

`jsonifier_core<>` is designed for reuse. Once created, it holds a scratch buffer and an error list that are reused across every call. The pattern is:

```cpp
jsonifier::jsonifier_core<> parser;

catalog data1, data2, data3;

parser.parseJson(data1, json_string_1);
parser.parseJson(data2, json_string_2);
parser.parseJson(data3, json_string_3);

std::string output;
parser.serializeJson(data1, output);
parser.serializeJson(data2, output);
```

Each call clears the error list and reuses the scratch buffer. There's no hidden cost to creating a parser once and keeping it around for the lifetime of your program.

## Full Example

Parse, mutate, and re-serialize:

```cpp
#include <jsonifier>
#include <iostream>

struct event {
    int64_t id{};
    std::string name{};
    std::optional<std::string> logo{};
    std::vector<int64_t> topicIds{};
};

struct catalog {
    std::unordered_map<std::string, event> events{};
    std::string schema_version{};
};

template<> struct jsonifier::core<event> {
    using value_type = event;
    static constexpr auto parseValue = createValue
        &value_type::id,
        &value_type::name,
        &value_type::logo,
        &value_type::topicIds>();
};

template<> struct jsonifier::core<catalog> {
    using value_type = catalog;
    static constexpr auto parseValue = createValue
        &value_type::events,
        makeJsonEntity<&value_type::schema_version, "schema-version">()>();
};

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string input = R"({"events":{"42":{"id":42,"name":"Concert","logo":null,"topicIds":[1,2,3]}},"schema-version":"1.0"})";
    catalog data;

    if (!parser.parseJson(data, input)) {
        for (auto& error : parser.getErrors()) {
            std::cout << "Parse error: " << error << std::endl;
        }
        return 1;
    }

    data.events["43"] = { 43, "New Event", std::nullopt, { 4, 5 } };
    data.schema_version = "1.1";

    std::string output;
    parser.serializeJson<jsonifier::serialize_options{ .prettify = true }>(data, output);
    std::cout << output << std::endl;

    return 0;
}
```

## What's Next

- **[Known Order Parsing](Known_Order.md)** — the biggest single parsing optimization when applicable
- **[Partial Reading](PartialReading.md)** — for unordered or partial JSON structures
- **[Optimizing For Minified JSON](Optimizing_For_Minified_Json.md)** — details on the `minified` option
- **[UTF-8 Validation](UTF8_Validation.md)** — the `validateUtf8` option and how the validator works
- **[Error Handling](Errors.md)** — full breakdown of the error type and `parse_statuses` enum
- **[Prettifying](Prettifying.md)** and **[Minifying](Minifying.md)** — for reformatting JSON strings without going through typed objects

---