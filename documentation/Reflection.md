# Reflection

Jsonifier uses compile-time reflection to map between your C++ structs and JSON. There are no macros, no code generation, no runtime type registry — you register a type by specializing a template, and the compiler takes it from there.

## The Basics

To make a type parseable and serializable, specialize `jsonifier::core<T>` and give it a static `parseValue` built by `createValue<...>()`:

```cpp
struct available_tag {
    std::string name{};
    bool moderated{};
    int32_t id{};
};

template<> struct jsonifier::core<available_tag> {
    using value_type = available_tag;
    static constexpr auto parseValue = createValue<
        &value_type::name,
        &value_type::moderated,
        &value_type::id>();
};
```

That's the entire registration. Now `available_tag` can be parsed and serialized:

```cpp
jsonifier::jsonifier_core<> parser;
available_tag tag;
std::string json = R"({"name":"urgent","moderated":true,"id":42})";
parser.parseJson(tag, json);
```

## How `createValue` Works

`createValue` is variadic and takes member pointers as non-type template parameters. Each `&value_type::member` you list becomes a field the parser and serializer know about. Members you don't list are ignored during parsing and skipped during serialization.

The JSON key for each member defaults to the C++ member name — `&value_type::moderated` maps to the JSON key `"moderated"`. Jsonifier extracts this name at compile time from the member pointer itself (using `std::source_location`), so you never write the string literally.

The order you pass the members in matters for the [Known Order](Known_Order.md) optimization but is otherwise cosmetic — the parser handles keys in any order by default.

## Custom JSON Key Names

When a JSON key isn't a legal C++ identifier — kebab-case, digit-prefixed, reserved words, or anything else — use `makeJsonEntity` to map a member to a custom string:

```cpp
struct twitter_user_data {
    std::string name{};
    bool protectedVal{};
};

template<> struct jsonifier::core<twitter_user_data> {
    using value_type = twitter_user_data;
    static constexpr auto parseValue = createValue<
        &value_type::name,
        makeJsonEntity<&value_type::protectedVal, "protected">()>();
};
```

Here the C++ member `protectedVal` maps to the JSON key `"protected"`. This is entirely compile-time — the mapping is baked into the binary with zero runtime cost.

You can freely mix bare member pointers and `makeJsonEntity` results inside a single `createValue`, in any order:

```cpp
static constexpr auto parseValue = createValue<
    &value_type::id,
    makeJsonEntity<&value_type::is_public, "public">(),
    &value_type::name,
    makeJsonEntity<&value_type::is_private, "private">()>();
```

Common use cases for `makeJsonEntity`:

- **Kebab-case JSON keys** — `"schema-version"`, `"content-type"`
- **Digit-prefixed keys** — `"337100890"`, `"2fa_enabled"`
- **C++ reserved words** — `"public"`, `"private"`, `"class"`, `"typename"`
- **Any other JSON key that isn't a legal C++ identifier** — dots, spaces, hyphens, non-ASCII characters

## Nested Structs

Reflection composes. Register each struct once, and you can nest them freely:

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

template<> struct jsonifier::core<event> {
    using value_type = event;
    static constexpr auto parseValue = createValue<
        &value_type::id,
        &value_type::name,
        &value_type::logo,
        &value_type::topicIds>();
};

template<> struct jsonifier::core<catalog> {
    using value_type = catalog;
    static constexpr auto parseValue = createValue<
        &value_type::events,
        makeJsonEntity<&value_type::schema_version, "schema-version">()>();
};
```

The parser walks into `event` values inside the map automatically because `event` is registered. The same principle applies to `std::vector<event>`, `std::optional<event>`, `std::shared_ptr<event>`, or any other container of a registered type.

## Empty Structs

For a struct with no fields — placeholder types, tag structs, or JSON objects you intentionally want to accept-and-ignore — use `createValue()` with no arguments:

```cpp
struct names {};

