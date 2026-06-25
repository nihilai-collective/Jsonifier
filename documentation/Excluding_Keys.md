# Excluding Keys at Runtime

Sometimes you need to control which fields participate in serialization or parsing based on runtime state — omitting a password field from log output, hiding internal-only fields when serializing for an external API, or skipping fields you know you don't need when parsing. Jsonifier supports this with a per-instance excluded-keys mechanism: add a `jsonifierExcludedKeys` member to your struct, put the JSON key names you want skipped into it, and both serialization and parsing will respect the list.

## The Basics

Add a set-like member named `jsonifierExcludedKeys` to your struct and put the JSON key names you want to exclude into it:

```cpp
struct my_object {
    std::string name{};
    int32_t age{};
    std::string secret{};

    std::unordered_set<std::string> jsonifierExcludedKeys{ "secret" };
};

template<> struct jsonifier::core<my_object> {
    using value_type = my_object;
    static constexpr auto parseValue = createValue
        &value_type::name,
        &value_type::age,
        &value_type::secret>();
};
```

When you serialize this object, the `secret` field is skipped. When you parse into it, incoming `"secret"` fields are read past and their values discarded — the `secret` member keeps whatever it had before the parse.

```cpp
jsonifier::jsonifier_core<> parser;
my_object obj{ "John", 30, "hunter2" };

std::string output;
parser.serializeJson(obj, output);
// output: {"name":"John","age":30}
```

## What Container Types Work

jsonifierExcludedKeys can be any container that exposes a find() method, an end() sentinel, and a nested key_type. That means all of the following work:

std::set<std::string>
std::unordered_set<std::string>
std::set<jsonifier::string>
std::unordered_set<jsonifier::string>
std::map<std::string, T> for any T
std::unordered_map<std::string, T> for any T

The map types work because the exclusion check is just a key lookup — the mapped values are ignored. If you already have an unordered_map around for some other purpose (say, per-key metadata or per-key exclusion rules), you can reuse it directly as the excluded-keys set.

Pick whichever container matches your usage pattern. For a small handful of excluded keys, std::set is fine. For frequently-modified lists or large exclusion sets, std::unordered_set gives you O(1) lookup.

## How It Works

Both the parser and the serializer detect the `jsonifierExcludedKeys` member via a compile-time concept check. If your struct has it, the generated parse and serialize paths include a lookup against the set on every field; if it doesn't, the check compiles out entirely and there's no cost.

**During serialization**, before emitting each registered field, the serializer checks whether the field's JSON key name is in the excluded set. If it is, the field is skipped — no key, no value, no comma. The output JSON simply doesn't contain that field.

**During parsing**, when the parser dispatches to a registered field, it checks whether that field's JSON key name is in the excluded set. If it is, the parser skips the value in the input (using the fast structural-skip when `partialRead = true`, or the scalar skip otherwise) and continues to the next field. The corresponding member on your struct is left untouched.

The check uses the **JSON key name**, not the C++ member name. If you used `makeJsonEntity` to remap a member, put the JSON name in the excluded set:

```cpp
struct catalog {
    std::string schema_version{};
    std::unordered_set<std::string> jsonifierExcludedKeys{};
};

template<> struct jsonifier::core<catalog> {
    using value_type = catalog;
    static constexpr auto parseValue = createValue
        makeJsonEntity<&value_type::schema_version, "schema-version">()>();
};

catalog c;
c.jsonifierExcludedKeys.insert("schema-version");
```

## Runtime-Dynamic Exclusion

The exclusion set is a normal runtime member — you can mutate it whenever you want. This makes the mechanism useful for context-dependent output:

```cpp
my_object obj{ "John", 30, "hunter2" };

std::string public_json, internal_json;

obj.jsonifierExcludedKeys.insert("secret");
parser.serializeJson(obj, public_json);
// public_json: {"name":"John","age":30}

obj.jsonifierExcludedKeys.clear();
parser.serializeJson(obj, internal_json);
// internal_json: {"name":"John","age":30,"secret":"hunter2"}
```

Same object, different output based on runtime state. No separate types, no serialization-time flags — just mutate the set.

## Full Example

```cpp
#include <jsonifier>
#include <unordered_set>
#include <iostream>

struct user_profile {
    std::string name{};
    std::string email{};
    std::string password_hash{};
    int64_t created_at{};

    std::unordered_set<std::string> jsonifierExcludedKeys{};
};

template<> struct jsonifier::core<user_profile> {
    using value_type = user_profile;
    static constexpr auto parseValue = createValue
        &value_type::name,
        &value_type::email,
        &value_type::password_hash,
        &value_type::created_at>();
};

int main() {
    jsonifier::jsonifier_core<> parser;
    user_profile user{ "Jane", "jane@example.com", "$2b$12$....", 1728000000 };

    user.jsonifierExcludedKeys = { "password_hash", "email" };
    std::string public_view;
    parser.serializeJson(user, public_view);
    std::cout << "Public: " << public_view << std::endl;

    user.jsonifierExcludedKeys.clear();
    std::string internal_view;
    parser.serializeJson(user, internal_view);
    std::cout << "Internal: " << internal_view << std::endl;

    return 0;
}
```

Output:

```
Public: {"name":"Jane","created_at":1728000000}
Internal: {"name":"Jane","email":"jane@example.com","password_hash":"$2b$12$....","created_at":1728000000}
```

## Zero Cost When Not Used

If your struct doesn't have a `jsonifierExcludedKeys` member, the concept check fails at compile time and none of the exclusion code is generated. Structs that don't opt in pay nothing — no extra branch, no extra field lookup, no extra generated code.

## Interaction With Other Options

- **`partialRead`** — Fully compatible. Excluded keys are skipped using the structural-tape skip path when `partialRead = true`, which is faster than the default scalar skip.
- **`knownOrder`** — Fully compatible. The exclusion check happens after key dispatch, so it doesn't disrupt the known-order fast path for keys that aren't excluded.
- **`makeJsonEntity`** — Compatible; use the JSON key name (not the C++ member name) in the excluded set.

## What's Next

- **[Reflection](Reflection.md)** — full details on `makeJsonEntity` and how JSON key names get bound to C++ members
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — the runtime API that respects the exclusion set
- **[Parsing Arbitrary Data](Parsing_Arbitrary_Data.md)** — for when you don't want to declare a schema at all

---