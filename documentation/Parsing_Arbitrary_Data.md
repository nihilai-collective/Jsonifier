# Parsing Arbitrary Data

Not every JSON parse has a fixed schema. Sometimes you're building a JSON linter, a data-inspection tool, or a proxy that routes messages based on partial content. Sometimes your schema is 90% fixed but has one field that holds "some other JSON, whatever it is." For these cases, Jsonifier provides `jsonifier::raw_json_data` — a type that holds arbitrary JSON as a dynamically typed tree.

## The Basics

Use `raw_json_data` as the destination for any JSON you don't want to (or can't) fully describe with a registered struct:

```cpp
#include <jsonifier>
#include <iostream>

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string json = R"({"name":"Concert","count":42,"tags":["music","live"]})";

    jsonifier::raw_json_data document;
    parser.parseJson(document, json);

    if (document.getType() == jsonifier::json_type::object) {
        std::cout << "name: " << document["name"].getString() << std::endl;
        std::cout << "count: " << document["count"].getUint() << std::endl;
        std::cout << "tags: " << document["tags"].size() << std::endl;
    }

    return 0;
}
```

Nothing needs to be registered. `raw_json_data` accepts any valid JSON document.

## How It Stores Data

When `raw_json_data` is parsed, the parser first skips over the value to find its extent, then decodes it into a `std::variant` tree right away:

| Alternative | Type |
|-------------|------|
| `object_type` | `std::unordered_map<jsonifier::string, raw_json_data>` |
| `array_type` | `std::vector<raw_json_data>` |
| `string_type` | `jsonifier::string` |
| `number_type` | `jsonifier::json_number` |
| `bool_type` | `bool` |
| `null_type` | `std::nullptr_t` |

A default-constructed `raw_json_data` holds `null`. If decoding a nested object, array, or string fails, the value is reset to `null`.

## Accessors

| Method | Returns |
|--------|---------|
| `getType()` | `jsonifier::json_type`: `object`, `array`, `string`, `number`, `boolean`, `null`, or `unset` |
| `getObject()` | Reference to `object_type` |
| `getArray()` | Reference to `array_type` |
| `getString()` | Reference to `string_type` |
| `getNumber()` | Reference to the `json_number` |
| `getInt()` | The number as `int64_t` |
| `getUint()` | The number as `uint64_t` |
| `getDouble()` | The number as `double` |
| `getBool()` | Reference to the `bool` |
| `operator[](key)` | Object member by key (anything convertible to `string_view`) |
| `operator[](index)` | Array element by unsigned index |
| `contains(key)` | Whether an object has the key |
| `size()` | Member count for objects, element count for arrays, length for strings, `0` otherwise |
| `operator==` | Structural equality of the decoded variants |

`getObject()`, `getArray()`, `getString()`, `getNumber()`, and `getBool()` use `std::get`, so calling one on the wrong alternative throws `std::bad_variant_access`. Check `getType()` first when you don't know the shape.

## Number Handling

JSON has one number grammar, but C++ has three number families. `json_number` picks one per value based on its shape:

- **Starts with `-` and parses as an integer** → `int64_t`
- **Contains `.`, `e`, or `E`** → `double`
- **Otherwise** → `uint64_t`
- Anything that fails those falls back to a `double` parse

`json_number::getType()` reports which one was chosen (`number_types::uint64`, `int64`, or `double64`). The accessors convert as follows:

- **`getUint()`** returns the value only if it was stored as `uint64_t`, otherwise `0`
- **`getDouble()`** returns the value only if it was stored as `double`, otherwise `0.0`
- **`getInt()`** converts from any storage — unsigned values are cast, doubles are truncated

So for a non-negative integer like `42`, use `getUint()` or `getInt()`; `getDouble()` would return `0.0`.

## Mixed Schemas

The most powerful use is embedding `raw_json_data` inside a registered struct. This lets you have a fully-typed schema for the parts you care about and dynamic access for the parts you don't:

```cpp
struct message {
    std::string type{};
    int64_t timestamp{};
    jsonifier::raw_json_data payload{};
};

template<> struct jsonifier::core<message> {
    using value_type = message;
    static constexpr auto parseValue = createValue<
        &value_type::type,
        &value_type::timestamp,
        &value_type::payload>();
};

int main() {
    jsonifier::jsonifier_core<> parser;
    message msg;

    std::string json = R"({
        "type": "user.updated",
        "timestamp": 1728000000,
        "payload": { "user_id": 42, "changes": {"email": "new@example.com"} }
    })";

    parser.parseJson(msg, json);

    std::cout << msg.type << " at " << msg.timestamp << std::endl;
    if (msg.payload.getType() == jsonifier::json_type::object) {
        std::cout << "user_id: " << msg.payload["user_id"].getUint() << std::endl;
    }
}
```

