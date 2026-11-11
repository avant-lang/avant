/* The compiler's in-process LLVM (Stage 28, D162, D172): textual LLVM IR
 * in, an object file out, through the stable LLVM C API only.
 *
 * Linked into the compiler only, never into a user program:
 * tools/build-llvm.sh compiles this file against llvm-config-20 (or, with
 * LLVM_CONFIG=llvm-config-23, against LLVM 23: the W18L probe) and merges
 * it with the LLVM members it needs (and libstdc++, zlib, zstd) into one
 * relocatable object, runtime/build/avant_llvm.o, whose only global
 * definitions are the avant_llvm_* functions below. The compiler binds
 * them in compiler/llvm/glue.av; no LLVM type crosses that boundary: C
 * strings and ints go in, an int status comes out, and the message of the
 * last failure is read back as an Avant string.
 *
 * Status codes (the same in compiler/llvm/glue.av):
 *   0 ok, 1 target (no native target or target machine), 2 parse (the
 *   text or file is not valid IR), 3 verify (the module fails the
 *   verifier), 4 passes (the pipeline string is invalid or failed),
 *   5 emit (the object could not be written).
 *
 * Nothing here aborts the compiler: every step uses an error-returning
 * API, and LLVM's own fatal error path (which should not be reachable for
 * a verified module) is redirected to a one-line message and exit(70).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/ErrorHandling.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/Support.h>

#include <llvm/Config/llvm-config.h>

#include "avant_rt.h"

/* Defined in the X86 target libraries; declared here because llvm-c/Target.h
 * only offers them through macros over every configured target. */
void LLVMInitializeX86TargetInfo(void);
void LLVMInitializeX86Target(void);
void LLVMInitializeX86TargetMC(void);
void LLVMInitializeX86AsmPrinter(void);
void LLVMInitializeX86AsmParser(void);

enum {
  AVANT_LLVM_OK = 0,
  AVANT_LLVM_TARGET = 1,
  AVANT_LLVM_PARSE = 2,
  AVANT_LLVM_VERIFY = 3,
  AVANT_LLVM_PASSES = 4,
  AVANT_LLVM_EMIT = 5
};

/* The last failure's message, owned here (malloc), one per thread. */
static _Thread_local char *g_error;
static int g_initialized;

static void set_error(const char *prefix, const char *msg) {
  size_t a = strlen(prefix);
  size_t b = msg ? strlen(msg) : 0;
  char *e = (char *)malloc(a + b + 1);
  free(g_error);
  g_error = e;
  if (!e) {
    return;
  }
  memcpy(e, prefix, a);
  if (b) {
    memcpy(e + a, msg, b);
  }
  e[a + b] = 0;
  /* LLVM messages often end in a newline; the caller adds its own. */
  while (b > 0 && (e[a + b - 1] == '\n' || e[a + b - 1] == ' ')) {
    b--;
    e[a + b] = 0;
  }
}

static void clear_error(void) {
  free(g_error);
  g_error = NULL;
}

static void on_fatal(const char *reason) {
  fprintf(stderr, "avant: internal LLVM error: %s\n", reason ? reason : "(no message)");
  fflush(stderr);
  exit(70);
}

/* Initializes the native (x86-64) target once. 0 or AVANT_LLVM_TARGET. */
int32_t avant_llvm_init(void) {
  if (!g_initialized) {
    LLVMInstallFatalErrorHandler(on_fatal);
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();
    /* Code placement (Stage 29 WP, O186): innermost loop headers aligned
     * to 32 bytes (LLVM's x86 default is 16). With every function aligned
     * to 64 (place_functions below), a loop's offset within its 64-byte
     * line no longer depends on the code before it, and a short loop
     * (up to 32 bytes) never straddles a line: Etc::Sieve's 30-byte inner
     * loop cost 13% when it did. LLVM 20 offers no per-target-machine
     * setting for it in the C API (llc's -align-loops lives in its own
     * flags library), so it is this X86 option, set once per process;
     * the compiler compiles nothing else with this LLVM. */
    {
      static const char *const argv[] = {"avant", "-x86-experimental-pref-innermost-loop-alignment=5"};
      LLVMParseCommandLineOptions(2, argv, "avant");
    }
    g_initialized = 1;
  }
  return AVANT_LLVM_OK;
}

static LLVMCodeGenOptLevel codegen_level(int32_t level) {
  switch (level) {
  case 0: return LLVMCodeGenLevelNone;
  case 1: return LLVMCodeGenLevelLess;
  case 3: return LLVMCodeGenLevelAggressive;
  default: return LLVMCodeGenLevelDefault;
  }
}

/* The CPU level of the code generated (Stage 28 W18S, O140): set by
 * avant_llvm_set_cpu before a compile, read by finish_module. Only LLVM's
 * target machine sees it (its optimization and code generation): the
 * module text the emitter writes, which the fixed point compares, never
 * does. 1 is x86-64 itself ("generic"), 2-4 the psABI levels x86-64-v2 to
 * x86-64-v4, 5 the compiling machine's own CPU and features (native). */
static int32_t g_cpu = 1;

