<p align="center"><img src="./Logo.png?raw=true" width="450"/></p>

# jsonifier::hash_map

The compile-time hash map from [Jsonifier](https://github.com/nihilai-collective/jsonifier), extracted into one header with no dependencies beyond the standard library. It needs C++20.

```cpp
#include <jsonifier/hash_map.hpp>

using protocols = jsonifier::hash_map<"http", "https", "ftp", "ws", "wss", "file">;

const uint64_t index = protocols::findIndex(input);
if (index < protocols::keyCount) {
	// input is the key declared at position index.
}
```

`findIndex` takes a `std::string_view`. It returns the key's declaration index, or `keyCount` when the key is absent. Every candidate is compared against the real key, so misses are exact.

## Limits

- At most 254 keys.
- The control and index storage is capped at 2048 slots.
- A key set that no strategy can place fails to compile with a `static_assert`.

## Strategy selection

The map picks the first strategy that fits the key set:

| Strategy | Used when |
| --- | --- |
| `empty`, `single_element` | The set has zero keys or one key. |
| `double_element`, `triple_element` | Two or three keys separate on one byte column. |
| `single_byte` | One column is unique across all keys. |
| `first_byte_and_unique_index` | Each first-byte group has its own unique column. |
| `unique_byte_and_length` | One column XORed with the length is collision free. |
| `unique_per_length` | Each length group has its own unique column. |
| `simd_full_length` | Anything else: a seeded hash selects a bucket, and control bytes are searched with SIMD. |

## SIMD

The instruction set comes from the compiler's own macros, so no configure step is needed: AVX-512BW, AVX2, SSE2, and AArch64 NEON, with a scalar fallback. Pass `-march=native` or `/arch:AVX2` to select a wider path.

## Compile-time limits

Large key sets can exceed the default constant evaluation limits. Raise them with `-fconstexpr-steps` on Clang, `-fconstexpr-ops-limit` on GCC, or `/constexpr:steps` on MSVC.

## Tests

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hash_map_tests
```

## Differences from Jsonifier's parser map

- Keys are declared as string literals rather than through `jsonifier::core<T>`.
- The length comes from the caller, so there is no scan for a closing quote.
- `findIndex` verifies the key itself, where the parser verifies it in a later step.
