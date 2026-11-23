// sitegen-speed v4 — a zero-copy static site generator for makefun-site 1.0.0.
//
// Architecture (a ground-up rewrite, different from v1/v2/v3):
//   * One bump arena per build() call. No per-item malloc, no Val/DOM tree.
//   * JSON is parsed in place into a flat token array; strings stay as spans
//     (unescaped once, in the arena, only when they hold a backslash).
//   * Pages are structs of spans. No card/page/tag objects are ever built.
//   * Templates compile once per case into a node tree whose {{paths}} are
//     pre-resolved to (slot, field) accessors — no hash lookups at render.
//   * Markdown renders in one streaming pass straight into each page's HTML
//     span in the arena; excerpt and TOC entries are spans of that buffer.
//   * Layouts render straight into the answers.json buffer with HTML+JSON
//     escaping fused, so no intermediate layout string is ever built.
//   * answers.json is emitted in the reference's key order.
//
// The build pipeline (front matter, template dialect, Markdown subset, slug
// rule, report) is implemented byte for byte from makefun-site 1.0.0.

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SITEGEN_ARENA_MB
#define SITEGEN_ARENA_MB 128
#endif

// ---------------------------------------------------------------- arena

typedef struct {
  char *base;
  char *ptr;
  char *end;
} Arena;

static int arena_init(Arena *a, size_t cap) {
  a->base = (char *)malloc(cap ? cap : 1);
  if (!a->base) return 0;
  a->ptr = a->base;
  a->end = a->base + cap;
  return 1;
}

static void arena_free(Arena *a) {
  free(a->base);
  a->base = a->ptr = a->end = NULL;
}

// Rewind the bump pointer so the next case reuses the same block.
static void arena_reset(Arena *a) { a->ptr = a->base; }

static void *arena_alloc(Arena *a, size_t n) {
  if ((size_t)(a->end - a->ptr) < n) return NULL;
  void *p = a->ptr;
  a->ptr += n;
  return p;
}

// A byte span: pointer + length. Spans point into the input, the arena, or a
// template source; they are never NUL-terminated unless noted.
typedef struct {
  const char *p;
  uint32_t n;
} Span;

static Span span_make(const char *p, size_t n) {
  Span s;
  s.p = p;
  s.n = (uint32_t)n;
  return s;
}

static Span span_cstr(const char *s) { return span_make(s, strlen(s)); }

static int span_eq(Span a, Span b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

static int span_eq_c(Span a, const char *b) {
  size_t n = strlen(b);
  return a.n == n && (n == 0 || memcmp(a.p, b, n) == 0);
}

// Byte order (== JS code-unit order for the ASCII data this sorts).
static int span_cmp(Span a, Span b) {
  size_t n = a.n < b.n ? a.n : b.n;
  int c = n ? memcmp(a.p, b.p, n) : 0;
  if (c != 0) return c;
  return (int)a.n - (int)b.n;
}

static int span_starts_with(Span a, const char *prefix) {
  size_t n = strlen(prefix);
  return a.n >= n && memcmp(a.p, prefix, n) == 0;
}

static int span_ends_with(Span a, const char *suffix) {
  size_t n = strlen(suffix);
  return a.n >= n && memcmp(a.p + a.n - n, suffix, n) == 0;
}

// ---------------------------------------------------------------- growable buffer (malloc-backed; the answers output)

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} Buf;

static int buf_init(Buf *b, size_t cap) {
  b->buf = (char *)malloc(cap ? cap : 1);
  if (!b->buf) return 0;
  b->len = 0;
  b->cap = cap;
  return 1;
}

static void buf_free(Buf *b) {
  free(b->buf);
  b->buf = NULL;
  b->len = b->cap = 0;
}

static int buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra <= b->cap) return 1;
  size_t nc = b->cap ? b->cap : 64;
  while (nc < b->len + extra) nc *= 2;
  char *nb = (char *)realloc(b->buf, nc);
  if (!nb) return 0;
  b->buf = nb;
  b->cap = nc;
  return 1;
}

static int buf_put(Buf *b, const char *s, size_t n) {
  if (!buf_reserve(b, n)) return 0;
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  return 1;
}

static int buf_putc(Buf *b, char c) { return buf_put(b, &c, 1); }

static int buf_put_span(Buf *b, Span s) { return buf_put(b, s.p, s.n); }

static int buf_puts(Buf *b, const char *s) { return buf_put(b, s, strlen(s)); }

// ---------------------------------------------------------------- UTF-8

// Decode one code point at p (len bytes left); returns the code point and
// advances *adv. Invalid bytes yield U+FFFD and advance 1.
static uint32_t utf8_decode(const char *p, size_t len, size_t *adv) {
  unsigned char c = (unsigned char)p[0];
  if (c < 0x80) {
    *adv = 1;
    return c;
  }
  if (c >= 0xC2 && c <= 0xDF && len >= 2 && ((unsigned char)p[1] & 0xC0) == 0x80) {
    *adv = 2;
    return ((uint32_t)(c & 0x1F) << 6) | (uint32_t)((unsigned char)p[1] & 0x3F);
  }
  if (c >= 0xE0 && c <= 0xEF && len >= 3 && ((unsigned char)p[1] & 0xC0) == 0x80 &&
      ((unsigned char)p[2] & 0xC0) == 0x80) {
    uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((unsigned char)p[1] & 0x3F) << 6) |
                  (uint32_t)((unsigned char)p[2] & 0x3F);
    if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
      *adv = 3;
      return cp;
    }
  }
  if (c >= 0xF0 && c <= 0xF4 && len >= 4 && ((unsigned char)p[1] & 0xC0) == 0x80 &&
      ((unsigned char)p[2] & 0xC0) == 0x80 && ((unsigned char)p[3] & 0xC0) == 0x80) {
    uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((unsigned char)p[1] & 0x3F) << 12) |
                  ((uint32_t)((unsigned char)p[2] & 0x3F) << 6) |
                  (uint32_t)((unsigned char)p[3] & 0x3F);
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      *adv = 4;
      return cp;
    }
  }
  *adv = 1;
  return 0xFFFD;
}

// Encode cp as UTF-8; returns the byte count.
static size_t utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// ---------------------------------------------------------------- slugify
//
// The slug of a text: NFKD, combining marks dropped, A-Z lowered, every run
// of characters other than a-z and 0-9 one "-", none at either end, at most
// 64 characters (a "-" left at the cut dropped), "untitled" when nothing is
// left. A code point contributes the ASCII letters its NFKD form leaves
// after the marks are dropped; anything else is a gap. slug_cp writes that
// contribution into out (at most 3 bytes); a 0x01 byte in it means "gap".

// Latin-1 Supplement U+00A0..U+00FF, one byte per code point ('.' = gap,
// '?' = multi-byte, handled in slug_cp). Rows of 32.
static const char SLUG_LATIN1_A[33] = "..........a..... ..23....1o.???";
static const char SLUG_LATIN1_B[33] = "aaaaaa.ceeeeiiii.nooooo..uuuuy..";
static const char SLUG_LATIN1_C[33] = "aaaaaa.ceeeeiiii.nooooo..uuuuy.y";

// Latin Extended-A U+0100..U+017F, rows of 32 ('?' = multi-byte).
static const char SLUG_LATA_A[33] = "aaaaaaccccccccdd..eeeeeeeeeegggg";
static const char SLUG_LATA_B[33] = "gggghh..iiiiiiiii.??jjkk.llllll?";
static const char SLUG_LATA_C[33] = "?..nnnnnn?..oooooo..rrrrrrssssss";
static const char SLUG_LATA_D[33] = "sstttt..uuuuuuuuuuuuwwyyyzzzzzzs";

// Latin Extended-B U+0180..U+024F, rows of 32 ('?' = multi-byte).
static const char SLUG_LATB_A[33] = "................................";
static const char SLUG_LATB_B[33] = "oo.............uu...............";
static const char SLUG_LATB_C[33] = "....?????????aaiioouuuuuuuuuu.aa";
static const char SLUG_LATB_D[33] = "aa....ggkkoooo..j???gg..nnaa....";
static const char SLUG_LATB_E[33] = "aaaaeeeeiiiioooorrrruuuusstt..hh";
static const char SLUG_LATB_F[33] = "......aaeeooooooooyy............";
static const char SLUG_LATB_G[17] = "................";

// Latin Extended Additional U+1E00..U+1EFF, rows of 32 ('?' = multi-byte).
static const char SLUG_LATX_A[33] = "aabbbbbbccddddddddddeeeeeeeeeeff";
static const char SLUG_LATX_B[33] = "gghhhhhhhhhhiiiikkkkkkllllllllmm";
static const char SLUG_LATX_C[33] = "mmmmnnnnnnnnoooooooopppprrrrrrrr";
static const char SLUG_LATX_D[33] = "ssssssssssttttttttuuuuuuuuuuvvvv";
static const char SLUG_LATX_E[33] = "wwwwwwwwwwxxxxyyzzzzzzhtwy?s....";
static const char SLUG_LATX_F[33] = "aaaaaaaaaaaaaaaaaaaaaaaaeeeeeeee";
static const char SLUG_LATX_G[33] = "eeeeeeeeiiiioooooooooooooooooooo";
static const char SLUG_LATX_H[33] = "oooouuuuuuuuuuuuuuyyyyyyyy......";

// Letterlike Symbols U+2100..U+214F, one byte per code point ('.' = gap,
// '?' = multi-byte, handled in slug_cp). Rows of 32.
static const char SLUG_LETTER_A[33] = "??c??.?g?hh..iill.n??pqr.....";
static const char SLUG_LETTER_B[33] = "sm...tel.tm..z...zkabce.f..m.o";
static const char SLUG_LETTER_C[33] = "....i...fax...dd.eij.........";

// The ASCII letters a code point contributes after NFKD with marks
// dropped; out holds at most 3 bytes (0x01 = gap). Returns the count.
static int slug_cp(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    if (cp >= 'A' && cp <= 'Z') {
      out[0] = (char)(cp + 32);
      return 1;
    }
    if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
      out[0] = (char)cp;
      return 1;
    }
    return 0;
  }
  // Fullwidth ASCII U+FF01..U+FF5E: the ASCII character itself (lowered
  // when a letter; a gap when punctuation).
  if (cp >= 0xFF01 && cp <= 0xFF5E) {
    char c = (char)(cp - 0xFEE0);
    if (c >= 'A' && c <= 'Z') {
      out[0] = (char)(c + 32);
      return 1;
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[0] = c;
      return 1;
    }
    return 0;
  }
  // Ligatures U+FB00..U+FB06.
  if (cp >= 0xFB00 && cp <= 0xFB06) {
    static const char *LIG[7] = {"ff", "fi", "fl", "ffi", "ffl", "st", "st"};
    const char *s = LIG[cp - 0xFB00];
    size_t n = strlen(s);
    memcpy(out, s, n);
    return (int)n;
  }
  // Roman numerals U+2160..U+217F.
  if (cp >= 0x2160 && cp <= 0x217F) {
    static const char *ROM[16] = {"i", "ii", "iii", "iv", "v", "vi", "vii", "viii",
                                  "ix", "x", "xi", "xii", "l", "c", "d", "m"};
    const char *s = ROM[cp & 0xF];
    size_t n = strlen(s);
    memcpy(out, s, n);
    return (int)n;
  }
  // Vulgar fractions U+00BC..U+00BE and U+2150..U+215F.
  if (cp >= 0xBC && cp <= 0xBE) {
    static const char *FR[3] = {"1\x01" "4", "1\x01" "2", "3\x01" "4"};
    const char *s = FR[cp - 0xBC];
    size_t n = strlen(s);
    memcpy(out, s, n);
    return (int)n;
  }
  if (cp >= 0x2150 && cp <= 0x215F) {
    static const char *FR[16] = {"1\x01" "7", "1\x01" "9", "1\x01" "10", "1\x01" "3",
                                 "2\x01" "3", "1\x01" "5", "2\x01" "5", "3\x01" "5",
                                 "4\x01" "5", "1\x01" "6", "5\x01" "6", "1\x01" "8",
                                 "3\x01" "8", "5\x01" "8", "7\x01" "8", "1\x01"};
    const char *s = FR[cp - 0x2150];
    size_t n = strlen(s);
    memcpy(out, s, n);
    return (int)n;
  }
  // Superscripts and subscripts.
  if (cp == 0x2070) { out[0] = '0'; return 1; }
  if (cp == 0x2071) { out[0] = 'i'; return 1; }
  if (cp >= 0x2074 && cp <= 0x2079) { out[0] = (char)('0' + (cp - 0x2070)); return 1; }
  if (cp == 0x207F) { out[0] = 'n'; return 1; }
  if (cp >= 0x2080 && cp <= 0x2089) { out[0] = (char)('0' + (cp - 0x2080)); return 1; }
  if (cp >= 0x2090 && cp <= 0x209C) {
    static const char SUB[13] = "aeoxhklmnpst";
    char c = SUB[cp - 0x2090];
    if (c == 0) return 0;
    out[0] = c;
    return 1;
  }
  // Circled and parenthesized numbers and letters U+2460..U+24E9.
  if (cp >= 0x2460 && cp <= 0x2473) {
    // Circled 1..20.
    int v = (int)(cp - 0x2460) + 1;
    if (v < 10) {
      out[0] = (char)('0' + v);
      return 1;
    }
    out[0] = '1';
    out[1] = (char)('0' + (v - 10));
    return 2;
  }
  if (cp >= 0x2474 && cp <= 0x2487) {
    // Parenthesized 1..20: gap, digits, gap.
    int v = (int)(cp - 0x2474) + 1;
    out[0] = 0x01;
    if (v < 10) {
      out[1] = (char)('0' + v);
      out[2] = 0x01;
      return 3;
    }
    out[1] = '1';
    out[2] = (char)('0' + (v - 10));
    out[3] = 0x01;
    return 4;
  }
  if (cp >= 0x2488 && cp <= 0x249B) {
    // Digit followed by a full stop: digits, gap.
    int v = (int)(cp - 0x2488) + 1;
    if (v < 10) {
      out[0] = (char)('0' + v);
      out[1] = 0x01;
      return 2;
    }
    out[0] = '1';
    out[1] = (char)('0' + (v - 10));
    out[2] = 0x01;
    return 3;
  }
  if (cp >= 0x249C && cp <= 0x24B5) {
    // Parenthesized a..z: gap, letter, gap.
    out[0] = 0x01;
    out[1] = (char)('a' + (cp - 0x249C));
    out[2] = 0x01;
    return 3;
  }
  if (cp >= 0x24B6 && cp <= 0x24CF) {
    out[0] = (char)('a' + (cp - 0x24B6));
    return 1;
  }
  if (cp >= 0x24D0 && cp <= 0x24E9) {
    out[0] = (char)('a' + (cp - 0x24D0));
    return 1;
  }
  if (cp == 0x24EA) { out[0] = '0'; return 1; }
  // Modifier letters that decompose to a Latin letter.
  if (cp >= 0x1D2C && cp <= 0x1DBB) {
    static const struct { uint16_t cp; char c; } MOD[] = {
        {0x1D2C, 'a'}, {0x1D2E, 'b'}, {0x1D30, 'd'}, {0x1D31, 'e'}, {0x1D33, 'g'},
        {0x1D34, 'h'}, {0x1D35, 'i'}, {0x1D36, 'j'}, {0x1D37, 'k'}, {0x1D38, 'l'},
        {0x1D39, 'm'}, {0x1D3A, 'n'}, {0x1D3C, 'o'}, {0x1D3E, 'p'}, {0x1D3F, 'r'},
        {0x1D40, 't'}, {0x1D41, 'u'}, {0x1D42, 'w'}, {0x1D43, 'a'}, {0x1D47, 'b'},
        {0x1D48, 'd'}, {0x1D49, 'e'}, {0x1D4D, 'g'}, {0x1D4F, 'k'}, {0x1D50, 'm'},
        {0x1D52, 'o'}, {0x1D56, 'p'}, {0x1D57, 't'}, {0x1D58, 'u'}, {0x1D5B, 'v'},
        {0x1D62, 'i'}, {0x1D63, 'r'}, {0x1D64, 'u'}, {0x1D65, 'v'}, {0x1D9C, 'c'},
        {0x1DA0, 'f'}, {0x1DBB, 'z'},
    };
    for (size_t i = 0; i < sizeof(MOD) / sizeof(MOD[0]); i++)
      if (MOD[i].cp == cp) {
        out[0] = MOD[i].c;
        return 1;
      }
    return 0;
  }
  if (cp == 0x2B0) { out[0] = 'h'; return 1; }
  if (cp == 0x2B2) { out[0] = 'j'; return 1; }
  if (cp == 0x2B3) { out[0] = 'r'; return 1; }
  if (cp == 0x2B7) { out[0] = 'w'; return 1; }
  if (cp == 0x2B8) { out[0] = 'y'; return 1; }
  if (cp == 0x2E1) { out[0] = 'l'; return 1; }
  if (cp == 0x2E2) { out[0] = 's'; return 1; }
  if (cp == 0x2E3) { out[0] = 'x'; return 1; }
  if (cp == 0x2C7C) { out[0] = 'j'; return 1; }
  if (cp == 0x2C7D) { out[0] = 'v'; return 1; }
  if (cp == 0xA7F1) { out[0] = 's'; return 1; }
  if (cp == 0xA7F2) { out[0] = 'c'; return 1; }
  if (cp == 0xA7F3) { out[0] = 'f'; return 1; }
  if (cp == 0xA7F4) { out[0] = 'q'; return 1; }
  if (cp == 0x20A8) { out[0] = 'r'; out[1] = 's'; return 2; }
  // Latin-1 Supplement U+00A0..U+00FF.
  if (cp >= 0xA0 && cp <= 0xFF) {
    char c;
    if (cp < 0xC0) c = SLUG_LATIN1_A[cp - 0xA0];
    else if (cp < 0xE0) c = SLUG_LATIN1_B[cp - 0xC0];
    else c = SLUG_LATIN1_C[cp - 0xE0];
    if (c == '.') return 0;
    if (c == '?') {
      // A vulgar fraction: "1\x014", "1\x012", "3\x014".
      static const char *FR[3] = {"1\x01" "4", "1\x01" "2", "3\x01" "4"};
      const char *s = FR[cp - 0xBC];
      size_t n = strlen(s);
      memcpy(out, s, n);
      return (int)n;
    }
    out[0] = c;
    return 1;
  }
  // Latin Extended-A U+0100..U+017F.
  if (cp >= 0x100 && cp <= 0x17F) {
    if (cp == 0x132 || cp == 0x133) { out[0] = 'i'; out[1] = 'j'; return 2; }
    if (cp == 0x13F || cp == 0x140) { out[0] = 'l'; out[1] = 0x01; return 2; }
    if (cp == 0x149) { out[0] = 0x01; out[1] = 'n'; return 2; }
    char c;
    if (cp < 0x120) c = SLUG_LATA_A[cp - 0x100];
    else if (cp < 0x140) c = SLUG_LATA_B[cp - 0x120];
    else if (cp < 0x160) c = SLUG_LATA_C[cp - 0x140];
    else c = SLUG_LATA_D[cp - 0x160];
    if (c == '.' || c == '?') return 0;
    out[0] = c;
    return 1;
  }
  // Latin Extended-B U+0180..U+024F.
  if (cp >= 0x180 && cp <= 0x24F) {
    if (cp >= 0x1C4 && cp <= 0x1C6) { out[0] = 'd'; out[1] = 'z'; return 2; }
    if (cp >= 0x1C7 && cp <= 0x1C9) { out[0] = 'l'; out[1] = 'j'; return 2; }
    if (cp >= 0x1CA && cp <= 0x1CC) { out[0] = 'n'; out[1] = 'j'; return 2; }
    if (cp >= 0x1F1 && cp <= 0x1F3) { out[0] = 'd'; out[1] = 'z'; return 2; }
    char c;
    if (cp < 0x1A0) c = SLUG_LATB_A[cp - 0x180];
    else if (cp < 0x1C0) c = SLUG_LATB_B[cp - 0x1A0];
    else if (cp < 0x1E0) c = SLUG_LATB_C[cp - 0x1C0];
    else if (cp < 0x200) c = SLUG_LATB_D[cp - 0x1E0];
    else if (cp < 0x220) c = SLUG_LATB_E[cp - 0x200];
    else if (cp < 0x240) c = SLUG_LATB_F[cp - 0x220];
    else c = SLUG_LATB_G[cp - 0x240];
    if (c == '.' || c == '?') return 0;
    out[0] = c;
    return 1;
  }
  // Latin Extended Additional U+1E00..U+1EFF.
  if (cp >= 0x1E00 && cp <= 0x1EFF) {
    if (cp == 0x1E9A) { out[0] = 'a'; out[1] = 0x01; return 2; }
    char c;
    if (cp < 0x1E20) c = SLUG_LATX_A[cp - 0x1E00];
    else if (cp < 0x1E40) c = SLUG_LATX_B[cp - 0x1E20];
    else if (cp < 0x1E60) c = SLUG_LATX_C[cp - 0x1E40];
    else if (cp < 0x1E80) c = SLUG_LATX_D[cp - 0x1E60];
    else if (cp < 0x1EA0) c = SLUG_LATX_E[cp - 0x1E80];
    else if (cp < 0x1EC0) c = SLUG_LATX_F[cp - 0x1EA0];
    else if (cp < 0x1EE0) c = SLUG_LATX_G[cp - 0x1EC0];
    else c = SLUG_LATX_H[cp - 0x1EE0];
    if (c == '.' || c == '?') return 0;
    out[0] = c;
    return 1;
  }
  // Letterlike Symbols U+2100..U+214F.
  if (cp >= 0x2100 && cp <= 0x214F) {
    static const struct { uint16_t cp; const char *s; } EX[] = {
        {0x2100, "a\x01" "c"}, {0x2101, "a\x01" "s"}, {0x2103, "\x01" "c"},
        {0x2105, "c\x01" "o"}, {0x2106, "c\x01" "u"}, {0x2109, "\x01" "f"},
        {0x2116, "no"},        {0x2120, "sm"},       {0x2121, "tel"},
        {0x2122, "tm"},        {0x213B, "fax"},
    };
    for (size_t i = 0; i < sizeof(EX) / sizeof(EX[0]); i++)
      if (EX[i].cp == cp) {
        size_t n = strlen(EX[i].s);
        memcpy(out, EX[i].s, n);
        return (int)n;
      }
    char c;
    if (cp < 0x2120) c = SLUG_LETTER_A[cp - 0x2100];
    else if (cp < 0x2140) c = SLUG_LETTER_B[cp - 0x2120];
    else c = SLUG_LETTER_C[cp - 0x2140];
    if (c == '.' || c == '?') return 0;
    out[0] = c;
    return 1;
  }
  // 0x2189 (vulgar fraction 0/3): "0\x013".
  if (cp == 0x2189) {
    out[0] = '0';
    out[1] = 0x01;
    out[2] = '3';
    return 3;
  }
  return 0;
}

