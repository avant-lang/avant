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

@avant_gc_top = external thread_local(localexec) global ptr

declare ptr @avant_alloc(i64, i32)
declare void @avant_barrier(ptr)
declare void @avant_gc_frame_enter(ptr)
declare void @avant_gc_frame_leave(ptr)
declare void @avant_io_init_argv(i32, ptr)
declare ptr @avant_str_from_int(i64)
declare void @avant_type_map(i32, i64)
declare i32 @printf(ptr, ...)
declare i1 @llvm.expect.i1(i1, i1)
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
  %gcf = alloca { ptr, ptr, i32, i32, i32, [2 x ptr] }, align 8
  store ptr %a1, ptr %_1.name, align 8
  br label %bb0

bb0:
  store ptr null, ptr %_0, align 8
  %t0 = getelementptr inbounds i8, ptr %gcf, i64 32
  %t1 = getelementptr inbounds i8, ptr %gcf, i64 8
  store ptr %t0, ptr %t1, align 8
  %t2 = getelementptr inbounds i8, ptr %gcf, i64 16
  store i32 2, ptr %t2, align 8
  %t3 = getelementptr inbounds i8, ptr %gcf, i64 20
  store i32 2, ptr %t3, align 4
  %t4 = getelementptr inbounds i8, ptr %gcf, i64 24
  store i32 0, ptr %t4, align 8
  store ptr %_0, ptr %t0, align 8
  %t5 = getelementptr inbounds i8, ptr %gcf, i64 40
  store ptr %_1.name, ptr %t5, align 8
  call void @__avant_frame_push(ptr %gcf)
  %t6 = load i32, ptr @g_avant_shared, align 4
  %t7 = icmp eq i32 %t6, 0
  %t8 = load i32, ptr @avant_gc_fast_ok, align 4
  %t9 = icmp eq i32 %t8, 1
  %t10 = load ptr, ptr @avant_gc_bump, align 8
  %t11 = load ptr, ptr @avant_gc_limit, align 8
  %t12 = ptrtoint ptr %t10 to i64
  %t13 = icmp ne i64 %t12, 0
  %t14 = add i64 %t12, 16
  %t15 = ptrtoint ptr %t11 to i64
  %t16 = icmp ule i64 %t14, %t15
  %t17 = and i1 %t7, %t9
  %t18 = and i1 %t13, %t16
  %t19 = and i1 %t17, %t18
  br i1 %t19, label %bb0.s3.fast, label %bb0.s3.slow

bb0.s3.fast:
  %t20 = getelementptr inbounds i8, ptr %t10, i64 16
  store ptr %t20, ptr @avant_gc_bump, align 8
  %t21 = load ptr, ptr @avant_gc_block, align 8
  %t22 = getelementptr inbounds i8, ptr %t21, i64 32
  store ptr %t20, ptr %t22, align 8
  store i64 -9214364833305067512, ptr %t10, align 8
  %t23 = getelementptr inbounds i8, ptr %t10, i64 8
  call void @llvm.memset.p0.i64(ptr %t23, i8 0, i64 8, i1 false)
  %t24 = load ptr, ptr @avant_gc_base, align 8
  %t25 = ptrtoint ptr %t10 to i64
  %t26 = ptrtoint ptr %t24 to i64
  %t27 = sub i64 %t25, %t26
  %t28 = load ptr, ptr @avant_gc_starts, align 8
  %t29 = lshr i64 %t27, 9
  %t30 = lshr i64 %t27, 3
  %t31 = and i64 %t30, 63
  %t32 = shl i64 1, %t31
  %t33 = getelementptr inbounds i64, ptr %t28, i64 %t29
  %t34 = load i64, ptr %t33, align 8
  %t35 = or i64 %t34, %t32
  store i64 %t35, ptr %t33, align 8
  %t36 = getelementptr inbounds i8, ptr @avant_g_stats, i64 24
  %t37 = load i64, ptr %t36, align 8
  %t38 = add i64 %t37, 16
  store i64 %t38, ptr %t36, align 8
  %t39 = load i64, ptr @avant_since_gc, align 8
  %t40 = add i64 %t39, 16
  store i64 %t40, ptr @avant_since_gc, align 8
  %t41 = load i64, ptr @avant_nursery_held, align 8
  %t42 = add i64 %t41, 16
  store i64 %t42, ptr @avant_nursery_held, align 8
  %t43 = load i64, ptr @avant_inline_allocs, align 8
  %t44 = add i64 %t43, 1
  store i64 %t44, ptr @avant_inline_allocs, align 8
  br label %bb0.s3.cont

