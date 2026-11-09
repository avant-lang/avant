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
declare ptr @avant_str_from_int(i32)
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
  %t6 = load ptr, ptr @avant_gc_top, align 8
  %t7 = icmp ne ptr %t6, null
  %t8 = call i1 @llvm.expect.i1(i1 %t7, i1 true)
  br i1 %t8, label %bb0.s1.link, label %bb0.s1.enter

bb0.s1.link:
  store ptr %t6, ptr %gcf, align 8
  store volatile ptr %gcf, ptr @avant_gc_top, align 8
  br label %bb0.s1.entered

bb0.s1.enter:
  call void @avant_gc_frame_enter(ptr %gcf)
  br label %bb0.s1.entered

bb0.s1.entered:
  %t9 = load i32, ptr @g_avant_shared, align 4
  %t10 = icmp eq i32 %t9, 0
  %t11 = load i32, ptr @avant_gc_fast_ok, align 4
  %t12 = icmp eq i32 %t11, 1
  %t13 = load ptr, ptr @avant_gc_bump, align 8
  %t14 = load ptr, ptr @avant_gc_limit, align 8
  %t15 = ptrtoint ptr %t13 to i64
  %t16 = icmp ne i64 %t15, 0
  %t17 = add i64 %t15, 16
  %t18 = ptrtoint ptr %t14 to i64
  %t19 = icmp ule i64 %t17, %t18
  %t20 = and i1 %t10, %t12
  %t21 = and i1 %t16, %t19
  %t22 = and i1 %t20, %t21
  br i1 %t22, label %bb0.s3.fast, label %bb0.s3.slow

bb0.s3.fast:
  %t23 = getelementptr inbounds i8, ptr %t13, i64 16
  store ptr %t23, ptr @avant_gc_bump, align 8
  %t24 = load ptr, ptr @avant_gc_block, align 8
  %t25 = getelementptr inbounds i8, ptr %t24, i64 32
  store ptr %t23, ptr %t25, align 8
  store i64 -9214364833305067512, ptr %t13, align 8
  %t26 = getelementptr inbounds i8, ptr %t13, i64 8
  call void @llvm.memset.p0.i64(ptr %t26, i8 0, i64 8, i1 false)
  %t27 = load ptr, ptr @avant_gc_base, align 8
  %t28 = ptrtoint ptr %t13 to i64
  %t29 = ptrtoint ptr %t27 to i64
  %t30 = sub i64 %t28, %t29
  %t31 = load ptr, ptr @avant_gc_starts, align 8
  %t32 = lshr i64 %t30, 9
  %t33 = lshr i64 %t30, 3
  %t34 = and i64 %t33, 63
  %t35 = shl i64 1, %t34
  %t36 = getelementptr inbounds i64, ptr %t31, i64 %t32
  %t37 = load i64, ptr %t36, align 8
  %t38 = or i64 %t37, %t35
  store i64 %t38, ptr %t36, align 8
  %t39 = getelementptr inbounds i8, ptr @avant_g_stats, i64 24
  %t40 = load i64, ptr %t39, align 8
  %t41 = add i64 %t40, 16
  store i64 %t41, ptr %t39, align 8
  %t42 = load i64, ptr @avant_since_gc, align 8
  %t43 = add i64 %t42, 16
  store i64 %t43, ptr @avant_since_gc, align 8
  %t44 = load i64, ptr @avant_nursery_held, align 8
  %t45 = add i64 %t44, 16
  store i64 %t45, ptr @avant_nursery_held, align 8
  %t46 = load i64, ptr @avant_inline_allocs, align 8
  %t47 = add i64 %t46, 1
  store i64 %t47, ptr @avant_inline_allocs, align 8
  br label %bb0.s3.cont

bb0.s3.slow:
  %t48 = call ptr @avant_alloc(i64 8, i32 1)
  br label %bb0.s3.cont

