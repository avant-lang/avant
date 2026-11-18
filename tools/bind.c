/*
 * avant bind — C header → Avant lib/fun and C layouts.
 *
 * Uses libclang. Hand-written lib blocks stay the same grammar; this
 * writes an editable starting point, not a second FFI.
 *
 * A record is emitted only when every field has a type map_field_type maps
 * and clang's size, align, and offsets equal the natural layout of those
 * Avant types (LP64). Anything else is skipped, named on stderr, and the
 * rest of the header is still emitted. A record passed or returned by
 * value fails the whole bind. An unsigned 32-bit type has no Avant type
 * at the same width and signedness, so it fails the whole bind too: it is
 * never silently mapped to Int.
 *
 * Usage: avant-bind LIBNAME HEADER.h
 */

#include <clang-c/Index.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RECS 128
#define MAX_NAME 128

typedef struct Bind Bind;

typedef struct {
  FILE *out;
  int failed;
  char err[256];
  Bind *bind;
} Emit;

static void fail(Emit *e, const char *msg) {
  if (!e->failed) {
    snprintf(e->err, sizeof(e->err), "%s", msg);
    e->failed = 1;
  }
}

static int valid_ident(const char *s) {
  if (!s || !s[0]) {
    return 0;
  }
  unsigned char c = (unsigned char)s[0];
  if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
    return 0;
  }
  for (s++; *s; s++) {
    c = (unsigned char)*s;
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
      return 0;
    }
  }
  return 1;
}

static CXType unwrap(CXType t) {
  while (t.kind == CXType_Typedef || t.kind == CXType_Elaborated || t.kind == CXType_Attributed) {
    if (t.kind == CXType_Typedef) {
      t = clang_getCanonicalType(t);
    } else if (t.kind == CXType_Elaborated) {
      t = clang_Type_getNamedType(t);
    } else {
      t = clang_Type_getModifiedType(t);
    }
  }
  return t;
}

static const char *strip_struct(const char *spelling) {
  if (strncmp(spelling, "struct ", 7) == 0) {
    return spelling + 7;
  }
  if (strncmp(spelling, "union ", 6) == 0) {
    return spelling + 6;
  }
  if (strncmp(spelling, "enum ", 5) == 0) {
    return spelling + 5;
  }
  if (strncmp(spelling, "const ", 6) == 0) {
    return strip_struct(spelling + 6);
  }
  return spelling;
}

static int is_char_type(CXType t) {
  t = unwrap(t);
  return t.kind == CXType_Char_S || t.kind == CXType_Char_U || t.kind == CXType_SChar;
}

/* The C scalar types: the Avant name of a
 * C integer or float type by its exact width and signedness, or NULL when
 * Avant has none (long double, a 128-bit integer). */
static const char *scalar_name(CXType t) {
  long long sz;
  int is_unsigned;
  t = unwrap(t);
  switch (t.kind) {
  case CXType_Char_S:
  case CXType_SChar:
  case CXType_Char_U:
  case CXType_UChar:
  case CXType_Short:
  case CXType_UShort:
  case CXType_Int:
  case CXType_UInt:
  case CXType_Long:
  case CXType_ULong:
  case CXType_LongLong:
  case CXType_ULongLong:
    break;
  case CXType_Float:
    return clang_Type_getSizeOf(t) == 4 ? "Float32" : NULL;
  case CXType_Double:
    return clang_Type_getSizeOf(t) == 8 ? "Float64" : NULL;
  default:
    return NULL;
  }
  sz = clang_Type_getSizeOf(t);
  is_unsigned = t.kind == CXType_Char_U || t.kind == CXType_UChar || t.kind == CXType_UShort || t.kind == CXType_UInt ||
                t.kind == CXType_ULong || t.kind == CXType_ULongLong;
  switch (sz) {
  case 1:
    return is_unsigned ? "UInt8" : "Int8";
  case 2:
    return is_unsigned ? "UInt16" : "Int16";
  /* C's int is Int32 and a 64-bit integer is Int (Avant's Int is
   * 64-bit). */
  case 4:
    return is_unsigned ? "UInt32" : "Int32";
  case 8:
    return is_unsigned ? "UInt64" : "Int";
  default:
    return NULL;
  }
}

static int bind_record_name(Bind *b, CXType t, char *out, size_t n);
static int record_by_value(Emit *e, CXType t);

static void emit_type(Emit *e, CXType t);
static const char *bind_enum_name(Bind *b, CXType t);
static const char *bind_enum_scalar(Bind *b, CXType t);

/* A pointer that is not a C string: Ptr(Void), Ptr(T), and a pointer to
 * a pointer as Ptr(Ptr(T)?), since C may leave the inner one NULL (an
 * out-parameter C writes, an array of pointers); char ** is
 * Ptr(Ptr(Int8)?). */
static void emit_ptr_plain(Emit *e, CXType pointee) {
  pointee = unwrap(pointee);
  if (pointee.kind == CXType_Void) {
    fputs("Ptr(Void)", e->out);
    return;
  }
  fputs("Ptr(", e->out);
  if (pointee.kind == CXType_Pointer) {
    emit_ptr_plain(e, clang_getPointeeType(pointee));
    fputs("?", e->out);
  } else {
    emit_type(e, pointee);
  }
  fputs(")", e->out);
}

static void emit_ptr(Emit *e, CXType pointee) {
  pointee = unwrap(pointee);
  if (is_char_type(pointee)) {
    fputs("String", e->out);
    return;
  }
  emit_ptr_plain(e, pointee);
}

