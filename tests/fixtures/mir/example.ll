source_filename = "main"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%"main.Box" = type { ptr }

@str.0 = private constant [6 x i8] c"small\00", align 1
@str.1 = private constant [4 x i8] c"%s\0A\00", align 1

@g_avant_shared = external global i32
@avant_gc_fast_ok = external thread_local global i32
@avant_gc_bump = external thread_local global ptr
@avant_gc_limit = external thread_local global ptr
@avant_gc_block = external thread_local global ptr
@avant_gc_base = external thread_local global ptr
@avant_gc_starts = external thread_local global ptr
@avant_g_stats = external global i64
@avant_since_gc = external global i64
@avant_nursery_held = external global i64
@avant_inline_allocs = external global i64

declare ptr @avant_alloc(i64, i32)
declare void @avant_barrier(ptr)
declare void @avant_gc_enter()
declare void @avant_gc_leave()
declare ptr @avant_gc_root_begin(i32)
declare void @avant_io_init_argv(i32, ptr)
declare ptr @avant_str_from_int(i32)
declare void @avant_type_map(i32, i64)
declare i32 @printf(ptr, ...)
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)

define internal void @"av.main/Box.initialize"(ptr %a1, ptr %a2) #0 {
alloca:
  %_1.self = alloca ptr, align 8
  %_2.name = alloca ptr, align 8
  store ptr %a1, ptr %_1.self, align 8
  store ptr %a2, ptr %_2.name, align 8
  br label %bb0

bb0:
  %t0 = load ptr, ptr %_1.self, align 8
  %t1 = icmp eq ptr %t0, null
  br i1 %t1, label %bb0.s0.cont, label %bb0.s0.test

bb0.s0.test:
  %t2 = getelementptr inbounds i8, ptr %t0, i64 -2
  %t3 = load i16, ptr %t2, align 2
  %t4 = and i16 %t3, -32768
  %t5 = icmp ne i16 %t4, 0
  %t6 = and i16 %t3, 48
  %t7 = icmp ne i16 %t6, 0
  %t8 = and i1 %t5, %t7
  br i1 %t8, label %bb0.s0.cont, label %bb0.s0.call

bb0.s0.call:
  call void @avant_barrier(ptr %t0)
  br label %bb0.s0.cont

bb0.s0.cont:
  %t9 = load ptr, ptr %_2.name, align 8
  %t10 = load ptr, ptr %_1.self, align 8
  %t11 = getelementptr inbounds %"main.Box", ptr %t10, i32 0, i32 0
  store ptr %t9, ptr %t11, align 8
  ret void
}

define internal ptr @"av.main/Box.new"(ptr %a1) #0 {
alloca:
  %_0 = alloca ptr, align 8
  %_1.name = alloca ptr, align 8
  store ptr %a1, ptr %_1.name, align 8
  br label %bb0

bb0:
  store ptr null, ptr %_0, align 8
  call void @avant_gc_enter()
  %r = call ptr @avant_gc_root_begin(i32 2)
  store ptr %_0, ptr %r, align 8
  %r.1 = getelementptr inbounds ptr, ptr %r, i64 1
  store ptr %_1.name, ptr %r.1, align 8
  %t0 = load i32, ptr @g_avant_shared, align 4
  %t1 = icmp eq i32 %t0, 0
  %t2 = load i32, ptr @avant_gc_fast_ok, align 4
  %t3 = icmp eq i32 %t2, 1
  %t4 = load ptr, ptr @avant_gc_bump, align 8
  %t5 = load ptr, ptr @avant_gc_limit, align 8
  %t6 = ptrtoint ptr %t4 to i64
  %t7 = icmp ne i64 %t6, 0
  %t8 = add i64 %t6, 16
  %t9 = ptrtoint ptr %t5 to i64
  %t10 = icmp ule i64 %t8, %t9
  %t11 = and i1 %t1, %t3
  %t12 = and i1 %t7, %t10
  %t13 = and i1 %t11, %t12
  br i1 %t13, label %bb0.s3.fast, label %bb0.s3.slow

