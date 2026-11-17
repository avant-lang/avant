source_filename = "main"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%"main.Box" = type { ptr }

@str.0 = private constant [6 x i8] c"small\00", align 1
@str.1 = private constant [4 x i8] c"%s\0A\00", align 1

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
  %_0 = alloca ptr, align 8
  %_1.name = alloca ptr, align 8
  %gcf = alloca { ptr, ptr, i32, i32, i32, [2 x ptr] }, align 8
  store ptr %a1, ptr %_1.name, align 8
  br label %bb0

bb0:
  store ptr null, ptr %_0, align 8, !tbaa !9
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
  %t6 = load ptr, ptr @avant_gc_bump, align 8
  %t7 = load ptr, ptr @avant_gc_limit, align 8
  %t8 = ptrtoint ptr %t6 to i64
  %t9 = add i64 %t8, 16
  %t10 = ptrtoint ptr %t7 to i64
  %t11 = icmp ule i64 %t9, %t10
  br i1 %t11, label %bb0.s3.fast, label %bb0.s3.slow

bb0.s3.fast:
  %t12 = getelementptr inbounds i8, ptr %t6, i64 16
  store ptr %t12, ptr @avant_gc_bump, align 8
  store i64 -9214364833305067512, ptr %t6, align 8
  %t13 = getelementptr inbounds i8, ptr %t6, i64 8
  call void @llvm.memset.p0.i64(ptr %t13, i8 0, i64 8, i1 false)
  br label %bb0.s3.cont

bb0.s3.slow:
  %t14 = call ptr @__avant_alloc_slow(i64 8, i32 1)
  br label %bb0.s3.cont

bb0.s3.cont:
  %t15 = phi ptr [ %t13, %bb0.s3.fast ], [ %t14, %bb0.s3.slow ]
  store ptr %t15, ptr %_0, align 8
  %t16 = load ptr, ptr %_0, align 8, !tbaa !9
  %t17 = load ptr, ptr %_1.name, align 8, !tbaa !9
  call void @"av.main/Box.initialize"(ptr %t16, ptr %t17)
  call void @__avant_frame_pop(ptr %gcf)
  %t18 = load ptr, ptr %_0, align 8
  ret ptr %t18
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
  store ptr null, ptr %_0, align 8, !tbaa !9
  store ptr null, ptr %_2.b, align 8, !tbaa !9
  store ptr null, ptr %_4, align 8, !tbaa !9
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
  %t8 = load i64, ptr %_1.n, align 8, !tbaa !9
  %t9 = icmp sgt i64 %t8, 9
  store i1 %t9, ptr %_3, align 1, !tbaa !9
  %t10 = load i1, ptr %_3, align 1, !tbaa !9
  br i1 %t10, label %bb1, label %bb2

bb1:
  %t11 = load i64, ptr %_1.n, align 8, !tbaa !9
  %t12 = call ptr @avant_str_from_int(i64 %t11)
  store ptr %t12, ptr %_4, align 8
  %t13 = load ptr, ptr %_2.b, align 8, !tbaa !9
  %t14 = icmp eq ptr %t13, null
  br i1 %t14, label %bb1.s1.cont, label %bb1.s1.test

bb1.s1.test:
  %t15 = getelementptr inbounds i8, ptr %t13, i64 -2
  %t16 = load i16, ptr %t15, align 2
  %t17 = and i16 %t16, -32720
  %t18 = icmp ugt i16 %t17, -32768
  br i1 %t18, label %bb1.s1.cont, label %bb1.s1.call

bb1.s1.call:
  call void @avant_barrier(ptr %t13)
  br label %bb1.s1.cont

bb1.s1.cont:
  %t19 = load ptr, ptr %_4, align 8, !tbaa !9
  %t20 = load ptr, ptr %_2.b, align 8, !tbaa !9
  %t21 = getelementptr inbounds %"main.Box", ptr %t20, i32 0, i32 0
  store ptr %t19, ptr %t21, align 8, !tbaa !11
  br label %bb2

bb2:
  %t22 = load ptr, ptr %_2.b, align 8, !tbaa !9
  %t23 = getelementptr inbounds %"main.Box", ptr %t22, i32 0, i32 0
  %t24 = load ptr, ptr %t23, align 8, !tbaa !11
  store ptr %t24, ptr %_0, align 8, !tbaa !9
  call void @__avant_frame_pop(ptr %gcf)
  %t25 = load ptr, ptr %_0, align 8
  ret ptr %t25
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
  store ptr null, ptr %_3.last, align 8, !tbaa !9
  store ptr null, ptr %_6, align 8, !tbaa !9
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
  %t7 = load i32, ptr %_1.argc, align 4, !tbaa !9
  %t8 = load ptr, ptr %_2.argv, align 8, !tbaa !9
  call void @avant_io_init_argv(i32 %t7, ptr %t8)
  %t9 = call ptr @"av.main/tag"(i64 0)
  store ptr %t9, ptr %_3.last, align 8
  store i64 0, ptr %_4.i, align 8, !tbaa !9
  br label %bb1

bb1:
  %t10 = load i64, ptr %_4.i, align 8, !tbaa !9
  %t11 = icmp slt i64 %t10, 200000
  store i1 %t11, ptr %_5, align 1, !tbaa !9
  %t12 = load i1, ptr %_5, align 1, !tbaa !9
  br i1 %t12, label %bb2, label %bb3

bb2:
  %t13 = load i64, ptr %_4.i, align 8, !tbaa !9
  %t14 = call ptr @"av.main/tag"(i64 %t13)
  store ptr %t14, ptr %_3.last, align 8
  %t15 = load i64, ptr %_4.i, align 8, !tbaa !9
  %t16 = add i64 %t15, 1
  store i64 %t16, ptr %_4.i, align 8, !tbaa !9
  br label %bb1

bb3:
  %t17 = load ptr, ptr %_3.last, align 8, !tbaa !9
  %t18 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t17)
  %t19 = call ptr @"av.main/tag"(i64 3)
  store ptr %t19, ptr %_6, align 8
  %t20 = load ptr, ptr %_6, align 8, !tbaa !9
  %t21 = call i32 (ptr, ...) @printf(ptr @str.1, ptr %t20)
  store i32 0, ptr %_0, align 4, !tbaa !9
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
  %t23 = getelementptr inbounds i8, ptr %bump, i64 %total
  store ptr %t23, ptr @avant_gc_bump, align 8
  %t24 = zext i32 %ty to i64
  %t25 = shl i64 %t24, 32
  %t26 = or i64 %size, %t25
  %t27 = or i64 %t26, -9214364837600034816
  store i64 %t27, ptr %bump, align 8
  %t28 = getelementptr inbounds i8, ptr %bump, i64 8
  call void @llvm.memset.p0.i64(ptr %t28, i8 0, i64 %size, i1 false)
  ret ptr %t28
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