bb0.s3.slow:
  %t45 = call ptr @avant_alloc(i64 8, i32 1)
  br label %bb0.s3.cont

bb0.s3.cont:
  %t46 = phi ptr [ %t23, %bb0.s3.fast ], [ %t45, %bb0.s3.slow ]
  store ptr %t46, ptr %_0, align 8
  %t47 = load ptr, ptr %_0, align 8
  %t48 = load ptr, ptr %_1.name, align 8
  call void @"av.main/Box.initialize"(ptr %t47, ptr %t48)
  call void @__avant_frame_pop(ptr %gcf)
  %t49 = load ptr, ptr %_0, align 8
  ret ptr %t49
}

define internal ptr @"av.main/tag"(i64 %a1) #0 {
alloca:
  %_0 = alloca ptr, align 8
  %_1.n = alloca i64, align 8
  %_2.b = alloca ptr, align 8
  %_3 = alloca i1, align 1
  %_4 = alloca ptr, align 8
  %gcf = alloca { ptr, ptr, i32, i32, i32, [3 x ptr] }, align 8
  store i64 %a1, ptr %_1.n, align 8
  br label %bb0

bb0:
  store ptr null, ptr %_0, align 8
  store ptr null, ptr %_2.b, align 8
  store ptr null, ptr %_4, align 8
  %t0 = getelementptr inbounds i8, ptr %gcf, i64 32
  %t1 = getelementptr inbounds i8, ptr %gcf, i64 8
  store ptr %t0, ptr %t1, align 8
  %t2 = getelementptr inbounds i8, ptr %gcf, i64 16
  store i32 3, ptr %t2, align 8
  %t3 = getelementptr inbounds i8, ptr %gcf, i64 20
  store i32 3, ptr %t3, align 4
  %t4 = getelementptr inbounds i8, ptr %gcf, i64 24
  store i32 0, ptr %t4, align 8
  store ptr %_0, ptr %t0, align 8
  %t5 = getelementptr inbounds i8, ptr %gcf, i64 40
  store ptr %_2.b, ptr %t5, align 8
  %t6 = getelementptr inbounds i8, ptr %gcf, i64 48
  store ptr %_4, ptr %t6, align 8
  call void @__avant_frame_push(ptr %gcf)
  %t7 = call ptr @"av.main/Box.new"(ptr @str.0)
  store ptr %t7, ptr %_2.b, align 8
  %t8 = load i64, ptr %_1.n, align 8
  %t9 = icmp sgt i64 %t8, 9
  store i1 %t9, ptr %_3, align 1
  %t10 = load i1, ptr %_3, align 1
  br i1 %t10, label %bb1, label %bb2

bb1:
  %t11 = load i64, ptr %_1.n, align 8
  %t12 = call ptr @avant_str_from_int(i64 %t11)
  store ptr %t12, ptr %_4, align 8
  %t13 = load ptr, ptr %_2.b, align 8
  %t14 = icmp eq ptr %t13, null
  br i1 %t14, label %bb1.s1.cont, label %bb1.s1.test

bb1.s1.test:
  %t15 = getelementptr inbounds i8, ptr %t13, i64 -2
  %t16 = load i16, ptr %t15, align 2
  %t17 = and i16 %t16, -32768
  %t18 = icmp ne i16 %t17, 0
  %t19 = and i16 %t16, 48
  %t20 = icmp ne i16 %t19, 0
  %t21 = and i1 %t18, %t20
  br i1 %t21, label %bb1.s1.cont, label %bb1.s1.call

bb1.s1.call:
  call void @avant_barrier(ptr %t13)
  br label %bb1.s1.cont

bb1.s1.cont:
  %t22 = load ptr, ptr %_4, align 8
  %t23 = load ptr, ptr %_2.b, align 8
  %t24 = getelementptr inbounds %"main.Box", ptr %t23, i32 0, i32 0
  store ptr %t22, ptr %t24, align 8
  br label %bb2

bb2:
  %t25 = load ptr, ptr %_2.b, align 8
  %t26 = getelementptr inbounds %"main.Box", ptr %t25, i32 0, i32 0
  %t27 = load ptr, ptr %t26, align 8
  store ptr %t27, ptr %_0, align 8
  call void @__avant_frame_pop(ptr %gcf)
  %t28 = load ptr, ptr %_0, align 8
  ret ptr %t28
}

