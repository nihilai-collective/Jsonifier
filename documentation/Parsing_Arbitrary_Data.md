# Parsing Arbitrary Data

Not every JSON parse has a fixed schema. Sometimes you're building a JSON linter, a data-inspection tool, or a proxy that routes messages based on partial content. Sometimes your schema is 90% fixed but has one field that holds "some other JSON, whatever it is." For these cases, Jsonifier provides `jsonifier::raw_json_data` — a type that holds arbitrary JSON with lazy typed access.

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

    std::cout << document.rawJson() << std::endl;

    if (document.getType() == jsonifier::json_type::Object) {
        auto& obj = document.getObject();
        std::cout << "name: " << obj["name"].getString() << std::endl;
        std::cout << "count: " << obj["count"].getInt64() << std::endl;
    }

    return 0;
}
```

Nothing needs to be registered. `raw_json_data` accepts any valid JSON document.

## Two Access Modes

`raw_json_data` supports two ways of looking at the same data.

**The raw view** — `rawJson()` returns a `string_view` into the original JSON text:

```cpp
auto raw = document.rawJson();
std::cout << raw << std::endl;
```

This is always available and free — no parsing beyond the initial structural scan.

**The typed view** — the JSON is decomposed into a nested tree of typed values on first access. Ask for the JSON type via `getType()`, then reach for the appropriate accessor:

| Method | Returns |
|--------|---------|
| `getType()` | `json_type` enum: `Object`, `Array`, `String`, `Number`, `Bool`, `Null`, or `Unset` |
| `getObject()` | Reference to `object_type` (a map of `string` → `raw_json_data`) |
| `getArray()` | Reference to `array_type` (a `vector<raw_json_data>`) |
| `getString()` | Reference to the string value |
| `getInt64()` | The value as `int64_t` (numbers only) |
| `getUint64()` | The value as `uint64_t` (numbers only) |
| `getDouble()` | The value as `double` (numbers only) |
| `getBool()` | The value as `bool` |
| `rawJson()` | The raw JSON string view |

Typed access is populated lazily — the first call to any typed accessor triggers the full parse into the variant tree. Subsequent calls use the cached result.

## Number Handling

JSON has one number grammar, but C++ has three number families (signed integers, unsigned integers, and floating-point). Jsonifier picks a type for each number in the input based on its shape:

- **Contains `.`, `e`, or `E`** → parsed as `double`
- **Contains `-`** → parsed as `int64_t`
- **Otherwise** → parsed as `uint64_t`

The type is stored in the variant, and the corresponding `getX()` method returns the value. **Calling the wrong accessor returns a default value, not an error.** Calling `getInt64()` on a number that was parsed as `double` returns 0. If you don't know a number's shape in advance, call `getType()` first to check, then reach for the matching accessor.

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
    static constexpr auto parseValue = createValue
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
    if (msg.payload.getType() == jsonifier::json_type::Object) {
        auto& p = msg.payload.getObject();
        std::cout << "user_id: " << p["user_id"].getInt64() << std::endl;
    }
}
```

This pattern is useful for:

- **Event or message envelopes** where the envelope is stable but the payload varies by type
- **API responses** where the top-level fields are known but nested objects change over time
- **Discriminated unions** where the discriminator tells you what shape the rest takes
- **Middleware** that needs to extract a routing key without fully parsing the message body

## Serializing `raw_json_data`

Serializing a `raw_json_data` writes its raw JSON string back verbatim — round-trips are byte-preserving for the raw content:

```cpp
message msg;
parser.parseJson(msg, json);

std::string output;
parser.serializeJson(msg, output);
```

The `type` and `timestamp` fields are re-serialized from their typed values; the `payload` field is written back exactly as it appeared in the input.

## Comparison and Streaming

`raw_json_data` has comparison and stream operators for common use cases:

```cpp
if (msg1.payload == msg2.payload) {
    // ...
}

std::cout << msg.payload << std::endl;
```

Equality compares the raw JSON strings, which is fast but strict — `{"a":1,"b":2}` and `{"b":2,"a":1}` compare unequal even though they represent the same object. If you need semantic equality, walk the typed views yourself.

## When to Use `raw_json_data` vs. Alternatives

**Use `raw_json_data` when:**

- You genuinely don't know the JSON schema ahead of time
- The schema varies at runtime and you can't or don't want to enumerate every possibility
- You want to pass a JSON blob through your system without touching it
- Part of your schema is stable and part is dynamic (embed `raw_json_data` for the dynamic part)

**Prefer a registered struct when:**

- The schema is known and stable
- You want type safety at the C++ level
- Parsing performance matters (a registered struct is faster than `raw_json_data` on the same input, because `raw_json_data` has to track more state during parse)

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
    static constexpr auto parseValue = createValue
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
    std::cout << "m1 data channel: " << m1.data.getObject()["channel_id"].getString() << std::endl;
    std::cout << "m1 data content: " << m1.data.getObject()["content"].getString() << std::endl;

    std::cout << "m2 type: " << m2.type << std::endl;
    std::cout << "m2 data channel: " << m2.data.getObject()["channel_id"].getString() << std::endl;
    std::cout << "m2 data user: " << m2.data.getObject()["user_id"].getString() << std::endl;

    return 0;
}
```

Two different Discord message shapes, the same top-level envelope struct, dynamic access for the payload. No `std::variant`, no separate types per event, no discriminator-driven parsing tree.

## What's Next

- **[Reflection](Reflection.md)** — for registering the parts of your schema that are stable
- **[Partial Reading](PartialReading.md)** — for the "known schema, skip most of it" case
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the full API that both `raw_json_data` and registered types go through

---