/* The highest x86-64 level this machine runs (1-4). */
int32_t avant_llvm_host_level(void) {
  __builtin_cpu_init();
  if (__builtin_cpu_supports("x86-64-v4")) {
    return 4;
  }
  if (__builtin_cpu_supports("x86-64-v3")) {
    return 3;
  }
  if (__builtin_cpu_supports("x86-64-v2")) {
    return 2;
  }
  return 1;
}

/* The level of the next compiles: 1-5 as above. 0 ok, 1 out of range. */
int32_t avant_llvm_set_cpu(int32_t level) {
  if (level < 1 || level > 5) {
    return 1;
  }
  g_cpu = level;
  return 0;
}

/* Every program records the level its code needs (a module that defines
 * main): `__avant_cpu_level`, read at startup by the runtime
 * (avant_rt_traps.c, avant_cpu_check), which stops with a message naming
 * the flag to rebuild with on a CPU without it. A native build records the
 * compiling machine's level with 0x100 set. */
static void record_level(LLVMModuleRef mod) {
  LLVMValueRef fmain = LLVMGetNamedFunction(mod, "main");
  LLVMValueRef g;
  int32_t level = g_cpu == 5 ? (avant_llvm_host_level() | 0x100) : g_cpu;
  if (!fmain || LLVMIsDeclaration(fmain) || LLVMGetNamedGlobal(mod, "__avant_cpu_level")) {
    return;
  }
  g = LLVMAddGlobal(mod, LLVMInt32TypeInContext(LLVMGetModuleContext(mod)), "__avant_cpu_level");
  LLVMSetInitializer(g, LLVMConstInt(LLVMInt32TypeInContext(LLVMGetModuleContext(mod)), (unsigned long long)level, 0));
  LLVMSetGlobalConstant(g, 1);
  LLVMSetLinkage(g, LLVMExternalLinkage);
}

/* Code placement (Stage 29 WP, O186): every function the module defines,
 * except the cold ones (they go to .text.unlikely), starts on a 64-byte
 * boundary. The program's code is linked after the PLT, the runtime's
 * cold and start-up code and the C start files, and after the standard
 * library's own functions; any of them growing moved every hot loop by
 * 16-byte steps and moved tasks' times by up to 14% (the WP probe). A
 * 64-byte start keeps each function's lines and loop offsets the same
 * whatever comes before it. The runtime is built the same way
 * (runtime/build.sh, -falign-functions=64). Cost: about 1% of a
 * program's own code. */
static void place_functions(LLVMModuleRef mod) {
  unsigned cold = LLVMGetEnumAttributeKindForName("cold", 4);
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f; f = LLVMGetNextFunction(f)) {
    if (LLVMIsDeclaration(f) || LLVMGetAlignment(f) >= 64) {
      continue;
    }
    if (cold && LLVMGetEnumAttributeAtIndex(f, LLVMAttributeFunctionIndex, cold)) {
      continue;
    }
    LLVMSetAlignment(f, 64);
  }
}

/* Verify, run the pipeline, emit. Consumes nothing; the caller disposes. */
static int32_t finish_module(LLVMModuleRef mod, const char *passes, int32_t level, const char *obj_path) {
  char *msg = NULL;
  char *triple = NULL;
  LLVMTargetRef target = NULL;
  LLVMTargetMachineRef tm = NULL;
  int32_t status = AVANT_LLVM_OK;

  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &msg)) {
    set_error("", msg);
    LLVMDisposeMessage(msg);
    return AVANT_LLVM_VERIFY;
  }
  LLVMDisposeMessage(msg);
  msg = NULL;

  /* The module's own triple when it names one, else the host's. */
  const char *mt = LLVMGetTarget(mod);
  if (mt && mt[0]) {
    triple = LLVMCreateMessage(mt);
  } else {
    triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(mod, triple);
  }
  if (LLVMGetTargetFromTriple(triple, &target, &msg)) {
    set_error("", msg);
    LLVMDisposeMessage(msg);
    LLVMDisposeMessage(triple);
    return AVANT_LLVM_TARGET;
  }
  /* The CPU level (g_cpu: generic x86-64, x86-64-v2 to v4, or the host's
   * CPU and features), position-independent code: the link is a default
   * PIE, as the myc-llvm objects were. */
  {
    static const char *names[] = {"generic", "generic", "x86-64-v2", "x86-64-v3", "x86-64-v4"};
    if (g_cpu == 5) {
      char *host = LLVMGetHostCPUName();
      char *features = LLVMGetHostCPUFeatures();
      tm = LLVMCreateTargetMachine(target, triple, host, features, codegen_level(level), LLVMRelocPIC, LLVMCodeModelDefault);
      LLVMDisposeMessage(host);
      LLVMDisposeMessage(features);
    } else {
      tm = LLVMCreateTargetMachine(target, triple, names[g_cpu], "", codegen_level(level), LLVMRelocPIC, LLVMCodeModelDefault);
    }
  }
  LLVMDisposeMessage(triple);
  record_level(mod);
  if (!tm) {
    set_error("", "cannot create a target machine");
    return AVANT_LLVM_TARGET;
  }
  const char *dl = LLVMGetDataLayoutStr(mod);
  if (!dl || !dl[0]) {
    LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
    LLVMSetModuleDataLayout(mod, td);
    LLVMDisposeTargetData(td);
  }

  if (passes && passes[0]) {
    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef err = LLVMRunPasses(mod, passes, tm, opts);
    LLVMDisposePassBuilderOptions(opts);
    if (err) {
      char *em = LLVMGetErrorMessage(err);
      set_error("", em);
      LLVMDisposeErrorMessage(em);
      status = AVANT_LLVM_PASSES;
    }
  }

  if (status == AVANT_LLVM_OK) {
    place_functions(mod);
    if (LLVMTargetMachineEmitToFile(tm, mod, (char *)obj_path, LLVMObjectFile, &msg)) {
      set_error("", msg);
      status = AVANT_LLVM_EMIT;
    }
    LLVMDisposeMessage(msg);
  }
  LLVMDisposeTargetMachine(tm);
  return status;
}

