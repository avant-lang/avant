; A hand-written program against the frozen runtime ABI (the runtime's
; entry points, argv, roots, barriers and leave). It is what the
; old back end emits for a `main` that allocates, in the order the runtime
; expects:
;   root slots: entry allocas, volatile null stores before avant_gc_enter
;   avant_gc_enter, then avant_gc_root_begin(n) and one store per slot,
;   with no call in between
;   the type maps (built-in 1022, 1021, 1023, then class id 1, a Box with
;   one reference field at word 0), then avant_io_init_argv
;   every reference stored to its slot at once, and read back from the
;   slot after each collecting call
;   a barrier on the holder before a reference store into a heap field
;   (the generational barrier)
;   avant_gc_leave before the return
;   a string literal with the header a heap string has (its size word: the
;   bytes and the terminator), which the runtime reads its length from
; Output: "Hello Avant 42" and exit status 0. Run under AVANT_GC_VERIFY=1
; and AVANT_YOUNG_MB=1 by tests/cases/test_llvm_glue.av.

@str.hello = private constant { i64, [13 x i8] } { i64 4611686018427387917, [13 x i8] c"Hello Avant \00" }, align 8
@fmt.s = private constant [4 x i8] c"%s\0A\00", align 1

declare void @avant_gc_enter()
declare void @avant_gc_leave()
declare ptr @avant_gc_root_begin(i32)
declare void @avant_type_map(i32, i64)
declare void @avant_io_init_argv(i32, ptr)
declare ptr @avant_alloc(i64, i32)
declare void @avant_barrier(ptr)
declare void @avant_gc_collect_young()
declare ptr @avant_str_from_int(i32)
declare ptr @avant_str_concat(ptr, ptr)
declare i32 @printf(ptr, ...)

; Box { String } is class id 1; word 0 is a reference.
define void @__avant_type_maps() {
entry:
  call void @avant_type_map(i32 1022, i64 1)
  call void @avant_type_map(i32 1021, i64 1)
  call void @avant_type_map(i32 1023, i64 7)
  call void @avant_type_map(i32 1, i64 1)
  ret void
}

define i32 @main(i32 %argc, ptr %argv) {
alloca:
  %box = alloca ptr, align 8
  %str = alloca ptr, align 8
  store volatile ptr null, ptr %box, align 8
  store volatile ptr null, ptr %str, align 8
  br label %entry

entry:
  call void @avant_gc_enter()
  %roots = call ptr @avant_gc_root_begin(i32 2)
  store ptr %box, ptr %roots, align 8
  %roots.1 = getelementptr inbounds ptr, ptr %roots, i64 1
  store ptr %str, ptr %roots.1, align 8
  call void @__avant_type_maps()
  call void @avant_io_init_argv(i32 %argc, ptr %argv)

  ; box = Box.new (collecting), straight into its slot
  %b0 = call ptr @avant_alloc(i64 8, i32 1)
  store volatile ptr %b0, ptr %box, align 8

  ; s = 42.to_s (collecting): box is read back from its slot afterwards
  %s0 = call ptr @avant_str_from_int(i32 42)
  store volatile ptr %s0, ptr %str, align 8
  %b1 = load volatile ptr, ptr %box, align 8
  %s1 = load volatile ptr, ptr %str, align 8
  call void @avant_barrier(ptr %b1)
  store ptr %s1, ptr %b1, align 8

  ; a young collection: box moves, and its field is traced through map 1
  call void @avant_gc_collect_young()
  %b2 = load volatile ptr, ptr %box, align 8
  %f = load ptr, ptr %b2, align 8

  ; "Hello Avant " + box.value (collecting), then puts
  %m0 = call ptr @avant_str_concat(ptr getelementptr inbounds (i8, ptr @str.hello, i64 8), ptr %f)
  store volatile ptr %m0, ptr %str, align 8
  %m1 = load volatile ptr, ptr %str, align 8
  %p = call i32 (ptr, ...) @printf(ptr @fmt.s, ptr %m1)

  %keep.box = load volatile ptr, ptr %box, align 8
  %keep.str = load volatile ptr, ptr %str, align 8
  call void @avant_gc_leave()
  ret i32 0
}