// The slug of `text`, written to `out` (a Buf). At most 64 chars, trailing
// "-" dropped, "untitled" when empty.
static void slugify_into(Span text, Buf *out) {
  size_t start = out->len;
  int gap = 0;
  size_t i = 0;
  while (i < text.n) {
    size_t adv;
    uint32_t cp = utf8_decode(text.p + i, text.n - i, &adv);
    i += adv;
    char tmp[4];
    int n = slug_cp(cp, tmp);
    for (int k = 0; k < n; k++) {
      char c = tmp[k];
      if (c == 0x01) {
        if (out->len > start) gap = 1;
      } else {
        if (gap) {
          buf_putc(out, '-');
          gap = 0;
        }
        buf_putc(out, c);
      }
    }
    if (n == 0) {
      if (out->len > start) gap = 1;
    }
  }
  // At most 64 characters, a "-" left at the cut dropped.
  size_t len = out->len - start;
  if (len > 64) {
    out->len = start + 64;
    len = 64;
  }
  while (len > 0 && out->buf[start + len - 1] == '-') {
    out->len--;
    len--;
  }
  if (len == 0) buf_puts(out, "untitled");
}

// A slug as an arena span.
static Span slugify_span(Arena *a, Span text) {
  Buf tmp = {0};
  // Render into a scratch Buf backed by the arena's tail: use a stack buffer
  // (slugs are short; fall back to the arena for the pathological case).
  char stackbuf[256];
  tmp.buf = stackbuf;
  tmp.cap = sizeof(stackbuf);
  tmp.len = 0;
  slugify_into(text, &tmp);
  if (tmp.buf != stackbuf) {
    // slugify_into grew the buffer via realloc; keep the malloc'd copy.
    Span s = span_make(tmp.buf, tmp.len);
    return s;
  }
  char *p = (char *)arena_alloc(a, tmp.len + 1);
  if (!p) return span_cstr("untitled");
  memcpy(p, tmp.buf, tmp.len);
  p[tmp.len] = '\0';
  return span_make(p, tmp.len);
}

// ---------------------------------------------------------------- JSON tokenizer
//
// The input is a single JSON object. The tokenizer walks the bytes and
// records, for each string value, a span into the input; unescaping happens
// lazily (see json_unescape_span) only for strings that carry a backslash.
// Numbers stay as spans; the only number the pipeline reads is per_page (a
// small integer). Booleans and null are single tokens.

typedef enum {
  J_STR,  // string: p/n cover the raw contents between the quotes
  J_NUM,  // number: p/n cover the raw literal
  J_TRUE,
  J_FALSE,
  J_NULL,
  J_OPEN_OBJ,
  J_CLOSE_OBJ,
  J_OPEN_ARR,
  J_CLOSE_ARR,
  J_COMMA,
  J_COLON
} JKind;

typedef struct {
  JKind kind;
  const char *p;
  uint32_t n;
} JTok;

typedef struct {
  const char *cur;
  const char *end;
  JTok tok;
} JScan;

static void jskip_ws(JScan *s) {
  while (s->cur < s->end) {
    char c = *s->cur;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s->cur++;
    else break;
  }
}

// Read the next token. Returns 0 at end of input or on malformed input.
static int jnext(JScan *s) {
  jskip_ws(s);
  if (s->cur >= s->end) return 0;
  const char *p = s->cur;
  char c = *p;
  if (c == '"') {
    const char *q = p + 1;
    while (q < s->end) {
      unsigned char u = (unsigned char)*q;
      if (u == '\\') {
        q += 2;
        continue;
      }
      if (u == '"') break;
      q++;
    }
    if (q >= s->end) return 0;
    s->tok.kind = J_STR;
    s->tok.p = p + 1;
    s->tok.n = (uint32_t)(q - (p + 1));
    s->cur = q + 1;
    return 1;
  }
  if (c == '{') {
    s->tok.kind = J_OPEN_OBJ;
    s->cur = p + 1;
    return 1;
  }
  if (c == '}') {
    s->tok.kind = J_CLOSE_OBJ;
    s->cur = p + 1;
    return 1;
  }
  if (c == '[') {
    s->tok.kind = J_OPEN_ARR;
    s->cur = p + 1;
    return 1;
  }
  if (c == ']') {
    s->tok.kind = J_CLOSE_ARR;
    s->cur = p + 1;
    return 1;
  }
  if (c == ',') {
    s->tok.kind = J_COMMA;
    s->cur = p + 1;
    return 1;
  }
  if (c == ':') {
    s->tok.kind = J_COLON;
    s->cur = p + 1;
    return 1;
  }
  if (c == 't' && s->end - p >= 4 && memcmp(p, "true", 4) == 0) {
    s->tok.kind = J_TRUE;
    s->cur = p + 4;
    return 1;
  }
  if (c == 'f' && s->end - p >= 5 && memcmp(p, "false", 5) == 0) {
    s->tok.kind = J_FALSE;
    s->cur = p + 5;
    return 1;
  }
  if (c == 'n' && s->end - p >= 4 && memcmp(p, "null", 4) == 0) {
    s->tok.kind = J_NULL;
    s->cur = p + 4;
    return 1;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    const char *q = p + 1;
    while (q < s->end) {
      char d = *q;
      if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-')
        q++;
      else
        break;
    }
    s->tok.kind = J_NUM;
    s->tok.p = p;
    s->tok.n = (uint32_t)(q - p);
    s->cur = q;
    return 1;
  }
  return 0;
}

// Skip one whole JSON value (object, array, or scalar).
static int jskip_value(JScan *s) {
  if (!jnext(s)) return 0;
  if (s->tok.kind == J_OPEN_OBJ || s->tok.kind == J_OPEN_ARR) {
    int depth = 1;
    JKind open = s->tok.kind;
    (void)open;
    while (depth > 0) {
      if (!jnext(s)) return 0;
      if (s->tok.kind == J_OPEN_OBJ || s->tok.kind == J_OPEN_ARR) depth++;
      else if (s->tok.kind == J_CLOSE_OBJ || s->tok.kind == J_CLOSE_ARR) depth--;
    }
    return 1;
  }
  return 1;
}

// Unescape a JSON string span into the arena. The input is well-formed
// (the reference generator only emits valid JSON). Returns an arena span.
static Span json_unescape_span(Arena *a, Span raw) {
  if (memchr(raw.p, '\\', raw.n) == NULL) return raw;
  char *out = (char *)arena_alloc(a, raw.n + 1);
  if (!out) return raw;
  size_t w = 0;
  size_t i = 0;
  while (i < raw.n) {
    char c = raw.p[i];
    if (c != '\\') {
      out[w++] = c;
      i++;
      continue;
    }
    i++;
    if (i >= raw.n) break;
    char e = raw.p[i];
    i++;
    switch (e) {
      case '"': out[w++] = '"'; break;
      case '\\': out[w++] = '\\'; break;
      case '/': out[w++] = '/'; break;
      case 'b': out[w++] = '\b'; break;
      case 'f': out[w++] = '\f'; break;
      case 'n': out[w++] = '\n'; break;
      case 'r': out[w++] = '\r'; break;
      case 't': out[w++] = '\t'; break;
      case 'u': {
        if (i + 4 > raw.n) break;
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
          char h = raw.p[i + k];
          v <<= 4;
          if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
          else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
        }
        i += 4;
        uint32_t cp = v;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= raw.n && raw.p[i] == '\\' &&
            raw.p[i + 1] == 'u') {
          unsigned lo = 0;
          int ok = 1;
          for (int k = 0; k < 4; k++) {
            char h = raw.p[i + 2 + k];
            lo <<= 4;
            if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
            else ok = 0;
          }
          if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        char enc[4];
        size_t n = utf8_encode(cp, enc);
        memcpy(out + w, enc, n);
        w += n;
        break;
      }
      default: out[w++] = e; break;
    }
  }
  out[w] = '\0';
  return span_make(out, w);
}

// ---------------------------------------------------------------- template parser
//
// The template dialect: {{ path }} escaped, {{{ path }}} raw,
// {% for x in path %}, {% if [not] path %} ... {% else %} ... {% endif %},
// {% include "name" %}. A {% %} tag alone on its line consumes its whole
// line plus the newline. {{ }} tags are not line-swallowed. A template is
// compiled once per case into a node tree; paths are pre-resolved at
// compile time to (slot, field) accessors, so rendering does no lookups.

#define MAX_TEMPLATES 64
#define MAX_INCLUDES_DEPTH 8

typedef enum {
  TN_TEXT,    // literal text
  TN_VAR,     // {{ path }} or {{{ path }}}
  TN_FOR,     // {% for x in path %} ... {% endfor %}
  TN_IF,      // {% if [not] path %} ... {% else %} ... {% endif %}
  TN_INCLUDE  // {% include "name" %}
} TNodeKind;

// Field ids (one global enum; a field means the same on every object that
// carries it). F_NONE = the object or list itself.
enum {
  F_NONE = 0,
  F_TITLE, F_URL, F_DATE, F_DATE_LONG, F_SUMMARY, F_EXCERPT,
  F_READING_MINUTES, F_TAGS, F_SLUG, F_CONTENT, F_TOC, F_NEWER, F_OLDER,
  F_IS_POST, F_NAME, F_COUNT, F_POSTS, F_LEVEL, F_ID, F_HTML, F_NUMBER,
  F_TOTAL, F_NEWER_URL, F_OLDER_URL, F_BASE_URL, F_INDEX, F_FIRST, F_LAST,
  F_LENGTH
};

// Element kinds a {% for %} binds its variable to.
enum { EL_UNKNOWN = 0, EL_CARD, EL_TAGOBJ, EL_TAGLINK, EL_TOC };

// A template node. Paths are stored raw and resolved at render time
// against the scope chain, so an included template reads its caller's
// loop variables (the reference resolves paths when rendering, not when
// parsing).
typedef struct TNode TNode;
struct TNode {
  TNodeKind kind;
  Span text;           // TN_TEXT: the literal
  Span path;           // TN_VAR / TN_IF / TN_FOR: the dotted path
  uint8_t raw;         // TN_VAR: {{{ }}} unescaped
  int32_t include_idx; // TN_INCLUDE: template index (-1 = missing)
  Span loop_var;       // TN_FOR: the loop variable name
  uint8_t neg;         // TN_IF: {% if not ... %}
  TNode *first;        // TN_FOR/TN_IF: body
  TNode *second;       // TN_IF: else body (NULL = none)
  TNode *next;         // sibling
};

typedef struct {
  Span name;
  Span source;
  TNode *root;     // NULL = parse error (template dropped)
  char *err;       // error text (arena) when root == NULL
  size_t err_len;
} Template;

static const char *tpl_end(Template *t) { return t->source.p + t->source.n; }

// Find the next "{{" or "{%" in [p, end). Returns NULL when none.
static const char *tpl_find_tag(const char *p, const char *end) {
  while (p + 1 < end) {
    if (p[0] == '{' && (p[1] == '{' || p[1] == '%')) return p;
    p++;
  }
  return NULL;
}

// The field id of one path segment, or F_NONE when it names no field.
static int field_id(Span s) {
  if (span_eq_c(s, "title")) return F_TITLE;
  if (span_eq_c(s, "url")) return F_URL;
  if (span_eq_c(s, "date")) return F_DATE;
  if (span_eq_c(s, "date_long")) return F_DATE_LONG;
  if (span_eq_c(s, "summary")) return F_SUMMARY;
  if (span_eq_c(s, "excerpt")) return F_EXCERPT;
  if (span_eq_c(s, "reading_minutes")) return F_READING_MINUTES;
  if (span_eq_c(s, "tags")) return F_TAGS;
  if (span_eq_c(s, "slug")) return F_SLUG;
  if (span_eq_c(s, "content")) return F_CONTENT;
  if (span_eq_c(s, "toc")) return F_TOC;
  if (span_eq_c(s, "newer")) return F_NEWER;
  if (span_eq_c(s, "older")) return F_OLDER;
  if (span_eq_c(s, "is_post")) return F_IS_POST;
  if (span_eq_c(s, "name")) return F_NAME;
  if (span_eq_c(s, "count")) return F_COUNT;
  if (span_eq_c(s, "posts")) return F_POSTS;
  if (span_eq_c(s, "level")) return F_LEVEL;
  if (span_eq_c(s, "id")) return F_ID;
  if (span_eq_c(s, "html")) return F_HTML;
  if (span_eq_c(s, "number")) return F_NUMBER;
  if (span_eq_c(s, "total")) return F_TOTAL;
  if (span_eq_c(s, "newer_url")) return F_NEWER_URL;
  if (span_eq_c(s, "older_url")) return F_OLDER_URL;
  if (span_eq_c(s, "base_url")) return F_BASE_URL;
  if (span_eq_c(s, "index")) return F_INDEX;
  if (span_eq_c(s, "first")) return F_FIRST;
  if (span_eq_c(s, "last")) return F_LAST;
  if (span_eq_c(s, "length")) return F_LENGTH;
  return F_NONE;
}

