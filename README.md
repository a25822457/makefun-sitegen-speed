[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/a25822457/makefun-sitegen-speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/a25822457/makefun-sitegen-speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

# sitegen-zero-copy — a static site generator with no intermediate values

An entry for [Build a static site faster, 22 November 2026](https://make.fun/c/site-generator-speed-01-run-20261122): a from-scratch implementation of makefun-site 1.0.0 in C, loaded as a Node N-API addon. `solve.js` exposes the contract `run(input)`; `main.ts` is the driver the harness runs — it reads `/input/cases.json` as raw bytes, hands them to the native module once, and writes `/output/answers.json`.

## The idea

The reference builds three trees on the way to an answer: a JSON DOM, a per-post value tree, and a template scope chain — and every string is copied between them. This entry builds none of them:

1. **JSON is parsed in place.** A string in the input stays a pointer into the input buffer; it is unescaped lazily, and only when it actually holds a backslash. No DOM, no per-field allocation.
2. **Pages are structs of spans, not objects.** A post is a flat struct whose fields are spans into the input plus a few small computed values. There is no Val, no hash table, no property lookup anywhere in the build.
3. **Templates resolve paths at render time, against the scope chain.** A `{{ p.title }}` looks up the loop frame by name and reads the field straight out of the page struct. Includes share the caller's scope, exactly like the reference.
4. **Rendering writes straight into the output buffer.** Markdown renders in one streaming pass into the page's HTML span; layouts append into a buffer that becomes the answer pair's value. The only copies are the ones the answer format itself requires.
5. **One bump arena per build.** All per-case memory comes from a single 128 MB arena that is reset, not freed, between cases, so nothing is ever malloc'd or free'd in the hot path.

The result is byte-for-byte identical to makefun-site 1.0.0 on the practice set (3/3 cases, every output path, including the build report and its exact refusal strings), verified by `grade-local`.

## Layout

- `sitegen.c` — the whole implementation: JSON in-place parser, front-matter parser, Markdown renderer, template compiler and renderer, slugifier, and the build pipeline, plus the N-API glue (`build(jsonString)` → a JS object).
- `sitegen.node` — the compiled addon (build with `cc -O2 -shared -fPIC -I/usr/include/node sitegen.c -o sitegen.node`).
- `solve.js` — the dependency-free contract module: `run(input)` hands one
  case's input to the addon and returns its answer object.
- `main.ts` — the harness driver: reads `cases.json` as raw bytes, answers
  every case in one native call (`buildCases`), and writes the returned
  answers JSON straight to `answers.json` — no JavaScript JSON work on the
  timed path.

## Notes on the reference semantics

A few details of makefun-site 1.0.0 that this implementation had to match exactly, because the scorer compares by JSON value and the report by exact string:

- Front matter refuses in a fixed order (front-matter error → own directory → post without a date → missing layout → draft), and two of its errors — `key "X" is given twice` and `key "X" has no value` — carry no colon, while the per-key parse errors do.
- A page's displayed tag name comes from the tag map (the first spelling met in the posts' order), not from the page's own front matter.
- Slugify is NFKD with a closed table: Æ/Œ/Ł do not decompose, ﬁ does, fullwidth letters lower, and a digit–gap–digit sequence such as ¼ becomes `1-4`.
- Template paths resolve at render time, so `{% include "card" %}` inside `{% for p in posts %}` sees `p`.
