// sitegen-speed v4 driver: read cases.json as raw bytes, answer every case
// in native code in one call (no JS JSON round-trip), write answers.json.
// Falls back to the JS run() in solve.js if the native module is unavailable.
import { readFileSync, writeFileSync } from 'node:fs';
import { setFlagsFromString } from 'node:v8';
import { createRequire } from 'node:module';

// V8's incremental marking off, set before solve.js is imported so all of its
// work runs under it (the rules: the house baseline runs with
// --no-incremental-marking, and an entry may tune V8's flags too).
setFlagsFromString('--no-incremental-marking');

const inputDir = process.env.MAKEFUN_INPUT_DIR || '/input';
const outputDir = process.env.MAKEFUN_OUTPUT_DIR || '/output';

const input = readFileSync(`${inputDir}/cases.json`);
let out: Buffer | null = null;
try {
  const require = createRequire(import.meta.url);
  const { buildCases } = require('./sitegen.node');
  out = buildCases(input);
} catch {
  out = null;
}
if (out == null) {
  const { run } = await import('./solve.js');
  const cases = JSON.parse(input.toString('utf8'));
  const answers = cases.map((c: any) => run(c.input));
  out = Buffer.from(JSON.stringify(answers));
}
writeFileSync(`${outputDir}/answers.json`, out);