template<> struct jsonifier::core<names> {
    using value_type = names;
    static constexpr auto parseValue = createValue();
};
```

The parser will accept any JSON object at this position and discard its contents.

## Supported Member Types

Out of the box, Jsonifier handles:

- **Primitives** — `bool`, `char`, all integer types (`int32_t`, `uint64_t`, etc.), `float`, `double`
- **Strings** — `std::string`, `std::string_view`, `jsonifier::string`
- **Containers** — `std::vector`, `std::array`, `jsonifier::internal::array`, `std::map`, `std::unordered_map`, `std::tuple`
- **Smart pointers** — `std::unique_ptr`, `std::shared_ptr`
- **Wrappers** — `std::optional`, `std::variant`
- **Enums** — serialized as their underlying integer value
- **Null placeholders** — `std::nullptr_t` for fields that are always `null` in the JSON
- **Nested registered types** — any type with its own `jsonifier::core<T>` specialization
- **Arbitrary JSON** — `jsonifier::raw_json_data` for members that hold unstructured JSON verbatim (see [Parsing Arbitrary Data](Parsing_Arbitrary_Data.md))

## A Real-World Example

Here's a slice from Jsonifier's own test suite — the CitmCatalog benchmark payload — showing everything working together:

```cpp
struct event {
    std::nullptr_t description{};
    int64_t id{};
    std::optional<std::string> logo{};
    std::string name{};
    std::vector<int64_t> subTopicIds{};
    std::nullptr_t subjectCode{};
    std::nullptr_t subtitle{};
    std::vector<int64_t> topicIds{};
};

struct audience_sub_category_names {
    std::string the337100890{};
};

template<> struct jsonifier::core<event> {
    using value_type = event;
    static constexpr auto parseValue = createValue<
        &value_type::description,
        &value_type::id,
        &value_type::logo,
        &value_type::name,
        &value_type::subTopicIds,
        &value_type::subjectCode,
        &value_type::subtitle,
        &value_type::topicIds>();
};

template<> struct jsonifier::core<audience_sub_category_names> {
    using value_type = audience_sub_category_names;
    static constexpr auto parseValue = createValue<
        makeJsonEntity<&value_type::the337100890, "337100890">()>();
};
```

That last one is a fun edge case — the JSON has a key that's literally the digit string `"337100890"`, which isn't legal as a C++ identifier. `makeJsonEntity` handles it at compile time.

## Under the Hood

Everything on this page is `constexpr` and `consteval`. When you write `createValue<&value_type::name, &value_type::moderated>()`, here's roughly what happens:

1. Each member pointer is captured as a non-type template parameter.
2. Its name is extracted from `std::source_location::current().function_name()` — the compiler's own pretty-printed function signature carries the member name, and Jsonifier parses it out at compile time. The trick varies per compiler (MSVC, GCC, and Clang each format the signature differently), but the mechanism is the same.
3. The resulting `{ member_pointer, key_string }` pairs are packed into a compile-time tuple.
4. That tuple is sorted three different ways at compile time — by original declaration order, by first byte, and by key length — because different hash-map strategies key off different orderings.
5. The parser's hash-map dispatch is generated from these sorted views, specialized for the exact set of keys your struct declares.

The upshot: for a struct with N fields, key lookup during parsing is a compile-time-generated hash-map dispatch specialized to those specific N keys. There is no runtime string hashing, no bucket walk, no dynamic dispatch.

For the deep architectural detail — including how this feeds into the batched-drain fused-scan stage-1 parser — see the [Stage-1 Document](../Batched-Drain.md) and [Full Arch Document](../Two-Stages.md).

The reflection interface itself is adapted from ideas in [Stephen Berry's Glaze library](https://github.com/stephenberry/glaze).

## What's Next

- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — every option available on `parseJson` and `serializeJson`
- **[Known Order Parsing](Known_Order.md)** — when the JSON keys are guaranteed to arrive in declaration order, the parser can skip lookup entirely
- **[Partial Reading](PartialReading.md)** — for JSON where key order isn't guaranteed
- **[Parsing Arbitrary Data](Parsing_Arbitrary_Data.md)** — for the `jsonifier::raw_json_data` escape hatch when the schema isn't fully known

---