This pattern is useful for:

- **Event or message envelopes** where the envelope is stable but the payload varies by type
- **API responses** where the top-level fields are known but nested objects change over time
- **Discriminated unions** where the discriminator tells you what shape the rest takes
- **Middleware** that needs to extract a routing key without fully parsing the message body

## Serializing `raw_json_data`

Serializing a `raw_json_data` writes out its current decoded tree, so edits you make through the accessors show up in the output:

```cpp
message msg;
parser.parseJson(msg, json);

std::string output;
parser.serializeJson(msg, output);
```

Each node goes through the same writers as typed values, so `serialize_options{ .prettify = true }` and indentation apply normally. Formatting is canonical rather than byte-preserving: input whitespace is not kept, numbers are re-written from their stored `uint64_t` / `int64_t` / `double`, object keys come out in `std::unordered_map` iteration order, and a default-constructed `raw_json_data` serializes as `null`.

## Equality

`raw_json_data` compares with `operator==` by comparing the decoded variants, so it is semantic rather than textual: `{"a":1,"b":2}` and `{"b":2,"a":1}` compare equal, and numbers compare by their stored type and value.

```cpp
if (msg1.payload == msg2.payload) {
}
```

## When to Use `raw_json_data` vs. Alternatives

**Use `raw_json_data` when:**

- You genuinely don't know the JSON schema ahead of time
- The schema varies at runtime and you can't or don't want to enumerate every possibility
- Part of your schema is stable and part is dynamic (embed `raw_json_data` for the dynamic part)

**Prefer a registered struct when:**

- The schema is known and stable
- You want type safety at the C++ level
- Parsing performance matters (a registered struct is faster than `raw_json_data` on the same input, because `raw_json_data` builds a heap-allocated variant tree)

**Prefer [Generic Parsing](Generic_Parsing.md) when:**

- You only need to read some values out of unknown JSON and don't need to keep, edit or serialize them
- You want to avoid building a heap-allocated tree

**Prefer [Partial Reading](PartialReading.md) when:**

- The schema is known, but you only care about a small subset of fields
- You want to skip most of the document efficiently without decomposing it into a dynamic tree

`raw_json_data` and Partial Reading solve different problems: Partial Reading is a performance optimization for extracting a few fields from a big document; `raw_json_data` is a semantic tool for holding JSON whose shape you don't know.

## Full Example

```cpp
#include <jsonifier>
#include <iostream>

struct discord_message {
    std::string type{};
    jsonifier::raw_json_data data{};
};

template<> struct jsonifier::core<discord_message> {
    using value_type = discord_message;
    static constexpr auto parseValue = createValue<
        &value_type::type,
        &value_type::data>();
};

int main() {
    jsonifier::jsonifier_core<> parser;

    std::string message_created = R"({
        "type": "MESSAGE_CREATE",
        "data": {"channel_id": "12345", "content": "hello", "author": {"id": "67890"}}
    })";

    std::string typing_start = R"({
        "type": "TYPING_START",
        "data": {"channel_id": "12345", "user_id": "67890"}
    })";

    discord_message m1, m2;
    parser.parseJson(m1, message_created);
    parser.parseJson(m2, typing_start);

    std::cout << "m1 type: " << m1.type << std::endl;
    std::cout << "m1 data channel: " << m1.data["channel_id"].getString() << std::endl;
    std::cout << "m1 data content: " << m1.data["content"].getString() << std::endl;

    std::cout << "m2 type: " << m2.type << std::endl;
    std::cout << "m2 data channel: " << m2.data["channel_id"].getString() << std::endl;
    std::cout << "m2 data user: " << m2.data["user_id"].getString() << std::endl;

    return 0;
}
```

Two different Discord message shapes, the same top-level envelope struct, dynamic access for the payload. No `std::variant`, no separate types per event, no discriminator-driven parsing tree.

## What's Next

- **[Reflection](Reflection.md)** — for registering the parts of your schema that are stable
- **[Generic Parsing](Generic_Parsing.md)** — lazy, schema-free reads without building a tree
- **[Partial Reading](PartialReading.md)** — for the "known schema, skip most of it" case
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the full API that both `raw_json_data` and registered types go through

---