static void emit_type(Emit *e, CXType t) {
  if (e->failed) {
    return;
  }
  t = unwrap(t);
  switch (t.kind) {
  case CXType_Void:
    fputs("Void", e->out);
    return;
  case CXType_Bool:
    fputs("Bool", e->out);
    return;
  case CXType_Char_S:
  case CXType_SChar:
  case CXType_Char_U:
  case CXType_UChar:
  case CXType_Short:
  case CXType_UShort:
  case CXType_Int:
  case CXType_UInt:
  case CXType_Long:
  case CXType_ULong:
  case CXType_LongLong:
  case CXType_ULongLong:
  case CXType_Float:
  case CXType_Double: {
    const char *name = scalar_name(t);
    if (!name) {
      goto unsupported_ty;
    }
    fputs(name, e->out);
    return;
  }
  case CXType_Pointer:
    emit_ptr(e, clang_getPointeeType(t));
    return;
  case CXType_Enum: {
    /* A C enum the header declares: its Avant enum
     * when one was emitted, else its integer type, as before. */
    const char *en = bind_enum_name(e->bind, t);
    if (en) {
      fputs(en, e->out);
      return;
    }
    const char *name = bind_enum_scalar(e->bind, t);
    if (!name) {
      goto unsupported_ty;
    }
    fputs(name, e->out);
    return;
  }
  case CXType_Record: {
    char chosen[MAX_NAME];
    if (bind_record_name(e->bind, t, chosen, sizeof(chosen))) {
      fputs(chosen, e->out);
      return;
    }
    CXString spelling = clang_getTypeSpelling(t);
    const char *raw = clang_getCString(spelling);
    const char *name = strip_struct(raw ? raw : "");
    if (!valid_ident(name)) {
      fail(e, "C record name is not an Avant identifier");
    } else {
      fputs(name, e->out);
    }
    clang_disposeString(spelling);
    return;
  }
  default:
  unsupported_ty: {
    CXString spelling = clang_getTypeSpelling(t);
    snprintf(e->err, sizeof(e->err), "unsupported C type %s", clang_getCString(spelling));
    e->failed = 1;
    clang_disposeString(spelling);
    return;
  }
  }
}

typedef struct {
  Emit *e;
  int index;
} ParamState;

static enum CXChildVisitResult visit_param(CXCursor cursor, CXCursor parent, CXClientData data) {
  (void)parent;
  ParamState *st = (ParamState *)data;
  if (clang_getCursorKind(cursor) != CXCursor_ParmDecl) {
    return CXChildVisit_Continue;
  }
  if (st->e->failed) {
    return CXChildVisit_Break;
  }
  if (st->index > 0) {
    fputs(", ", st->e->out);
  }

  CXType raw = clang_getCursorType(cursor);
  if (record_by_value(st->e, raw)) {
    return CXChildVisit_Break;
  }

  CXString spelling = clang_getCursorSpelling(cursor);
  const char *name = clang_getCString(spelling);
  if (name && name[0] && valid_ident(name)) {
    fprintf(st->e->out, "%s: ", name);
  } else {
    fprintf(st->e->out, "arg%d: ", st->index);
  }
  clang_disposeString(spelling);

  emit_type(st->e, raw);
  st->index++;
  return CXChildVisit_Continue;
}

static void emit_fun(Emit *e, CXCursor cursor) {
  CXString spelling = clang_getCursorSpelling(cursor);
  const char *name = clang_getCString(spelling);
  if (!valid_ident(name)) {
    fail(e, "C function name is not an Avant identifier");
    clang_disposeString(spelling);
    return;
  }

  fprintf(e->out, "  fun %s", name);
  clang_disposeString(spelling);

  int nargs = clang_Cursor_getNumArguments(cursor);
  if (nargs > 0) {
    fputs("(", e->out);
    ParamState st = {e, 0};
    clang_visitChildren(cursor, visit_param, &st);
    fputs(")", e->out);
  }

  CXType ret = clang_getCursorResultType(cursor);
  if (record_by_value(e, ret)) {
    return;
  }
  CXType uret = unwrap(ret);
  if (uret.kind != CXType_Void) {
    fputs(": ", e->out);
    emit_type(e, ret);
    /* A char * result may be NULL ("absent", "failed"): String? reads it
     * as nil, where a String result traps on NULL. Parameters stay String.
     * This is the only union a binding
     * gets: the test is emit_type's own for String, and a `fun`
     * signature refuses every union but an optional reference. */
    if (uret.kind == CXType_Pointer && is_char_type(unwrap(clang_getPointeeType(uret)))) {
      fputs("?", e->out);
    }
  }
  fputc('\n', e->out);
}

typedef struct {
  CXType canon;
  CXCursor decl;
  char tag[MAX_NAME];
  char typedef_name[MAX_NAME];
  char avant[MAX_NAME];
  int typedefs;
  int complete;
  int accepted;
  int size;
  int align;
} RecInfo;

/* A C enum: emitted as an Avant enum with explicit
 * values when it has a name, is 4 bytes, and its constants are distinct
 * Avant members within Int32. */
#define MAX_ENUMS 128

typedef struct {
  CXType canon;
  CXCursor decl;
  char tag[MAX_NAME];
  char typedef_name[MAX_NAME];
  char avant[MAX_NAME];
  int typedefs;
  int complete;
  int accepted;
  int wide; /* a constant outside Int32 */
  int anon; /* no tag and no typedef: its constants are C constants */
  char reason[400];
} EnumInfo;

struct Bind {
  Emit *e;
  RecInfo *recs;
  int nrecs;
  EnumInfo *enums;
  int nenums;
};

typedef struct {
  enum CXCursorKind kind;
  int found;
} AttrSeek;

typedef struct {
  Bind *b;
  RecInfo *rec;
  int ok;
  int offset;
  int max_align;
  int nfields;
  char reason[160];
} FieldState;

static int align_up(int n, int a) {
  if (a <= 1) {
    return n;
  }
  return (n + a - 1) / a * a;
}

static RecInfo *find_rec(Bind *b, CXType canon) {
  int i;
  for (i = 0; i < b->nrecs; i++) {
    if (clang_equalTypes(b->recs[i].canon, canon)) {
      return &b->recs[i];
    }
  }
  return NULL;
}

static int is_struct_record(CXType t) {
  CXCursor decl;
  t = unwrap(t);
  if (t.kind != CXType_Record) {
    return 0;
  }
  decl = clang_getTypeDeclaration(clang_getCanonicalType(t));
  return clang_getCursorKind(decl) == CXCursor_StructDecl;
}

static void copy_name(char *dst, size_t n, const char *src) {
  if (!src) {
    src = "";
  }
  snprintf(dst, n, "%s", src);
}

