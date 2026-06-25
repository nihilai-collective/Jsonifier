# Quick Start

This page gets you parsing and serializing real JSON in five minutes. If you haven't installed Jsonifier yet, see [Installation](Installation.md) first.

## The Setup

Include the header and you're ready:

```cpp
#include <jsonifier>
```

Every Jsonifier operation goes through a `jsonifier::jsonifier_core<>` instance:

```cpp
jsonifier::jsonifier_core<> parser;
```

Create it once, use it for both parsing and serialization. Reusing the same instance across many operations is faster than constructing one per call.

## Define Your Types

Let's model a small event catalog — nested structs, optionals, containers, and one field whose JSON name doesn't match its C++ name:

```cpp
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
```

These are plain C++ structs. No inheritance, no macros, no base class. Jsonifier doesn't touch your types — it reflects on them from the outside.

## Register the Types

For each struct you want to serialize or parse, specialize `jsonifier::core` and list its members using `createValue`:

```cpp
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
```

Two things to notice here.

**First, the members are passed as pointers-to-member as non-type template parameters.** No strings, no macros, no runtime dispatch — the schema is entirely visible to the compiler, which means key lookups collapse into compile-time hash-map dispatch and unused code paths get eliminated before they exist.

**Second, `makeJsonEntity` maps a C++ member to a JSON key with a different name.** Here, `schema_version` in C++ becomes `"schema-version"` in JSON. This is compile-time — the mapping is baked into the binary with zero runtime cost, so you can use kebab-case, digit-prefixed keys (`"337100890"`), reserved C++ words, or any other JSON key that isn't a legal C++ identifier without paying for it at runtime.

Members without `makeJsonEntity` use the C++ name directly as the JSON key.

## Parse

```cpp
std::string json = R"({
    "events": {
        "42": {"id": 42, "name": "Concert", "logo": null, "topicIds": [1, 2, 3]}
    },
    "schema-version": "1.0"
})";

catalog data;
parser.parseJson(data, json);
```

That's it. `parseJson` takes the destination object first, then the source JSON. The nested structs, the map, the optional handling `null`, the renamed key — all handled automatically because the schema was registered.

If parsing fails, `parser.getErrors()` returns a list of errors with source location tracking. See [Error Handling](Errors.md) for the details.

## Serialize

```cpp
std::string output;
parser.serializeJson(data, output);
```

Order is the same: destination first, source second. `output` now holds the JSON representation of `data`.

By default, output is minified. To pretty-print:

```cpp
parser.serializeJson<jsonifier::serialize_options{ .prettify = true }>(data, output);
```

## Full Example

Everything together:

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

    std::string json = R"({"events":{"42":{"id":42,"name":"Concert","logo":null,"topicIds":[1,2,3]}},"schema-version":"1.0"})";
    catalog data;
    parser.parseJson(data, json);

    for (auto& error : parser.getErrors()) {
        std::cout << "Parse error: " << error << std::endl;
    }

    std::string output;
    parser.serializeJson(data, output);
    std::cout << output << std::endl;

    return 0;
}
```

Compile with your usual C++20 setup and Jsonifier linked (`Jsonifier::Jsonifier` if you're using CMake) and it runs.

## What's Next

You now have the basics. From here, pick what you need:

- **[Reflection](Reflection.md)** — the full guide to registering types, including inheritance, custom serialization, and more advanced `makeJsonEntity` patterns
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — every parse and serialize option, including the ones this page skipped
- **[Known Order Parsing](Known_Order.md)** — when you can guarantee JSON keys arrive in declaration order, the parser can skip lookup entirely for a significant speedup
- **[Partial Reading](PartialReading.md)** — for parsing JSON where the key order isn't guaranteed
- **[Error Handling](Errors.md)** — for production error reporting with source locations

If you're benchmarking or shipping to production, [Optimizing For Minified JSON](Optimizing_For_Minified_Json.md) and [CPU Architecture Selection](CPU_Architecture_Selection.md) are also worth a look.
