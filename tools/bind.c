/*
 * avant bind — C header → Avant lib/fun (D36).
 *
 * Uses libclang. Hand-written lib blocks stay the same grammar; this
 * writes an editable starting point, not a second FFI.
 *
 * Usage: avant-bind LIBNAME HEADER.h
 */

#include <clang-c/Index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *out;
  int failed;
  char err[256];
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
    fputs("Int", e->out);
    return;
  case CXType_Double:
    fputs("Float64", e->out);
    return;
  case CXType_Pointer:
    emit_ptr(e, clang_getPointeeType(t));
    return;
  case CXType_Record: {
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
  default: {
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

  CXString spelling = clang_getCursorSpelling(cursor);
  const char *name = clang_getCString(spelling);
  if (name && name[0] && valid_ident(name)) {
    fprintf(st->e->out, "%s: ", name);
  } else {
    fprintf(st->e->out, "arg%d: ", st->index);
  }
  clang_disposeString(spelling);

  emit_type(st->e, clang_getCursorType(cursor));
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
  CXType uret = unwrap(ret);
  if (uret.kind != CXType_Void) {
    fputs(": ", e->out);
    emit_type(e, ret);
  }
  fputc('\n', e->out);
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
  e.out = stdout;
  e.failed = 0;
  e.err[0] = 0;

  printf("// generated by avant bind from %s\n", header);
  printf("lib %s {\n", lib);
  clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &e);
  printf("}\n");

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);

  if (e.failed) {
    fprintf(stderr, "%s\n", e.err);
    return 1;
  }
  return 0;
}