// List kinds a {% for %} iterates (a value's list kind).
enum { L_NONE = 0, L_TAGS, L_RECENT, L_POSTS, L_TAG_POSTS, L_PAGE_TAGS, L_PAGE_TOC, L_CARD_TAGS, L_VAR_POSTS };

// Whether `path` matches the template path grammar:
// names of [A-Za-z_][A-Za-z0-9_]* joined by dots.
static int path_valid(Span path) {
  if (path.n == 0) return 0;
  size_t i = 0;
  while (1) {
    if (i >= path.n) return 0;
    char c = path.p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
    i++;
    while (i < path.n) {
      c = path.p[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') i++;
      else break;
    }
    if (i >= path.n) return 1;
    if (path.p[i] != '.') return 0;
    i++;
  }
}

// Trim ASCII whitespace from both ends of a span.
static Span span_trim(Span s) {
  while (s.n > 0 && (*s.p == ' ' || *s.p == '\t' || *s.p == '\n' || *s.p == '\r')) {
    s.p++;
    s.n--;
  }
  while (s.n > 0) {
    char c = s.p[s.n - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s.n--;
    else break;
  }
  return s;
}

// Parser state for one template.
typedef struct {
  Arena *a;
  Template *tpl;
  const char *p;    // current position
  const char *end;
  char *err;        // error text (arena)
  size_t err_len;
  int failed;
} TParse;

// The templates of the current case (for {% include %} resolution).
static Template *g_templates;
static int g_ntemplates;

static void tparse_fail(TParse *st, const char *msg, size_t n) {
  if (st->failed) return;
  st->failed = 1;
  // Every template error reads `template "<name>": <msg>`.
  Span name = st->tpl->name;
  size_t total = 10 + name.n + 3 + n;
  st->err_len = total;
  st->err = (char *)arena_alloc(st->a, total + 1);
  if (!st->err) return;
  size_t w = 0;
  memcpy(st->err + w, "template \"", 10);
  w += 10;
  memcpy(st->err + w, name.p, name.n);
  w += name.n;
  memcpy(st->err + w, "\": ", 3);
  w += 3;
  memcpy(st->err + w, msg, n);
  w += n;
  st->err[w] = '\0';
}

// A template error for a path that is not a path:
// `template "<name>": "<path>" is not a path` (the path JSON-quoted).
static void tparse_fail_path(TParse *st, Span path) {
  if (st->failed) return;
  Span name = st->tpl->name;
  // Worst case: every byte of the path escapes to 6 bytes.
  size_t total = 10 + name.n + 2 + 1 + path.n * 6 + 1 + 15;
  char *buf = (char *)arena_alloc(st->a, total + 1);
  if (!buf) {
    tparse_fail(st, "is not a path", 13);
    return;
  }
  size_t w = 0;
  memcpy(buf + w, "template \"", 10);
  w += 10;
  memcpy(buf + w, name.p, name.n);
  w += name.n;
  memcpy(buf + w, "\": \"", 4);
  w += 4;
  for (size_t i = 0; i < path.n; i++) {
    unsigned char c = (unsigned char)path.p[i];
    if (c == '"') {
      memcpy(buf + w, "\\\"", 2);
      w += 2;
    } else if (c == '\\') {
      memcpy(buf + w, "\\\\", 2);
      w += 2;
    } else if (c == '\n') {
      memcpy(buf + w, "\\n", 2);
      w += 2;
    } else if (c == '\r') {
      memcpy(buf + w, "\\r", 2);
      w += 2;
    } else if (c == '\t') {
      memcpy(buf + w, "\\t", 2);
      w += 2;
    } else if (c < 0x20) {
      static const char *HEX = "0123456789abcdef";
      memcpy(buf + w, "\\u00", 4);
      w += 4;
      buf[w++] = HEX[(c >> 4) & 0xF];
      buf[w++] = HEX[c & 0xF];
    } else {
      buf[w++] = (char)c;
    }
  }
  memcpy(buf + w, "\" is not a path", 15);
  w += 15;
  buf[w] = '\0';
  st->failed = 1;
  st->err = buf;
  st->err_len = w;
}

static TNode *tnode_new(TParse *st, TNodeKind kind) {
  TNode *n = (TNode *)arena_alloc(st->a, sizeof(TNode));
  if (!n) {
    tparse_fail(st, "out of memory", 13);
    return NULL;
  }
  memset(n, 0, sizeof(*n));
  n->kind = kind;
  n->include_idx = -1;
  return n;
}

// Read a tag starting at `p` ("{{" or "{%"); returns the tag's inner span
// (trimmed) and sets *after to the byte after the closing tag, *is_expr for
// "{{", *is_raw for "{{{". Returns 0 on failure.
static int tpl_read_tag(TParse *st, const char *p, Span *inner, const char **after,
                        int *is_expr, int *is_raw) {
  const char *end = st->end;
  *is_expr = (p[1] == '{');
  *is_raw = 0;
  const char *q = p + 2;
  if (*is_expr && q < end && *q == '{') {
    *is_raw = 1;
    q++;
  }
  const char *close = *is_expr ? (const char *)"}}" : (const char *)"%}";
  size_t clen = 2;
  if (*is_raw) {
    close = (const char *)"}}}";
    clen = 3;
  }
  const char *r = q;
  while (r + clen <= end) {
    if (memcmp(r, close, clen) == 0) break;
    r++;
  }
  if (r + clen > end) {
    if (*is_raw) tparse_fail(st, "a {{{ is not closed", sizeof("a {{{ is not closed") - 1);
    else if (*is_expr) tparse_fail(st, "a {{ is not closed", sizeof("a {{ is not closed") - 1);
    else tparse_fail(st, "a {% is not closed", sizeof("a {% is not closed") - 1);
    return 0;
  }
  *inner = span_trim(span_make(q, (size_t)(r - q)));
  *after = r + clen;
  return 1;
}

// A {% %} tag is "alone on its line" when only spaces/tabs precede it on
// its line and only spaces/tabs follow it up to the newline.
static int tpl_tag_alone(TParse *st, const char *tag_start, const char *after) {
  const char *p = tag_start - 1;
  while (p >= st->tpl->source.p && *p != '\n') {
    if (*p != ' ' && *p != '\t') return 0;
    p--;
  }
  const char *q = after;
  while (q < st->end && *q != '\n') {
    if (*q != ' ' && *q != '\t') return 0;
    q++;
  }
  return 1;
}

// Forward declaration.
static TNode *tpl_parse_seq(TParse *st,
                            const char *stop1, const char *stop2, Span *stop_hit);

// `template "<name>": {% <inner> %} is not a tag`.
static void tparse_fail_not_a_tag(TParse *st, Span inner) {
  if (st->failed) return;
  Span name = st->tpl->name;
  size_t total = 10 + name.n + 6 + inner.n + 16;
  char *buf = (char *)arena_alloc(st->a, total + 1);
  if (!buf) {
    tparse_fail(st, "is not a tag", 12);
    return;
  }
  size_t w = 0;
  memcpy(buf + w, "template \"", 10);
  w += 10;
  memcpy(buf + w, name.p, name.n);
  w += name.n;
  memcpy(buf + w, "\": {% ", 6);
  w += 6;
  memcpy(buf + w, inner.p, inner.n);
  w += inner.n;
  memcpy(buf + w, " %} is not a tag", 16);
  w += 16;
  buf[w] = '\0';
  st->failed = 1;
  st->err = buf;
  st->err_len = w;
}

// Whether a span holds only ASCII whitespace (the \S+ of a for/if path).
static int span_has_ws(Span s) {
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 1;
  }
  return 0;
}

// Parse a sequence of nodes until end of template or a stop tag
// (stop1/stop2 are literal inner texts like "else", "endif", "endfor").
// On a stop tag, *stop_hit is the matched literal and the parser stops
// just after the tag (line-swallowed when alone).
static TNode *tpl_parse_seq(TParse *st,
                            const char *stop1, const char *stop2, Span *stop_hit) {
  TNode *head = NULL;
  TNode **tail = &head;
  while (st->p < st->end && !st->failed) {
    const char *tag = tpl_find_tag(st->p, st->end);
    if (!tag) {
      // Trailing text.
      if (st->p < st->end) {
        TNode *n = tnode_new(st, TN_TEXT);
        if (!n) break;
        n->text = span_make(st->p, (size_t)(st->end - st->p));
        *tail = n;
        tail = &n->next;
      }
      st->p = st->end;
      break;
    }
    if (tag > st->p) {
      TNode *n = tnode_new(st, TN_TEXT);
      if (!n) break;
      n->text = span_make(st->p, (size_t)(tag - st->p));
      *tail = n;
      tail = &n->next;
    }
    Span inner;
    const char *after;
    int is_expr, is_raw;
    if (!tpl_read_tag(st, tag, &inner, &after, &is_expr, &is_raw)) break;
    if (!is_expr && tpl_tag_alone(st, tag, after)) {
      // The tag takes its whole line: drop the spaces/tabs before it on
      // its line (they sit at the end of the text node just appended).
      TNode *last = head;
      TNode *prev_text = NULL;
      while (last) {
        if (last->kind == TN_TEXT) prev_text = last;
        last = last->next;
      }
      if (prev_text) {
        while (prev_text->text.n > 0) {
          char c = prev_text->text.p[prev_text->text.n - 1];
          if (c == ' ' || c == '\t') prev_text->text.n--;
          else break;
        }
      }
    }
    if (is_expr) {
      if (!path_valid(inner)) {
        tparse_fail_path(st, inner);
        break;
      }
      TNode *n = tnode_new(st, TN_VAR);
      if (!n) break;
      n->path = inner;
      n->raw = is_raw ? 1 : 0;
      *tail = n;
      tail = &n->next;
      st->p = after;
      continue;
    }
    // A {% %} tag.
    if (stop1 && span_eq_c(inner, stop1)) {
      if (stop_hit) *stop_hit = span_cstr(stop1);
      st->p = after;
      if (tpl_tag_alone(st, tag, after) && st->p < st->end && *st->p == '\n') st->p++;
      return head;
    }
    if (stop2 && span_eq_c(inner, stop2)) {
      if (stop_hit) *stop_hit = span_cstr(stop2);
      st->p = after;
      if (tpl_tag_alone(st, tag, after) && st->p < st->end && *st->p == '\n') st->p++;
      return head;
    }
    if (span_eq_c(inner, "else")) {
      tparse_fail(st, "an else outside an if", sizeof("an else outside an if") - 1);
      break;
    }
    if (span_eq_c(inner, "endfor")) {
      tparse_fail(st, "an endfor with no open for", sizeof("an endfor with no open for") - 1);
      break;
    }
    if (span_eq_c(inner, "endif")) {
      tparse_fail(st, "an endif with no open if", sizeof("an endif with no open if") - 1);
      break;
    }
    if (span_starts_with(inner, "for ")) {
      // {% for x in path %}: /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/
      Span rest = span_make(inner.p + 4, inner.n - 4);
      size_t i = 0;
      if (i >= rest.n) {
        tparse_fail_not_a_tag(st, inner);
        break;
      }
      char c = rest.p[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
        tparse_fail_not_a_tag(st, inner);
        break;
      }
      i++;
      while (i < rest.n) {
        c = rest.p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') i++;
        else break;
      }
      Span var = span_make(rest.p, i);
      if (i + 4 > rest.n || memcmp(rest.p + i, " in ", 4) != 0) {
        tparse_fail_not_a_tag(st, inner);
        break;
      }
      Span path = span_make(rest.p + i + 4, rest.n - (i + 4));
      if (path.n == 0 || span_has_ws(path)) {
        tparse_fail_not_a_tag(st, inner);
        break;
      }
      if (!path_valid(path)) {
        tparse_fail_path(st, path);
        break;
      }
      TNode *n = tnode_new(st, TN_FOR);
      if (!n) break;
      n->path = path;
      n->loop_var = var;
      st->p = after;
      if (tpl_tag_alone(st, tag, after) && st->p < st->end && *st->p == '\n') st->p++;
      Span hit = {0};
      n->first = tpl_parse_seq(st, "endfor", NULL, &hit);
      if (st->failed) break;
      if (hit.n == 0) {
        tparse_fail(st, "a for is not closed", sizeof("a for is not closed") - 1);
        break;
      }
      *tail = n;
      tail = &n->next;
      continue;
    }
    if (span_starts_with(inner, "if ")) {
      // {% if [not] path %}: /^if (not )?(\S+)$/
      Span rest = span_make(inner.p + 3, inner.n - 3);
      int neg = 0;
      if (span_starts_with(rest, "not ")) {
        neg = 1;
        rest = span_make(rest.p + 4, rest.n - 4);
      }
      if (rest.n == 0 || span_has_ws(rest)) {
        tparse_fail_not_a_tag(st, inner);
        break;
      }
      if (!path_valid(rest)) {
        tparse_fail_path(st, rest);
        break;
      }
      TNode *n = tnode_new(st, TN_IF);
      if (!n) break;
      n->path = rest;
      n->neg = neg ? 1 : 0;
      st->p = after;
      if (tpl_tag_alone(st, tag, after) && st->p < st->end && *st->p == '\n') st->p++;
      Span hit = {0};
      n->first = tpl_parse_seq(st, "else", "endif", &hit);
      if (st->failed) break;
      if (hit.n == 0) {
        tparse_fail(st, "a if is not closed", sizeof("a if is not closed") - 1);
        break;
      }
      if (span_eq_c(hit, "else")) {
        Span hit2 = {0};
        n->second = tpl_parse_seq(st, "endif", NULL, &hit2);
        if (st->failed) break;
        if (hit2.n == 0) {
          tparse_fail(st, "a if is not closed", sizeof("a if is not closed") - 1);
          break;
        }
      }
      *tail = n;
      tail = &n->next;
      continue;
    }
    if (span_starts_with(inner, "include ")) {
      // {% include "name" %}: /^include "([a-z0-9-]+)"$/
      Span rest = span_make(inner.p + 8, inner.n - 8);
      int ok = 0;
      if (rest.n >= 2 && rest.p[0] == '"' && rest.p[rest.n - 1] == '"') {
        ok = rest.n > 2;
        for (size_t k = 1; ok && k + 1 < rest.n; k++) {
          char d = rest.p[k];
          if (!((d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') || d == '-')) ok = 0;
        }
      }
      if (!ok) {
        tparse_fail_not_a_tag(st, inner);
        break;
      }
      Span name = span_make(rest.p + 1, rest.n - 2);
      TNode *n = tnode_new(st, TN_INCLUDE);
      if (!n) break;
      n->include_idx = -1;
      for (int i = 0; i < g_ntemplates; i++) {
        if (span_eq(g_templates[i].name, name)) {
          n->include_idx = i;
          break;
        }
      }
      *tail = n;
      tail = &n->next;
      st->p = after;
      if (tpl_tag_alone(st, tag, after) && st->p < st->end && *st->p == '\n') st->p++;
      continue;
    }
    // Unknown {% %} tag.
    tparse_fail_not_a_tag(st, inner);
    break;
  }
  return head;
}

// Compile one template. Returns 1 on success (root set), 0 on parse error
// (tpl->err set, root NULL).
static int tpl_compile(Arena *a, Template *tpl) {
  TParse st;
  st.a = a;
  st.tpl = tpl;
  st.p = tpl->source.p;
  st.end = tpl_end(tpl);
  st.err = NULL;
  st.err_len = 0;
  st.failed = 0;
  Span hit = {0};
  TNode *root = tpl_parse_seq(&st, NULL, NULL, &hit);
  if (st.failed) {
    tpl->root = NULL;
    tpl->err = st.err;
    tpl->err_len = st.err_len;
    return 0;
  }
  tpl->root = root;
  return 1;
}

// ---------------------------------------------------------------- values
//
// Template values are resolved lazily from the page/site structs — there
// is no Val object. A "value" at render time is one of:
//   * a span (string/number/date), rendered as textOf
//   * a list (tags, pages, toc, related, prev/next), iterated or counted
//   * a boolean (is_post), tested for truthiness
//   * nothing (missing lookup), rendering as ""

// ---------------------------------------------------------------- front matter
//
// A source's front matter is the strict subset: at most 7 keys
// (title, date, tags, draft, layout, slug, summary), one per line, inside
// "---\n...\n---\n". Values: strings (quoted or bare), dates (YYYY-MM-DD),
// booleans, or a bracketed list of strings. Anything else is a refusal.

typedef struct {
  Span title;    // raw value span (unescaped lazily)
  Span date;     // raw date span (YYYY-MM-DD) or empty
  Span *tags;    // array of raw tag spans
  int ntags;
  int draft;     // 0/1
  Span layout;   // raw layout span or empty (default later)
  Span slug;     // raw slug span or empty
  Span summary;  // raw summary span or empty
  int has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
} FrontMatter;

// The days of month `m` (1-12) of year `y`, by the Gregorian rule.
static int fm_days_in(int y, int m) {
  if (m == 2) return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 29 : 28;
  return m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31;
}

// Build an error span in the arena: `key "<key>": <msg>` when colon is
// set, `key "<key>" <msg>` otherwise. The reference writes the colon for
// the per-key parse errors (date, tags, draft, layout, slug, string)
// but not for `is given twice` / `has no value`.
static Span fm_key_error(Arena *a, Span key, const char *msg, int colon) {
  size_t n = strlen(msg);
  char *buf = (char *)arena_alloc(a, 5 + key.n + 2 + n + 1);
  if (!buf) return span_cstr("out of memory");
  size_t w = 0;
  memcpy(buf + w, "key \"", 5);
  w += 5;
  memcpy(buf + w, key.p, key.n);
  w += key.n;
  if (colon) {
    memcpy(buf + w, "\": ", 3);
    w += 3;
  } else {
    memcpy(buf + w, "\" ", 2);
    w += 2;
  }
  memcpy(buf + w, msg, n);
  w += n;
  buf[w] = '\0';
  return span_make(buf, w);
}

// A quoted-or-bare string value: "..." with \" and \\ its only escapes, or
// bare, the rest of the line (never opening on " or [). Returns 1 on
// success (*out = the value, unescaped into the arena when quoted),
// 0 on refusal (*err = the reason, without the `key "...": ` prefix).
static int fm_string_of(Arena *a, Span raw, Span *out, Span *err) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      *err = span_cstr("a quoted string is not closed");
      return 0;
    }
    char *buf = (char *)arena_alloc(a, raw.n + 1);
    if (!buf) {
      *err = span_cstr("out of memory");
      return 0;
    }
    size_t w = 0;
    for (size_t i = 1; i + 1 < raw.n; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        char n = raw.p[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.n - 1) {
          *err = span_cstr("a quoted string holds an escape other than \\\" or \\\\");
          return 0;
        }
        buf[w++] = n;
        i++;
      } else if (c == '"') {
        *err = span_cstr("a quoted string holds a stray quote");
        return 0;
      } else {
        buf[w++] = c;
      }
    }
    buf[w] = '\0';
    *out = span_make(buf, w);
    return 1;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    *err = span_cstr("a bare string opens on [");
    return 0;
  }
  *out = raw;
  return 1;
}

