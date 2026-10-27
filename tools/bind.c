/*
 * avant bind — C header → Avant lib/fun (D36) and C layouts (Stage 18).
 *
 * Uses libclang. Hand-written lib blocks stay the same grammar; this
 * writes an editable starting point, not a second FFI.
 *
 * A record is emitted only when every field is in the Stage 18 subset
 * and clang's size, align, and offsets equal the natural layout of those
 * Avant types (LP64). Anything else is skipped, named on stderr, and the
 * rest of the header is still emitted. A record passed or returned by
 * value fails the whole bind. An unsigned 32-bit type has no Avant type
 * at the same width and signedness, so it fails the whole bind too
 * (Wave G, Q87): it is never silently mapped to Int.
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

static int bind_record_name(Bind *b, CXType t, char *out, size_t n);
static int record_by_value(Emit *e, CXType t);

static void emit_type(Emit *e, CXType t);

static void emit_ptr(Emit *e, CXType pointee) {
  pointee = unwrap(pointee);
  if (is_char_type(pointee)) {
    fputs("String", e->out);
    return;
  }
  if (pointee.kind == CXType_Void) {
    fputs("Ptr(Void)", e->out);
    return;
  }
  fputs("Ptr(", e->out);
  emit_type(e, pointee);
  fputs(")", e->out);
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
  case CXType_Int:
  case CXType_UInt:
  case CXType_Long:
  case CXType_ULong:
  case CXType_LongLong:
  case CXType_ULongLong: {
    long long sz = clang_Type_getSizeOf(t);
    int unsigned_kind = t.kind == CXType_UInt || t.kind == CXType_ULong || t.kind == CXType_ULongLong;
    if (sz == 4) {
      if (unsigned_kind) {
        CXString spelling = clang_getTypeSpelling(t);
        snprintf(e->err, sizeof(e->err), "unsigned 32-bit C type %s has no Avant type; bind failed",
                 clang_getCString(spelling));
        e->failed = 1;
        clang_disposeString(spelling);
        return;
      }
      fputs("Int", e->out);
      return;
    }
    if (sz == 8) {
      fputs(unsigned_kind ? "UInt64" : "Int64", e->out);
      return;
    }
    goto unsupported_ty;
  }
  case CXType_Double:
    fputs("Float64", e->out);
    return;
  case CXType_Pointer:
    emit_ptr(e, clang_getPointeeType(t));
    return;
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

struct Bind {
  Emit *e;
  RecInfo *recs;
  int nrecs;
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
  if (kind == CXCursor_TypedefDecl) {
    note_typedef(b, cursor);
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
  copy_name(rec->avant, sizeof(rec->avant), s);
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
    snprintf(reason, reason_n, "enum field; skipped");
    return 0;
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
    if (pointee.kind == CXType_UChar) {
      snprintf(reason, reason_n, "unsigned char* is not String; skipped");
      return 0;
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
  if (t.kind == CXType_Short || t.kind == CXType_UShort || t.kind == CXType_Char_S || t.kind == CXType_Char_U ||
      t.kind == CXType_SChar || t.kind == CXType_UChar || t.kind == CXType_Float) {
    spelling = clang_getTypeSpelling(t);
    raw = clang_getCString(spelling);
    snprintf(reason, reason_n, "unsupported field type %s; skipped", raw ? raw : "?");
    clang_disposeString(spelling);
    return 0;
  }
  if (t.kind == CXType_Int || t.kind == CXType_UInt || t.kind == CXType_Long || t.kind == CXType_ULong ||
      t.kind == CXType_LongLong || t.kind == CXType_ULongLong) {
    long long sz = clang_Type_getSizeOf(t);
    long long al = clang_Type_getAlignOf(t);
    int unsigned_kind = t.kind == CXType_UInt || t.kind == CXType_ULong || t.kind == CXType_ULongLong;
    if (sz == 4 && al == 4) {
      if (unsigned_kind) {
        snprintf(reason, reason_n, "unsigned 32-bit field has no Avant type; bind failed");
        snprintf(b->e->err, sizeof(b->e->err), "unsigned 32-bit field has no Avant type; bind failed");
        b->e->failed = 1;
        return 0;
      }
      copy_name(avant, avant_n, "Int");
      *size = 4;
      *align = 4;
      return 1;
    }
    if (sz == 8 && al == 8) {
      copy_name(avant, avant_n, unsigned_kind ? "UInt64" : "Int64");
      *size = 8;
      *align = 8;
      return 1;
    }
    snprintf(reason, reason_n, "integer field is not 4 or 8 bytes; skipped");
    return 0;
  }
  if (t.kind == CXType_ConstantArray || t.kind == CXType_IncompleteArray || t.kind == CXType_VariableArray) {
    snprintf(reason, reason_n, "array field; skipped");
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
  bind->e = &e;
  e.bind = bind;

  clang_visitChildren(clang_getTranslationUnitCursor(tu), collect_rec, bind);
  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    free(bind->recs);
    free(bind);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }
  for (i = 0; i < bind->nrecs; i++) {
    choose_name(&bind->recs[i]);
  }
  for (i = 0; i < bind->nrecs; i++) {
    decide_rec(bind, &bind->recs[i]);
  }
  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    free(bind->recs);
    free(bind);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 1;
  }

  printf("// generated by avant bind from %s\n", header);
  printf("lib %s {\n", lib);
  emit_layouts(bind);
  clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &e);
  printf("}\n");
  free(bind->recs);
  free(bind);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);

  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    return 1;
  }
  return 0;
}
