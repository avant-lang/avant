source_filename = "main"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%"main.Box" = type { ptr }

@str.0 = private constant { i64, [6 x i8] } { i64 4611686018427387910, [6 x i8] c"small\00" }, align 8
@str.1 = private constant { i64, [4 x i8] } { i64 4611686018427387908, [4 x i8] c"%s\0A\00" }, align 8

@avant_str_headered = constant i32 1, align 4

@avant_gc_bump = external thread_local(localexec) global ptr
@avant_gc_limit = external thread_local(localexec) global ptr

@avant_gc_top = external thread_local(localexec) global ptr

declare ptr @avant_alloc(i64, i32)
declare ptr @avant_alloc_slow(i64, i32)
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
  %t0 = load ptr, ptr %_1.self, align 8, !tbaa !9
  %t1 = icmp eq ptr %t0, null
  br i1 %t1, label %bb0.s0.cont, label %bb0.s0.test

bb0.s0.test:
  %t2 = getelementptr inbounds i8, ptr %t0, i64 -2
  %t3 = load i16, ptr %t2, align 2
  %t4 = and i16 %t3, -32720
  %t5 = icmp ugt i16 %t4, -32768
  br i1 %t5, label %bb0.s0.cont, label %bb0.s0.call

bb0.s0.call:
  call void @avant_barrier(ptr %t0)
  br label %bb0.s0.cont

bb0.s0.cont:
  %t6 = load ptr, ptr %_2.name, align 8, !tbaa !9
  %t7 = load ptr, ptr %_1.self, align 8, !tbaa !9
  %t8 = getelementptr inbounds %"main.Box", ptr %t7, i32 0, i32 0
  store ptr %t6, ptr %t8, align 8, !tbaa !11
  ret void
}

define internal ptr @"av.main/Box.new"(ptr %a1) #0 {
alloca:
  %gcf = alloca { ptr, i64, [2 x i64] }, align 8
  %t0 = getelementptr inbounds i8, ptr %gcf, i64 24
  store ptr %a1, ptr %t0, align 8
  br label %bb0

bb0:
  %t1 = getelementptr inbounds i8, ptr %gcf, i64 16
  store ptr null, ptr %t1, align 8, !tbaa !9
  %t2 = getelementptr inbounds i8, ptr %gcf, i64 8
  store i64 7, ptr %t2, align 8
  %t3 = load ptr, ptr @avant_gc_top, align 8
  store ptr %t3, ptr %gcf, align 8
  store volatile ptr %gcf, ptr @avant_gc_top, align 8
  %t4 = load ptr, ptr @avant_gc_bump, align 8
  %t5 = load ptr, ptr @avant_gc_limit, align 8
  %t6 = ptrtoint ptr %t4 to i64
  %t7 = add i64 %t6, 16
  %t8 = ptrtoint ptr %t5 to i64
  %t9 = icmp ule i64 %t7, %t8
  br i1 %t9, label %bb0.s3.fast, label %bb0.s3.slow

bb0.s3.fast:
  %t10 = getelementptr inbounds i8, ptr %t4, i64 16
  store ptr %t10, ptr @avant_gc_bump, align 8
  store i64 -9214364833305067512, ptr %t4, align 8
  %t11 = getelementptr inbounds i8, ptr %t4, i64 8
  call void @llvm.memset.p0.i64(ptr %t11, i8 0, i64 8, i1 false)
  br label %bb0.s3.cont

bb0.s3.slow:
  %t12 = call ptr @__avant_alloc_slow(i64 8, i32 1)
  br label %bb0.s3.cont

bb0.s3.cont:
  %t13 = phi ptr [ %t11, %bb0.s3.fast ], [ %t12, %bb0.s3.slow ]
  %t14 = getelementptr inbounds i8, ptr %gcf, i64 16
  store ptr %t13, ptr %t14, align 8
  %t15 = getelementptr inbounds i8, ptr %gcf, i64 16
  %t16 = load ptr, ptr %t15, align 8, !tbaa !9
  %t17 = getelementptr inbounds i8, ptr %gcf, i64 24
  %t18 = load ptr, ptr %t17, align 8, !tbaa !9
  call void @"av.main/Box.initialize"(ptr %t16, ptr %t18)
  %t19 = load ptr, ptr %gcf, align 8
  store volatile ptr %t19, ptr @avant_gc_top, align 8
  %t20 = getelementptr inbounds i8, ptr %gcf, i64 16
  %t21 = load ptr, ptr %t20, align 8
  ret ptr %t21
}