// Parse `text` (one source, "\n" line ends) as front matter + body.
// Returns 1 on success (fm filled, *body = the body span), 0 on refusal
// (*err = the exact refusal reason).
static int fm_parse(Arena *a, Span text, FrontMatter *fm, Span *body, Span *err) {
  memset(fm, 0, sizeof(*fm));
  fm->tags = NULL;
  fm->ntags = 0;
  // Line 0 must be exactly "---".
  const char *p = text.p;
  const char *end = text.p + text.n;
  const char *l0e = p;
  while (l0e < end && *l0e != '\n') l0e++;
  if ((size_t)(l0e - p) != 3 || memcmp(p, "---", 3) != 0) {
    *err = span_cstr("no front matter");
    return 0;
  }
  // Find the closing "---" line.
  const char *close_end = NULL;  // just past the closing line's '\n'
  const char *q = l0e < end ? l0e + 1 : end;
  int line_no = 1;  // 0-based index of the line starting at q
  while (q < end) {
    const char *le = q;
    while (le < end && *le != '\n') le++;
    if ((size_t)(le - q) == 3 && memcmp(q, "---", 3) == 0) {
      close_end = le < end ? le + 1 : end;
      break;
    }
    q = le < end ? le + 1 : end;
    line_no++;
  }
  if (!close_end) {
    *err = span_cstr("the front matter is not closed");
    return 0;
  }
  int seen = 0;  // bitmask of keys seen
  // Walk the lines between the --- lines (indices 1..line_no-1).
  q = l0e < end ? l0e + 1 : end;
  for (int idx = 1; idx < line_no; idx++) {
    const char *le = q;
    while (le < end && *le != '\n') le++;
    Span line = span_make(q, (size_t)(le - q));
    q = le < end ? le + 1 : end;
    // Blank lines and comments ("#" first) are skipped.
    int blank = 1;
    for (size_t i = 0; i < line.n; i++) {
      if (line.p[i] != ' ' && line.p[i] != '\t') {
        blank = 0;
        break;
      }
    }
    if (blank || (line.n > 0 && line.p[0] == '#')) continue;
    // LINE = /^([a-z_]+):(?: (.*))?$/
    size_t ki = 0;
    while (ki < line.n && ((line.p[ki] >= 'a' && line.p[ki] <= 'z') || line.p[ki] == '_')) ki++;
    if (ki == 0 || ki >= line.n || line.p[ki] != ':') {
      char tmp[64];
      int n = snprintf(tmp, sizeof(tmp), "line %d is not \"key: value\"", idx + 1);
      char *buf = (char *)arena_alloc(a, (size_t)n + 1);
      if (!buf) {
        *err = span_cstr("out of memory");
        return 0;
      }
      memcpy(buf, tmp, (size_t)n + 1);
      *err = span_make(buf, (size_t)n);
      return 0;
    }
    Span key = span_make(line.p, ki);
    Span raw;
    if (ki + 1 >= line.n) {
      raw = span_make(line.p + line.n, 0);
    } else if (line.p[ki + 1] == ' ') {
      raw = span_make(line.p + ki + 2, line.n - (ki + 2));
    } else {
      char tmp[64];
      int n = snprintf(tmp, sizeof(tmp), "line %d is not \"key: value\"", idx + 1);
      char *buf = (char *)arena_alloc(a, (size_t)n + 1);
      if (!buf) {
        *err = span_cstr("out of memory");
        return 0;
      }
      memcpy(buf, tmp, (size_t)n + 1);
      *err = span_make(buf, (size_t)n);
      return 0;
    }
    // raw right-trimmed of spaces and tabs.
    while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;
    int bit = 0;
    if (span_eq_c(key, "title")) bit = 1;
    else if (span_eq_c(key, "date")) bit = 2;
    else if (span_eq_c(key, "tags")) bit = 4;
    else if (span_eq_c(key, "draft")) bit = 8;
    else if (span_eq_c(key, "layout")) bit = 16;
    else if (span_eq_c(key, "slug")) bit = 32;
    else if (span_eq_c(key, "summary")) bit = 64;
    else {
      char *buf = (char *)arena_alloc(a, 13 + key.n + 1 + 1);
      if (!buf) {
        *err = span_cstr("out of memory");
        return 0;
      }
      size_t w = 0;
      memcpy(buf + w, "unknown key \"", 13);
      w += 13;
      memcpy(buf + w, key.p, key.n);
      w += key.n;
      buf[w++] = '"';
      buf[w] = '\0';
      *err = span_make(buf, w);
      return 0;
    }
    if (seen & bit) {
      *err = fm_key_error(a, key, "is given twice", 0);
      return 0;
    }
    seen |= bit;
    if (raw.n == 0) {
      *err = fm_key_error(a, key, "has no value", 0);
      return 0;
    }
    switch (bit) {
      case 1:
      case 64: {
        Span v;
        Span e;
        if (!fm_string_of(a, raw, &v, &e)) {
          *err = fm_key_error(a, key, e.p, 1);
          return 0;
        }
        if (bit == 1) {
          fm->has_title = 1;
          fm->title = v;
        } else {
          fm->has_summary = 1;
          fm->summary = v;
        }
        break;
      }
      case 2: {
        // DATE = /^(\d{4})-(\d{2})-(\d{2})$/
        int ok = 0;
        int y = 0, mo = 0, d = 0;
        if (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-') {
          ok = 1;
          for (int k = 0; k < 10; k++) {
            if (k == 4 || k == 7) continue;
            if (raw.p[k] < '0' || raw.p[k] > '9') ok = 0;
          }
          if (ok) {
            y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
            mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
            d = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
            if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > fm_days_in(y, mo)) ok = 0;
          }
        }
        if (!ok) {
          *err = fm_key_error(a, key, "not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)", 1);
          return 0;
        }
        fm->has_date = 1;
        fm->date = raw;
        break;
      }
      case 4: {
        if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
          *err = fm_key_error(a, key, "not a list [a, b]", 1);
          return 0;
        }
        Span inner = span_make(raw.p + 1, raw.n - 2);
        Span t = span_trim(inner);
        if (t.n == 0) {
          fm->has_tags = 1;
          fm->tags = NULL;
          fm->ntags = 0;
          break;
        }
        // Count the comma-separated items.
        int count = 1;
        for (size_t k = 0; k < inner.n; k++)
          if (inner.p[k] == ',') count++;
        fm->tags = (Span *)arena_alloc(a, sizeof(Span) * (count > 0 ? count : 1));
        if (!fm->tags) {
          *err = span_cstr("out of memory");
          return 0;
        }
        fm->ntags = 0;
        size_t start = 0;
        for (size_t k = 0; k <= inner.n; k++) {
          if (k == inner.n || inner.p[k] == ',') {
            Span item = span_trim(span_make(inner.p + start, k - start));
            int bad = item.n == 0;
            if (!bad) {
              for (size_t m = 0; m < item.n; m++) {
                char ch = item.p[m];
                if (ch == '"' || ch == '[' || ch == ']') {
                  bad = 1;
                  break;
                }
              }
            }
            if (bad) {
              *err = fm_key_error(a, key, "an empty tag, or one holding \" [ or ]", 1);
              return 0;
            }
            fm->tags[fm->ntags++] = item;
            start = k + 1;
          }
        }
        fm->has_tags = 1;
        break;
      }
      case 8:
        fm->has_draft = 1;
        if (span_eq_c(raw, "true")) fm->draft = 1;
        else if (span_eq_c(raw, "false")) fm->draft = 0;
        else {
          *err = fm_key_error(a, key, "true or false", 1);
          return 0;
        }
        break;
      case 16: {
        int ok = raw.n > 0;
        for (size_t k = 0; ok && k < raw.n; k++) {
          char ch = raw.p[k];
          if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) ok = 0;
        }
        if (!ok) {
          *err = fm_key_error(a, key, "not a template name", 1);
          return 0;
        }
        fm->has_layout = 1;
        fm->layout = raw;
        break;
      }
      case 32: {
        // SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/ and at most 64 characters.
        int ok = raw.n > 0 && raw.n <= 64;
        if (ok) {
          int prev_dash = 1;  // must start with [a-z0-9]
          for (size_t k = 0; k < raw.n; k++) {
            char ch = raw.p[k];
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
              prev_dash = 0;
            } else if (ch == '-' && !prev_dash) {
              prev_dash = 1;
            } else {
              ok = 0;
              break;
            }
          }
          if (prev_dash) ok = 0;  // must not end with '-'
        }
        if (!ok) {
          *err = fm_key_error(a, key, "not a slug", 1);
          return 0;
        }
        fm->has_slug = 1;
        fm->slug = raw;
        break;
      }
    }
  }
  if (!fm->has_title) {
    *err = span_cstr("no title");
    return 0;
  }
  *body = span_make(close_end, (size_t)(end - close_end));
  return 1;
}

// ---------------------------------------------------------------- pages

#define MAX_PAGES 1024
#define MAX_TAGS 1024

typedef struct Page Page;

// One tag link as templates read it: { name, slug, url }.
typedef struct {
  Span name;
  Span slug;
  Span url;
} TagLink;

// One TOC entry: { level, id, html }.
typedef struct {
  int level;
  Span id;
  Span html;
} TocEntry;

struct Page {
  Span path;        // source path, e.g. "posts/a.md"
  Span dir;         // directory part ("" for root), without trailing "/"
  Span title;       // unescaped
  Span date;        // "YYYY-MM-DD" or empty
  Span layout;      // layout template name
  Span slug;        // the page slug
  Span summary;     // unescaped ("" when absent)
  Span url;         // the page URL (arena)
  Span out_dir;     // the output directory (arena)
  Span content;     // rendered HTML body (arena)
  Span excerpt;     // first paragraph's inline HTML (arena, "" when none)
  TagLink *taglinks;  // the page's tags, each once, in its own order
  int ntaglinks;
  int is_post;
  // TOC entries.
  TocEntry *toc;
  int ntoc;
  // Neighbours within the post collection (newest first).
  Page *newer;  // newer post, or NULL
  Page *older;  // older post, or NULL
  // The source body (after front matter), as a span into the input.
  Span body;
  int words;  // wordsOf(body), for reading_minutes
};

typedef struct {
  Span name;       // tag name (first met in the posts' order)
  Span slug;       // tag slug
  Span url;        // `${base}/tags/${slug}/`
  Page **pages;    // posts with this tag, newest first
  int npages;
  int pages_cap;
} Tag;

// One static file.
typedef struct {
  Span path;
  Span content;
} StaticFile;

// The per-case build state.
typedef struct {
  Arena *a;
  Span site_title;
  Span base;  // site.base_url with trailing slashes stripped
  int per_page;
  int recent;
  Template templates[MAX_TEMPLATES];
  int ntemplates;
  Page pages[MAX_PAGES];
  int npages;
  Page *posts[MAX_PAGES];  // posts, newest first, then by slug
  int nposts;
  Tag tags[MAX_TAGS];
  int ntags;
  StaticFile statics[MAX_PAGES];
  int nstatics;
  // Report lines (arena spans), kept in insertion order; sorted at emit.
  Span *report;
  int nreport;
  int report_cap;
} Build;

static int build_report(Build *b, Span line) {
  if (b->nreport >= b->report_cap) {
    int nc = b->report_cap ? b->report_cap * 2 : 64;
    Span *nr = (Span *)arena_alloc(b->a, sizeof(Span) * nc);
    if (!nr) return 0;
    if (b->report) memcpy(nr, b->report, sizeof(Span) * b->nreport);
    b->report = nr;
    b->report_cap = nc;
  }
  b->report[b->nreport++] = line;
  return 1;
}

// A report line "refused <path>: <reason>" / "draft <path>" /
// "broken link <url> in <path>" / "template \"<name>\": <err>".
static void report_refused(Build *b, Span path, Span reason) {
  char *buf = (char *)arena_alloc(b->a, 9 + path.n + 2 + reason.n + 1);
  if (!buf) return;
  size_t w = 0;
  memcpy(buf + w, "refused ", 8);
  w += 8;
  memcpy(buf + w, path.p, path.n);
  w += path.n;
  memcpy(buf + w, ": ", 2);
  w += 2;
  memcpy(buf + w, reason.p, reason.n);
  w += reason.n;
  buf[w] = '\0';
  build_report(b, span_make(buf, w));
}

static void report_draft(Build *b, Span path) {
  char *buf = (char *)arena_alloc(b->a, 6 + path.n + 1);
  if (!buf) return;
  size_t w = 0;
  memcpy(buf + w, "draft ", 6);
  w += 6;
  memcpy(buf + w, path.p, path.n);
  w += path.n;
  buf[w] = '\0';
  build_report(b, span_make(buf, w));
}

static void report_broken_link(Build *b, Span path, Span target) {
  // `broken-link <source path> -> <target>`; a set, so each line once.
  char *buf = (char *)arena_alloc(b->a, 12 + path.n + 4 + target.n + 1);
  if (!buf) return;
  size_t w = 0;
  memcpy(buf + w, "broken-link ", 12);
  w += 12;
  memcpy(buf + w, path.p, path.n);
  w += path.n;
  memcpy(buf + w, " -> ", 4);
  w += 4;
  memcpy(buf + w, target.p, target.n);
  w += target.n;
  buf[w] = '\0';
  Span line = span_make(buf, w);
  for (int i = 0; i < b->nreport; i++)
    if (span_eq(b->report[i], line)) return;
  build_report(b, line);
}

static void report_template_error(Build *b, Span name, Span err) {
  char *buf = (char *)arena_alloc(b->a, 10 + name.n + 2 + err.n + 1);
  if (!buf) return;
  size_t w = 0;
  memcpy(buf + w, "template \"", 10);
  w += 10;
  memcpy(buf + w, name.p, name.n);
  w += name.n;
  memcpy(buf + w, "\": ", 3);
  w += 3;
  memcpy(buf + w, err.p, err.n);
  w += err.n;
  buf[w] = '\0';
  build_report(b, span_make(buf, w));
}

// ---------------------------------------------------------------- markdown
//
// makefun-site's Markdown subset, lexed into block tokens and rendered,
// in the shape of marked's lexer and parser.

typedef enum { BT_HEADING, BT_CODE, BT_HR, BT_QUOTE, BT_LIST, BT_PARA } BTKind;

typedef struct {
  BTKind kind;
  int depth;      // heading
  Span text;      // heading text / paragraph joined text
  Span lang;      // code
  Span *lines;    // code lines (raw) / quote lines (trimmed, non-empty)
  int nlines;
  int ordered;    // list
  long start;     // list
  Span *items;    // list items (joined with '\n')
  int nitems;
} BTok;

// The link environment for one source's Markdown.
typedef struct {
  Build *b;
  Span dir;       // the source's directory
  Span src_path;  // the source's path (for broken-link reports)
} MdEnv;