bb0.s3.cont:
  %t49 = phi ptr [ %t26, %bb0.s3.fast ], [ %t48, %bb0.s3.slow ]
  store ptr %t49, ptr %_0, align 8
  %t50 = load ptr, ptr %_0, align 8
  %t51 = load ptr, ptr %_1.name, align 8
  call void @"av.main/Box.initialize"(ptr %t50, ptr %t51)
  %t52 = load ptr, ptr @avant_gc_top, align 8
  %t53 = load ptr, ptr %gcf, align 8
  %t54 = icmp eq ptr %t52, %gcf
  %t55 = icmp ne ptr %t53, null
  %t56 = and i1 %t54, %t55
  %t57 = call i1 @llvm.expect.i1(i1 %t56, i1 true)
  br i1 %t57, label %bb0.s5.unlink, label %bb0.s5.leave

bb0.s5.unlink:
  store volatile ptr %t53, ptr @avant_gc_top, align 8
  br label %bb0.s5.left

bb0.s5.leave:
  call void @avant_gc_frame_leave(ptr %gcf)
  br label %bb0.s5.left

bb0.s5.left:
  %t58 = load ptr, ptr %_0, align 8
  ret ptr %t58
}

define internal ptr @"av.main/tag"(i32 %a1) #0 {
alloca:
  %_0 = alloca ptr, align 8
  %_1.n = alloca i32, align 4
  %_2.b = alloca ptr, align 8
  %_3 = alloca i1, align 1
  %_4 = alloca ptr, align 8
  %gcf = alloca { ptr, ptr, i32, i32, i32, [3 x ptr] }, align 8
  store i32 %a1, ptr %_1.n, align 4
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
  %t7 = load ptr, ptr @avant_gc_top, align 8
  %t8 = icmp ne ptr %t7, null
  %t9 = call i1 @llvm.expect.i1(i1 %t8, i1 true)
  br i1 %t9, label %bb0.s3.link, label %bb0.s3.enter

bb0.s3.link:
  store ptr %t7, ptr %gcf, align 8
  store volatile ptr %gcf, ptr @avant_gc_top, align 8
  br label %bb0.s3.entered

bb0.s3.enter:
  call void @avant_gc_frame_enter(ptr %gcf)
  br label %bb0.s3.entered

bb0.s3.entered:
  %t10 = call ptr @"av.main/Box.new"(ptr @str.0)
  store ptr %t10, ptr %_2.b, align 8
  %t11 = load i32, ptr %_1.n, align 4
  %t12 = icmp sgt i32 %t11, 9
  store i1 %t12, ptr %_3, align 1
  %t13 = load i1, ptr %_3, align 1
  br i1 %t13, label %bb1, label %bb2

bb1:
  %t14 = load i32, ptr %_1.n, align 4
  %t15 = call ptr @avant_str_from_int(i32 %t14)
  store ptr %t15, ptr %_4, align 8
  %t16 = load ptr, ptr %_2.b, align 8
  %t17 = icmp eq ptr %t16, null
  br i1 %t17, label %bb1.s1.cont, label %bb1.s1.test

bb1.s1.test:
  %t18 = getelementptr inbounds i8, ptr %t16, i64 -2
  %t19 = load i16, ptr %t18, align 2
  %t20 = and i16 %t19, -32768
  %t21 = icmp ne i16 %t20, 0
  %t22 = and i16 %t19, 48
  %t23 = icmp ne i16 %t22, 0
  %t24 = and i1 %t21, %t23
  br i1 %t24, label %bb1.s1.cont, label %bb1.s1.call

bb1.s1.call:
  call void @avant_barrier(ptr %t16)
  br label %bb1.s1.cont

bb1.s1.cont:
  %t25 = load ptr, ptr %_4, align 8
  %t26 = load ptr, ptr %_2.b, align 8
  %t27 = getelementptr inbounds %"main.Box", ptr %t26, i32 0, i32 0
  store ptr %t25, ptr %t27, align 8
  br label %bb2

