## What Known Order Actually Does

**The core mechanism is the `antiHashStatesNew` thread-local array.** Each object type gets an array of size `memberCount`, indexed by the member's **declaration order position** (`json_entity_type::index`). Each entry stores **which member the parser expects to find AT that position**.

Walking through `json_entity_parse::processIndex`:

1. **First member (index 0):** parser expects `antiHashStatesNew[0]`, which starts as `0`. If it matches → happy path, move on.
2. **If it doesn't match** (JSON member isn't at expected position), fall back to the compile-time hash map to find which member IS there.
3. **When the hash map finds it, update the memory:** `antiHashStatesNew[json_entity_type::index] = indexNew2`. So next time this exact JSON shape comes through, position N will remember what member actually shows up there.

**This is genuinely brilliant.** It's not "assume declaration order" — it's "assume the LAST seen order for this position, and self-correct if wrong." Adaptive memoization at parse position level.

## The Fast-Fast Path (minified + knownOrder + string_view_ptr)

There's a SPECIAL fast path when ALL of: `options.minified && options.knownOrder && !structural_context<context_type>`:

```cpp
static constexpr auto memberLiteral = makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
```

This constructs a compile-time literal like `,"key":` (or `"key":` for index 0) and does a **single `memcmp` against the raw stream position**. No hash lookup, no colon-collection, no whitespace skipping — just a fused compare-and-advance. If it matches, member parsed. If not, fall through.

This is the "if you promise your JSON matches your declaration order AND is minified, we can skip almost the entire parser dispatch machinery" mode. That's the peak-performance path.

## What Happens on Order Mismatch

**Nothing bad.** The parser falls through to the hash map, finds the member, parses it, and updates the memoization for next time. Zero correctness cost, just some perf cost that self-heals on subsequent parses of the same shape.

## When It's Cheap vs. Expensive

- **Same JSON schema, same field order every time (typical case for machine-generated JSON, API responses, log lines):** the memoization table converges after 1-2 parses, subsequent parses are pure fast-path
- **Same schema, randomly-ordered fields:** memoization thrashes, you pay hash-map cost on every field (still correct, just no speedup)
- **Minified fast-fast path (`minified + knownOrder + non-partialRead`):** the compile-time literal compare skips even more work

Now I have everything I need. Here's **Known Order Parsing** 👇

---

# Known Order Parsing

Known Order is an adaptive parsing mode that speeds up hot paths where the same JSON shape shows up repeatedly. It's turned on with a single flag and has zero correctness cost — worst case it degrades to the normal parse path.

## The Idea

Most real-world JSON is machine-generated. An API returns the same fields in the same order every call. A log line writes its fields the same way every time. A message schema arrives on a socket with fields in the order the sender's struct declared them. In these cases, the field-order in the JSON matches the field-order you registered with `createValue`.

Known Order exploits this. When the parser is walking your object and expects to see field N, it can **check the raw stream directly for the expected key at position N** — no hash lookup needed. If it's there, parsing continues on a fast path. If it isn't, the parser falls through to the normal hash-map lookup.

## Turning It On

```cpp
parser.parseJson<jsonifier::parse_options{ .knownOrder = true }>(data, json);
```

That's it. Everything else — how you registered your types, the shape of your data, how you handle errors — stays exactly the same.

## What Happens Under the Hood

The parser maintains a small thread-local table for each object type, one entry per registered field. Each entry stores **which field the parser most recently found at that position in the JSON**.

Walk-through for a struct with three fields `{ id, name, tags }`:

1. **First parse.** Parser expects `id` at position 0. If the JSON has `"id"` first, fast path — no lookup needed, table entry 0 already says `id`. If the JSON has `"name"` first instead, the parser falls through to the hash map, finds `name`, updates table entry 0 to say `name`.
2. **Second parse of the same shape.** If position 0 was `name` last time, that's what the parser now expects to find there. If the JSON matches, fast path. If not, fall through and update.
3. **Steady state.** After one or two parses of a stable JSON shape, the table has converged and every field takes the fast path.

**The result: known-order parsing is self-tuning.** You don't need to guarantee any specific order — you just need the same shape to show up more than once. If field order shifts between parses, the parser silently re-learns.

## When It's Worth Turning On

**Almost always, if you're parsing the same schema more than once.** The convergence cost is one or two parses of the "wrong" order; after that you're on the fast path indefinitely.

Some places where it's a definite win:

- **API clients** — the server returns the same JSON shape on every response
- **Log ingestion** — every log line has the same fields in the same order
- **Message deserialization** — protocol messages have fixed layouts
- **Batch parsing** — parsing an array of many objects of the same type, all sharing a shape

Places where it might be neutral (but still not harmful):

- **User-authored JSON** — humans reorder fields when editing, so the memoization keeps re-learning
- **Deeply nested unique shapes** — each nested object type has its own table, so many one-off nested types won't converge

## The Fast-Fast Path

There's an even faster mode when you combine `knownOrder = true` with `minified = true` (and you're not using `partialRead`). In this mode, the parser generates a compile-time string literal for each field including the surrounding punctuation — for a field named `id`, at position 1, that literal is `,"id":`. Parsing the field becomes a single `memcmp` against the raw stream and a pointer advance. No colon-collection, no whitespace-skipping, no hash lookup, no dispatch table.

For minified server-to-server JSON with stable schemas, this is the peak-performance path.

```cpp
parser.parseJson<jsonifier::parse_options{
    .knownOrder = true,
    .minified = true
}>(data, json);
```

## What It Doesn't Do

**It doesn't require the JSON to match declaration order.** The fast path is taken *when* the JSON matches the memoized order, but the parser always handles arbitrary orders correctly. There is no "known order violation" error — mismatched orders just cost a hash-map lookup and update the memoization.

**It doesn't skip validation.** All the parser's normal correctness checks — bounds, delimiters, types, escapes — still run.

**It doesn't require every field to be present.** Missing optional fields work fine. Extra unknown fields work fine. The memoization only tracks fields you registered.

**It doesn't matter for arrays or primitives.** Known Order only applies to object parsing (registered types). Arrays, strings, numbers, booleans, and nulls are unaffected.

## Interaction With Other Options

- **`partialRead`** — Known Order applies in both partial and non-partial modes, but the fastest fast-fast path (with the fused string-literal compare) only kicks in for non-partial mode.
- **`minified`** — Combining with `knownOrder` unlocks the fast-fast path described above.
- **`validateUtf8`** — Fully compatible. Known Order doesn't touch string content, only object key dispatch.

## A Simple Test

To see whether Known Order is helping your workload, parse a representative sample twice and time it:

```cpp
auto t1 = clock::now();
parser.parseJson<jsonifier::parse_options{ .knownOrder = false }>(data, json);
auto t2 = clock::now();

parser.parseJson<jsonifier::parse_options{ .knownOrder = true }>(data, json);
parser.parseJson<jsonifier::parse_options{ .knownOrder = true }>(data, json);
auto t3 = clock::now();
parser.parseJson<jsonifier::parse_options{ .knownOrder = true }>(data, json);
auto t4 = clock::now();
```

The third `knownOrder = true` call (`t3 → t4`) is the steady-state number. Compare against the `knownOrder = false` baseline (`t1 → t2`) to see the speedup on your specific data.

## What's Next

- **[Partial Reading](PartialReading.md)** — for JSON where the schema might not be fully known ahead of time
- **[Optimizing For Minified JSON](Optimizing_For_Minified_Json.md)** — the `minified` flag details, including the fast-fast-path interaction
- **[Serializing & Parsing](Usage_Serializing_Parsing.md)** — full reference on all parse options

---