static Page *page_by_path(Build *b, Span path) {
  int lo = 0, hi = b->npages - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = span_cmp(path, b->pages[mid].path);
    if (c == 0) return &b->pages[mid];
    if (c < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return NULL;
}

static int static_has(Build *b, Span path) {
  int lo = 0, hi = b->nstatics - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = span_cmp(path, b->statics[mid].path);
    if (c == 0) return 1;
    if (c < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return 0;
}

static Span span_cat2(Arena *a, Span x, Span y) {
  char *buf = (char *)arena_alloc(a, x.n + y.n + 1);
  if (!buf) return x;
  memcpy(buf, x.p, x.n);
  memcpy(buf + x.n, y.p, y.n);
  buf[x.n + y.n] = '\0';
  return span_make(buf, x.n + y.n);
}

static Span span_cat3(Arena *a, Span x, Span y, Span z) {
  char *buf = (char *)arena_alloc(a, x.n + y.n + z.n + 1);
  if (!buf) return x;
  memcpy(buf, x.p, x.n);
  memcpy(buf + x.n, y.p, y.n);
  memcpy(buf + x.n + y.n, z.p, z.n);
  buf[x.n + y.n + z.n] = '\0';
  return span_make(buf, x.n + y.n + z.n);
}

static Span arena_span_of(Arena *a, const char *p, size_t n) {
  char *dst = (char *)arena_alloc(a, n + 1);
  if (!dst) return span_cstr("");
  memcpy(dst, p, n);
  dst[n] = '\0';
  return span_make(dst, n);
}

// A long as decimal digits (an arena span).
static Span num_span(Arena *a, long v) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%ld", v);
  return arena_span_of(a, tmp, (size_t)n);
}

// The spans joined with '\n' (no trailing newline).
static Span span_join_nl(Arena *a, Span *parts, int n) {
  if (n <= 0) return span_make("", 0);
  size_t total = 0;
  for (int i = 0; i < n; i++) total += parts[i].n + 1;
  char *buf = (char *)arena_alloc(a, total + 1);
  if (!buf) return parts[0];
  size_t w = 0;
  for (int i = 0; i < n; i++) {
    if (i > 0) buf[w++] = '\n';
    memcpy(buf + w, parts[i].p, parts[i].n);
    w += parts[i].n;
  }
  buf[w] = '\0';
  return span_make(buf, w);
}

// `slug` made unique against `taken` (and added to it).
static Span unique_slug(Arena *a, Span slug, Span *taken, int *ntaken) {
  for (int n = 1;; n++) {
    Span cand;
    if (n == 1) {
      cand = slug;
    } else {
      char tmp[16];
      int m = snprintf(tmp, sizeof(tmp), "-%d", n);
      cand = span_cat2(a, slug, span_make(tmp, (size_t)m));
    }
    int found = 0;
    for (int i = 0; i < *ntaken; i++) {
      if (span_eq(taken[i], cand)) {
        found = 1;
        break;
      }
    }
    if (!found) {
      taken[(*ntaken)++] = cand;
      return cand;
    }
  }
}

// `rel` (a link's path, "/"-separated) from the directory `dir` of a
// source, or 0 when it climbs out of the root.
static int md_resolve_path(Arena *a, Span dir, Span rel, Span *out) {
  Span parts[128];
  int nparts = 0;
  if (dir.n > 0) {
    size_t start = 0;
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (nparts < 128) parts[nparts++] = span_make(dir.p + start, i - start);
        start = i + 1;
      }
    }
  }
  size_t start = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Span seg = span_make(rel.p + start, i - start);
      start = i + 1;
      if (seg.n == 0 || (seg.n == 1 && seg.p[0] == '.')) continue;
      if (seg.n == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
        if (nparts == 0) return 0;
        nparts--;
      } else {
        if (nparts < 128) parts[nparts++] = seg;
      }
    }
  }
  size_t total = 0;
  for (int i = 0; i < nparts; i++) total += parts[i].n + 1;
  if (total == 0) {
    *out = span_make("", 0);
    return 1;
  }
  char *buf = (char *)arena_alloc(a, total + 1);
  if (!buf) return 0;
  size_t w = 0;
  for (int i = 0; i < nparts; i++) {
    if (i > 0) buf[w++] = '/';
    memcpy(buf + w, parts[i].p, parts[i].n);
    w += parts[i].n;
  }
  buf[w] = '\0';
  *out = span_make(buf, w);
  return 1;
}

// A link target resolved to the pages and static files it names.
static Span md_link(MdEnv *env, Span target) {
  if (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#')) return target;
  // /^[A-Za-z][A-Za-z0-9+.-]*:/
  if (target.n >= 2) {
    char c0 = target.p[0];
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
      size_t i = 1;
      while (i < target.n) {
        char c = target.p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') i++;
        else break;
      }
      if (i < target.n && target.p[i] == ':') return target;
    }
  }
  size_t hash = target.n;
  for (size_t i = 0; i < target.n; i++) {
    if (target.p[i] == '#') {
      hash = i;
      break;
    }
  }
  Span rel = span_make(target.p, hash);
  Span frag = span_make(target.p + hash, target.n - hash);
  Span path;
  if (rel.n == 0 || !md_resolve_path(env->b->a, env->dir, rel, &path)) {
    report_broken_link(env->b, env->src_path, target);
    return target;
  }
  Page *pg = page_by_path(env->b, path);
  if (pg) return span_cat2(env->b->a, pg->url, frag);
  if (static_has(env->b, path)) return span_cat3(env->b->a, env->b->base, span_cstr("/"), span_cat2(env->b->a, path, frag));
  report_broken_link(env->b, env->src_path, target);
  return target;
}

// Split a span into its '\n'-separated lines (without the '\n').
static Span *split_lines(Arena *a, Span s, int *n_out) {
  int n = 1;
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] == '\n') n++;
  Span *lines = (Span *)arena_alloc(a, sizeof(Span) * (n > 0 ? n : 1));
  int k = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.n; i++) {
    if (i == s.n || s.p[i] == '\n') {
      lines[k++] = span_make(s.p + start, i - start);
      start = i + 1;
    }
  }
  *n_out = k;
  return lines;
}

// A line's spaces and tabs at either end dropped.
static Span trim_line(Span s) {
  while (s.n > 0 && (s.p[0] == ' ' || s.p[0] == '\t')) {
    s.p++;
    s.n--;
  }
  while (s.n > 0 && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t')) s.n--;
  return s;
}

static int line_blank(Span s) {
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] != ' ' && s.p[i] != '\t') return 0;
  return 1;
}