bb2:
  %t28 = load ptr, ptr %_2.b, align 8
  %t29 = getelementptr inbounds %"main.Box", ptr %t28, i32 0, i32 0
  %t30 = load ptr, ptr %t29, align 8
  store ptr %t30, ptr %_0, align 8
  %t31 = load ptr, ptr @avant_gc_top, align 8
  %t32 = load ptr, ptr %gcf, align 8
  %t33 = icmp eq ptr %t31, %gcf
  %t34 = icmp ne ptr %t32, null
  %t35 = and i1 %t33, %t34
  %t36 = call i1 @llvm.expect.i1(i1 %t35, i1 true)
  br i1 %t36, label %bb2.s1.unlink, label %bb2.s1.leave

bb2.s1.unlink:
  store volatile ptr %t32, ptr @avant_gc_top, align 8
  br label %bb2.s1.left

bb2.s1.leave:
  call void @avant_gc_frame_leave(ptr %gcf)
  br label %bb2.s1.left

bb2.s1.left:
  %t37 = load ptr, ptr %_0, align 8
  ret ptr %t37
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
  %t7 = load ptr, ptr @avant_gc_top, align 8
  %t8 = icmp ne ptr %t7, null
  %t9 = call i1 @llvm.expect.i1(i1 %t8, i1 true)
  br i1 %t9, label %bb0.s2.link, label %bb0.s2.enter

bb0.s2.link:
  store ptr %t7, ptr %gcf, align 8
  store volatile ptr %gcf, ptr @avant_gc_top, align 8
  br label %bb0.s2.entered

bb0.s2.enter:
  call void @avant_gc_frame_enter(ptr %gcf)
  br label %bb0.s2.entered

bb0.s2.entered:
  call void @__avant_type_maps()
  %t10 = load i32, ptr %_1.argc, align 4
  %t11 = load ptr, ptr %_2.argv, align 8
  call void @avant_io_init_argv(i32 %t10, ptr %t11)
  %t12 = call ptr @"av.main/tag"(i32 0)
  store ptr %t12, ptr %_3.last, align 8
  store i32 0, ptr %_4.i, align 4
  br label %bb1

bb1:
  %t13 = load i32, ptr %_4.i, align 4
  %t14 = icmp slt i32 %t13, 200000
  store i1 %t14, ptr %_5, align 1
  %t15 = load i1, ptr %_5, align 1
  br i1 %t15, label %bb2, label %bb3

bb2:
  %t16 = load i32, ptr %_4.i, align 4
  %t17 = call ptr @"av.main/tag"(i32 %t16)
  store ptr %t17, ptr %_3.last, align 8
  %t18 = load i32, ptr %_4.i, align 4
  %t19 = add i32 %t18, 1
  store i32 %t19, ptr %_4.i, align 4
  br label %bb1

bb3:
  %t20 = load ptr, ptr %_3.last, align 8
  %t21 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t20)
  %t22 = call ptr @"av.main/tag"(i32 3)
  store ptr %t22, ptr %_6, align 8
  %t23 = load ptr, ptr %_6, align 8
  %t24 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t23)
  store i32 0, ptr %_0, align 4
  %t25 = load ptr, ptr @avant_gc_top, align 8
  %t26 = load ptr, ptr %gcf, align 8
  %t27 = icmp eq ptr %t25, %gcf
  %t28 = icmp ne ptr %t26, null
  %t29 = and i1 %t27, %t28
  %t30 = call i1 @llvm.expect.i1(i1 %t29, i1 true)
  br i1 %t30, label %bb3.s4.unlink, label %bb3.s4.leave

bb3.s4.unlink:
  store volatile ptr %t26, ptr @avant_gc_top, align 8
  br label %bb3.s4.left

bb3.s4.leave:
  call void @avant_gc_frame_leave(ptr %gcf)
  br label %bb3.s4.left

bb3.s4.left:
  %t31 = load i32, ptr %_0, align 4
  ret i32 %t31
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
