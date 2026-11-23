// sitegen-speed v4: a correct drop-in for makefun-site 1.0.0.
//
// run(input) returns exactly the answer makefun-site 1.0.0's build returns
// for one case's input ({ site, templates, files }): a JSON object mapping
// every path the build writes (each page's index.html, the index and tag
// pages, build-report.txt and each static file) to that file's text.
//
// The build itself runs in the bundled native module (sitegen.node, compiled
// from the C sources in sitegen.c, which implement the makefun-site pipeline
// — front matter, the template dialect, the Markdown subset, the slug rule
// and the report — byte for byte). The native entry processes a whole
// cases.json (an array of cases) in one call and returns the answers JSON
// as bytes; this module only loads that sibling .node file; it has no npm
// dependencies. main.ts uses the same module directly on the timed path;
// run() here is the contract entry point and the fallback if the fast path
// is unavailable.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { buildCases } = require('./sitegen.node');

export function run(input) {
  // The native entry processes a whole cases.json (an array of cases) in one
  // call; wrap this single case, run it, and return its answer.
  const cases = Buffer.from(JSON.stringify([{ id: 'solve', input }]), 'utf8');
  const out = buildCases(cases);
  return JSON.parse(out.toString('utf8'))[0];
}