define internal ptr @"av.main/tag"(i64 %a1) #0 {
alloca:
  %gcf = alloca { ptr, i64, [3 x i64] }, align 8
  %_1.n = alloca i64, align 8
  %_3 = alloca i1, align 1
  store i64 %a1, ptr %_1.n, align 8
  br label %bb0

bb0:
  %t0 = getelementptr inbounds i8, ptr %gcf, i64 16
  store ptr null, ptr %t0, align 8, !tbaa !9
  %t1 = getelementptr inbounds i8, ptr %gcf, i64 24
  store ptr null, ptr %t1, align 8, !tbaa !9
  %t2 = getelementptr inbounds i8, ptr %gcf, i64 32
  store ptr null, ptr %t2, align 8, !tbaa !9
  %t3 = getelementptr inbounds i8, ptr %gcf, i64 8
  store i64 15, ptr %t3, align 8
  %t4 = load ptr, ptr @avant_gc_top, align 8
  store ptr %t4, ptr %gcf, align 8
  store volatile ptr %gcf, ptr @avant_gc_top, align 8
  %t5 = call ptr @"av.main/Box.new"(ptr getelementptr inbounds (i8, ptr @str.0, i64 8))
  %t6 = getelementptr inbounds i8, ptr %gcf, i64 24
  store ptr %t5, ptr %t6, align 8
  %t7 = load i64, ptr %_1.n, align 8, !tbaa !9
  %t8 = icmp sgt i64 %t7, 9
  store i1 %t8, ptr %_3, align 1, !tbaa !9
  %t9 = load i1, ptr %_3, align 1, !tbaa !9
  br i1 %t9, label %bb1, label %bb2

bb1:
  %t10 = load i64, ptr %_1.n, align 8, !tbaa !9
  %t11 = call ptr @avant_str_from_int(i64 %t10)
  %t12 = getelementptr inbounds i8, ptr %gcf, i64 32
  store ptr %t11, ptr %t12, align 8
  %t13 = getelementptr inbounds i8, ptr %gcf, i64 24
  %t14 = load ptr, ptr %t13, align 8, !tbaa !9
  %t15 = icmp eq ptr %t14, null
  br i1 %t15, label %bb1.s1.cont, label %bb1.s1.test

bb1.s1.test:
  %t16 = getelementptr inbounds i8, ptr %t14, i64 -2
  %t17 = load i16, ptr %t16, align 2
  %t18 = and i16 %t17, -32720
  %t19 = icmp ugt i16 %t18, -32768
  br i1 %t19, label %bb1.s1.cont, label %bb1.s1.call

bb1.s1.call:
  call void @avant_barrier(ptr %t14)
  br label %bb1.s1.cont

bb1.s1.cont:
  %t20 = getelementptr inbounds i8, ptr %gcf, i64 32
  %t21 = load ptr, ptr %t20, align 8, !tbaa !9
  %t22 = getelementptr inbounds i8, ptr %gcf, i64 24
  %t23 = load ptr, ptr %t22, align 8, !tbaa !9
  %t24 = getelementptr inbounds %"main.Box", ptr %t23, i32 0, i32 0
  store ptr %t21, ptr %t24, align 8, !tbaa !11
  br label %bb2

bb2:
  %t25 = getelementptr inbounds i8, ptr %gcf, i64 24
  %t26 = load ptr, ptr %t25, align 8, !tbaa !9
  %t27 = getelementptr inbounds %"main.Box", ptr %t26, i32 0, i32 0
  %t28 = load ptr, ptr %t27, align 8, !tbaa !11
  %t29 = getelementptr inbounds i8, ptr %gcf, i64 16
  store ptr %t28, ptr %t29, align 8, !tbaa !9
  %t30 = load ptr, ptr %gcf, align 8
  store volatile ptr %t30, ptr @avant_gc_top, align 8
  %t31 = getelementptr inbounds i8, ptr %gcf, i64 16
  %t32 = load ptr, ptr %t31, align 8
  ret ptr %t32
}

define i32 @main(i32 %a1, ptr %a2) #0 {
alloca:
  %gcf = alloca { ptr, i64, [3 x i64] }, align 8
  %_0 = alloca i32, align 4
  %_1.argc = alloca i32, align 4
  %_4.i = alloca i64, align 8
  %_5 = alloca i1, align 1
  store i32 %a1, ptr %_1.argc, align 4
  %t0 = getelementptr inbounds i8, ptr %gcf, i64 16
  store ptr %a2, ptr %t0, align 8
  br label %bb0