static int32_t compile_buffer(LLVMMemoryBufferRef buf, const char *passes, int32_t level, const char *obj_path) {
  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = NULL;
  char *msg = NULL;
  int32_t status;
#if LLVM_VERSION_MAJOR >= 22
  /* LLVMParseIRInContext is deprecated since LLVM 22; its successor does
   * not take ownership of buf, so it is disposed here either way. */
  LLVMBool failed = LLVMParseIRInContext2(ctx, buf, &mod, &msg);
  LLVMDisposeMemoryBuffer(buf);
#else
  /* Takes ownership of buf, on success and on failure. */
  LLVMBool failed = LLVMParseIRInContext(ctx, buf, &mod, &msg);
#endif
  if (failed) {
    set_error("", msg);
    LLVMDisposeMessage(msg);
    LLVMContextDispose(ctx);
    return AVANT_LLVM_PARSE;
  }
  status = finish_module(mod, passes, level, obj_path);
  LLVMDisposeModule(mod);
  LLVMContextDispose(ctx);
  return status;
}

/* Compiles the textual IR file ll_path to the object obj_path: parse,
 * verify, run the new-pass-manager pipeline `passes` (empty: none), emit
 * with codegen level `level` (0 None, 1 Less, 2 Default, 3 Aggressive). */
int32_t avant_llvm_compile_file(const char *ll_path, const char *passes, int32_t level, const char *obj_path) {
  LLVMMemoryBufferRef buf = NULL;
  char *msg = NULL;
  clear_error();
  avant_llvm_init();
  if (LLVMCreateMemoryBufferWithContentsOfFile(ll_path, &buf, &msg)) {
    set_error("cannot read: ", msg);
    LLVMDisposeMessage(msg);
    return AVANT_LLVM_PARSE;
  }
  return compile_buffer(buf, passes, level, obj_path);
}

/* The same from IR text in memory; `name` labels it in messages. */
int32_t avant_llvm_compile_text(const char *text, const char *name, const char *passes, int32_t level, const char *obj_path) {
  clear_error();
  avant_llvm_init();
  LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(text, strlen(text), name);
  return compile_buffer(buf, passes, level, obj_path);
}

/* gc-lint: holds nothing (S is C memory: LLVM's messages, a C buffer). */
static char *avant_string(const char *s) {
  size_t n = s ? strlen(s) : 0;
  char *p = avant_str_alloc((uint64_t)n);
  if (n) {
    memcpy(p, s, n);
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* The message of this thread's last failure, as an Avant string ("" when
 * the last call succeeded). */
char *avant_llvm_error(void) {
  return avant_string(g_error ? g_error : "");
}

/* The linked LLVM's version, "20.1.8" (or "23.1.6" built against LLVM 23). */
char *avant_llvm_version(void) {
  unsigned major = 0, minor = 0, patch = 0;
  char buf[48];
  LLVMGetVersion(&major, &minor, &patch);
  snprintf(buf, sizeof buf, "%u.%u.%u", major, minor, patch);
  return avant_string(buf);
}

/* The text of a double constant in LLVM IR (Stage 28 W18S, O141): the
 * shortest decimal `d.ddd…e±XX` that reads back as exactly the same bits
 * (at most 17 significant digits always do), a form every LLVM from 20 on
 * parses (LLVM 23 deprecates, and 24 removes, the `0x` + 16 hex digits form
 * the emitter wrote before); a NaN or an infinity, which have no decimal,
 * as `bitcast (i64 BITS to double)`. -0.0 keeps its sign ("-0.0e+00"). */
char *avant_llvm_f64_text(int64_t bits) {
  char buf[64];
  double v;
  int p;
  memcpy(&v, &bits, sizeof v);
  if (((uint64_t)bits >> 52 & 0x7ff) == 0x7ff) {
    snprintf(buf, sizeof buf, "bitcast (i64 %lld to double)", (long long)bits);
    return avant_string(buf);
  }
  for (p = 1; p <= 17; p++) {
    double back;
    snprintf(buf, sizeof buf, "%.*e", p, v);
    back = strtod(buf, NULL);
    if (memcmp(&back, &v, sizeof v) == 0) {
      break;
    }
  }
  return avant_string(buf);
}
