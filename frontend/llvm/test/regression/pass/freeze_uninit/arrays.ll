; Physical seeds do not invent a cell for zero-count allocas.
target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
target triple = "i386-pc-linux-gnu"
@tail = global i32 0
declare void @observe32(i32*)
declare void @observe64(i64*)

define void @zero32() {
; CHECK-LABEL: define void @zero32()
; CHECK-NOT: @__ikos_nondet
; CHECK-NOT: store
; CHECK: call void @observe32
; CHECK: ret void
  %p = alloca i32, i32 0
  call void @observe32(i32* %p)
  ret void
}
define void @zero64() {
; CHECK-LABEL: define void @zero64()
; CHECK-NOT: @__ikos_nondet
; CHECK-NOT: store
; CHECK: call void @observe64
; CHECK: ret void
  %p = alloca i64, i32 0
  call void @observe64(i64* %p)
  ret void
}
define void @one32() {
; CHECK-LABEL: define void @one32()
; CHECK: [[N:%.*]] = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK-NEXT: store i32 [[N]], i32* %p, {{.*}}!ikos.freeze_uninit
; CHECK-NEXT: call void @observe32
  %p = alloca i32
  call void @observe32(i32* %p)
  ret void
}
define void @many64() {
; CHECK-LABEL: define void @many64()
; CHECK: [[N:%.*]] = call i64 @__ikos_nondet_int64(), !ikos.freeze_uninit
; CHECK-NEXT: store i64 [[N]], i64* %p, {{.*}}!ikos.freeze_uninit
; CHECK-NEXT: call void @observe64
  %p = alloca i64, i32 3
  call void @observe64(i64* %p)
  ret void
}
define void @dynamic32(i32 %n) {
; CHECK-LABEL: define void @dynamic32(i32 %n)
; CHECK: %p = alloca i32, i32 %n
; CHECK-NEXT: [[C:%.*]] = icmp ne i32 %n, 0
; CHECK-NEXT: br i1 [[C]], label %[[YES:.*]], label %[[TAIL:.*]]
; CHECK: [[YES]]:
; CHECK-NEXT: [[N:%.*]] = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK-NEXT: store i32 [[N]], i32* %p, {{.*}}!ikos.freeze_uninit
; CHECK-NEXT: br label %[[TAIL]]
; CHECK: [[TAIL]]:
; CHECK-NEXT: call void @observe32
; CHECK-NEXT: store i32 5, i32* @tail
  %p = alloca i32, i32 %n
  call void @observe32(i32* %p)
  store i32 5, i32* @tail
  ret void
}
define void @dynamic64(i32 %n) {
; CHECK-LABEL: define void @dynamic64(i32 %n)
; CHECK: [[C:%.*]] = icmp ne i32 %n, 0
; CHECK-NEXT: br i1 [[C]], label %[[YES:.*]], label %[[TAIL:.*]]
; CHECK: [[YES]]:
; CHECK-NEXT: [[N:%.*]] = call i64 @__ikos_nondet_int64(), !ikos.freeze_uninit
; CHECK-NEXT: store i64 [[N]], i64* %p, {{.*}}!ikos.freeze_uninit
; CHECK-NEXT: br label %[[TAIL]]
; CHECK: [[TAIL]]:
; CHECK-NEXT: call void @observe64
  %p = alloca i64, i32 %n
  call void @observe64(i64* %p)
  ret void
}
define void @wide_count(i64 %n) {
; CHECK-LABEL: define void @wide_count(i64 %n)
; CHECK: icmp ne i64 %n, 0
; CHECK: call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
  %p = alloca i32, i64 %n
  call void @observe32(i32* %p)
  ret void
}
define void @multiple(i32 %n, i32 %m) {
; CHECK-LABEL: define void @multiple
; CHECK: icmp ne i32 %n, 0
; CHECK: call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK: %scalar = alloca i64
; CHECK-NEXT: [[N:%.*]] = call i64 @__ikos_nondet_int64(), !ikos.freeze_uninit
; CHECK-NEXT: store i64 [[N]], i64* %scalar, {{.*}}!ikos.freeze_uninit
; CHECK: icmp ne i32 %m, 0
; CHECK: call i64 @__ikos_nondet_int64(), !ikos.freeze_uninit
; CHECK: call void @observe32
; CHECK: call void @observe64
  %p = alloca i32, i32 %n
  %scalar = alloca i64
  %q = alloca i64, i32 %m
  call void @observe32(i32* %p)
  call void @observe64(i64* %q)
  call void @observe64(i64* %scalar)
  ret void
}
define void @loop(i32 %n, i32 %end) {
; CHECK-LABEL: define void @loop
; CHECK: phi i32
; CHECK: icmp ne i32 %n, 0
; CHECK: call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK: call void @observe32
; CHECK: br i1 %continue, label %body, label %exit
entry:
  br label %body
body:
  %i = phi i32 [0, %entry], [%inc, %body]
  %p = alloca i32, i32 %n
  call void @observe32(i32* %p)
  %inc = add i32 %i, 1
  %continue = icmp slt i32 %inc, %end
  br i1 %continue, label %body, label %exit
exit:
  ret void
}
define i32 @successor_phi(i1 %take, i32 %n) {
; CHECK-LABEL: define i32 @successor_phi
; CHECK: icmp ne i32 %n, 0
; CHECK: call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK: phi i32
; CHECK: ret i32 %v
entry:
  br i1 %take, label %left, label %right
left:
  %p = alloca i32, i32 %n
  call void @observe32(i32* %p)
  br label %join
right:
  br label %join
join:
  %v = phi i32 [7, %left], [9, %right]
  ret i32 %v
}

define void @high_bit_count() {
; CHECK-LABEL: define void @high_bit_count()
; CHECK: [[N:%.*]] = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK-NEXT: store i32 [[N]], i32* %p, {{.*}}!ikos.freeze_uninit
; CHECK-NEXT: call void @observe32
  %p = alloca i32, i32 2147483648
  call void @observe32(i32* %p)
  ret void
}
define void @wide_nonzero_count() {
; CHECK-LABEL: define void @wide_nonzero_count()
; CHECK: [[N:%.*]] = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit
; CHECK-NEXT: store i32 [[N]], i32* %p, {{.*}}!ikos.freeze_uninit
; CHECK-NEXT: call void @observe32
  %p = alloca i32, i64 4294967296
  call void @observe32(i32* %p)
  ret void
}