static RecInfo *add_rec(Bind *b, CXType canon) {
  RecInfo *rec;
  if (b->nrecs >= MAX_RECS) {
    fail(b->e, "too many C records");
    return NULL;
  }
  rec = &b->recs[b->nrecs++];
  memset(rec, 0, sizeof(*rec));
  rec->canon = clang_getCanonicalType(canon);
  return rec;
}

static RecInfo *rec_of(Bind *b, CXType t) {
  CXType canon;
  RecInfo *rec;
  t = unwrap(t);
  if (t.kind != CXType_Record) {
    return NULL;
  }
  if (!is_struct_record(t)) {
    return NULL;
  }
  canon = clang_getCanonicalType(t);
  rec = find_rec(b, canon);
  if (!rec) {
    rec = add_rec(b, canon);
  }
  return rec;
}

static void note_struct(Bind *b, CXCursor cursor) {
  RecInfo *rec;
  CXString spelling;
  const char *tag;
  long long sz;
  rec = rec_of(b, clang_getCursorType(cursor));
  if (!rec) {
    return;
  }
  spelling = clang_getCursorSpelling(cursor);
  tag = clang_getCString(spelling);
  if (tag && tag[0] && !rec->tag[0]) {
    copy_name(rec->tag, sizeof(rec->tag), tag);
  }
  clang_disposeString(spelling);
  sz = clang_Type_getSizeOf(clang_getCursorType(cursor));
  if (sz >= 0) {
    rec->decl = cursor;
    rec->complete = 1;
  }
}

static void note_typedef(Bind *b, CXCursor cursor) {
  RecInfo *rec;
  CXType under;
  CXString spelling;
  const char *name;
  under = clang_getTypedefDeclUnderlyingType(cursor);
  rec = rec_of(b, under);
  if (!rec) {
    return;
  }
  spelling = clang_getCursorSpelling(cursor);
  name = clang_getCString(spelling);
  rec->typedefs++;
  if (rec->typedefs == 1) {
    copy_name(rec->typedef_name, sizeof(rec->typedef_name), name);
  }
  clang_disposeString(spelling);
}

static EnumInfo *enum_of(Bind *b, CXType t) {
  CXType canon;
  int i;
  t = unwrap(t);
  if (t.kind != CXType_Enum) {
    return NULL;
  }
  canon = clang_getCanonicalType(t);
  for (i = 0; i < b->nenums; i++) {
    if (clang_equalTypes(b->enums[i].canon, canon)) {
      return &b->enums[i];
    }
  }
  if (b->nenums >= MAX_ENUMS) {
    fail(b->e, "too many C enums");
    return NULL;
  }
  EnumInfo *en = &b->enums[b->nenums++];
  memset(en, 0, sizeof(*en));
  en->canon = canon;
  return en;
}

static void note_enum(Bind *b, CXCursor cursor) {
  EnumInfo *en = enum_of(b, clang_getCursorType(cursor));
  CXString spelling;
  const char *tag;
  if (!en) {
    return;
  }
  if (!clang_Cursor_isAnonymous(cursor)) {
    spelling = clang_getCursorSpelling(cursor);
    tag = clang_getCString(spelling);
    if (tag && tag[0] && !en->tag[0]) {
      copy_name(en->tag, sizeof(en->tag), tag);
    }
    clang_disposeString(spelling);
  }
  if (clang_isCursorDefinition(cursor)) {
    en->decl = cursor;
    en->complete = 1;
  }
}

static void note_enum_typedef(Bind *b, CXCursor cursor) {
  CXType under = unwrap(clang_getTypedefDeclUnderlyingType(cursor));
  EnumInfo *en;
  CXString spelling;
  if (under.kind != CXType_Enum) {
    return;
  }
  en = enum_of(b, under);
  if (!en) {
    return;
  }
  spelling = clang_getCursorSpelling(cursor);
  en->typedefs++;
  if (en->typedefs == 1) {
    copy_name(en->typedef_name, sizeof(en->typedef_name), clang_getCString(spelling));
  }
  clang_disposeString(spelling);
}

/* The Avant type name of a C enum: CamelCase, a trailing _t dropped
 * (`enum http_method` -> HttpMethod, `color_t` -> Color). */
static void enum_camel(const char *src, char *out, size_t n) {
  size_t len = strlen(src);
  size_t i;
  size_t k = 0;
  int up = 1;
  if (len > 2 && src[len - 2] == '_' && src[len - 1] == 't') {
    len -= 2;
  }
  for (i = 0; i < len && k + 1 < n; i++) {
    char c = src[i];
    if (c == '_') {
      up = 1;
      continue;
    }
    if (up && c >= 'a' && c <= 'z') {
      c = (char)(c - 32);
    }
    up = 0;
    out[k++] = c;
  }
  out[k] = 0;
}

