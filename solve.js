// sitegen-fast: a fast drop-in for makefun-site 1.0.0.
//
// Same answers as makefun-site, byte for byte, built a different way:
//   - one pass over each source instead of a pipeline of re-scans,
//   - Markdown lexed and rendered in a single sweep with a special-character
//     scanner (plain runs are copied, never escaped char by char),
//   - templates rendered by a flat interpreter over pre-parsed nodes with
//     array emission instead of recursive string concatenation,
//   - ASCII fast paths for slugs and HTML escaping, with a per-build cache
//     for repeated slugs.
// No WebAssembly, no native code, no dependencies: plain JavaScript.

/** Code-unit order (never the locale's). */
const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const hasOwn = Object.hasOwn;

// ---------------------------------------------------------------- text

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const ESC_RE = /[&<>"']/g;

/** `text` with & < > " and ' escaped. One native scan; slices only around hits. */
export function escapeHtml(text) {
  ESC_RE.lastIndex = 0;
  let m;
  let out = '';
  let last = 0;
  let found = false;
  while ((m = ESC_RE.exec(text)) !== null) {
    found = true;
    if (m.index > last) out += text.slice(last, m.index);
    out += ESCAPES[m[0]];
    last = m.index + 1;
  }
  if (!found) return text;
  if (last < text.length) out += text.slice(last);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/** A YYYY-MM-DD date in words, from the string alone. */
export function longDate(date) {
  const [y, m, d] = date.split('-');
  return `${Number(d)} ${MONTHS[Number(m) - 1]} ${y}`;
}

/** The slug of pure-ASCII `text` (NFKD is the identity there, no marks to drop). */
function slugAscii(text) {
  let out = '';
  let gap = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    let keep = 0;
    if (c >= 65 && c <= 90) keep = c + 32;
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep === 0) {
      if (out.length > 0) gap = true;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += String.fromCharCode(keep);
  }
  out = out.slice(0, 64).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

/** The slug of `text` with non-ASCII in it: NFKD, combining marks dropped. */
function slugUnicode(text) {
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, 64).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

/** A per-build slug function: ASCII fast path, memoised within one build. */
function makeSlugger() {
  const cache = new Map();
  const nonAscii = /[^\x00-\x7F]/;
  return function slugify(text) {
    const hit = cache.get(text);
    if (hit !== undefined) return hit;
    const out = nonAscii.test(text) ? slugUnicode(text) : slugAscii(text);
    cache.set(text, out);
    return out;
  };
}

/** `slug` made unique against `taken` (and added to it). */
function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');

// ---------------------------------------------------------------- front matter

const FM_KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const FM_LINE = /^([a-z_]+):(?: (.*))?$/;
const FM_DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const FM_NAME = /^[a-z0-9-]+$/;
const FM_SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const FM_TAG = /^[^"[\]]+$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

/** A string value: "..." with \" and \\ its only escapes, or bare. */
function fmString(raw) {
  if (raw.charCodeAt(0) === 34) {
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' };
  return { value: raw };
}

/** One key's value, or why it is refused. */
function fmValue(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = fmString(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = FM_DATE.exec(raw);
      const y = m ? Number(m[1]) : 0;
      const mo = m ? Number(m[2]) : 0;
      const d = m ? Number(m[3]) : 0;
      if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (!raw.startsWith('[') || !raw.endsWith(']')) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',').map((t) => t.trim());
      if (tags.some((t) => t === '' || !FM_TAG.test(t))) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return FM_NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return FM_SLUG.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

const isBlankLine = (text, a, b) => {
  for (let i = a; i < b; i++) {
    const c = text.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
};

/**
 * A source's front matter and body: { data, body }, or { error } with the
 * first reason it is refused. Scans lines by index; the body is one slice.
 */
function parseFrontMatter(text) {
  const nl = text.indexOf('\n');
  if ((nl === -1 ? text : text.slice(0, nl)) !== '---') return { error: 'no front matter' };
  // Find the closing '---' line.
  let pos = nl === -1 ? text.length : nl + 1;
  let lineNo = 2;
  let closePos = -1;
  let closeLine = -1;
  for (;;) {
    if (pos > text.length) break;
    const e = text.indexOf('\n', pos);
    const line = e === -1 ? text.slice(pos) : text.slice(pos, e);
    if (line === '---') {
      closePos = pos;
      closeLine = lineNo;
      break;
    }
    if (e === -1) break;
    pos = e + 1;
    lineNo++;
  }
  if (closePos === -1) return { error: 'the front matter is not closed' };
  const data = {};
  pos = nl === -1 ? text.length : nl + 1;
  lineNo = 2;
  while (lineNo < closeLine) {
    const e = text.indexOf('\n', pos);
    const line = e === -1 ? text.slice(pos) : text.slice(pos, e);
    if (!isBlankLine(text, pos, e === -1 ? text.length : e) && line.charCodeAt(0) !== 35) {
      const m = FM_LINE.exec(line);
      if (!m) return { error: `line ${lineNo} is not "key: value"` };
      const key = m[1];
      const raw = (m[2] ?? '').replace(/[ \t]+$/, '');
      if (!FM_KEYS.includes(key)) return { error: `unknown key "${key}"` };
      if (hasOwn(data, key)) return { error: `key "${key}" is given twice` };
      if (raw === '') return { error: `key "${key}" has no value` };
      const v = fmValue(key, raw);
      if ('error' in v) return v;
      data[key] = v.value;
    }
    pos = e + 1;
    lineNo++;
  }
  if (!hasOwn(data, 'title')) return { error: 'no title' };
  const bodyStart = closePos + 3 + (text.charCodeAt(closePos + 3) === 10 ? 1 : 0);
  return { data, body: text.slice(bodyStart) };
}

// ---------------------------------------------------------------- markdown

/** The characters a backslash escapes. */
const ESCAPABLE = new Set(['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']);
const ESCAPED_OUT = { '\\': '\\', '`': '`', '*': '*', '_': '_', '[': '[', ']': ']', '(': '(', ')': ')', '#': '#', '+': '+', '-': '-', '.': '.', '!': '!', '>': '&gt;' };
const ENTITY = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
/** Every character the inline scanner stops at. */
const INLINE_RE = /[\\`*\[&<>"']/g;

function countBackslashes(s, j) {
  let n = 0;
  let k = j - 1;
  while (k >= 0 && s.charCodeAt(k) === 92) {
    n++;
    k--;
  }
  return n;
}

/** The first '**' at or after `from` outside a backslash escape, or -1. */
function findDoubleStar(s, from) {
  let f = from;
  for (;;) {
    const j = s.indexOf('**', f);
    if (j === -1) return -1;
    if (countBackslashes(s, j) % 2 === 1) {
      f = j + 1;
      continue;
    }
    return j;
  }
}

/** The first ']' at or after `from` outside a backslash escape, or -1. */
function findBracket(s, from) {
  let f = from;
  for (;;) {
    const j = s.indexOf(']', f);
    if (j === -1) return -1;
    if (countBackslashes(s, j) % 2 === 1) {
      f = j + 1;
      continue;
    }
    return j;
  }
}

/** The first single * at or after `from` (one whose run is not **), outside an escape, or -1. */
function findSingleStar(s, from) {
  let f = from;
  for (;;) {
    const j = s.indexOf('*', f);
    if (j === -1) return -1;
    if (countBackslashes(s, j) % 2 === 1) {
      f = j + 1;
      continue;
    }
    if (s.charCodeAt(j + 1) === 42) {
      f = j + 2;
      continue;
    }
    return j;
  }
}

/** `s` rendered inline into `out`: one sweep; plain runs are copied whole, never escaped char by char. */
function inlineInto(s, env, out) {
  const n = s.length;
  let i = 0;
  for (;;) {
    INLINE_RE.lastIndex = i;
    const m = INLINE_RE.exec(s);
    if (m === null) break;
    const j = m.index;
    if (j > i) out.push(s.slice(i, j));
    const c = m[0];
    if (c === '\\') {
      const nx = s[j + 1];
      if (nx !== undefined && ESCAPABLE.has(nx)) {
        out.push(ESCAPED_OUT[nx]);
        i = j + 2;
      } else {
        out.push('\\');
        i = j + 1;
      }
      continue;
    }
    if (c === '`') {
      const k = s.indexOf('`', j + 1);
      if (k > j + 1) {
        out.push('<code>', escapeHtml(s.slice(j + 1, k)), '</code>');
        i = k + 1;
      } else {
        out.push('`');
        i = j + 1;
      }
      continue;
    }
    if (c === '*') {
      if (s.charCodeAt(j + 1) === 42) {
        const k = findDoubleStar(s, j + 2);
        if (k > j + 2) {
          out.push('<strong>');
          inlineInto(s.slice(j + 2, k), env, out);
          out.push('</strong>');
          i = k + 2;
        } else {
          out.push('**');
          i = j + 2;
        }
      } else {
        const k = findSingleStar(s, j + 1);
        if (k > j + 1) {
          out.push('<em>');
          inlineInto(s.slice(j + 1, k), env, out);
          out.push('</em>');
          i = k + 1;
        } else {
          out.push('*');
          i = j + 1;
        }
      }
      continue;
    }
    if (c === '[') {
      const k = findBracket(s, j + 1);
      if (k !== -1 && s.charCodeAt(k + 1) === 40) {
        const m2 = s.indexOf(')', k + 2);
        const target = m2 === -1 ? '' : s.slice(k + 2, m2);
        if (target !== '' && !/\s/.test(target)) {
          const href = env.link(target);
          out.push('<a href="', escapeHtml(href), '">');
          inlineInto(s.slice(j + 1, k), env, out);
          out.push('</a>');
          i = m2 + 1;
          continue;
        }
      }
      out.push('[');
      i = j + 1;
      continue;
    }
    out.push(ENTITY[c]);
    i = j + 1;
  }
  if (i < n) out.push(s.slice(i));
}

/** Trim spaces and tabs at both ends of src[a, b): returns [start, end]. */
function trimBounds(src, a, b) {
  while (a < b && (src.charCodeAt(a) === 32 || src.charCodeAt(a) === 9)) a++;
  while (b > a && (src.charCodeAt(b - 1) === 32 || src.charCodeAt(b - 1) === 9)) b--;
  return [a, b];
}

/** Whether the line src[a, b) opens a fence: ``` + language + spaces/tabs only. */
function fenceOpen(src, a, b) {
  if (src.charCodeAt(a) !== 96 || src.charCodeAt(a + 1) !== 96 || src.charCodeAt(a + 2) !== 96) return null;
  let p = a + 3;
  while (p < b) {
    const ch = src.charCodeAt(p);
    if ((ch >= 65 && ch <= 90) || (ch >= 97 && ch <= 122) || (ch >= 48 && ch <= 57) || ch === 95 || ch === 43 || ch === 45) p++;
    else break;
  }
  const lang = src.slice(a + 3, p);
  while (p < b) {
    const ch = src.charCodeAt(p);
    if (ch !== 32 && ch !== 9) return null;
    p++;
  }
  return lang;
}

/** Whether the line src[a, b) closes a fence: ``` + spaces/tabs only. */
function fenceClose(src, a, b) {
  if (src.charCodeAt(a) !== 96 || src.charCodeAt(a + 1) !== 96 || src.charCodeAt(a + 2) !== 96) return false;
  for (let p = a + 3; p < b; p++) {
    const ch = src.charCodeAt(p);
    if (ch !== 32 && ch !== 9) return false;
  }
  return true;
}

/** A heading line's depth and text bounds, or null (a heading's text is never empty). */
function headingOf(src, a, b) {
  let h = a;
  while (h < b && src.charCodeAt(h) === 35) h++;
  const depth = h - a;
  if (depth < 1 || depth > 6) return null;
  if (h >= b || (src.charCodeAt(h) !== 32 && src.charCodeAt(h) !== 9)) return null;
  const [ts, te] = trimBounds(src, h, b);
  if (ts >= te) return null;
  return { depth, ts, te };
}

/** Whether the line src[a, b) is a thematic break: --- or *** + spaces/tabs only. */
function isHr(src, a, b) {
  if (b - a < 3) return false;
  const c = src.charCodeAt(a);
  if (c !== 45 && c !== 42) return false;
  if (src.charCodeAt(a + 1) !== c || src.charCodeAt(a + 2) !== c) return false;
  for (let p = a + 3; p < b; p++) {
    const ch = src.charCodeAt(p);
    if (ch !== 32 && ch !== 9) return false;
  }
  return true;
}

/** Whether the line src[a, b) ends a paragraph: blank, a fence, a heading or a break. */
function endsParagraph(src, a, b) {
  return isBlankLine(src, a, b) || fenceOpen(src, a, b) !== null || headingOf(src, a, b) !== null || isHr(src, a, b);
}

/**
 * `src` rendered: its HTML (each block on its own lines, a "\n" after the last),
 * its table of contents (every level 2 and 3 heading) and its excerpt (the
 * first paragraph's inline HTML, or ""). One sweep over the lines, by index.
 */
/** `src` rendered into `out` (each block, then a "\n"); returns its table of contents and excerpt. */
function renderMarkdown(src, env, slugger, out) {
  const toc = [];
  const ids = new Set();
  let excerpt = '';
  let hasExcerpt = false;
  const len = src.length;
  let pos = 0;
  while (pos < len) {
    let e = src.indexOf('\n', pos);
    if (e === -1) e = len;
    if (isBlankLine(src, pos, e)) {
      pos = e + 1;
      continue;
    }
    const lang = fenceOpen(src, pos, e);
    if (lang !== null) {
      const lines = [];
      pos = e + 1;
      while (pos < len) {
        let e2 = src.indexOf('\n', pos);
        if (e2 === -1) e2 = len;
        if (fenceClose(src, pos, e2)) {
          pos = e2 + 1;
          break;
        }
        lines.push(src.slice(pos, e2));
        pos = e2 + 1;
      }
      const cls = lang === '' ? '' : ` class="language-${escapeHtml(lang)}"`;
      out.push(`<pre><code${cls}>`);
      for (const l of lines) out.push(escapeHtml(l), '\n');
      out.push('</code></pre>', '\n');
      continue;
    }
    const heading = headingOf(src, pos, e);
    if (heading !== null) {
      const text = src.slice(heading.ts, heading.te);
      const id = uniqueSlug(slugger(text), ids);
      out.push(`<h${heading.depth} id="${id}">`);
      const mark = out.length;
      inlineInto(text, env, out);
      if (heading.depth === 2 || heading.depth === 3) toc.push({ level: heading.depth, id, html: out.slice(mark).join('') });
      out.push(`</h${heading.depth}>`, '\n');
      pos = e + 1;
      continue;
    }
    if (isHr(src, pos, e)) {
      out.push('<hr>', '\n');
      pos = e + 1;
      continue;
    }
    if (src.charCodeAt(pos) === 62) {
      const qlines = [];
      while (pos < len) {
        let e2 = src.indexOf('\n', pos);
        if (e2 === -1) e2 = len;
        if (src.charCodeAt(pos) !== 62) break;
        let s = pos + 1;
        if (src.charCodeAt(s) === 32) s++;
        const [a, b] = trimBounds(src, s, e2);
        if (a < b) qlines.push(src.slice(a, b));
        pos = e2 + 1;
      }
      if (qlines.length === 0) {
        out.push('<blockquote>\n</blockquote>', '\n');
      } else {
        out.push('<blockquote>\n<p>');
        inlineInto(qlines.join('\n'), env, out);
        out.push('</p>\n</blockquote>', '\n');
      }
      continue;
    }
    const c0 = src.charCodeAt(pos);
    const isBullet = (c0 === 45 || c0 === 42) && src.charCodeAt(pos + 1) === 32;
    let d = pos;
    while (d < e && src.charCodeAt(d) >= 48 && src.charCodeAt(d) <= 57) d++;
    const isOrdered = d > pos && d - pos <= 9 && src.charCodeAt(d) === 46 && src.charCodeAt(d + 1) === 32;
    if (isBullet || isOrdered) {
      const items = [];
      const start = isOrdered ? Number(src.slice(pos, d)) : 1;
      let [a, b] = trimBounds(src, isOrdered ? d + 2 : pos + 2, e);
      items.push([a < b ? src.slice(a, b) : '']);
      pos = e + 1;
      while (pos < len) {
        let e2 = src.indexOf('\n', pos);
        if (e2 === -1) e2 = len;
        const cc = src.charCodeAt(pos);
        let matched = false;
        if (isOrdered) {
          let d2 = pos;
          while (d2 < e2 && src.charCodeAt(d2) >= 48 && src.charCodeAt(d2) <= 57) d2++;
          if (d2 > pos && d2 - pos <= 9 && src.charCodeAt(d2) === 46 && src.charCodeAt(d2 + 1) === 32) {
            const [a2, b2] = trimBounds(src, d2 + 2, e2);
            items.push([a2 < b2 ? src.slice(a2, b2) : '']);
            matched = true;
          }
        } else if ((cc === 45 || cc === 42) && src.charCodeAt(pos + 1) === 32) {
          const [a2, b2] = trimBounds(src, pos + 2, e2);
          items.push([a2 < b2 ? src.slice(a2, b2) : '']);
          matched = true;
        }
        if (matched) {
          pos = e2 + 1;
          continue;
        }
        const line = src.slice(pos, e2);
        if (/^ {2,}\S/.test(line)) {
          const [a2, b2] = trimBounds(src, pos, e2);
          items[items.length - 1].push(a2 < b2 ? src.slice(a2, b2) : '');
          pos = e2 + 1;
          continue;
        }
        break;
      }
      const tag = isOrdered ? 'ol' : 'ul';
      const open = isOrdered && start !== 1 ? `<ol start="${start}">` : `<${tag}>`;
      out.push(open, '\n');
      for (const item of items) {
        out.push('<li>');
        inlineInto(item.join('\n'), env, out);
        out.push('</li>', '\n');
      }
      out.push(`</${tag}>`, '\n');
      continue;
    }
    const parts = [];
    {
      const [a, b] = trimBounds(src, pos, e);
      parts.push(a < b ? src.slice(a, b) : '');
    }
    pos = e + 1;
    while (pos < len) {
      let e2 = src.indexOf('\n', pos);
      if (e2 === -1) e2 = len;
      if (endsParagraph(src, pos, e2)) break;
      const [a, b] = trimBounds(src, pos, e2);
      parts.push(a < b ? src.slice(a, b) : '');
      pos = e2 + 1;
    }
    out.push('<p>');
    const mark = out.length;
    inlineInto(parts.join('\n'), env, out);
    if (!hasExcerpt) {
      excerpt = out.slice(mark).join('');
      hasExcerpt = true;
    }
    out.push('</p>', '\n');
  }
  return { toc, excerpt };
}

// ---------------------------------------------------------------- templates

/** A template that does not parse: its name and why. */
class TemplateError extends Error {}

const T_PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const T_FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const T_IF = /^if (not )?(\S+)$/;
const T_INCLUDE = /^include "([a-z0-9-]+)"$/;

/** The path `text` names, split at its dots, or a TemplateError. */
function tPathOf(name, text) {
  if (!T_PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

/** `src`'s tokens: text, {{ }}, {{{ }}} and {% %}; a standalone tag's line goes with it. */
function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: tPathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

/** `src` parsed into its tree of nodes (a TemplateError when it does not parse). */
function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = T_FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: tPathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = T_IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: tPathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = T_INCLUDE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

/** The value at `path` in the scope chain: first name from the nearest frame holding it, then own properties. */
function lookup(frames, path) {
  let s = frames.length - 1;
  while (s >= 0 && !hasOwn(frames[s], path[0])) s--;
  if (s < 0) return undefined;
  let v = frames[s][path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

/** A value as text: a string itself, a number in JavaScript's digits, true or false, an array's items joined by ", ", else "". */
function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

/** Whether a value counts as true in an if: not undefined, null, false, "", 0 or an empty array. */
function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

/** `nodes` rendered into `out` (an array of strings). `frames` is the scope chain, innermost last. */
function renderNodes(nodes, frames, ctx, out, depth) {
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    switch (n.kind) {
      case 'text':
        out.push(n.text);
        break;
      case 'var':
        out.push(escapeHtml(textOf(lookup(frames, n.path))));
        break;
      case 'raw':
        out.push(textOf(lookup(frames, n.path)));
        break;
      case 'if': {
        const v = truthy(lookup(frames, n.path));
        const branch = v !== n.not ? n.then : n.else;
        if (branch !== null) renderNodes(branch, frames, ctx, out, depth);
        break;
      }
      case 'for': {
        const xs = lookup(frames, n.path);
        if (!Array.isArray(xs)) break;
        for (let k = 0; k < xs.length; k++) {
          frames.push({ [n.name]: xs[k], loop: { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length } });
          renderNodes(n.body, frames, ctx, out, depth);
          frames.pop();
        }
        break;
      }
      case 'include': {
        const t = ctx.templates.get(n.name);
        if (t !== undefined && depth < 8) renderNodes(t, frames, ctx, out, depth + 1);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- build

/** `rel` (a link's path, "/"-separated) from the directory `dir` of a source, or null when it climbs out of the root. */
function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

/** Whether a character code is whitespace (the \s set). */
function isWS(c) {
  if (c < 160) return c === 9 || c === 10 || c === 11 || c === 12 || c === 13 || c === 32;
  return c === 160 || c === 0x1680 || (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
}

/** The words of a body: its runs of characters other than whitespace. */
function wordsOf(text) {
  let n = 0;
  let i = 0;
  const len = text.length;
  while (i < len) {
    let c = text.charCodeAt(i);
    while (i < len && isWS(c)) {
      i++;
      if (i < len) c = text.charCodeAt(i);
    }
    if (i >= len) break;
    n++;
    while (i < len && !isWS(c)) {
      i++;
      if (i < len) c = text.charCodeAt(i);
    }
  }
  return n;
}

/** A page's link record as templates read it. */
const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : longDate(s.data.date) });

/** A post's card, as lists show it. */
function cardOf(s) {
  return {
    title: s.data.title,
    url: s.url,
    date: s.data.date ?? '',
    date_long: s.data.date === undefined ? '' : longDate(s.data.date),
    summary: s.data.summary ?? '',
    excerpt: s.excerpt,
    reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
    tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
  };
}

/** A page as its layout reads it. */
function pageOf(s, card) {
  return {
    ...card,
    slug: s.slug,
    content: s.html,
    toc: s.toc.map((h) => ({ level: h.level, id: h.id, html: h.html })),
    newer: linkOf(s.newer ?? null),
    older: linkOf(s.older ?? null),
    is_post: s.post,
  };
}

/** One site's output tree: path -> text. */
export function run(input) {
  const slugger = makeSlugger();
  const base = input.site.base_url.replace(/\/+$/, '');

  // Templates, parsed in code-unit order of name; one that does not parse is reported and left out.
  const templates = new Map();
  const report = [];
  for (const name of Object.keys(input.templates).sort(byCodeUnits)) {
    try {
      templates.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }

  // Sources, sorted by path in code-unit order; line ends made "\n".
  const files = [...input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  const sources = [];
  const statics = [];
  for (const f of files) {
    if (f.path.endsWith('.md')) sources.push({ path: f.path, text: f.content.replace(/\r\n?/g, '\n') });
    else statics.push(f);
  }

  // Front matter, in source order; the first failing check is the one reported.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      report.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    const own = s.path.startsWith('tags/') ? 'tags' : s.path.startsWith('page/') ? 'page' : undefined;
    if (own !== undefined) {
      report.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !hasOwn(s.data, 'date')) {
      report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!templates.has(layout)) {
      report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      report.push(`draft ${s.path}`);
      continue;
    }
    kept.push(s);
  }

  // Permalinks: each slug unique in its directory, in source order.
  const taken = new Map();
  const bySource = new Map();
  for (const s of kept) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) {
      set = new Set(dir === '' ? ['posts', 'tags', 'page'] : []);
      taken.set(dir, set);
    }
    s.slug = uniqueSlug(s.data.slug ?? slugger(s.data.title), set);
    s.outDir = dir === '' ? s.slug : `${dir}/${s.slug}`;
    s.url = `${base}/${s.outDir}/`;
    bySource.set(s.path, s);
  }

  // Markdown, each body's links resolved to the pages and static files they name.
  const staticPaths = new Set(statics.map((f) => f.path));
  const broken = new Set();
  for (const s of kept) {
    const dir = dirOf(s.path);
    const env = {
      link: (target) => {
        if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(target) || target.startsWith('/') || target.startsWith('#')) return target;
        const hash = target.indexOf('#');
        const rel = hash === -1 ? target : target.slice(0, hash);
        const frag = hash === -1 ? '' : target.slice(hash);
        const path = rel === '' ? null : resolvePath(dir, rel);
        if (path !== null && bySource.has(path)) return `${bySource.get(path).url}${frag}`;
        if (path !== null && staticPaths.has(path)) return `${base}/${path}${frag}`;
        broken.add(`broken-link ${s.path} -> ${target}`);
        return target;
      },
    };
    const htmlOut = [];
    const md = renderMarkdown(s.body, env, slugger, htmlOut);
    s.html = htmlOut.join('');
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // Collections: posts newest first, equal dates by slug; newer and older neighbours.
  const posts = kept.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  posts.forEach((p, k) => {
    p.newer = k === 0 ? null : posts[k - 1];
    p.older = k === posts.length - 1 ? null : posts[k + 1];
  });

  // Tags: each post's tags, each slug once, in its own order; ordered by count, then slug.
  const bySlug = new Map();
  const ownTags = (s) => {
    const seen = new Set();
    const out = [];
    for (const name of (s.data.tags ?? [])) {
      const slug = slugger(name);
      if (!seen.has(slug)) {
        seen.add(slug);
        out.push(name);
      }
    }
    return out;
  };
  for (const p of posts) {
    p.tagRefs = [];
    for (const name of ownTags(p)) {
      const slug = slugger(name);
      let tag = bySlug.get(slug);
      if (tag === undefined) {
        tag = { name, slug, url: `${base}/tags/${slug}/`, posts: [] };
        bySlug.set(slug, tag);
      }
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of kept) {
    if (!s.post) s.tagRefs = ownTags(s).map((name) => bySlug.get(slugger(name)) ?? { name, slug: slugger(name), url: '', posts: [] });
  }
  const tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  // Layouts: every page through its layout, the index pages and the tag pages through theirs.
  const cards = new Map();
  for (const p of posts) cards.set(p, cardOf(p));
  const site = { title: input.site.title, base_url: base };
  const allTags = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = posts.slice(0, input.site.recent).map((p) => cards.get(p));
  const ctx = { templates, escape: escapeHtml };
  const output = {};
  // One shared frame for site/tags/recent; each render adds its own vars as
  // the innermost frame, which is what the single spread object did.
  const shared = { site, tags: allTags, recent };
  const render = (name, vars) => {
    const nodes = templates.get(name);
    if (nodes === undefined) return null;
    const out = [];
    renderNodes(nodes, [shared, vars], ctx, out, 0);
    return out.join('');
  };
  for (const s of kept) {
    const card = s.post ? cards.get(s) : cardOf(s);
    output[`${s.outDir}/index.html`] = render(s.layout, { page: pageOf(s, card) });
  }
  const per = input.site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const html = render('index', {
      posts: posts.slice((k - 1) * per, k * per).map((p) => cards.get(p)),
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (const t of tags) {
    const html = render('tag', { tag: { name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cards.get(p)) } });
    if (html !== null) output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', {});
  if (tagsHtml !== null) output['tags/index.html'] = tagsHtml;
  for (const f of statics) {
    if (hasOwn(output, f.path)) report.push(`shadowed ${f.path}: a built page has its path`);
    else output[f.path] = f.content;
  }

  // The report: every line reported above and each broken link, sorted, then the counts.
  const lines = [...report, ...broken].sort(byCodeUnits);
  const pageCount = kept.filter((s) => !s.post).length;
  lines.push(`built ${pageCount} pages, ${posts.length} posts, ${tags.length} tags and ${statics.length} static files`);
  output['build-report.txt'] = `${lines.join('\n')}\n`;
  return output;
}