bb0.s3.fast:
  %t14 = getelementptr inbounds i8, ptr %t4, i64 16
  store ptr %t14, ptr @avant_gc_bump, align 8
  %t15 = load ptr, ptr @avant_gc_block, align 8
  %t16 = getelementptr inbounds i8, ptr %t15, i64 32
  store ptr %t14, ptr %t16, align 8
  store i64 -9214364833305067512, ptr %t4, align 8
  %t17 = getelementptr inbounds i8, ptr %t4, i64 8
  call void @llvm.memset.p0.i64(ptr %t17, i8 0, i64 8, i1 false)
  %t18 = load ptr, ptr @avant_gc_base, align 8
  %t19 = ptrtoint ptr %t4 to i64
  %t20 = ptrtoint ptr %t18 to i64
  %t21 = sub i64 %t19, %t20
  %t22 = load ptr, ptr @avant_gc_starts, align 8
  %t23 = lshr i64 %t21, 9
  %t24 = lshr i64 %t21, 3
  %t25 = and i64 %t24, 63
  %t26 = shl i64 1, %t25
  %t27 = getelementptr inbounds i64, ptr %t22, i64 %t23
  %t28 = load i64, ptr %t27, align 8
  %t29 = or i64 %t28, %t26
  store i64 %t29, ptr %t27, align 8
  %t30 = getelementptr inbounds i8, ptr @avant_g_stats, i64 24
  %t31 = load i64, ptr %t30, align 8
  %t32 = add i64 %t31, 16
  store i64 %t32, ptr %t30, align 8
  %t33 = load i64, ptr @avant_since_gc, align 8
  %t34 = add i64 %t33, 16
  store i64 %t34, ptr @avant_since_gc, align 8
  %t35 = load i64, ptr @avant_nursery_held, align 8
  %t36 = add i64 %t35, 16
  store i64 %t36, ptr @avant_nursery_held, align 8
  %t37 = load i64, ptr @avant_inline_allocs, align 8
  %t38 = add i64 %t37, 1
  store i64 %t38, ptr @avant_inline_allocs, align 8
  br label %bb0.s3.cont

bb0.s3.slow:
  %t39 = call ptr @avant_alloc(i64 8, i32 1)
  br label %bb0.s3.cont

bb0.s3.cont:
  %t40 = phi ptr [ %t17, %bb0.s3.fast ], [ %t39, %bb0.s3.slow ]
  store ptr %t40, ptr %_0, align 8
  %t41 = load ptr, ptr %_0, align 8
  %t42 = load ptr, ptr %_1.name, align 8
  call void @"av.main/Box.initialize"(ptr %t41, ptr %t42)
  call void @avant_gc_leave()
  %t43 = load ptr, ptr %_0, align 8
  ret ptr %t43
}

define internal ptr @"av.main/tag"(i32 %a1) #0 {
alloca:
  %_0 = alloca ptr, align 8
  %_1.n = alloca i32, align 4
  %_2.b = alloca ptr, align 8
  %_3 = alloca i1, align 1
  %_4 = alloca ptr, align 8
  store i32 %a1, ptr %_1.n, align 4
  br label %bb0

bb0:
  store ptr null, ptr %_0, align 8
  store ptr null, ptr %_2.b, align 8
  store ptr null, ptr %_4, align 8
  call void @avant_gc_enter()
  %r = call ptr @avant_gc_root_begin(i32 3)
  store ptr %_0, ptr %r, align 8
  %r.1 = getelementptr inbounds ptr, ptr %r, i64 1
  store ptr %_2.b, ptr %r.1, align 8
  %r.2 = getelementptr inbounds ptr, ptr %r, i64 2
  store ptr %_4, ptr %r.2, align 8
  %t0 = call ptr @"av.main/Box.new"(ptr @str.0)
  store ptr %t0, ptr %_2.b, align 8
  %t1 = load i32, ptr %_1.n, align 4
  %t2 = icmp sgt i32 %t1, 9
  store i1 %t2, ptr %_3, align 1
  %t3 = load i1, ptr %_3, align 1
  br i1 %t3, label %bb1, label %bb2

bb1:
  %t4 = load i32, ptr %_1.n, align 4
  %t5 = call ptr @avant_str_from_int(i32 %t4)
  store ptr %t5, ptr %_4, align 8
  %t6 = load ptr, ptr %_2.b, align 8
  %t7 = icmp eq ptr %t6, null
  br i1 %t7, label %bb1.s1.cont, label %bb1.s1.test