static int avant_word(const char *s) {
  static const char *words[] = {"fn", "if", "else", "return", "struct", "class", "lib", "fun", "self", "while", "true", "false", "nil", "spawn", "break", "continue", "switch", "case", "import", "pub", "quote", "comptime", "interface", "for", "in", "pin", "release", "enum", "const",
                                /* what every enum has as a type-level name */
                                "members", "from_i", "from_name", NULL};
  int i;
  for (i = 0; words[i]; i++) {
    if (strcmp(s, words[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  EnumInfo *en;
  FILE *out;
  int count;
  int ok;
  int is_unsigned;
  char names[256][MAX_NAME];
  long long vals[256];
  char reason[200];
} EnumWalk;

static void lower_name(const char *src, char *out, size_t n) {
  size_t k = 0;
  for (; *src && k + 1 < n; src++) {
    char c = *src;
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 32);
    }
    out[k++] = c;
  }
  out[k] = 0;
}

/* Each constant: lower-cased; an alias (a value seen before) is dropped
 * and named; a value outside Int32, a name that is no Avant member, or a
 * collision after lower-casing skips the enum. With out set, writes the
 * members. */
static enum CXChildVisitResult walk_enum_const(CXCursor cursor, CXCursor parent, CXClientData data) {
  EnumWalk *w = (EnumWalk *)data;
  CXString spelling;
  const char *raw;
  char low[MAX_NAME];
  long long v;
  int i;
  (void)parent;
  if (!w->ok) {
    return CXChildVisit_Break;
  }
  if (clang_getCursorKind(cursor) != CXCursor_EnumConstantDecl) {
    return CXChildVisit_Continue;
  }
  spelling = clang_getCursorSpelling(cursor);
  raw = clang_getCString(spelling);
  lower_name(raw ? raw : "", low, sizeof(low));
  if (w->is_unsigned) {
    unsigned long long u = clang_getEnumConstantDeclUnsignedValue(cursor);
    v = u > 2147483647ULL ? 2147483648LL : (long long)u;
  } else {
    v = clang_getEnumConstantDeclValue(cursor);
  }
  if (v < -2147483648LL || v > 2147483647LL) {
    snprintf(w->reason, sizeof(w->reason), "the value of %s does not fit in Int32; skipped", raw ? raw : "?");
    w->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  if (!valid_ident(low) || avant_word(low)) {
    snprintf(w->reason, sizeof(w->reason), "%s is no Avant member name (%s); skipped", raw ? raw : "?", low);
    w->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  for (i = 0; i < w->count; i++) {
    if (strcmp(w->names[i], low) == 0) {
      snprintf(w->reason, sizeof(w->reason), "two constants are %s after lower-casing; skipped", low);
      w->ok = 0;
      clang_disposeString(spelling);
      return CXChildVisit_Break;
    }
  }
  for (i = 0; i < w->count; i++) {
    if (w->vals[i] == v) {
      if (w->out) {
        fprintf(w->out, "    // %s is an alias of %s (%lld); dropped\n", raw ? raw : "?", w->names[i], v);
      }
      clang_disposeString(spelling);
      return CXChildVisit_Continue;
    }
  }
  if (w->count >= 256) {
    snprintf(w->reason, sizeof(w->reason), "more than 256 constants; skipped");
    w->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  copy_name(w->names[w->count], MAX_NAME, low);
  w->vals[w->count] = v;
  w->count++;
  if (w->out) {
    fprintf(w->out, "    %s = %lld\n", low, v);
  }
  clang_disposeString(spelling);
  return CXChildVisit_Continue;
}

static int enum_unsigned(EnumInfo *en) {
  CXType it = unwrap(clang_getEnumDeclIntegerType(en->decl));
  return it.kind == CXType_UInt || it.kind == CXType_UChar || it.kind == CXType_UShort || it.kind == CXType_ULong ||
         it.kind == CXType_ULongLong || it.kind == CXType_Char_U;
}

static enum CXChildVisitResult see_wide_const(CXCursor cursor, CXCursor parent, CXClientData data) {
  EnumInfo *en = (EnumInfo *)data;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_EnumConstantDecl) {
    return CXChildVisit_Continue;
  }
  if (enum_unsigned(en) ? clang_getEnumConstantDeclUnsignedValue(cursor) > 2147483647ULL
                        : (clang_getEnumConstantDeclValue(cursor) < -2147483648LL || clang_getEnumConstantDeclValue(cursor) > 2147483647LL)) {
    en->wide = 1;
  }
  return CXChildVisit_Continue;
}

static void skip_enum(EnumInfo *en, const char *label, const char *reason) {
  en->accepted = 0;
  /* Named by a comment in the output (emit_enums), not on stderr: the
   * output stays a program that compiles. */
  snprintf(en->reason, sizeof(en->reason), "%s: %s", label, reason);
}

static void decide_enum(Bind *b, EnumInfo *en) {
  const char *s = NULL;
  EnumWalk *w;
  int i;
  if (!en->complete) {
    return;
  }
  clang_visitChildren(en->decl, see_wide_const, en);
  if (en->typedefs == 1) {
    s = en->typedef_name;
  } else if (en->tag[0]) {
    s = en->tag;
  }
  if (!s) {
    if (en->typedefs == 0) {
      /* `enum { FLAG_A = 1 };` is how C spells constants: emitted as
       * module constants after the lib block (emit_consts). */
      en->anon = 1;
      en->accepted = 0;
      return;
    }
    skip_enum(en, "enum", "two typedefs; skipped");
    return;
  }
  enum_camel(s, en->avant, sizeof(en->avant));
  if (!valid_ident(en->avant) || !(en->avant[0] >= 'A' && en->avant[0] <= 'Z')) {
    en->avant[0] = 0;
    skip_enum(en, s, "enum name is not an Avant type name; skipped");
    return;
  }
  for (i = 0; i < b->nrecs; i++) {
    if (strcmp(b->recs[i].avant, en->avant) == 0) {
      skip_enum(en, s, "enum name is a record's name; skipped");
      en->avant[0] = 0;
      return;
    }
  }
  for (i = 0; i < b->nenums; i++) {
    if (&b->enums[i] != en && b->enums[i].accepted && strcmp(b->enums[i].avant, en->avant) == 0) {
      skip_enum(en, s, "enum name is another enum's name; skipped");
      en->avant[0] = 0;
      return;
    }
  }
  if (clang_Type_getSizeOf(clang_getCursorType(en->decl)) != 4) {
    skip_enum(en, s, "enum is not 4 bytes (C's int); skipped");
    en->avant[0] = 0;
    return;
  }
  w = calloc(1, sizeof(EnumWalk));
  if (!w) {
    fail(b->e, "out of memory");
    return;
  }
  w->en = en;
  w->ok = 1;
  w->is_unsigned = enum_unsigned(en);
  clang_visitChildren(en->decl, walk_enum_const, w);
  if (w->ok && w->count == 0) {
    snprintf(w->reason, sizeof(w->reason), "enum has no constant; skipped");
    w->ok = 0;
  }
  if (!w->ok) {
    skip_enum(en, s, w->reason);
    en->avant[0] = 0;
    free(w);
    return;
  }
  free(w);
  en->accepted = 1;
}

static void emit_enums(Bind *b) {
  int i;
  for (i = 0; i < b->nenums; i++) {
    EnumInfo *en = &b->enums[i];
    if (!en->complete) {
      continue;
    }
    if (en->anon) {
      continue;
    }
    if (!en->accepted) {
      fprintf(b->e->out, "  // %s\n", en->reason);
      continue;
    }
    EnumWalk *w = calloc(1, sizeof(EnumWalk));
    if (!w) {
      fail(b->e, "out of memory");
      return;
    }
    w->en = en;
    w->ok = 1;
    w->out = b->e->out;
    w->is_unsigned = enum_unsigned(en);
    fprintf(b->e->out, "  enum %s {\n", en->avant);
    clang_visitChildren(en->decl, walk_enum_const, w);
    fprintf(b->e->out, "  }\n");
    free(w);
  }
}

/* The constants of the anonymous enums, after the lib block: each a
 * module constant (`pub const flag_a = 1`), its name the C name
 * lower-cased, its value C's (a constant takes its place's type, as a
 * literal does). A name that is no Avant name, a keyword, or one an
 * earlier constant took after lower-casing is left out with a comment. */
typedef struct {
  FILE *out;
  int unsigned_values;
  char (*names)[MAX_NAME];
  int *count;
} ConstWalk;

#define MAX_CONSTS 1024

static enum CXChildVisitResult walk_const(CXCursor cursor, CXCursor parent, CXClientData data) {
  ConstWalk *w = (ConstWalk *)data;
  CXString spelling;
  const char *raw;
  char low[MAX_NAME];
  int i;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_EnumConstantDecl) {
    return CXChildVisit_Continue;
  }
  spelling = clang_getCursorSpelling(cursor);
  raw = clang_getCString(spelling);
  lower_name(raw ? raw : "", low, sizeof(low));
  if (!valid_ident(low) || avant_word(low) || !(low[0] >= 'a' && low[0] <= 'z')) {
    fprintf(w->out, "// %s is no Avant constant name (%s); skipped\n", raw ? raw : "?", low);
    clang_disposeString(spelling);
    return CXChildVisit_Continue;
  }
  for (i = 0; i < *w->count; i++) {
    if (strcmp(w->names[i], low) == 0) {
      fprintf(w->out, "// %s is the constant %s again after lower-casing; skipped\n", raw ? raw : "?", low);
      clang_disposeString(spelling);
      return CXChildVisit_Continue;
    }
  }
  if (*w->count >= MAX_CONSTS) {
    fprintf(w->out, "// %s: more than %d constants; skipped\n", raw ? raw : "?", MAX_CONSTS);
    clang_disposeString(spelling);
    return CXChildVisit_Continue;
  }
  copy_name(w->names[*w->count], MAX_NAME, low);
  *w->count += 1;
  if (w->unsigned_values) {
    fprintf(w->out, "pub const %s = %llu\n", low, clang_getEnumConstantDeclUnsignedValue(cursor));
  } else {
    fprintf(w->out, "pub const %s = %lld\n", low, clang_getEnumConstantDeclValue(cursor));
  }
  clang_disposeString(spelling);
  return CXChildVisit_Continue;
}

static void emit_consts(Bind *b) {
  int i;
  int count = 0;
  int first = 1;
  ConstWalk w;
  char (*names)[MAX_NAME] = calloc(MAX_CONSTS, MAX_NAME);
  if (!names) {
    fail(b->e, "out of memory");
    return;
  }
  w.out = b->e->out;
  w.names = names;
  w.count = &count;
  for (i = 0; i < b->nenums; i++) {
    EnumInfo *en = &b->enums[i];
    if (!en->complete || !en->anon) {
      continue;
    }
    if (first) {
      fprintf(b->e->out, "\n// the constants of the header's anonymous enums\n");
      first = 0;
    }
    w.unsigned_values = enum_unsigned(en);
    clang_visitChildren(en->decl, walk_const, &w);
  }
  free(names);
}

/* The Avant type of a C enum emitted as none: Int32 (C's int) when it is
 * 4 bytes and its constants fit, else its integer type. */
static const char *bind_enum_scalar(Bind *b, CXType t) {
  int i;
  CXType canon;
  t = unwrap(t);
  canon = clang_getCanonicalType(t);
  if (clang_Type_getSizeOf(t) == 4 && b) {
    for (i = 0; i < b->nenums; i++) {
      if (clang_equalTypes(b->enums[i].canon, canon) && !b->enums[i].wide) {
        return "Int32";
      }
    }
  }
  return scalar_name(clang_getEnumDeclIntegerType(clang_getTypeDeclaration(t)));
}

static const char *bind_enum_name(Bind *b, CXType t) {
  int i;
  CXType canon;
  if (!b) {
    return NULL;
  }
  t = unwrap(t);
  if (t.kind != CXType_Enum) {
    return NULL;
  }
  canon = clang_getCanonicalType(t);
  for (i = 0; i < b->nenums; i++) {
    if (b->enums[i].accepted && clang_equalTypes(b->enums[i].canon, canon)) {
      return b->enums[i].avant;
    }
  }
  return NULL;
}

static enum CXChildVisitResult collect_rec(CXCursor cursor, CXCursor parent, CXClientData data) {
  Bind *b = (Bind *)data;
  enum CXCursorKind kind;
  (void)parent;
  if (b->e->failed) {
    return CXChildVisit_Break;
  }
  if (!clang_Location_isFromMainFile(clang_getCursorLocation(cursor))) {
    return CXChildVisit_Continue;
  }
  kind = clang_getCursorKind(cursor);
  if (kind == CXCursor_StructDecl) {
    note_struct(b, cursor);
    return CXChildVisit_Continue;
  }
  if (kind == CXCursor_EnumDecl) {
    note_enum(b, cursor);
    return CXChildVisit_Continue;
  }
  if (kind == CXCursor_TypedefDecl) {
    note_typedef(b, cursor);
    note_enum_typedef(b, cursor);
    clang_visitChildren(cursor, collect_rec, data);
  }
  return CXChildVisit_Continue;
}

static void choose_name(RecInfo *rec) {
  const char *s = NULL;
  rec->avant[0] = 0;
  if (rec->typedefs > 1) {
    return;
  }
  if (rec->typedefs == 1) {
    s = rec->typedef_name;
  } else if (rec->tag[0]) {
    s = rec->tag;
  }
  if (!s || !valid_ident(s)) {
    return;
  }
  /* A type name starts with a capital: the
   * record's CamelCase spelling, as an enum's (`struct req` -> Req). */
  enum_camel(s, rec->avant, sizeof(rec->avant));
  if (!(rec->avant[0] >= 'A' && rec->avant[0] <= 'Z')) {
    rec->avant[0] = 0;
  }
}

static const char *rec_label(RecInfo *rec) {
  if (rec->avant[0]) {
    return rec->avant;
  }
  if (rec->tag[0]) {
    return rec->tag;
  }
  if (rec->typedef_name[0]) {
    return rec->typedef_name;
  }
  return "record";
}

static void skip_rec(RecInfo *rec, const char *reason) {
  rec->accepted = 0;
  fprintf(stderr, "%s: %s\n", rec_label(rec), reason);
}

static enum CXChildVisitResult see_attr(CXCursor cursor, CXCursor parent, CXClientData data) {
  AttrSeek *seek = (AttrSeek *)data;
  (void)parent;
  if (clang_getCursorKind(cursor) == seek->kind) {
    seek->found = 1;
  }
  return CXChildVisit_Continue;
}

static int cursor_has(CXCursor cursor, enum CXCursorKind kind) {
  AttrSeek seek;
  seek.kind = kind;
  seek.found = 0;
  clang_visitChildren(cursor, see_attr, &seek);
  return seek.found;
}

/* A layout field's pointer that is no C string, no Ptr(Void) and no
 * pointer to a record: a pointer to a C scalar (Ptr(Int32), Ptr(UInt8)),
 * or to a pointer (Ptr(Ptr(T)?), T a scalar, Void or a layout); 0 when
 * the pointee has no Avant name. */
static int field_ptr_text(Bind *b, CXType pointee, char *out, size_t n) {
  char inner[MAX_NAME];
  const char *name;
  pointee = unwrap(pointee);
  if (pointee.kind == CXType_Void) {
    return snprintf(out, n, "Ptr(Void)") < (int)n;
  }
  if (pointee.kind == CXType_Pointer) {
    if (!field_ptr_text(b, clang_getPointeeType(pointee), inner, sizeof(inner))) {
      return 0;
    }
    return snprintf(out, n, "Ptr(%s?)", inner) < (int)n;
  }
  if (pointee.kind == CXType_Record && is_struct_record(pointee)) {
    RecInfo *rec = find_rec(b, clang_getCanonicalType(pointee));
    if (!rec || !rec->avant[0]) {
      return 0;
    }
    return snprintf(out, n, "Ptr(%s)", rec->avant) < (int)n;
  }
  name = pointee.kind == CXType_Bool ? "Bool" : scalar_name(pointee);
  if (!name) {
    return 0;
  }
  return snprintf(out, n, "Ptr(%s)", name) < (int)n;
}

static int map_field_type(Bind *b, CXType t, char *avant, size_t avant_n, int *size, int *align, char *reason, size_t reason_n) {
  CXString spelling;
  const char *raw;
  t = unwrap(t);
  if (t.kind == CXType_Bool) {
    if (clang_Type_getSizeOf(t) != 1 || clang_Type_getAlignOf(t) != 1) {
      snprintf(reason, reason_n, "Bool is not 1 byte; skipped");
      return 0;
    }
    copy_name(avant, avant_n, "Bool");
    *size = 1;
    *align = 1;
    return 1;
  }
  if (t.kind == CXType_Double) {
    if (clang_Type_getSizeOf(t) != 8 || clang_Type_getAlignOf(t) != 8) {
      snprintf(reason, reason_n, "Float64 is not 8 bytes; skipped");
      return 0;
    }
    copy_name(avant, avant_n, "Float64");
    *size = 8;
    *align = 8;
    return 1;
  }
  if (t.kind == CXType_Enum) {
    /* The Avant enum when one was emitted (C's
     * int, 4 bytes), else the enum's integer type. */
    const char *en = bind_enum_name(b, t);
    const char *iname = en ? en : bind_enum_scalar(b, t);
    long long sz = clang_Type_getSizeOf(t);
    long long al = clang_Type_getAlignOf(t);
    if (!iname || sz != al) {
      snprintf(reason, reason_n, "enum field of no Avant type; skipped");
      return 0;
    }
    copy_name(avant, avant_n, iname);
    *size = (int)sz;
    *align = (int)al;
    return 1;
  }
  if (t.kind == CXType_Pointer) {
    CXType pointee = unwrap(clang_getPointeeType(t));
    if (clang_Type_getSizeOf(t) != 8 || clang_Type_getAlignOf(t) != 8) {
      snprintf(reason, reason_n, "pointer is not 8 bytes; skipped");
      return 0;
    }
    if (is_char_type(pointee)) {
      copy_name(avant, avant_n, "String");
      *size = 8;
      *align = 8;
      return 1;
    }
    if (pointee.kind == CXType_Void) {
      copy_name(avant, avant_n, "Ptr(Void)");
      *size = 8;
      *align = 8;
      return 1;
    }
    if (pointee.kind == CXType_Record && is_struct_record(pointee)) {
      RecInfo *inner = find_rec(b, clang_getCanonicalType(pointee));
      if (!inner || !inner->avant[0]) {
        snprintf(reason, reason_n, "pointer to an unnamed record; skipped");
        return 0;
      }
      if (strlen(inner->avant) + 6 > avant_n) {
        snprintf(reason, reason_n, "record name is too long; skipped");
        return 0;
      }
      snprintf(avant, avant_n, "Ptr(%s)", inner->avant);
      *size = 8;
      *align = 8;
      return 1;
    }
    if (field_ptr_text(b, pointee, avant, avant_n)) {
      *size = 8;
      *align = 8;
      return 1;
    }
    spelling = clang_getTypeSpelling(t);
    raw = clang_getCString(spelling);
    snprintf(reason, reason_n, "unsupported field type %s; skipped", raw ? raw : "?");
    clang_disposeString(spelling);
    return 0;
  }
  if (t.kind == CXType_Record) {
    RecInfo *inner;
    if (!is_struct_record(t)) {
      snprintf(reason, reason_n, "union field; skipped");
      return 0;
    }
    inner = find_rec(b, clang_getCanonicalType(t));
    if (!inner || !inner->accepted) {
      snprintf(reason, reason_n, "nested record is not a complete C layout yet; skipped");
      return 0;
    }
    copy_name(avant, avant_n, inner->avant);
    *size = inner->size;
    *align = inner->align;
    return 1;
  }
  if (scalar_name(t)) {
    long long sz = clang_Type_getSizeOf(t);
    long long al = clang_Type_getAlignOf(t);
    if (sz != al) {
      snprintf(reason, reason_n, "scalar field is not aligned to its size; skipped");
      return 0;
    }
    copy_name(avant, avant_n, scalar_name(t));
    *size = (int)sz;
    *align = (int)al;
    return 1;
  }
  if (t.kind == CXType_ConstantArray) {
    /* C's `T name[N]` is the field `name: T[N]`, T a C scalar, a pointer
     * or a layout: N times the element's room, at its alignment. */
    char elem[MAX_NAME];
    int esize = 0;
    int ealign = 1;
    long long n = clang_getArraySize(t);
    CXType et = unwrap(clang_getArrayElementType(t));
    if (et.kind == CXType_ConstantArray) {
      snprintf(reason, reason_n, "array of arrays field; skipped");
      return 0;
    }
    if (is_char_type(et)) {
      copy_name(elem, sizeof(elem), "Int8");
      esize = 1;
      ealign = 1;
    } else if (!map_field_type(b, et, elem, sizeof(elem), &esize, &ealign, reason, reason_n)) {
      return 0;
    }
    if (n < 1 || n > 16777216 || strcmp(elem, "String") == 0) {
      snprintf(reason, reason_n, "array field of no Avant type; skipped");
      return 0;
    }
    if (snprintf(avant, avant_n, "%s[%lld]", elem, n) >= (int)avant_n) {
      snprintf(reason, reason_n, "array field type is too long; skipped");
      return 0;
    }
    *size = (int)(esize * n);
    *align = ealign;
    return 1;
  }
  if (t.kind == CXType_IncompleteArray || t.kind == CXType_VariableArray) {
    snprintf(reason, reason_n, "array field without a length; skipped");
    return 0;
  }
  spelling = clang_getTypeSpelling(t);
  raw = clang_getCString(spelling);
  snprintf(reason, reason_n, "unsupported field type %s; skipped", raw ? raw : "?");
  clang_disposeString(spelling);
  return 0;
}

static enum CXChildVisitResult visit_layout_field(CXCursor cursor, CXCursor parent, CXClientData data) {
  FieldState *st = (FieldState *)data;
  CXString spelling;
  const char *name;
  CXType field_ty;
  char avant[MAX_NAME];
  int size = 0;
  int align = 1;
  long long bits;
  int natural;
  (void)parent;
  if (!st->ok) {
    return CXChildVisit_Break;
  }
  if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
    return CXChildVisit_Continue;
  }
  if (clang_Cursor_isBitField(cursor)) {
    snprintf(st->reason, sizeof(st->reason), "bitfield; skipped");
    st->ok = 0;
    return CXChildVisit_Break;
  }
  spelling = clang_getCursorSpelling(cursor);
  name = clang_getCString(spelling);
  if (!name || !name[0] || clang_Cursor_isAnonymous(cursor)) {
    snprintf(st->reason, sizeof(st->reason), "anonymous field; skipped");
    st->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  if (!valid_ident(name)) {
    snprintf(st->reason, sizeof(st->reason), "field name is not an Avant identifier; skipped");
    st->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  if (cursor_has(cursor, CXCursor_PackedAttr) || cursor_has(cursor, CXCursor_AlignedAttr)) {
    snprintf(st->reason, sizeof(st->reason), "aligned field; skipped");
    st->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  field_ty = clang_getCursorType(cursor);
  if (!map_field_type(st->b, field_ty, avant, sizeof(avant), &size, &align, st->reason, sizeof(st->reason))) {
    st->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  natural = align_up(st->offset, align);
  bits = clang_Type_getOffsetOf(clang_getCursorType(st->rec->decl), name);
  if (bits < 0 || bits / 8 != natural) {
    snprintf(st->reason, sizeof(st->reason), "layout does not match; skipped");
    st->ok = 0;
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  st->offset = natural + size;
  if (align > st->max_align) {
    st->max_align = align;
  }
  st->nfields++;
  clang_disposeString(spelling);
  return CXChildVisit_Continue;
}

static void decide_rec(Bind *b, RecInfo *rec) {
  FieldState st;
  CXType ty;
  long long csize;
  long long calign;
  int size;
  choose_name(rec);
  if (!rec->complete) {
    return;
  }
  if (rec->typedefs > 1) {
    skip_rec(rec, "two typedefs; skipped");
    return;
  }
  if (!rec->avant[0]) {
    skip_rec(rec, "record name is not an Avant identifier; skipped");
    return;
  }
  if (cursor_has(rec->decl, CXCursor_PackedAttr)) {
    skip_rec(rec, "packed; skipped");
    return;
  }
  if (cursor_has(rec->decl, CXCursor_AlignedAttr)) {
    skip_rec(rec, "aligned; skipped");
    return;
  }
  memset(&st, 0, sizeof(st));
  st.b = b;
  st.rec = rec;
  st.ok = 1;
  st.max_align = 1;
  clang_visitChildren(rec->decl, visit_layout_field, &st);
  if (!st.ok) {
    skip_rec(rec, st.reason);
    return;
  }
  if (st.nfields == 0) {
    skip_rec(rec, "C layout needs a field; skipped");
    return;
  }
  size = align_up(st.offset, st.max_align);
  ty = clang_getCursorType(rec->decl);
  csize = clang_Type_getSizeOf(ty);
  calign = clang_Type_getAlignOf(ty);
  if (csize != size || calign != st.max_align) {
    skip_rec(rec, "layout does not match; skipped");
    return;
  }
  rec->accepted = 1;
  rec->size = size;
  rec->align = (int)calign;
}

static enum CXChildVisitResult emit_layout_field(CXCursor cursor, CXCursor parent, CXClientData data) {
  FieldState *st = (FieldState *)data;
  CXString spelling;
  const char *name;
  char avant[MAX_NAME];
  int size = 0;
  int align = 1;
  char reason[160];
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
    return CXChildVisit_Continue;
  }
  spelling = clang_getCursorSpelling(cursor);
  name = clang_getCString(spelling);
  if (!map_field_type(st->b, clang_getCursorType(cursor), avant, sizeof(avant), &size, &align, reason, sizeof(reason))) {
    fail(st->b->e, "internal layout emit failed");
    clang_disposeString(spelling);
    return CXChildVisit_Break;
  }
  fprintf(st->b->e->out, "    %s: %s\n", name ? name : "field", avant);
  clang_disposeString(spelling);
  return CXChildVisit_Continue;
}

static void emit_layouts(Bind *b) {
  int i;
  for (i = 0; i < b->nrecs; i++) {
    RecInfo *rec = &b->recs[i];
    FieldState st;
    if (!rec->accepted) {
      continue;
    }
    fprintf(b->e->out, "  struct %s {\n", rec->avant);
    memset(&st, 0, sizeof(st));
    st.b = b;
    st.rec = rec;
    st.ok = 1;
    clang_visitChildren(rec->decl, emit_layout_field, &st);
    fprintf(b->e->out, "  }\n");
  }
}

static int bind_record_name(Bind *b, CXType t, char *out, size_t n) {
  RecInfo *rec;
  if (!b) {
    return 0;
  }
  t = unwrap(t);
  if (t.kind != CXType_Record) {
    return 0;
  }
  rec = find_rec(b, clang_getCanonicalType(t));
  if (!rec || !rec->avant[0]) {
    return 0;
  }
  copy_name(out, n, rec->avant);
  return 1;
}

static int record_by_value(Emit *e, CXType t) {
  CXType raw = unwrap(t);
  char buf[256];
  CXString spelling;
  const char *shown;
  if (raw.kind != CXType_Record) {
    return 0;
  }
  if (is_struct_record(raw)) {
    fail(e, "C struct passed by value is not supported");
    return 1;
  }
  spelling = clang_getTypeSpelling(raw);
  shown = clang_getCString(spelling);
  snprintf(buf, sizeof(buf), "unsupported C type %s", shown ? shown : "record");
  clang_disposeString(spelling);
  fail(e, buf);
  return 1;
}

static enum CXChildVisitResult visit(CXCursor cursor, CXCursor parent, CXClientData data) {
  (void)parent;
  Emit *e = (Emit *)data;
  if (e->failed) {
    return CXChildVisit_Break;
  }
  if (clang_getCursorKind(cursor) != CXCursor_FunctionDecl) {
    return CXChildVisit_Continue;
  }
  if (!clang_Location_isFromMainFile(clang_getCursorLocation(cursor))) {
    return CXChildVisit_Continue;
  }
  enum CXLinkageKind link = clang_getCursorLinkage(cursor);
  if (link == CXLinkage_Internal) {
    return CXChildVisit_Continue;
  }
  emit_fun(e, cursor);
  return CXChildVisit_Continue;
}

static int emit_diagnostics(CXTranslationUnit tu) {
  unsigned n = clang_getNumDiagnostics(tu);
  int errors = 0;
  unsigned i;
  for (i = 0; i < n; i++) {
    CXDiagnostic d = clang_getDiagnostic(tu, i);
    enum CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(d);
    if (sev >= CXDiagnostic_Error) {
      CXString s = clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions());
      fprintf(stderr, "%s\n", clang_getCString(s));
      clang_disposeString(s);
      errors = 1;
    }
    clang_disposeDiagnostic(d);
  }
  return errors;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: avant-bind LIBNAME HEADER.h\n");
    return 2;
  }
  const char *lib = argv[1];
  const char *header = argv[2];
  if (!valid_ident(lib)) {
    fprintf(stderr, "lib name %s is not an Avant identifier\n", lib);
    return 2;
  }

  CXIndex index = clang_createIndex(0, 0);
  const char *clang_args[] = {"-x", "c", "-std=c11"};
  CXTranslationUnit tu = clang_parseTranslationUnit(
      index, header, clang_args, 3, NULL, 0, CXTranslationUnit_None);
  if (!tu) {
    fprintf(stderr, "failed to parse %s\n", header);
    clang_disposeIndex(index);
    return 1;
  }
  if (emit_diagnostics(tu)) {
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }

  Emit e;
  Bind *bind;
  int i;
  e.out = stdout;
  e.failed = 0;
  e.err[0] = 0;
  bind = calloc(1, sizeof(Bind));
  if (!bind) {
    fprintf(stderr, "out of memory\n");
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }
  bind->recs = calloc(MAX_RECS, sizeof(RecInfo));
  if (!bind->recs) {
    fprintf(stderr, "out of memory\n");
    free(bind);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }
  bind->enums = calloc(MAX_ENUMS, sizeof(EnumInfo));
  if (!bind->enums) {
    fprintf(stderr, "out of memory\n");
    free(bind->recs);
    free(bind->enums);
    free(bind);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }
  bind->e = &e;
  e.bind = bind;

  clang_visitChildren(clang_getTranslationUnitCursor(tu), collect_rec, bind);
  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    free(bind->recs);
    free(bind->enums);
    free(bind);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }
  for (i = 0; i < bind->nrecs; i++) {
    choose_name(&bind->recs[i]);
  }
  for (i = 0; i < bind->nenums; i++) {
    decide_enum(bind, &bind->enums[i]);
  }
  for (i = 0; i < bind->nrecs; i++) {
    decide_rec(bind, &bind->recs[i]);
  }
  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    free(bind->recs);
    free(bind->enums);
    free(bind);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }

  printf("// generated by avant bind from %s\n", header);
  printf("lib %s {\n", lib);
  emit_enums(bind);
  emit_layouts(bind);
  clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &e);
  printf("}\n");
  emit_consts(bind);
  free(bind->recs);
  free(bind->enums);
  free(bind);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);

  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    return 1;
  }
  return 0;
}
