# Generic (Schema-Free) Parsing

`jsonifier::generic` is a lazy, schema-free parser for documents whose shape you don't know at compile time. Its API is shaped like simdjson's On Demand: stage 1 builds a tape of structural offsets, and your code walks that tape with a cursor, parsing only the values it asks for. Unlike On Demand, a value handle can be read more than once and object fields can be read in any order.

Use it when you want to pull a handful of values out of arbitrary JSON without describing a schema and without building a heap-allocated tree. For the design and benchmark results, see the [Generic Parsing paper](../Generic-Parsing.md).

## The Basics

```cpp
#include <jsonifier>
#include <iostream>

int main() {
    jsonifier::generic::parser<> parser;

    std::string json = R"({"name":"Concert","count":42,"tags":["music","live"]})";

    jsonifier::generic::document doc = parser.iterate(json);

    std::string_view name{};
    uint64_t count{};
    if (doc["name"].get(name) == jsonifier::generic::error_code::success &&
        doc["count"].get(count) == jsonifier::generic::error_code::success) {
        std::cout << name << " x" << count << std::endl;
    }

    for (jsonifier::generic::value tag : doc["tags"].getArray()) {
        std::string_view text{};
        if (tag.get(text) == jsonifier::generic::error_code::success) {
            std::cout << text << std::endl;
        }
    }
}
```

`jsonifier::generic::parser<initialBufferSize>` is a thin wrapper around a `jsonifier_core<initialBufferSize>`, so it gets the same [runtime CPU dispatch](CPU_Architecture_Selection.md#runtime-selection) as every other call: stage 1 runs on the best SIMD tier the running CPU supports. `jsonifier_core<>` exposes `iterate` and `iterateMany` directly as well, if you already have one.

## Lifetimes

- The input buffer must outlive the `document` and every handle derived from it. Strings are returned zero-copy as views into it when they contain no escapes.
- A `document` points into its parser's tape. Calling `iterate`, `parseJson` or `validateJson` again on the same parser invalidates every handle from the previous document.
- A parser is not copyable. Use one per thread.

## Errors

Accessors don't throw and don't return `result<T>`. Each one returns a `[[nodiscard]] jsonifier::generic::error_code` and writes through an out-parameter. Navigation (`operator[]`, `findField`, `at`, `atPointer`, ...) returns a handle that carries any error forward, so you can chain lookups and check once at the end:

```cpp
double lat{};
auto err = doc["venue"]["location"]["lat"].get(lat);
```

| `error_code` | Meaning |
|--------------|---------|
| `success` | — |
| `empty` | No JSON in the input |
| `tape_error` | Stage 1 found a structural error |
| `unclosed_container` | An object or array was never closed |
| `trailing_content` | Data after the root value |
| `incorrect_type` | The value is not the requested type |
| `no_such_field` | Object has no such key |
| `index_out_of_bounds` | Array index past the end |
| `number_error` | Malformed or out-of-range number |
| `string_error` | Malformed string or invalid UTF-8 |
| `t_atom_error` / `f_atom_error` / `n_atom_error` | Malformed `true` / `false` / `null` |
| `invalid_json_pointer` | Malformed JSON Pointer |
| `capacity` | Input larger than the parser can index |

`document::atEnd()` reports whether the whole document was consumed without error.

## `value`

| Method | Description |
|--------|-------------|
| `type()` | `jsonifier::json_type` of the value |
| `get(out)` | Dispatches on `out`'s type: `double`, `int64_t`, `uint64_t`, `bool`, `std::string_view`, any string buffer, `object`, `array` or `value` |
| `getDouble` / `getInt64` / `getUint64` / `getBool` | Typed scalar reads |
| `getString(std::string_view&)` | Zero-copy when unescaped, otherwise unescaped into parser-owned storage |
| `getString(string_type&)` | Copies (and unescapes) into your own `std::string` / `jsonifier::string` |
| `isNull()` | Whether the value is `null` |
| `rawJson(string_view&)` | The value's exact source text |
| `getObject()` / `getArray()` | Container handles |
| `operator[](key)` / `findField(key)` / `findFieldUnordered(key)` | Object member lookup |
| `operator[](index)` | Array element |
| `atPointer(pointer)` | RFC 6901 JSON Pointer lookup, e.g. `"/tags/0"` |
| `error()` | The error carried by this handle |

## `object`

| Method | Description |
|--------|-------------|
| `begin()` / `end()` | Iterate `field`s in document order |
| `findField(key)` / `operator[](key)` | Look up a key, optimized for reading fields in document order |
| `findFieldUnordered(key)` | Look up a key when you read fields out of order |
| `countFields(uint64_t&)` | Number of members |
| `rawJson(string_view&)` / `atPointer(pointer)` / `error()` | As on `value` |

A `field` has `key()` (raw, escaped key text), `unescapedKey(std::string_view&)`, `keyEquals(key)` and `value()`.

In-order lookups cost a cursor step and a key compare. Out-of-order lookups rewind to the object, and an object that keeps being read out of order gets a SIMD-hashed field index built on demand, so their cost stays close to linear regardless of the order you ask for keys in.

## `array`

| Method | Description |
|--------|-------------|
| `begin()` / `end()` | Iterate elements as `value`s |
| `at(index)` / `operator[](index)` | Element by index |
| `countElements(uint64_t&)` | Number of elements |
| `rawJson(string_view&)` / `atPointer(pointer)` / `error()` | As on `value` |

## Streams: `iterateMany`

`iterateMany` reads a buffer holding many documents, such as NDJSON, the counterpart of simdjson's `iterate_many`:

```cpp
jsonifier::generic::parser<> parser;
std::string input = "{\"id\":1}\n{\"id\":2}\n{\"id\":3}\n";

auto stream = parser.iterateMany(input);
for (auto iter = stream.begin(); iter != stream.end(); ++iter) {
    jsonifier::generic::document doc = *iter;
    int64_t id{};
    if (doc["id"].get(id) == jsonifier::generic::error_code::success) {
        std::cout << iter.currentIndex() << ": " << iter.source() << " -> " << id << std::endl;
    }
}
```

- By default documents are separated by whitespace (`parse_options{ .newLineDelimited = true }`). Pass `iterateMany<jsonifier::parse_options{}>(input)` to accept comma-separated documents instead.
- Documents are indexed in windows of `batchSize` bytes (default `parser<>::defaultBatchSize`, 1 MiB), so the batch size must be at least as large as the largest single document.
- `iter.currentIndex()` is the byte offset of the current document and `iter.source()` its source text.
- `stream.truncatedBytes()` reports trailing bytes that did not form a complete document, and `stream.error()` the stream-level error.
- Each document is invalidated when the iterator advances.

## `generic` vs. `raw_json_data` vs. Registered Types

| | `generic::parser` | [`raw_json_data`](Parsing_Arbitrary_Data.md) | Registered struct |
|---|---|---|---|
| Schema needed | No | No | Yes |
| Allocates a tree | No | Yes | No |
| Values outlive the parser | No | Yes | Yes |
| Editable / serializable | No | Yes | Yes |
| Best for | Pulling a few values out of arbitrary JSON, fast | Holding or forwarding unknown JSON | Known shapes |

## What's Next

- **[CPU Architecture Selection](CPU_Architecture_Selection.md)** — how the SIMD tier for stage 1 is picked at runtime
- **[Parsing Arbitrary Data](Parsing_Arbitrary_Data.md)** — `raw_json_data`, the owning dynamic tree
- **[Generic Parsing paper](../Generic-Parsing.md)** — design and benchmarks against simdjson On Demand

---
