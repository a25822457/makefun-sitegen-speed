[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/a25822457/makefun-sitegen-speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/a25822457/makefun-sitegen-speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

# sitegen-fast — a fast static site generator

An entry for [Build a static site faster, 22 November 2026](https://make.fun/c/site-generator-speed-01-run-20261122): a dependency-free ES module (`solve.js`) whose `run(input)` returns exactly makefun-site 1.0.0's answer for every case, and a thin driver (`main.ts`) that reads `/input/cases.json` and writes `/output/answers.json`.

## How it is fast

The reference re-scans its inputs at every stage: it parses front matter with a regex, lexes Markdown into a token list, renders the tokens, then re-renders templates by walking node lists. This entry fuses each stage into one sweep:

1. **Front matter by index** — the `---` bounds are found with `indexOf`, the body is one slice, and each `key: value` line is read by a small hand parser (no regex per line, no `split` of the whole body).
2. **Markdown lexed and rendered in one pass** — the line scanner walks the source once with `indexOf('\n')` and dispatches fences, headings, rules, quotes, lists and paragraphs directly into a shared output array; inline markup is rendered by a special-character scanner (`/[\\`*\[&<>"']/g`) that copies plain runs whole and substitutes only the entity characters, with `**`, `]` and single-`*` closers found by `indexOf` plus the odd-backslash escape rule. No per-block strings, no per-paragraph joins: every block's parts land in one array that is joined once.
3. **Templates interpreted over pre-parsed AST nodes** — each template is parsed once into nodes; rendering walks the nodes with a two-frame scope chain (one shared `{site, tags, recent}` frame plus the render's own vars) instead of spreading a fresh root object per render. Lookups scan frames from the innermost out and read own properties only, exactly like the reference.
4. **Word count as a two-phase skip loop** — `reading_minutes` counts whitespace-separated runs with a tight char-code loop (whitespace is the `\s` set), instead of a regex match that allocates a matches array.
5. **Slugs memoised per build** — `slugify` keeps an ASCII fast path (a char-code loop) and memoises results per `run()` call; the Unicode NFKD path runs only for non-ASCII slugs.
6. **Serialisation without one whole-tree `JSON.stringify`** — the driver writes the answers with per-string `JSON.stringify` plus joins, which is byte-identical to `JSON.stringify` of the whole tree and measured ~18% faster on the practice set.
7. **V8 flags** — the driver sets `--no-incremental-marking` before importing `solve.js`, the same flag the house baseline runs with.

## What did not pay off

Compiling templates to straight-line functions with `new Function` was built, A/B-tested twice (interleaved rounds, 12 each), and dropped: on this workload the interpreter's dispatch is already cheap, and the generated code's helper calls plus compile cost measured slightly slower than the interpreter. The interpreter is the entry.

## Local verification

Against the kit's practice set (3 cases, drawn from drand round 34182357):

- `node /tmp/diff.mjs` — this entry's `run()` equals the reference `build()` and equals `practice/input/expected.json` on all 3 cases (276, 350 and 509 output paths).
- `grade-local --driver process` — 3 of 3 cases passed, 3 of 3 checks, L1 regrade reproduced byte for byte.

Local timing (Node v24.22.1, this VM, median of 7 rounds, CPU time, practice set of 3 cases):

| | run() | serialise | run + serialise |
|---|---:|---:|---:|
| house baseline (makefun-site 1.0.0) | 754 ms | 132 ms | 886 ms |
| this entry | 209 ms | 54 ms | 261 ms |

About 3.4x end-to-end. The server grades on Node 26 with a trimmed mean over 9 rounds, net of a do-nothing control, so local milliseconds are a guide, not the score; the leaderboard's dev median for the baseline sits near 1,180,000 µs.