// FENCE_OPEN = /^```([A-Za-z0-9_+-]*)[ \t]*$/
static int fence_open(Span s, Span *lang) {
  if (s.n < 3 || memcmp(s.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < s.n) {
    char c = s.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') i++;
    else break;
  }
  *lang = span_make(s.p + 3, i - 3);
  while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t')) i++;
  return i == s.n;
}

// FENCE_CLOSE = /^```[ \t]*$/
static int fence_close(Span s) {
  if (s.n < 3 || memcmp(s.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t')) i++;
  return i == s.n;
}

// A heading line's level, or 0 (a heading's text is never empty).
static int heading_of(Span s, Span *text) {
  size_t i = 0;
  while (i < s.n && s.p[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  if (i >= s.n || (s.p[i] != ' ' && s.p[i] != '\t')) return 0;
  size_t j = i;
  while (j < s.n && (s.p[j] == ' ' || s.p[j] == '\t')) j++;
  size_t e = s.n;
  while (e > j && (s.p[e - 1] == ' ' || s.p[e - 1] == '\t')) e--;
  if (e == j) return 0;
  *text = span_make(s.p + j, e - j);
  return (int)i;
}

// HR = /^(?:---|\*\*\*)[ \t]*$/
static int line_hr(Span s) {
  if (s.n < 3) return 0;
  if (memcmp(s.p, "---", 3) != 0 && memcmp(s.p, "***", 3) != 0) return 0;
  size_t i = 3;
  while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t')) i++;
  return i == s.n;
}

// Whether `line` ends a paragraph: a blank line, a fence, a heading or a
// thematic break.
static int md_ends_para(Span line) {
  Span lang;
  Span htext;
  return line_blank(line) || fence_open(line, &lang) || heading_of(line, &htext) != 0 || line_hr(line);
}

// `src`'s block tokens, in order.
static BTok *md_lex(Arena *a, Span src, int *n_out) {
  int nlines = 0;
  Span *lines = split_lines(a, src, &nlines);
  BTok *toks = (BTok *)arena_alloc(a, sizeof(BTok) * (nlines > 0 ? nlines : 1));
  int nt = 0;
  int i = 0;
  while (i < nlines) {
    Span line = lines[i];
    if (line_blank(line)) {
      i++;
      continue;
    }
    Span lang;
    if (fence_open(line, &lang)) {
      int start = i + 1;
      int j = start;
      while (j < nlines && !fence_close(lines[j])) j++;
      BTok *t = &toks[nt++];
      t->kind = BT_CODE;
      t->lang = lang;
      t->lines = lines + start;
      t->nlines = j - start;
      i = j + 1;
      continue;
    }
    Span htext;
    int depth = heading_of(line, &htext);
    if (depth > 0) {
      BTok *t = &toks[nt++];
      t->kind = BT_HEADING;
      t->depth = depth;
      t->text = htext;
      i++;
      continue;
    }
    if (line_hr(line)) {
      BTok *t = &toks[nt++];
      t->kind = BT_HR;
      i++;
      continue;
    }
    if (line.n > 0 && line.p[0] == '>') {
      int start = i;
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') i++;
      int count = i - start;
      Span *bl = (Span *)arena_alloc(a, sizeof(Span) * (count > 0 ? count : 1));
      int nb = 0;
      for (int k = start; k < i; k++) {
        Span l = lines[k];
        l.p++;
        l.n--;
        if (l.n > 0 && l.p[0] == ' ') {
          l.p++;
          l.n--;
        }
        l = trim_line(l);
        if (l.n > 0) bl[nb++] = l;
      }
      BTok *t = &toks[nt++];
      t->kind = BT_QUOTE;
      t->lines = bl;
      t->nlines = nb;
      continue;
    }
    // A list: BULLET /^[-*] (.*)$/ or ORDERED /^(\d{1,9})\. (.*)$/.
    int is_bullet = line.n >= 2 && (line.p[0] == '-' || line.p[0] == '*') && line.p[1] == ' ';
    int is_ordered = 0;
    long ord_start = 1;
    if (!is_bullet) {
      size_t d = 0;
      while (d < line.n && d < 9 && line.p[d] >= '0' && line.p[d] <= '9') d++;
      if (d >= 1 && d < line.n && line.p[d] == '.' && d + 1 < line.n && line.p[d + 1] == ' ') {
        is_ordered = 1;
        long v = 0;
        for (size_t k = 0; k < d; k++) v = v * 10 + (line.p[k] - '0');
        ord_start = v;
      }
    }
    if (is_bullet || is_ordered) {
      int maxl = nlines - i;
      Span *parts = (Span *)arena_alloc(a, sizeof(Span) * (maxl > 0 ? maxl : 1));
      int *bounds = (int *)arena_alloc(a, sizeof(int) * (maxl + 1 > 0 ? maxl + 1 : 1));
      int nparts = 0, nbounds = 0;
      while (i < nlines) {
        Span l = lines[i];
        int m = 0;
        Span content = {0};
        if (is_ordered) {
          size_t d = 0;
          while (d < l.n && d < 9 && l.p[d] >= '0' && l.p[d] <= '9') d++;
          if (d >= 1 && d < l.n && l.p[d] == '.' && d + 1 < l.n && l.p[d + 1] == ' ') {
            m = 1;
            content = span_make(l.p + d + 2, l.n - (d + 2));
          }
        } else {
          if (l.n >= 2 && (l.p[0] == '-' || l.p[0] == '*') && l.p[1] == ' ') {
            m = 1;
            content = span_make(l.p + 2, l.n - 2);
          }
        }
        if (m) {
          bounds[nbounds++] = nparts;
          parts[nparts++] = trim_line(content);
          i++;
        } else if (nparts > 0) {
          // CONTINUED = /^ {2,}\S/
          size_t sp = 0;
          while (sp < l.n && l.p[sp] == ' ') sp++;
          if (sp >= 2 && sp < l.n && l.p[sp] != ' ' && l.p[sp] != '\t') {
            parts[nparts++] = trim_line(l);
            i++;
          } else break;
        } else break;
      }
      bounds[nbounds] = nparts;
      Span *items = (Span *)arena_alloc(a, sizeof(Span) * (nbounds > 0 ? nbounds : 1));
      for (int k = 0; k < nbounds; k++)
        items[k] = span_join_nl(a, parts + bounds[k], bounds[k + 1] - bounds[k]);
      BTok *t = &toks[nt++];
      t->kind = BT_LIST;
      t->ordered = is_ordered;
      t->start = is_ordered ? ord_start : 1;
      t->items = items;
      t->nitems = nbounds;
      continue;
    }
    // A paragraph.
    {
      int start = i;
      int j = i + 1;
      while (j < nlines && !md_ends_para(lines[j])) j++;
      int count = j - start;
      Span *parts = (Span *)arena_alloc(a, sizeof(Span) * (count > 0 ? count : 1));
      for (int k = 0; k < count; k++) parts[k] = trim_line(lines[start + k]);
      BTok *t = &toks[nt++];
      t->kind = BT_PARA;
      t->text = span_join_nl(a, parts, count);
      i = j;
      continue;
    }
  }
  *n_out = nt;
  return toks;
}

// The HTML escape table, in this order: & < > " '. Clean runs are copied
// in bulk; only a character that needs an escape breaks the run.
static void md_escape_into(Buf *out, Span s) {
  size_t i = 0;
  while (i < s.n) {
    size_t j = i;
    while (j < s.n) {
      char c = s.p[j];
      if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') break;
      j++;
    }
    if (j > i) buf_put(out, s.p + i, j - i);
    if (j >= s.n) break;
    switch (s.p[j]) {
      case '&': buf_puts(out, "&amp;"); break;
      case '<': buf_puts(out, "&lt;"); break;
      case '>': buf_puts(out, "&gt;"); break;
      case '"': buf_puts(out, "&quot;"); break;
      case '\'': buf_puts(out, "&#39;"); break;
    }
    i = j + 1;
  }
}

static void md_escape_char(Buf *out, char c) {
  switch (c) {
    case '&': buf_puts(out, "&amp;"); break;
    case '<': buf_puts(out, "&lt;"); break;
    case '>': buf_puts(out, "&gt;"); break;
    case '"': buf_puts(out, "&quot;"); break;
    case '\'': buf_puts(out, "&#39;"); break;
    default: buf_putc(out, c); break;
  }
}

// The characters a backslash escapes.
static int md_escapable(char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']':
    case '(': case ')': case '#': case '+': case '-': case '.':
    case '!': case '>': return 1;
    default: return 0;
  }
}

// The first index from `from` where `s` holds `what` outside a backslash
// escape, or -1.
static long md_find_unescaped(Span s, const char *what, size_t wlen, size_t from) {
  for (size_t j = from; j + wlen <= s.n; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && md_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s.p + j, what, wlen) == 0) return (long)j;
  }
  return -1;
}

// The first single * from `from` (one whose run is not **), outside an
// escape, or -1.
static long md_find_single_star(Span s, size_t from) {
  for (size_t j = from; j < s.n; j++) {
    char c = s.p[j];
    if (c == '\\' && j + 1 < s.n && md_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return (long)j;
  }
  return -1;
}

static long md_index_of(Span s, char c, size_t from) {
  for (size_t i = from; i < s.n; i++)
    if (s.p[i] == c) return (long)i;
  return -1;
}

// `s` rendered inline; `env.link(target)` is a link's href.
static void md_inline(Buf *out, Span s, MdEnv *env) {
  size_t i = 0;
  while (i < s.n) {
    char c = s.p[i];
    if (c == '\\' && i + 1 < s.n && md_escapable(s.p[i + 1])) {
      md_escape_char(out, s.p[i + 1]);
      i += 2;
      continue;
    }
    if (c == '`') {
      long j = md_index_of(s, '`', i + 1);
      if (j > (long)i + 1) {
        buf_puts(out, "<code>");
        md_escape_into(out, span_make(s.p + i + 1, (size_t)(j - (long)i - 1)));
        buf_puts(out, "</code>");
        i = (size_t)j + 1;
      } else {
        buf_putc(out, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.n && s.p[i + 1] == '*') {
        long j = md_find_unescaped(s, "**", 2, i + 2);
        if (j > (long)i + 2) {
          buf_puts(out, "<strong>");
          md_inline(out, span_make(s.p + i + 2, (size_t)(j - (long)i - 2)), env);
          buf_puts(out, "</strong>");
          i = (size_t)j + 2;
        } else {
          buf_puts(out, "**");
          i += 2;
        }
        continue;
      }
      long j = md_find_single_star(s, i + 1);
      if (j > (long)i + 1) {
        buf_puts(out, "<em>");
        md_inline(out, span_make(s.p + i + 1, (size_t)(j - (long)i - 1)), env);
        buf_puts(out, "</em>");
        i = (size_t)j + 1;
      } else {
        buf_putc(out, '*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      long k = md_find_unescaped(s, "]", 1, i + 1);
      long m = -1;
      if (k != -1 && (size_t)k + 1 < s.n && s.p[k + 1] == '(') m = md_index_of(s, ')', (size_t)k + 2);
      Span target = {0};
      if (m != -1) target = span_make(s.p + k + 2, (size_t)(m - k - 2));
      if (target.n > 0 && !span_has_ws(target)) {
        buf_puts(out, "<a href=\"");
        Span href = md_link(env, target);
        md_escape_into(out, href);
        buf_puts(out, "\">");
        md_inline(out, span_make(s.p + i + 1, (size_t)(k - (long)i - 1)), env);
        buf_puts(out, "</a>");
        i = (size_t)m + 1;
        continue;
      }
      buf_putc(out, '[');
      i++;
      continue;
    }
    // A plain run: no markdown special and nothing to escape, copied whole.
    {
      size_t j = i;
      while (j < s.n) {
        char c = s.p[j];
        if (c == '\\' || c == '`' || c == '*' || c == '[' ||
            c == '&' || c == '<' || c == '>' || c == '"' || c == '\'')
          break;
        j++;
      }
      if (j > i) {
        buf_put(out, s.p + i, j - i);
        i = j;
        continue;
      }
    }
    md_escape_char(out, c);
    i++;
  }
}

typedef struct {
  Span html;
  Span excerpt;
  TocEntry *toc;
  int ntoc;
} MdResult;

// `src` rendered: its HTML (each block on its own lines, a "\n" after the
// last), its table of contents (every level 2 and 3 heading) and its
// excerpt (the first paragraph's inline HTML, or "").
static MdResult md_render(Arena *a, Span src, MdEnv *env) {
  int ntoks = 0;
  BTok *toks = md_lex(a, src, &ntoks);
  Span *ids = (Span *)arena_alloc(a, sizeof(Span) * (ntoks > 0 ? ntoks : 1));
  int nids = 0;
  TocEntry *toc = (TocEntry *)arena_alloc(a, sizeof(TocEntry) * (ntoks > 0 ? ntoks : 1));
  int ntoc = 0;
  Buf out;
  buf_init(&out, 4096);
  Span excerpt = span_cstr("");
  int have_excerpt = 0;
  for (int ti = 0; ti < ntoks; ti++) {
    BTok *t = &toks[ti];
    if (t->kind == BT_HEADING) {
      Span slug = slugify_span(a, t->text);
      Span id = unique_slug(a, slug, ids, &nids);
      Buf ib;
      buf_init(&ib, 256);
      md_inline(&ib, t->text, env);
      char tmp[32];
      int n = snprintf(tmp, sizeof(tmp), "<h%d id=\"", t->depth);
      buf_put(&out, tmp, (size_t)n);
      buf_put_span(&out, id);
      buf_puts(&out, "\">");
      buf_put(&out, ib.buf, ib.len);
      n = snprintf(tmp, sizeof(tmp), "</h%d>\n", t->depth);
      buf_put(&out, tmp, (size_t)n);
      if (t->depth == 2 || t->depth == 3) {
        toc[ntoc].level = t->depth;
        toc[ntoc].id = id;
        toc[ntoc].html = arena_span_of(a, ib.buf, ib.len);
        ntoc++;
      }
      buf_free(&ib);
    } else if (t->kind == BT_CODE) {
      buf_puts(&out, "<pre><code");
      if (t->lang.n > 0) {
        buf_puts(&out, " class=\"language-");
        md_escape_into(&out, t->lang);
        buf_putc(&out, '"');
      }
      buf_putc(&out, '>');
      for (int k = 0; k < t->nlines; k++) {
        md_escape_into(&out, t->lines[k]);
        buf_putc(&out, '\n');
      }
      buf_puts(&out, "</code></pre>\n");
    } else if (t->kind == BT_HR) {
      buf_puts(&out, "<hr>\n");
    } else if (t->kind == BT_QUOTE) {
      if (t->nlines == 0) {
        buf_puts(&out, "<blockquote>\n</blockquote>\n");
      } else {
        Span joined = span_join_nl(a, t->lines, t->nlines);
        Buf ib;
        buf_init(&ib, 256);
        md_inline(&ib, joined, env);
        buf_puts(&out, "<blockquote>\n<p>");
        buf_put(&out, ib.buf, ib.len);
        buf_puts(&out, "</p>\n</blockquote>\n");
        buf_free(&ib);
      }
    } else if (t->kind == BT_LIST) {
      const char *tag = t->ordered ? "ol" : "ul";
      if (t->ordered && t->start != 1) {
        char tmp[48];
        int n = snprintf(tmp, sizeof(tmp), "<ol start=\"%ld\">\n", t->start);
        buf_put(&out, tmp, (size_t)n);
      } else {
        buf_putc(&out, '<');
        buf_puts(&out, tag);
        buf_puts(&out, ">\n");
      }
      for (int k = 0; k < t->nitems; k++) {
        Buf ib;
        buf_init(&ib, 256);
        md_inline(&ib, t->items[k], env);
        buf_puts(&out, "<li>");
        buf_put(&out, ib.buf, ib.len);
        buf_puts(&out, "</li>\n");
        buf_free(&ib);
      }
      buf_puts(&out, "</");
      buf_puts(&out, tag);
      buf_puts(&out, ">\n");
    } else {
      Buf ib;
      buf_init(&ib, 256);
      md_inline(&ib, t->text, env);
      if (!have_excerpt) {
        excerpt = arena_span_of(a, ib.buf, ib.len);
        have_excerpt = 1;
      }
      buf_puts(&out, "<p>");
      buf_put(&out, ib.buf, ib.len);
      buf_puts(&out, "</p>\n");
      buf_free(&ib);
    }
  }
  MdResult r;
  r.html = arena_span_of(a, out.buf, out.len);
  r.excerpt = excerpt;
  r.toc = toc;
  r.ntoc = ntoc;
  buf_free(&out);
  return r;
}

// ---------------------------------------------------------------- template rendering
//
// A template renders in a chain of scopes; the values are resolved lazily
// from the page/site/tag structs — no Val object is ever built.

typedef struct {
  Span name;     // the loop variable name (a path's head matches this)
  uint8_t elem;  // EL_CARD, EL_TAGOBJ, EL_TAGLINK, EL_TOC
  const void *ptr;
  long index;   // 1-based
  long length;
} LoopFrame;

// The render context: everything a template can read.
typedef struct {
  Build *b;
  Page *page;        // the page being laid out, or NULL
  Page **posts;      // the index page's slice, or NULL
  int nposts;
  long pager_number, pager_total;
  Span pager_url, pager_newer_url, pager_older_url;
  int has_pager;
  Tag *tag;          // the tag page's tag, or NULL
  LoopFrame frames[8];
  int nframes;
} RCtx;

typedef enum { V_MISSING, V_STR, V_NUM, V_BOOL, V_LIST, V_OBJ } VKind;

typedef struct {
  VKind kind;
  Span str;          // V_STR
  long num;          // V_NUM
  int boolean;       // V_BOOL
  int list;          // V_LIST: L_* kind
  const void *owner;  // V_LIST: the owner (Build / Page / Tag / RCtx)
} Value;

static Value v_missing(void) {
  Value v;
  v.kind = V_MISSING;
  v.str = span_cstr("");
  v.num = 0;
  v.boolean = 0;
  v.list = L_NONE;
  v.owner = NULL;
  return v;
}

static Value v_str(Span s) {
  Value v = v_missing();
  v.kind = V_STR;
  v.str = s;
  return v;
}

static Value v_num(long n) {
  Value v = v_missing();
  v.kind = V_NUM;
  v.num = n;
  return v;
}

static Value v_bool(int b) {
  Value v = v_missing();
  v.kind = V_BOOL;
  v.boolean = b ? 1 : 0;
  return v;
}

static Value v_list(int list, const void *owner) {
  Value v = v_missing();
  v.kind = V_LIST;
  v.list = list;
  v.owner = owner;
  return v;
}

static Value v_obj(void) {
  Value v = v_missing();
  v.kind = V_OBJ;
  return v;
}

static const char *MONTH_NAMES[12] = {"January", "February", "March", "April", "May", "June",
                                      "July", "August", "September", "October", "November", "December"};

// A YYYY-MM-DD date in words, from the string alone ("2026-11-09" is
// "9 November 2026"); "" when there is no date.
static Span long_date(Arena *a, Span date) {
  if (date.n == 0) return span_cstr("");
  int m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
  int d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%d %s %.*s", d, MONTH_NAMES[m - 1], 4, date.p);
  return arena_span_of(a, tmp, (size_t)n);
}

// reading_minutes: max(1, ceil(words / 200)).
static long reading_minutes_of(Page *pg) {
  long m = (pg->words + 199) / 200;
  return m < 1 ? 1 : m;
}

// A card's field (a post as lists show it).
static Value card_field(RCtx *ctx, Page *pg, int f) {
  switch (f) {
    case F_TITLE: return v_str(pg->title);
    case F_URL: return v_str(pg->url);
    case F_DATE: return v_str(pg->date);
    case F_DATE_LONG: return v_str(long_date(ctx->b->a, pg->date));
    case F_SUMMARY: return v_str(pg->summary);
    case F_EXCERPT: return v_str(pg->excerpt);
    case F_READING_MINUTES: return v_num(reading_minutes_of(pg));
    case F_TAGS: return v_list(L_PAGE_TAGS, pg);
    default: return v_missing();
  }
}

// A tag-as-lists-show-it field ({ name, slug, url, count }).
static Value tagobj_field(Tag *t, int f) {
  switch (f) {
    case F_NAME: return v_str(t->name);
    case F_SLUG: return v_str(t->slug);
    case F_URL: return v_str(t->url);
    case F_COUNT: return v_num(t->npages);
    case F_POSTS: return v_list(L_TAG_POSTS, t);
    default: return v_missing();
  }
}

// A page's tag link field ({ name, slug, url }).
static Value taglink_field(TagLink *tl, int f) {
  switch (f) {
    case F_NAME: return v_str(tl->name);
    case F_SLUG: return v_str(tl->slug);
    case F_URL: return v_str(tl->url);
    default: return v_missing();
  }
}

// A TOC entry's field ({ level, id, html }).
static Value toc_field(TocEntry *te, int f) {
  switch (f) {
    case F_LEVEL: return v_num(te->level);
    case F_ID: return v_str(te->id);
    case F_HTML: return v_str(te->html);
    default: return v_missing();
  }
}

// A link's field ({ title, url, date, date_long }); the page is NULL when
// the link is missing.
static Value link_field(RCtx *ctx, Page *pg, int f) {
  if (!pg) return v_missing();
  switch (f) {
    case F_TITLE: return v_str(pg->title);
    case F_URL: return v_str(pg->url);
    case F_DATE: return v_str(pg->date);
    case F_DATE_LONG: return v_str(long_date(ctx->b->a, pg->date));
    default: return v_missing();
  }
}

// Resolve one dotted path at render time against the scope chain:
// `loop` is the innermost loop object; a loop variable shadows the
// top-level names when it shares one; then site/page/posts/pager/tag/
// tags/recent. The reference resolves paths the same way (lookup walks
// the scopes), so an included template reads its caller's loop variable.
static Value val_of(RCtx *ctx, Span path) {
  Span seg[5];
  int nseg = 0;
  size_t i = 0;
  while (i <= path.n && nseg < 5) {
    size_t j = i;
    while (j < path.n && path.p[j] != '.') j++;
    seg[nseg++] = span_make(path.p + i, j - i);
    i = j + 1;
  }
  if (nseg == 0 || nseg > 4) return v_missing();
  Span head = seg[0];
  // `loop` always names the innermost loop object, even when a loop
  // variable is also called `loop` (the reference's literal puts the
  // loop object after the variable).
  if (span_eq_c(head, "loop") && ctx->nframes > 0) {
    if (nseg == 1) return v_obj();
    LoopFrame *fr = &ctx->frames[ctx->nframes - 1];
    int f = field_id(seg[1]);
    if (nseg == 2) {
      switch (f) {
        case F_INDEX: return v_num(fr->index);
        case F_FIRST: return v_bool(fr->index == 1);
        case F_LAST: return v_bool(fr->index == fr->length);
        case F_LENGTH: return v_num(fr->length);
        default: return v_missing();
      }
    }
    return v_missing();
  }
  // Loop variables, innermost first.
  for (int k = ctx->nframes - 1; k >= 0; k--) {
    LoopFrame *fr = &ctx->frames[k];
    if (!span_eq(head, fr->name)) continue;
    if (nseg == 1) return v_obj();
    if (nseg == 2) {
      int f = field_id(seg[1]);
      switch (fr->elem) {
        case EL_CARD: return card_field(ctx, (Page *)fr->ptr, f);
        case EL_TAGOBJ: return tagobj_field((Tag *)fr->ptr, f);
        case EL_TAGLINK: return taglink_field((TagLink *)fr->ptr, f);
        case EL_TOC: return toc_field((TocEntry *)fr->ptr, f);
        default: return v_missing();
      }
    }
    return v_missing();
  }
  // Top-level names.
  if (span_eq_c(head, "site")) {
    if (nseg == 1) return v_obj();
    if (nseg == 2) {
      int f = field_id(seg[1]);
      if (f == F_TITLE) return v_str(ctx->b->site_title);
      if (f == F_BASE_URL) return v_str(ctx->b->base);
    }
    return v_missing();
  }
  if (span_eq_c(head, "page")) {
    Page *pg = ctx->page;
    if (!pg) return v_missing();
    if (nseg == 1) return v_obj();
    if (nseg == 2) {
      int f = field_id(seg[1]);
      switch (f) {
        case F_TITLE: return v_str(pg->title);
        case F_URL: return v_str(pg->url);
        case F_DATE: return v_str(pg->date);
        case F_DATE_LONG: return v_str(long_date(ctx->b->a, pg->date));
        case F_SUMMARY: return v_str(pg->summary);
        case F_EXCERPT: return v_str(pg->excerpt);
        case F_READING_MINUTES: return v_num(reading_minutes_of(pg));
        case F_TAGS: return v_list(L_PAGE_TAGS, pg);
        case F_SLUG: return v_str(pg->slug);
        case F_CONTENT: return v_str(pg->content);
        case F_TOC: return v_list(L_PAGE_TOC, pg);
        case F_IS_POST: return v_bool(pg->is_post);
        case F_NEWER: return pg->newer ? v_obj() : v_missing();
        case F_OLDER: return pg->older ? v_obj() : v_missing();
        default: return v_missing();
      }
    }
    if (nseg == 3) {
      int f0 = field_id(seg[1]);
      int f1 = field_id(seg[2]);
      if (f0 == F_NEWER || f0 == F_OLDER) {
        Page *link = f0 == F_NEWER ? pg->newer : pg->older;
        if (!link) return v_missing();
        return link_field(ctx, link, f1);
      }
    }
    return v_missing();
  }
  if (span_eq_c(head, "posts")) {
    if (nseg == 1) return v_list(L_POSTS, ctx);
    return v_missing();
  }
  if (span_eq_c(head, "pager")) {
    if (!ctx->has_pager) return v_missing();
    if (nseg == 1) return v_obj();
    if (nseg == 2) {
      int f = field_id(seg[1]);
      switch (f) {
        case F_NUMBER: return v_num(ctx->pager_number);
        case F_TOTAL: return v_num(ctx->pager_total);
        case F_URL: return v_str(ctx->pager_url);
        case F_NEWER_URL: return v_str(ctx->pager_newer_url);
        case F_OLDER_URL: return v_str(ctx->pager_older_url);
        default: return v_missing();
      }
    }
    return v_missing();
  }
  if (span_eq_c(head, "tag")) {
    Tag *t = ctx->tag;
    if (!t) return v_missing();
    if (nseg == 1) return v_obj();
    if (nseg == 2) return tagobj_field(t, field_id(seg[1]));
    return v_missing();
  }
  if (span_eq_c(head, "tags")) {
    if (nseg == 1) return v_list(L_TAGS, ctx->b);
    return v_missing();
  }
  if (span_eq_c(head, "recent")) {
    if (nseg == 1) return v_list(L_RECENT, ctx->b);
    return v_missing();
  }
  return v_missing();
}

// The element kind a list value's items have.
static uint8_t elem_of_list(int list) {
  switch (list) {
    case L_TAGS: return EL_TAGOBJ;
    case L_RECENT:
    case L_POSTS:
    case L_TAG_POSTS:
    case L_VAR_POSTS: return EL_CARD;
    case L_PAGE_TAGS:
    case L_CARD_TAGS: return EL_TAGLINK;
    case L_PAGE_TOC: return EL_TOC;
    default: return EL_UNKNOWN;
  }
}

// How many elements a list value holds.
static long list_count(Value v) {
  switch (v.list) {
    case L_TAGS: return ((Build *)v.owner)->ntags;
    case L_RECENT: {
      Build *b = (Build *)v.owner;
      return b->recent < b->nposts ? b->recent : b->nposts;
    }
    case L_POSTS: return ((RCtx *)v.owner)->nposts;
    case L_TAG_POSTS: return ((Tag *)v.owner)->npages;
    case L_PAGE_TAGS: return ((Page *)v.owner)->ntaglinks;
    case L_PAGE_TOC: return ((Page *)v.owner)->ntoc;
    case L_CARD_TAGS: return ((Page *)v.owner)->ntaglinks;
    case L_VAR_POSTS: return ((Tag *)v.owner)->npages;
    default: return 0;
  }
}

// The k-th element of a list value (a Page*, Tag*, TagLink* or TocEntry*).
static const void *list_elem_ptr(Value v, long k) {
  switch (v.list) {
    case L_TAGS: return &((Build *)v.owner)->tags[k];
    case L_RECENT: return ((Build *)v.owner)->posts[k];
    case L_POSTS: return ((RCtx *)v.owner)->posts[k];
    case L_TAG_POSTS: return ((Tag *)v.owner)->pages[k];
    case L_PAGE_TAGS: return &((Page *)v.owner)->taglinks[k];
    case L_PAGE_TOC: return &((Page *)v.owner)->toc[k];
    case L_CARD_TAGS: return &((Page *)v.owner)->taglinks[k];
    case L_VAR_POSTS: return ((Tag *)v.owner)->pages[k];
    default: return NULL;
  }
}

// Whether a value counts as true in an if: not undefined, null, false, "",
// 0 or an empty array.
static int value_truthy(Value v) {
  switch (v.kind) {
    case V_MISSING: return 0;
    case V_STR: return v.str.n > 0;
    case V_NUM: return v.num != 0;
    case V_BOOL: return v.boolean;
    case V_LIST: return list_count(v) > 0;
    case V_OBJ: return 1;
    default: return 0;
  }
}

// A value as text: a string itself, a number in JavaScript's digits, true
// or false, an array's items joined by ", ", else "".
static void render_var(Buf *out, Value v, int raw) {
  switch (v.kind) {
    case V_STR:
      if (raw) buf_put_span(out, v.str);
      else md_escape_into(out, v.str);
      break;
    case V_NUM: {
      char tmp[32];
      int n = snprintf(tmp, sizeof(tmp), "%ld", v.num);
      buf_put(out, tmp, (size_t)n);
      break;
    }
    case V_BOOL:
      buf_puts(out, v.boolean ? "true" : "false");
      break;
    case V_LIST: {
      // Every item is an object, so each renders as ""; the separators
      // are the only bytes.
      long n = list_count(v);
      for (long i = 1; i < n; i++) buf_puts(out, ", ");
      break;
    }
    default:
      break;
  }
}

// `nodes` rendered in this scope. `depth` is the include depth.
static void render_nodes(Buf *out, TNode *nodes, RCtx *ctx, int depth) {
  for (TNode *n = nodes; n; n = n->next) {
    switch (n->kind) {
      case TN_TEXT:
        buf_put_span(out, n->text);
        break;
      case TN_VAR: {
        Value v = val_of(ctx, n->path);
        render_var(out, v, n->raw);
        break;
      }
      case TN_IF: {
        Value v = val_of(ctx, n->path);
        int t = value_truthy(v);
        TNode *branch = (t != n->neg) ? n->first : n->second;
        if (branch) render_nodes(out, branch, ctx, depth);
        break;
      }
      case TN_FOR: {
        Value v = val_of(ctx, n->path);
        if (v.kind != V_LIST) break;
        long count = list_count(v);
        for (long k = 0; k < count; k++) {
          if (ctx->nframes >= 8) break;
          LoopFrame *fr = &ctx->frames[ctx->nframes++];
          fr->name = n->loop_var;
          fr->elem = elem_of_list(v.list);
          fr->ptr = list_elem_ptr(v, k);
          fr->index = k + 1;
          fr->length = count;
          render_nodes(out, n->first, ctx, depth);
          ctx->nframes--;
        }
        break;
      }
      case TN_INCLUDE: {
        if (n->include_idx < 0) break;
        Template *t = &ctx->b->templates[n->include_idx];
        if (t->root == NULL) break;
        if (depth < 8) render_nodes(out, t->root, ctx, depth + 1);
        break;
      }
    }
  }
}

// Render one layout by name; returns 0 when the template is missing or
// did not parse (the reference's null).
static int render_layout(Build *b, Span name, RCtx *ctx, Buf *out) {
  for (int i = 0; i < b->ntemplates; i++) {
    if (span_eq(b->templates[i].name, name)) {
      if (b->templates[i].root == NULL) return 0;
      render_nodes(out, b->templates[i].root, ctx, 0);
      return 1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- build pipeline

static long span_to_long(Span s) {
  long v = 0;
  int neg = 0;
  size_t i = 0;
  if (i < s.n && s.p[i] == '-') {
    neg = 1;
    i++;
  }
  for (; i < s.n; i++) {
    if (s.p[i] < '0' || s.p[i] > '9') break;
    v = v * 10 + (s.p[i] - '0');
  }
  return neg ? -v : v;
}

// Whether a code point is JavaScript whitespace (\s).
static int is_js_space(uint32_t cp) {
  if (cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20) return 1;
  if (cp == 0xA0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF) return 1;
  if (cp >= 0x2000 && cp <= 0x200A) return 1;
  return 0;
}

// The words of a body: its runs of characters other than whitespace.
static int words_of(Span s) {
  int n = 0;
  int in_word = 0;
  size_t i = 0;
  while (i < s.n) {
    size_t adv;
    uint32_t cp = utf8_decode(s.p + i, s.n - i, &adv);
    i += adv;
    if (is_js_space(cp)) in_word = 0;
    else if (!in_word) {
      in_word = 1;
      n++;
    }
  }
  return n;
}

// A source's "\r\n" and lone "\r" line ends made "\n" (a copy only when a
// '\r' is present).
static Span normalize_crlf(Arena *a, Span s) {
  if (memchr(s.p, '\r', s.n) == NULL) return s;
  char *buf = (char *)arena_alloc(a, s.n + 1);
  if (!buf) return s;
  size_t w = 0;
  for (size_t i = 0; i < s.n; i++) {
    if (s.p[i] == '\r') {
      if (i + 1 < s.n && s.p[i + 1] == '\n') continue;
      buf[w++] = '\n';
    } else {
      buf[w++] = s.p[i];
    }
  }
  buf[w] = '\0';
  return span_make(buf, w);
}

// The directory part of a path ("" when there is none).
static Span dir_of(Span path) {
  for (size_t i = path.n; i > 0; i--) {
    if (path.p[i - 1] == '/') return span_make(path.p, i - 1);
  }
  return span_make("", 0);
}

static int template_parses(Build *b, Span name) {
  for (int i = 0; i < b->ntemplates; i++) {
    if (span_eq(b->templates[i].name, name)) return b->templates[i].root != NULL;
  }
  return 0;
}

static void report_shadowed(Build *b, Span path) {
  const char *suffix = ": a built page has its path";
  size_t n = strlen(suffix);
  char *buf = (char *)arena_alloc(b->a, 9 + path.n + n + 1);
  if (!buf) return;
  size_t w = 0;
  memcpy(buf + w, "shadowed ", 9);
  w += 9;
  memcpy(buf + w, path.p, path.n);
  w += path.n;
  memcpy(buf + w, suffix, n);
  w += n;
  buf[w] = '\0';
  build_report(b, span_make(buf, w));
}

// ---------------------------------------------------------------- input parsing

typedef struct {
  Span path;
  Span content;
} InFile;

typedef struct {
  Span name;
  Span source;
} InTpl;

// Read the next member key of an object (after '{' or ','). Returns 1
// with *key set, 0 at the closing '}', -1 on error.
static int jobj_next(JScan *s, Span *key) {
  if (!jnext(s)) return -1;
  if (s->tok.kind == J_CLOSE_OBJ) return 0;
  if (s->tok.kind != J_STR) return -1;
  *key = span_make(s->tok.p, s->tok.n);
  if (!jnext(s) || s->tok.kind != J_COLON) return -1;
  return 1;
}

// After a member value: ',' (1), '}' (0), or an error (-1).
static int jobj_sep(JScan *s) {
  if (!jnext(s)) return -1;
  if (s->tok.kind == J_COMMA) return 1;
  if (s->tok.kind == J_CLOSE_OBJ) return 0;
  return -1;
}

static int cmp_infile_path(const void *x, const void *y) {
  return span_cmp(((const InFile *)x)->path, ((const InFile *)y)->path);
}

static int cmp_intpl_name(const void *x, const void *y) {
  return span_cmp(((const InTpl *)x)->name, ((const InTpl *)y)->name);
}

// Parse the case input JSON into the Build. Returns 0 on malformed input.
static int parse_input(Arena *a, const char *json, size_t len, Build *b,
                       InTpl **tpls_out, int *ntpls_out,
                       InFile **files_out, int *nfiles_out) {
  JScan s;
  s.cur = json;
  s.end = json + len;
  if (!jnext(&s) || s.tok.kind != J_OPEN_OBJ) return 0;
  InTpl *tpls = NULL;
  int ntpls = 0, tplcap = 0;
  InFile *files = NULL;
  int nfiles = 0, filecap = 0;
  while (1) {
    Span key;
    int r = jobj_next(&s, &key);
    if (r == 0) break;
    if (r < 0) return 0;
    if (span_eq_c(key, "site")) {
      if (!jnext(&s) || s.tok.kind != J_OPEN_OBJ) return 0;
      while (1) {
        Span sk;
        int r2 = jobj_next(&s, &sk);
        if (r2 == 0) break;
        if (r2 < 0) return 0;
        if (!jnext(&s)) return 0;
        if (span_eq_c(sk, "title") && s.tok.kind == J_STR) {
          b->site_title = json_unescape_span(a, span_make(s.tok.p, s.tok.n));
        } else if (span_eq_c(sk, "base_url") && s.tok.kind == J_STR) {
          b->base = json_unescape_span(a, span_make(s.tok.p, s.tok.n));
        } else if (span_eq_c(sk, "per_page") && s.tok.kind == J_NUM) {
          b->per_page = (int)span_to_long(span_make(s.tok.p, s.tok.n));
        } else if (span_eq_c(sk, "recent") && s.tok.kind == J_NUM) {
          b->recent = (int)span_to_long(span_make(s.tok.p, s.tok.n));
        } else {
          // An unexpected site member: skip a nested value.
          if (s.tok.kind == J_OPEN_OBJ || s.tok.kind == J_OPEN_ARR) {
            int depth = 1;
            while (depth > 0) {
              if (!jnext(&s)) return 0;
              if (s.tok.kind == J_OPEN_OBJ || s.tok.kind == J_OPEN_ARR) depth++;
              else if (s.tok.kind == J_CLOSE_OBJ || s.tok.kind == J_CLOSE_ARR) depth--;
            }
          }
        }
        int sep = jobj_sep(&s);
        if (sep == 0) break;
        if (sep < 0) return 0;
      }
    } else if (span_eq_c(key, "templates")) {
      if (!jnext(&s) || s.tok.kind != J_OPEN_OBJ) return 0;
      while (1) {
        Span name;
        int r2 = jobj_next(&s, &name);
        if (r2 == 0) break;
        if (r2 < 0) return 0;
        if (!jnext(&s) || s.tok.kind != J_STR) return 0;
        if (ntpls >= tplcap) {
          int nc = tplcap ? tplcap * 2 : 16;
          InTpl *nt = (InTpl *)arena_alloc(a, sizeof(InTpl) * nc);
          if (!nt) return 0;
          if (tpls) memcpy(nt, tpls, sizeof(InTpl) * ntpls);
          tpls = nt;
          tplcap = nc;
        }
        tpls[ntpls].name = json_unescape_span(a, name);
        tpls[ntpls].source = json_unescape_span(a, span_make(s.tok.p, s.tok.n));
        ntpls++;
        int sep = jobj_sep(&s);
        if (sep == 0) break;
        if (sep < 0) return 0;
      }
    } else if (span_eq_c(key, "files")) {
      if (!jnext(&s) || s.tok.kind != J_OPEN_ARR) return 0;
      if (!jnext(&s)) return 0;
      if (s.tok.kind != J_CLOSE_ARR) {
        while (1) {
          if (s.tok.kind != J_OPEN_OBJ) return 0;
          Span fpath = {0}, fcontent = {0};
          while (1) {
            Span fk;
            int r2 = jobj_next(&s, &fk);
            if (r2 == 0) break;
            if (r2 < 0) return 0;
            if (!jnext(&s) || s.tok.kind != J_STR) return 0;
            Span v = json_unescape_span(a, span_make(s.tok.p, s.tok.n));
            if (span_eq_c(fk, "path")) fpath = v;
            else if (span_eq_c(fk, "content")) fcontent = v;
            int sep = jobj_sep(&s);
            if (sep == 0) break;
            if (sep < 0) return 0;
          }
          if (nfiles >= filecap) {
            int nc = filecap ? filecap * 2 : 64;
            InFile *nf = (InFile *)arena_alloc(a, sizeof(InFile) * nc);
            if (!nf) return 0;
            if (files) memcpy(nf, files, sizeof(InFile) * nfiles);
            files = nf;
            filecap = nc;
          }
          files[nfiles].path = fpath;
          files[nfiles].content = fcontent;
          nfiles++;
          if (!jnext(&s)) return 0;
          if (s.tok.kind == J_CLOSE_ARR) break;
          if (s.tok.kind != J_COMMA) return 0;
          if (!jnext(&s)) return 0;
        }
      }
    } else {
      if (!jskip_value(&s)) return 0;
    }
    int sep = jobj_sep(&s);
    if (sep == 0) break;
    if (sep < 0) return 0;
  }
  if (ntpls > 1) qsort(tpls, (size_t)ntpls, sizeof(InTpl), cmp_intpl_name);
  if (nfiles > 1) qsort(files, (size_t)nfiles, sizeof(InFile), cmp_infile_path);
  *tpls_out = tpls;
  *ntpls_out = ntpls;
  *files_out = files;
  *nfiles_out = nfiles;
  return 1;
}

// ---------------------------------------------------------------- the plugins, in order

// 1. The sources, in code-unit order of their paths, line ends made "\n";
// a source that is not .md is static.
static void pipeline_read_sources(Build *b, InFile *files, int nfiles) {
  for (int i = 0; i < nfiles; i++) {
    Span path = files[i].path;
    if (span_ends_with(path, ".md")) {
      if (b->npages >= MAX_PAGES) continue;
      Page *pg = &b->pages[b->npages++];
      memset(pg, 0, sizeof(*pg));
      pg->path = path;
      pg->body = normalize_crlf(b->a, files[i].content);
    } else {
      if (b->nstatics >= MAX_PAGES) continue;
      b->statics[b->nstatics].path = path;
      b->statics[b->nstatics].content = files[i].content;
      b->nstatics++;
    }
  }
}

// 2. Each source's front matter: a refused source is reported and dropped,
// and so is a draft.
static void pipeline_front_matter(Build *b) {
  int kept = 0;
  for (int i = 0; i < b->npages; i++) {
    Page *pg = &b->pages[i];
    FrontMatter fm;
    Span body, err;
    if (!fm_parse(b->a, pg->body, &fm, &body, &err)) {
      report_refused(b, pg->path, err);
      continue;
    }
    pg->body = body;
    pg->title = fm.title;
    pg->date = fm.has_date ? fm.date : span_cstr("");
    pg->summary = fm.has_summary ? fm.summary : span_cstr("");
    pg->slug = fm.has_slug ? fm.slug : span_cstr("");
    pg->is_post = span_starts_with(pg->path, "posts/") ? 1 : 0;
    Span own = {0};
    if (span_starts_with(pg->path, "tags/")) own = span_cstr("tags");
    else if (span_starts_with(pg->path, "page/")) own = span_cstr("page");
    if (own.n > 0) {
      Span reason = span_cat2(b->a, span_cat2(b->a, own, span_cstr("/")), span_cstr(" is the generator's own"));
      report_refused(b, pg->path, reason);
      continue;
    }
    if (pg->is_post && !fm.has_date) {
      report_refused(b, pg->path, span_cstr("a post needs a date"));
      continue;
    }
    Span layout = fm.has_layout ? fm.layout : (pg->is_post ? span_cstr("post") : span_cstr("page"));
    if (!template_parses(b, layout)) {
      Span reason = span_cat3(b->a, span_cstr("layout \""), span_cat2(b->a, layout, span_cstr("\"")), span_cstr(" is no template that parses"));
      report_refused(b, pg->path, reason);
      continue;
    }
    pg->layout = layout;
    if (fm.has_draft && fm.draft) {
      report_draft(b, pg->path);
      continue;
    }
    // The page's tags, each slug once, in its own order.
    if (fm.has_tags && fm.ntags > 0) {
      pg->taglinks = (TagLink *)arena_alloc(b->a, sizeof(TagLink) * fm.ntags);
      pg->ntaglinks = 0;
      for (int k = 0; k < fm.ntags; k++) {
        Span slug = slugify_span(b->a, fm.tags[k]);
        int dup = 0;
        for (int m = 0; m < pg->ntaglinks; m++) {
          if (span_eq(pg->taglinks[m].slug, slug)) {
            dup = 1;
            break;
          }
        }
        if (dup) continue;
        pg->taglinks[pg->ntaglinks].name = fm.tags[k];
        pg->taglinks[pg->ntaglinks].slug = slug;
        pg->taglinks[pg->ntaglinks].url = span_cstr("");
        pg->ntaglinks++;
      }
    }
    if (kept != i) b->pages[kept] = b->pages[i];
    kept++;
  }
  b->npages = kept;
}

typedef struct {
  Span dir;
  Span *taken;
  int ntaken;
} DirTaken;

// 3. Each page's output directory and url: its slug made unique in its
// directory, in source order.
static void pipeline_permalinks(Build *b) {
  DirTaken *dirs = (DirTaken *)arena_alloc(b->a, sizeof(DirTaken) * (b->npages + 1 > 0 ? b->npages + 1 : 1));
  int ndirs = 0;
  for (int i = 0; i < b->npages; i++) {
    Page *pg = &b->pages[i];
    Span dir = pg->is_post ? span_cstr("posts") : dir_of(pg->path);
    pg->dir = dir;
    DirTaken *dt = NULL;
    for (int k = 0; k < ndirs; k++) {
      if (span_eq(dirs[k].dir, dir)) {
        dt = &dirs[k];
        break;
      }
    }
    if (!dt) {
      dt = &dirs[ndirs++];
      dt->dir = dir;
      dt->taken = (Span *)arena_alloc(b->a, sizeof(Span) * (b->npages + 4));
      dt->ntaken = 0;
      if (dir.n == 0) {
        dt->taken[dt->ntaken++] = span_cstr("posts");
        dt->taken[dt->ntaken++] = span_cstr("tags");
        dt->taken[dt->ntaken++] = span_cstr("page");
      }
    }
    Span want = pg->slug.n > 0 ? pg->slug : slugify_span(b->a, pg->title);
    Span slug = unique_slug(b->a, want, dt->taken, &dt->ntaken);
    pg->slug = slug;
    pg->out_dir = dir.n == 0 ? slug : span_cat2(b->a, span_cat2(b->a, dir, span_cstr("/")), slug);
    pg->url = span_cat3(b->a, span_cat2(b->a, b->base, span_cstr("/")), pg->out_dir, span_cstr("/"));
  }
}

// 4. Each body's Markdown, its links resolved to the pages and static
// files they name.
static void pipeline_markdown(Build *b) {
  for (int i = 0; i < b->npages; i++) {
    Page *pg = &b->pages[i];
    MdEnv env;
    env.b = b;
    env.dir = dir_of(pg->path);
    env.src_path = pg->path;
    MdResult r = md_render(b->a, pg->body, &env);
    pg->content = r.html;
    pg->excerpt = r.excerpt;
    pg->toc = r.toc;
    pg->ntoc = r.ntoc;
    pg->words = words_of(pg->body);
  }
}

static int cmp_post(const void *x, const void *y) {
  Page *a = *(Page **)x, *bb = *(Page **)y;
  int c = span_cmp(bb->date, a->date);  // date, newest first
  if (c != 0) return c;
  return span_cmp(a->slug, bb->slug);  // then by slug
}

// 5. The posts, newest first and then by slug; each post's newer and
// older neighbours.
static void pipeline_collections(Build *b) {
  b->nposts = 0;
  for (int i = 0; i < b->npages; i++)
    if (b->pages[i].is_post) b->posts[b->nposts++] = &b->pages[i];
  if (b->nposts > 1) qsort(b->posts, (size_t)b->nposts, sizeof(Page *), cmp_post);
  for (int k = 0; k < b->nposts; k++) {
    b->posts[k]->newer = k == 0 ? NULL : b->posts[k - 1];
    b->posts[k]->older = k == b->nposts - 1 ? NULL : b->posts[k + 1];
  }
}

static int cmp_tag(const void *x, const void *y) {
  const Tag *a = (const Tag *)x, *bb = (const Tag *)y;
  if (a->npages != bb->npages) return bb->npages - a->npages;  // count, most first
  return span_cmp(a->slug, bb->slug);  // then by slug
}

// 6. The tags: grouped by their slug, each named as first met in the
// posts' order, ordered by count and then slug.
static void pipeline_tags(Build *b) {
  for (int k = 0; k < b->nposts; k++) {
    Page *p = b->posts[k];
    for (int t = 0; t < p->ntaglinks; t++) {
      TagLink *tl = &p->taglinks[t];
      Tag *tag = NULL;
      for (int m = 0; m < b->ntags; m++) {
        if (span_eq(b->tags[m].slug, tl->slug)) {
          tag = &b->tags[m];
          break;
        }
      }
      if (!tag) {
        if (b->ntags >= MAX_TAGS) continue;
        tag = &b->tags[b->ntags++];
        memset(tag, 0, sizeof(*tag));
        tag->name = tl->name;
        tag->slug = tl->slug;
        tag->url = span_cat3(b->a, span_cat2(b->a, b->base, span_cstr("/tags/")), tl->slug, span_cstr("/"));
      }
      if (tag->npages >= tag->pages_cap) {
        int nc = tag->pages_cap ? tag->pages_cap * 2 : 8;
        Page **np = (Page **)arena_alloc(b->a, sizeof(Page *) * nc);
        if (tag->pages) memcpy(np, tag->pages, sizeof(Page *) * tag->npages);
        tag->pages = np;
        tag->pages_cap = nc;
      }
      tag->pages[tag->npages++] = p;
    }
  }
  // A page's tags link to the posts' tag of the same slug, and to nothing
  // (url "") where no post has it. The name shown is the tag's name as
  // first met in the posts' order, not the page's own spelling.
  for (int i = 0; i < b->npages; i++) {
    Page *pg = &b->pages[i];
    for (int t = 0; t < pg->ntaglinks; t++) {
      TagLink *tl = &pg->taglinks[t];
      tl->url = span_cstr("");
      for (int m = 0; m < b->ntags; m++) {
        if (span_eq(b->tags[m].slug, tl->slug)) {
          tl->url = b->tags[m].url;
          tl->name = b->tags[m].name;
          break;
        }
      }
    }
  }
  if (b->ntags > 1) qsort(b->tags, (size_t)b->ntags, sizeof(Tag), cmp_tag);
}

// One output pair (path -> text), in reference insertion order.
typedef struct {
  Span key;
  Span value;
} OutPair;

static Span index_url(Build *b, int k) {
  if (k == 1) return span_cat2(b->a, b->base, span_cstr("/"));
  Span u = span_cat3(b->a, b->base, span_cstr("/page/"), num_span(b->a, k));
  return span_cat2(b->a, u, span_cstr("/"));
}

// 7. Every page through its layout, the index pages and the tag pages
// through theirs.
static void pipeline_layouts(Build *b, OutPair **pairs_out, int *npairs_out) {
  int cap = b->npages + b->nposts + b->ntags + b->nstatics + 16;
  OutPair *pairs = (OutPair *)arena_alloc(b->a, sizeof(OutPair) * (cap > 0 ? cap : 1));
  int np = 0;
  for (int i = 0; i < b->npages; i++) {
    Page *pg = &b->pages[i];
    RCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.b = b;
    ctx.page = pg;
    Buf out;
    buf_init(&out, 4096);
    if (render_layout(b, pg->layout, &ctx, &out)) {
      pairs[np].key = span_cat2(b->a, pg->out_dir, span_cstr("/index.html"));
      pairs[np].value = arena_span_of(b->a, out.buf, out.len);
      np++;
    }
    buf_free(&out);
  }
  int per = b->per_page > 0 ? b->per_page : 1;
  int npages_idx = (b->nposts + per - 1) / per;
  if (npages_idx < 1) npages_idx = 1;
  for (int k = 1; k <= npages_idx; k++) {
    RCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.b = b;
    int start = (k - 1) * per;
    int count = b->nposts - start;
    if (count > per) count = per;
    if (count < 0) count = 0;
    ctx.posts = b->posts + start;
    ctx.nposts = count;
    ctx.has_pager = 1;
    ctx.pager_number = k;
    ctx.pager_total = npages_idx;
    ctx.pager_url = index_url(b, k);
    ctx.pager_newer_url = k == 1 ? span_cstr("") : index_url(b, k - 1);
    ctx.pager_older_url = k == npages_idx ? span_cstr("") : index_url(b, k + 1);
    Buf out;
    buf_init(&out, 4096);
    if (render_layout(b, span_cstr("index"), &ctx, &out)) {
      if (k == 1) {
        pairs[np].key = span_cstr("index.html");
      } else {
        pairs[np].key = span_cat3(b->a, span_cstr("page/"), num_span(b->a, k), span_cstr("/index.html"));
      }
      pairs[np].value = arena_span_of(b->a, out.buf, out.len);
      np++;
    }
    buf_free(&out);
  }
  for (int t = 0; t < b->ntags; t++) {
    RCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.b = b;
    ctx.tag = &b->tags[t];
    Buf out;
    buf_init(&out, 4096);
    if (render_layout(b, span_cstr("tag"), &ctx, &out)) {
      pairs[np].key = span_cat3(b->a, span_cstr("tags/"), b->tags[t].slug, span_cstr("/index.html"));      pairs[np].value = arena_span_of(b->a, out.buf, out.len);
      np++;
    }
    buf_free(&out);
  }
  {
    RCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.b = b;
    Buf out;
    buf_init(&out, 4096);
    if (render_layout(b, span_cstr("tags"), &ctx, &out)) {
      pairs[np].key = span_cstr("tags/index.html");
      pairs[np].value = arena_span_of(b->a, out.buf, out.len);
      np++;
    }
    buf_free(&out);
  }
  for (int i = 0; i < b->nstatics; i++) {
    Span path = b->statics[i].path;
    int shadowed = 0;
    for (int k = 0; k < np; k++) {
      if (span_eq(pairs[k].key, path)) {
        shadowed = 1;
        break;
      }
    }
    if (shadowed) {
      report_shadowed(b, path);
    } else {
      pairs[np].key = path;
      pairs[np].value = b->statics[i].content;
      np++;
    }
  }
  *pairs_out = pairs;
  *npairs_out = np;
}

static int cmp_span_qsort(const void *x, const void *y) {
  return span_cmp(*(const Span *)x, *(const Span *)y);
}

// 8. The report: every refusal, draft, broken link and template that did
// not parse, sorted, then the counts.
static void pipeline_report(Build *b, OutPair *pairs, int *npairs) {
  if (b->nreport > 1) qsort(b->report, (size_t)b->nreport, sizeof(Span), cmp_span_qsort);
  int npages = 0;
  for (int i = 0; i < b->npages; i++)
    if (!b->pages[i].is_post) npages++;
  char tmp[160];
  int n = snprintf(tmp, sizeof(tmp), "built %d pages, %d posts, %d tags and %d static files",
                   npages, b->nposts, b->ntags, b->nstatics);
  size_t total = 0;
  for (int i = 0; i < b->nreport; i++) total += b->report[i].n + 1;
  total += (size_t)n + 1;
  char *buf = (char *)arena_alloc(b->a, total + 1);
  size_t w = 0;
  for (int i = 0; i < b->nreport; i++) {
    memcpy(buf + w, b->report[i].p, b->report[i].n);
    w += b->report[i].n;
    buf[w++] = '\n';
  }
  memcpy(buf + w, tmp, (size_t)n);
  w += (size_t)n;
  buf[w++] = '\n';
  buf[w] = '\0';
  pairs[*npairs].key = span_cstr("build-report.txt");
  pairs[*npairs].value = span_make(buf, w);
  (*npairs)++;
}

// Build one case: parse the input JSON, run the pipeline, return the
// output pairs (keys in reference insertion order). Returns 0 on
// malformed input.
static int build_case(Arena *a, const char *json, size_t len, OutPair **pairs_out, int *npairs_out) {
  Build *b = (Build *)calloc(1, sizeof(Build));
  if (!b) return 0;
  b->a = a;
  InTpl *tpls = NULL;
  int ntpls = 0;
  InFile *files = NULL;
  int nfiles = 0;
  if (!parse_input(a, json, len, b, &tpls, &ntpls, &files, &nfiles)) {
    free(b);
    return 0;
  }
  // base = base_url with trailing slashes stripped.
  while (b->base.n > 0 && b->base.p[b->base.n - 1] == '/') b->base.n--;
  // Templates, in code-unit order of their names; a template that does
  // not parse is reported and dropped from the map.
  b->ntemplates = ntpls < MAX_TEMPLATES ? ntpls : MAX_TEMPLATES;
  for (int i = 0; i < b->ntemplates; i++) {
    b->templates[i].name = tpls[i].name;
    b->templates[i].source = tpls[i].source;
    b->templates[i].root = NULL;
    b->templates[i].err = NULL;
    b->templates[i].err_len = 0;
  }
  g_templates = b->templates;
  g_ntemplates = b->ntemplates;
  for (int i = 0; i < b->ntemplates; i++) {
    if (!tpl_compile(a, &b->templates[i])) {
      build_report(b, span_make(b->templates[i].err, b->templates[i].err_len));
    }
  }
  pipeline_read_sources(b, files, nfiles);
  pipeline_front_matter(b);
  pipeline_permalinks(b);
  pipeline_markdown(b);
  pipeline_collections(b);
  pipeline_tags(b);
  OutPair *pairs = NULL;
  int npairs = 0;
  pipeline_layouts(b, &pairs, &npairs);
  pipeline_report(b, pairs, &npairs);
  *pairs_out = pairs;
  *npairs_out = npairs;
  free(b);
  return 1;
}

// ---------------------------------------------------------------- the fast path
//
// buildCases(casesJson) answers every case in one native call and returns
// the answers JSON as bytes, so the driver never parses or stringifies
// JSON in JavaScript. The scorer compares answers by JSON value, so the
// emitter only has to produce valid JSON that decodes to the same strings
// (control characters escaped, everything else passed through as UTF-8).

// One span as a JSON string literal. Clean runs are copied in bulk; only
// a character that needs an escape breaks the run.
static void json_emit_str(Buf *out, Span s) {
  buf_putc(out, '"');
  size_t i = 0;
  while (i < s.n) {
    size_t j = i;
    while (j < s.n) {
      unsigned char c = (unsigned char)s.p[j];
      if (c == '"' || c == '\\' || c < 0x20) break;
      j++;
    }
    if (j > i) buf_put(out, s.p + i, j - i);
    if (j >= s.n) break;
    unsigned char c = (unsigned char)s.p[j];
    switch (c) {
      case '"': buf_put(out, "\\\"", 2); break;
      case '\\': buf_put(out, "\\\\", 2); break;
      case '\n': buf_put(out, "\\n", 2); break;
      case '\r': buf_put(out, "\\r", 2); break;
      case '\t': buf_put(out, "\\t", 2); break;
      case '\b': buf_put(out, "\\b", 2); break;
      case '\f': buf_put(out, "\\f", 2); break;
      default: {
        char tmp[8];
        int n = snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        buf_put(out, tmp, (size_t)n);
      }
    }
    i = j + 1;
  }
  buf_putc(out, '"');
}

// One case's answer as a JSON object, keys in reference insertion order.
static void json_emit_answer(Buf *out, OutPair *pairs, int npairs) {
  buf_putc(out, '{');
  for (int i = 0; i < npairs; i++) {
    if (i > 0) buf_putc(out, ',');
    json_emit_str(out, pairs[i].key);
    buf_putc(out, ':');
    json_emit_str(out, pairs[i].value);
  }
  buf_putc(out, '}');
}

// The whole cases.json answered at once: `[{id, input}, ...]` in,
// `[{...answer...}, ...]` out. The arena is reset between cases; each
// answer is emitted into `out` before the reset, so its spans are copied
// out while they are still live. Returns 0 on malformed input.
static int build_cases(Arena *a, const char *json, size_t len, Buf *out) {
  JScan s;
  s.cur = json;
  s.end = json + len;
  if (!jnext(&s) || s.tok.kind != J_OPEN_ARR) return 0;
  buf_putc(out, '[');
  int first_case = 1;
  while (1) {
    if (!jnext(&s)) return 0;
    if (s.tok.kind == J_CLOSE_ARR) break;
    if (s.tok.kind != J_OPEN_OBJ) return 0;
    // Walk this element's members; the "input" value's raw span is the
    // case's input JSON, fed straight to build_case.
    const char *input_start = NULL;
    const char *input_end = NULL;
    while (1) {
      Span key;
      int r = jobj_next(&s, &key);
      if (r == 0) break;
      if (r < 0) return 0;
      if (span_eq_c(key, "input")) {
        if (!jnext(&s) || s.tok.kind != J_OPEN_OBJ) return 0;
        // A `{` token carries no pointer; the scanner sits just past it.
        input_start = s.cur - 1;
        int depth = 1;
        while (depth > 0) {
          if (!jnext(&s)) return 0;
          if (s.tok.kind == J_OPEN_OBJ || s.tok.kind == J_OPEN_ARR) depth++;
          else if (s.tok.kind == J_CLOSE_OBJ || s.tok.kind == J_CLOSE_ARR) depth--;
        }
        // The scanner sits just past the closing `}`.
        input_end = s.cur;
      } else {
        // jskip_value reads the value's first token itself.
        if (!jskip_value(&s)) return 0;
      }
      int sep = jobj_sep(&s);
      if (sep == 0) break;
      if (sep < 0) return 0;
    }
    if (!input_start || !input_end) return 0;
    OutPair *pairs = NULL;
    int npairs = 0;
    if (!build_case(a, input_start, (size_t)(input_end - input_start), &pairs, &npairs)) return 0;
    if (!first_case) buf_putc(out, ',');
    first_case = 0;
    json_emit_answer(out, pairs, npairs);
    arena_reset(a);
    if (!jnext(&s)) return 0;
    if (s.tok.kind == J_CLOSE_ARR) break;
    if (s.tok.kind != J_COMMA) return 0;
  }
  buf_putc(out, ']');
  return 1;
}

// ---------------------------------------------------------------- N-API glue
//
// build(json) takes one case's input as a JSON string and returns its
// output tree as a JavaScript object (keys in reference insertion order).

#include <node_api.h>

static napi_value BuildCase(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_status st = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (st != napi_ok || argc < 1) {
    napi_value nullv;
    napi_get_null(env, &nullv);
    return nullv;
  }
  size_t len = 0;
  st = napi_get_value_string_utf8(env, argv[0], NULL, 0, &len);
  if (st != napi_ok) {
    napi_value nullv;
    napi_get_null(env, &nullv);
    return nullv;
  }
  char *input = (char *)malloc(len + 1);
  if (!input) {
    napi_value nullv;
    napi_get_null(env, &nullv);
    return nullv;
  }
  napi_get_value_string_utf8(env, argv[0], input, len + 1, &len);
  Arena arena;
  napi_value result = NULL;
  if (!arena_init(&arena, (size_t)SITEGEN_ARENA_MB * 1024 * 1024)) {
    free(input);
    napi_get_null(env, &result);
    return result;
  }
  OutPair *pairs = NULL;
  int npairs = 0;
  if (build_case(&arena, input, len, &pairs, &npairs)) {
    napi_create_object(env, &result);
    for (int i = 0; i < npairs; i++) {
      napi_value key, val;
      if (napi_create_string_utf8(env, pairs[i].key.p, pairs[i].key.n, &key) != napi_ok) continue;
      if (napi_create_string_utf8(env, pairs[i].value.p, pairs[i].value.n, &val) != napi_ok) continue;
      napi_set_property(env, result, key, val);
    }
  } else {
    napi_get_null(env, &result);
  }
  free(input);
  arena_free(&arena);
  return result;
}

// buildCases(casesJson) takes the whole cases.json — a Buffer of raw bytes
// (no JavaScript string conversion) — and returns the answers JSON as a
// Buffer, with no copy on the way out. Returns null on malformed input.
static void buf_finalizer(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  free(data);
}

static napi_value BuildCases(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_status st = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (st != napi_ok || argc < 1) {
    napi_value nullv;
    napi_get_null(env, &nullv);
    return nullv;
  }
  const char *input = NULL;
  size_t len = 0;
  char *owned = NULL;
  bool is_buffer = false;
  napi_is_buffer(env, argv[0], &is_buffer);
  if (is_buffer) {
    void *data = NULL;
    if (napi_get_buffer_info(env, argv[0], &data, &len) != napi_ok) {
      napi_value nullv;
      napi_get_null(env, &nullv);
      return nullv;
    }
    input = (const char *)data;
  } else {
    if (napi_get_value_string_utf8(env, argv[0], NULL, 0, &len) != napi_ok) {
      napi_value nullv;
      napi_get_null(env, &nullv);
      return nullv;
    }
    owned = (char *)malloc(len + 1);
    if (!owned) {
      napi_value nullv;
      napi_get_null(env, &nullv);
      return nullv;
    }
    napi_get_value_string_utf8(env, argv[0], owned, len + 1, &len);
    input = owned;
  }
  Arena arena;
  napi_value result = NULL;
  if (!arena_init(&arena, (size_t)SITEGEN_ARENA_MB * 1024 * 1024)) {
    free(owned);
    napi_get_null(env, &result);
    return result;
  }
  Buf out;
  buf_init(&out, 1 << 20);
  if (build_cases(&arena, input, len, &out)) {
    if (napi_create_external_buffer(env, out.len, out.buf, buf_finalizer, NULL, &result) != napi_ok) {
      buf_free(&out);
      napi_get_null(env, &result);
    }
  } else {
    buf_free(&out);
    napi_get_null(env, &result);
  }
  free(owned);
  arena_free(&arena);
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  napi_create_function(env, "build", NAPI_AUTO_LENGTH, BuildCase, NULL, &fn);
  napi_set_named_property(env, exports, "build", fn);
  napi_create_function(env, "buildCases", NAPI_AUTO_LENGTH, BuildCases, NULL, &fn);
  napi_set_named_property(env, exports, "buildCases", fn);
  return exports;
}