bb1.s1.test:
  %t8 = getelementptr inbounds i8, ptr %t6, i64 -2
  %t9 = load i16, ptr %t8, align 2
  %t10 = and i16 %t9, -32768
  %t11 = icmp ne i16 %t10, 0
  %t12 = and i16 %t9, 48
  %t13 = icmp ne i16 %t12, 0
  %t14 = and i1 %t11, %t13
  br i1 %t14, label %bb1.s1.cont, label %bb1.s1.call

bb1.s1.call:
  call void @avant_barrier(ptr %t6)
  br label %bb1.s1.cont

bb1.s1.cont:
  %t15 = load ptr, ptr %_4, align 8
  %t16 = load ptr, ptr %_2.b, align 8
  %t17 = getelementptr inbounds %"main.Box", ptr %t16, i32 0, i32 0
  store ptr %t15, ptr %t17, align 8
  br label %bb2

bb2:
  %t18 = load ptr, ptr %_2.b, align 8
  %t19 = getelementptr inbounds %"main.Box", ptr %t18, i32 0, i32 0
  %t20 = load ptr, ptr %t19, align 8
  store ptr %t20, ptr %_0, align 8
  call void @avant_gc_leave()
  %t21 = load ptr, ptr %_0, align 8
  ret ptr %t21
}

define i32 @main(i32 %a1, ptr %a2) #0 {
alloca:
  %_0 = alloca i32, align 4
  %_1.argc = alloca i32, align 4
  %_2.argv = alloca ptr, align 8
  %_3.last = alloca ptr, align 8
  %_4.i = alloca i32, align 4
  %_5 = alloca i1, align 1
  %_6 = alloca ptr, align 8
  store i32 %a1, ptr %_1.argc, align 4
  store ptr %a2, ptr %_2.argv, align 8
  br label %bb0

bb0:
  store ptr null, ptr %_3.last, align 8
  store ptr null, ptr %_6, align 8
  call void @avant_gc_enter()
  %r = call ptr @avant_gc_root_begin(i32 3)
  store ptr %_2.argv, ptr %r, align 8
  %r.1 = getelementptr inbounds ptr, ptr %r, i64 1
  store ptr %_3.last, ptr %r.1, align 8
  %r.2 = getelementptr inbounds ptr, ptr %r, i64 2
  store ptr %_6, ptr %r.2, align 8
  call void @__avant_type_maps()
  %t0 = load i32, ptr %_1.argc, align 4
  %t1 = load ptr, ptr %_2.argv, align 8
  call void @avant_io_init_argv(i32 %t0, ptr %t1)
  %t2 = call ptr @"av.main/tag"(i32 0)
  store ptr %t2, ptr %_3.last, align 8
  store i32 0, ptr %_4.i, align 4
  br label %bb1

bb1:
  %t3 = load i32, ptr %_4.i, align 4
  %t4 = icmp slt i32 %t3, 200000
  store i1 %t4, ptr %_5, align 1
  %t5 = load i1, ptr %_5, align 1
  br i1 %t5, label %bb2, label %bb3

bb2:
  %t6 = load i32, ptr %_4.i, align 4
  %t7 = call ptr @"av.main/tag"(i32 %t6)
  store ptr %t7, ptr %_3.last, align 8
  %t8 = load i32, ptr %_4.i, align 4
  %t9 = add i32 %t8, 1
  store i32 %t9, ptr %_4.i, align 4
  br label %bb1

bb3:
  %t10 = load ptr, ptr %_3.last, align 8
  %t11 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t10)
  %t12 = call ptr @"av.main/tag"(i32 3)
  store ptr %t12, ptr %_6, align 8
  %t13 = load ptr, ptr %_6, align 8
  %t14 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t13)
  store i32 0, ptr %_0, align 4
  call void @avant_gc_leave()
  %t15 = load i32, ptr %_0, align 4
  ret i32 %t15
}

define internal void @__avant_type_maps() #0 {
entry:
  call void @avant_type_map(i32 1022, i64 1)
  call void @avant_type_map(i32 1021, i64 1)
  call void @avant_type_map(i32 1023, i64 7)
  call void @avant_type_map(i32 1, i64 1)
  ret void
}

attributes #0 = { nounwind "frame-pointer"="all" }
