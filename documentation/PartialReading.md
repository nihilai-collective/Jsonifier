# Partial Reading

Partial Reading enables a two-stage parser architecture that's dramatically faster when you only care about a subset of the fields in a JSON document. It's turned on with `partialRead = true`.

## The Idea

The default parser walks the JSON input character by character. When it hits a value it doesn't need — an unregistered field, a nested object you're skipping, a huge array of primitives — it still has to scan through every character to find where that value ends. For documents where you're extracting a few fields from a large payload, that scanning cost dominates.

Partial Reading trades that per-character scan for a two-stage pipeline:

1. **Stage-1** does a single fast SIMD-accelerated scan over the whole input, building a tape of pointers to every structural character (`{`, `}`, `[`, `]`, `"`, `,`, `:`).
2. **Stage-2** walks that tape instead of the raw string. Skipping a value becomes a bracket-depth walk over structural indices — no character-by-character scanning, no whitespace handling, no string-content reading.

For documents where you skip most of what you're parsing, this can be a large speedup. For documents where you consume every field, the extra stage-1 cost usually isn't worth it.

## Turning It On

```cpp
parser.parseJson<jsonifier::parse_options{ .partialRead = true }>(data, json);
```

## When It's Worth It

**Turn it on when you're parsing large JSON documents and only reading a small fraction of the fields.**

Real-world examples:

- **API responses with a lot of metadata you don't need.** A Twitter status payload has dozens of fields; if you only want `text` and `screen_name`, Partial Reading lets you skip everything else efficiently.
- **Deeply nested payloads where you extract a summary.** Large GitHub events, log aggregations, or telemetry blobs where you drill into specific paths.
- **Extracting a few fields from every element of a big array.** If each element has thirty fields and you want three, you're skipping 27 fields per element — that adds up fast.
- **Streaming / pipeline stages that filter down.** Early stages that pull identifiers or routing information from full messages.

**Leave it off when you're parsing the whole document.** If your registered struct covers every field in the JSON, the default parser is faster — it has no stage-1 cost, just goes straight to reading values.

## A Concrete Example

Here's a partial view of the Twitter benchmark payload from Jsonifier's test suite. The full `twitter_message` has hundreds of fields per status; the partial version pulls only three:

```cpp
struct user_data_partial {
    std::string screen_name{};
};

struct status_data_partial {
    std::string text{};
    user_data_partial user{};
    int64_t retweet_count{};
};

struct twitter_partial_message {
    std::vector<status_data_partial> statuses{};
};

template<> struct jsonifier::core<user_data_partial> {
    using value_type = user_data_partial;
    static constexpr auto parseValue = createValue<&value_type::screen_name>();
};

template<> struct jsonifier::core<status_data_partial> {
    using value_type = status_data_partial;
    static constexpr auto parseValue = createValue
        &value_type::text,
        &value_type::user,
        &value_type::retweet_count>();
};

template<> struct jsonifier::core<twitter_partial_message> {
    using value_type = twitter_partial_message;
    static constexpr auto parseValue = createValue<&value_type::statuses>();
};

int main() {
    jsonifier::jsonifier_core<> parser;
    twitter_partial_message result;

    parser.parseJson<jsonifier::parse_options{ .partialRead = true }>(result, json);
}
```

You get back a `twitter_partial_message` with just the three fields per status you asked for, and everything else in the source JSON is efficiently skipped via the structural tape.

## What It Doesn't Change

**Correctness is identical.** Partial Reading is a performance optimization — the parser still validates JSON structure, still handles UTF-8, still respects `maxDepth`, still reports errors the same way. If the JSON is malformed, you get the same errors regardless of `partialRead`.

**Your registered types don't change.** A `core<T>` specialization written for the default parser works for Partial Reading unchanged. Same `createValue`, same `makeJsonEntity`, same nesting rules.

**Unregistered fields are always skipped.** This is true with or without `partialRead`. The difference is just how efficiently they're skipped.

## Interaction With Other Options

- **`knownOrder`** — Works with Partial Reading. The adaptive memoization applies to whichever fields you did register. Some of the fastest-fast-path optimizations described in [Known Order Parsing](Known_Order.md) are specific to the non-partial mode, so if you're on the peak-throughput path, benchmark both.
- **`minified`** — Fully compatible. Stage-1 still runs; stage-2 skips the whitespace-handling paths.
- **`validateUtf8`** — Fully compatible.
- **`nullTerminated`** — Same rules as the default parser. See the [Serializing & Parsing](Usage_Serializing_Parsing.md#-the-nullterminated-footgun) warning.

## When to Measure

The break-even point between the default parser and Partial Reading depends on your payload shape and how much of it you consume. Rough guidance:

- **Consuming < 30% of fields:** Partial Reading is usually a clear win.
- **Consuming > 70% of fields:** the default parser is usually faster.
- **Between:** measure both. Payload size, nesting depth, and string-heavy vs. number-heavy content all shift the crossover.

The measurement pattern is straightforward — parse the same input both ways and time it:

```cpp
auto t1 = clock::now();
parser.parseJson(data, json);
auto t2 = clock::now();

parser.parseJson<jsonifier::parse_options{ .partialRead = true }>(data, json);
auto t3 = clock::now();
```

## What's Next

- **[Known Order Parsing](Known_Order.md)** — often combined with Partial Reading for repeated-shape workloads
- **[Optimizing For Minified JSON](Optimizing_For_Minified_Json.md)** — the `minified` flag details
- **[Parsing Arbitrary Data](Parsing_Arbitrary_Data.md)** — for cases where the schema isn't fully known and `raw_json_data` fits better than Partial Reading
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — full parse-options reference

---