bb0:
  %t1 = getelementptr inbounds i8, ptr %gcf, i64 24
  store ptr null, ptr %t1, align 8, !tbaa !9
  %t2 = getelementptr inbounds i8, ptr %gcf, i64 32
  store ptr null, ptr %t2, align 8, !tbaa !9
  %t3 = getelementptr inbounds i8, ptr %gcf, i64 8
  store i64 15, ptr %t3, align 8
  call void @__avant_frame_push(ptr %gcf)
  call void @__avant_type_maps()
  %t4 = load i32, ptr %_1.argc, align 4, !tbaa !9
  %t5 = getelementptr inbounds i8, ptr %gcf, i64 16
  %t6 = load ptr, ptr %t5, align 8, !tbaa !9
  call void @avant_io_init_argv(i32 %t4, ptr %t6)
  %t7 = call ptr @"av.main/tag"(i64 0)
  %t8 = getelementptr inbounds i8, ptr %gcf, i64 24
  store ptr %t7, ptr %t8, align 8
  store i64 0, ptr %_4.i, align 8, !tbaa !9
  br label %bb1

bb1:
  %t9 = load i64, ptr %_4.i, align 8, !tbaa !9
  %t10 = icmp slt i64 %t9, 200000
  store i1 %t10, ptr %_5, align 1, !tbaa !9
  %t11 = load i1, ptr %_5, align 1, !tbaa !9
  br i1 %t11, label %bb2, label %bb3

bb2:
  %t12 = load i64, ptr %_4.i, align 8, !tbaa !9
  %t13 = call ptr @"av.main/tag"(i64 %t12)
  %t14 = getelementptr inbounds i8, ptr %gcf, i64 24
  store ptr %t13, ptr %t14, align 8
  %t15 = load i64, ptr %_4.i, align 8, !tbaa !9
  %t16 = add i64 %t15, 1
  store i64 %t16, ptr %_4.i, align 8, !tbaa !9
  br label %bb1

bb3:
  %t17 = getelementptr inbounds i8, ptr %gcf, i64 24
  %t18 = load ptr, ptr %t17, align 8, !tbaa !9
  %t19 = call i32 (ptr, ...) @printf(ptr getelementptr inbounds (i8, ptr @str.1, i64 8), ptr %t18)
  %t20 = call ptr @"av.main/tag"(i64 3)
  %t21 = getelementptr inbounds i8, ptr %gcf, i64 32
  store ptr %t20, ptr %t21, align 8
  %t22 = getelementptr inbounds i8, ptr %gcf, i64 32
  %t23 = load ptr, ptr %t22, align 8, !tbaa !9
  %t24 = call i32 (ptr, ...) @printf(ptr getelementptr inbounds (i8, ptr @str.1, i64 8), ptr %t23)
  store i32 0, ptr %_0, align 4, !tbaa !9
  call void @__avant_frame_pop(ptr %gcf)
  %t25 = load i32, ptr %_0, align 4
  ret i32 %t25
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

define internal ptr @__avant_alloc_slow(i64 %size, i32 %ty) noinline #0 {
entry:
  %s = call ptr @avant_alloc_slow(i64 %size, i32 %ty)
  %again = icmp eq ptr %s, null
  br i1 %again, label %redo, label %got

got:
  ret ptr %s

redo:
  %bump = load ptr, ptr @avant_gc_bump, align 8
  %up = add i64 %size, 15
  %total = and i64 %up, -8
  %t26 = getelementptr inbounds i8, ptr %bump, i64 %total
  store ptr %t26, ptr @avant_gc_bump, align 8
  %t27 = zext i32 %ty to i64
  %t28 = shl i64 %t27, 32
  %t29 = or i64 %size, %t28
  %t30 = or i64 %t29, -9214364837600034816
  store i64 %t30, ptr %bump, align 8
  %t31 = getelementptr inbounds i8, ptr %bump, i64 8
  call void @llvm.memset.p0.i64(ptr %t31, i8 0, i64 %size, i1 false)
  ret ptr %t31
}

attributes #0 = { nounwind "frame-pointer"="all" "probe-stack"="inline-asm" }

!1 = !{!"avant alias"}
!2 = !{!"element", !1, i64 0}
!3 = !{!2, !2, i64 0}
!4 = !{!"buffer reference", !1, i64 0}
!5 = !{!4, !4, i64 0}
!6 = !{!"size", !1, i64 0}
!7 = !{!6, !6, i64 0}
!8 = !{!"local", !1, i64 0}
!9 = !{!8, !8, i64 0}
!10 = !{!"object field", !1, i64 0}
!11 = !{!10, !10, i64 0}
!12 = !{i32 0, i32 -2147483648}
!13 = !{}
!14 = !{i32 1, i32 -2147483647}
