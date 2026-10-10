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

For the key at position N of an object, the parser tries three things in order:

1. **Declaration order.** It checks whether the key is the field you declared at position N in `createValue`. If so, that field is parsed directly — no lookup.
2. **The memo.** Otherwise it consults a small `thread_local` table (one per object type, one entry per registered field) recording **which field was most recently found at position N**, and tries that field.
3. **The hash map.** If neither matches, it falls back to the compile-time hash map, parses the field it finds, and writes that field into memo entry N.

Walk-through for a struct with three fields `{ id, name, tags }`:

1. **JSON in declaration order.** `"id"` at position 0 matches step 1 immediately, and so does every following key. The memo is never needed.
2. **JSON in a different but stable order.** If `"name"` arrives first, steps 1 and 2 miss on the first parse, the hash map finds `name`, and memo entry 0 becomes `name`. On later parses of the same shape, step 2 hits.
3. **Steady state.** After one parse of a stable shape, every field is found by step 1 or step 2.

**The result: known-order parsing is self-tuning.** JSON in declaration order is fastest, but you don't need to guarantee any specific order — a stable shape is enough. If field order shifts between parses, the parser silently re-learns.

## When It's Worth Turning On

**Almost always, if your JSON is in declaration order or you parse the same shape repeatedly.** The convergence cost for a non-declaration order is one parse; after that you're on the fast path indefinitely.

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

**It doesn't require the JSON to match declaration order.** The fast path is taken *when* the JSON matches declaration order or the memoized order, but the parser always handles arbitrary orders correctly. There is no "known order violation" error — mismatched orders just cost a hash-map lookup and update the memoization.

**It doesn't skip validation.** All the parser's normal correctness checks — bounds, delimiters, types, escapes — still run.

**It doesn't require every field to be present.** Missing optional fields work fine. Extra unknown fields work fine. The memoization only tracks fields you registered.

**It doesn't matter for arrays or primitives.** Known Order only applies to object parsing (registered types). Arrays, strings, numbers, booleans, and nulls are unaffected.

## Interaction With Other Options

- **`partialRead`** — Known Order applies in both partial and non-partial modes, but the fastest fast-fast path (with the fused string-literal compare) only kicks in for non-partial mode.
- **`minified`** — Combining with `knownOrder` unlocks the fast-fast path described above.

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