define i32 @main(i32 %a1, ptr %a2) #0 {
alloca:
  %_0 = alloca i32, align 4
  %_1.argc = alloca i32, align 4
  %_2.argv = alloca ptr, align 8
  %_3.last = alloca ptr, align 8
  %_4.i = alloca i64, align 8
  %_5 = alloca i1, align 1
  %_6 = alloca ptr, align 8
  %gcf = alloca { ptr, ptr, i32, i32, i32, [3 x ptr] }, align 8
  store i32 %a1, ptr %_1.argc, align 4
  store ptr %a2, ptr %_2.argv, align 8
  br label %bb0

bb0:
  store ptr null, ptr %_3.last, align 8
  store ptr null, ptr %_6, align 8
  %t0 = getelementptr inbounds i8, ptr %gcf, i64 32
  %t1 = getelementptr inbounds i8, ptr %gcf, i64 8
  store ptr %t0, ptr %t1, align 8
  %t2 = getelementptr inbounds i8, ptr %gcf, i64 16
  store i32 3, ptr %t2, align 8
  %t3 = getelementptr inbounds i8, ptr %gcf, i64 20
  store i32 3, ptr %t3, align 4
  %t4 = getelementptr inbounds i8, ptr %gcf, i64 24
  store i32 0, ptr %t4, align 8
  store ptr %_2.argv, ptr %t0, align 8
  %t5 = getelementptr inbounds i8, ptr %gcf, i64 40
  store ptr %_3.last, ptr %t5, align 8
  %t6 = getelementptr inbounds i8, ptr %gcf, i64 48
  store ptr %_6, ptr %t6, align 8
  call void @__avant_frame_push(ptr %gcf)
  call void @__avant_type_maps()
  %t7 = load i32, ptr %_1.argc, align 4
  %t8 = load ptr, ptr %_2.argv, align 8
  call void @avant_io_init_argv(i32 %t7, ptr %t8)
  %t9 = call ptr @"av.main/tag"(i64 0)
  store ptr %t9, ptr %_3.last, align 8
  store i64 0, ptr %_4.i, align 8
  br label %bb1

bb1:
  %t10 = load i64, ptr %_4.i, align 8
  %t11 = icmp slt i64 %t10, 200000
  store i1 %t11, ptr %_5, align 1
  %t12 = load i1, ptr %_5, align 1
  br i1 %t12, label %bb2, label %bb3

bb2:
  %t13 = load i64, ptr %_4.i, align 8
  %t14 = call ptr @"av.main/tag"(i64 %t13)
  store ptr %t14, ptr %_3.last, align 8
  %t15 = load i64, ptr %_4.i, align 8
  %t16 = add i64 %t15, 1
  store i64 %t16, ptr %_4.i, align 8
  br label %bb1

bb3:
  %t17 = load ptr, ptr %_3.last, align 8
  %t18 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t17)
  %t19 = call ptr @"av.main/tag"(i64 3)
  store ptr %t19, ptr %_6, align 8
  %t20 = load ptr, ptr %_6, align 8
  %t21 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t20)
  store i32 0, ptr %_0, align 4
  call void @__avant_frame_pop(ptr %gcf)
  %t22 = load i32, ptr %_0, align 4
  ret i32 %t22
}

define internal void @__avant_type_maps() #0 {
entry:
  call void @avant_type_map(i32 1022, i64 1)
  call void @avant_type_map(i32 1021, i64 1)
  call void @avant_type_map(i32 1023, i64 7)
  call void @avant_type_map(i32 1, i64 1)
  ret void
}

define internal void @__avant_frame_push(ptr %f) #0 {
entry:
  %t = load ptr, ptr @avant_gc_top, align 8
  %c = icmp ne ptr %t, null
  %x = call i1 @llvm.expect.i1(i1 %c, i1 true)
  br i1 %x, label %link, label %enter

link:
  store ptr %t, ptr %f, align 8
  store volatile ptr %f, ptr @avant_gc_top, align 8
  ret void

enter:
  call void @avant_gc_frame_enter(ptr %f)
  ret void
}

define internal void @__avant_frame_pop(ptr %f) #0 {
entry:
  %t = load ptr, ptr @avant_gc_top, align 8
  %p = load ptr, ptr %f, align 8
  %c1 = icmp eq ptr %t, %f
  %c2 = icmp ne ptr %p, null
  %c = and i1 %c1, %c2
  %x = call i1 @llvm.expect.i1(i1 %c, i1 true)
  br i1 %x, label %unlink, label %leave

unlink:
  store volatile ptr %p, ptr @avant_gc_top, align 8
  ret void

leave:
  call void @avant_gc_frame_leave(ptr %f)
  ret void
}

attributes #0 = { nounwind "frame-pointer"="all" }
