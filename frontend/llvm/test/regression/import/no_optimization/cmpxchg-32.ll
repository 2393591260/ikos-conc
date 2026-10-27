target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
target triple = "i386-pc-linux-gnu"

define { i32, i1 } @strong_sc(i32* %p, i32 %expected, i32 %desired) {
entry:
  %cx = cmpxchg  i32* %p, i32 %expected, i32 %desired seq_cst seq_cst, align 4
  ret { i32, i1 } %cx
}

define { i32, i1 } @weak_acqrel(i32* %p, i32 %expected, i32 %desired) {
entry:
  %cx = cmpxchg weak i32* %p, i32 %expected, i32 %desired acq_rel monotonic, align 4
  ret { i32, i1 } %cx
}

define { i32, i1 } @strong_release(i32* %p, i32 %expected, i32 %desired) {
entry:
  %cx = cmpxchg  i32* %p, i32 %expected, i32 %desired release monotonic, align 4
  ret { i32, i1 } %cx
}

define { i32, i1 } @strong_acquire(i32* %p, i32 %expected, i32 %desired) {
entry:
  %cx = cmpxchg  i32* %p, i32 %expected, i32 %desired acquire acquire, align 4
  ret { i32, i1 } %cx
}

define { i64, i1 } @weak_i64(i64* %p, i64 %expected, i64 %desired) {
entry:
  %cx = cmpxchg weak i64* %p, i64 %expected, i64 %desired seq_cst acquire, align 8
  ret { i64, i1 } %cx
}

define { i32, i1 } @volatile_weak(i32* %p, i32 %expected, i32 %desired) {
entry:
  %cx = cmpxchg weak volatile i32* %p, i32 %expected, i32 %desired seq_cst monotonic, align 4
  ret { i32, i1 } %cx
}

define { i8*, i1 } @strong_pointer(i8** %p, i8* %expected, i8* %desired) {
entry:
  %cx = cmpxchg i8** %p, i8* %expected, i8* %desired seq_cst acquire, align 4
  ret { i8*, i1 } %cx
}

; Outcome checks: a failed comparison never modifies memory; weak failure
; also remains feasible for equal observed and expected values.
; CHECK-LABEL: @strong_acquire(
; CHECK: #1 predecessors=
; CHECK: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sieq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store %p, %desired, align 4
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sine %expected
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
; CHECK-LABEL: @strong_pointer(
; CHECK: #1 predecessors=
; CHECK: si8* %1 = load %p, align 4
; CHECK-NEXT: %1 peq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store %p, %desired, align 4
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si8* %1 = load %p, align 4
; CHECK-NEXT: %1 pne %expected
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
; CHECK-LABEL: @strong_release(
; CHECK: #1 predecessors=
; CHECK: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sieq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store %p, %desired, align 4
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sine %expected
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
; CHECK-LABEL: @strong_sc(
; CHECK: #1 predecessors=
; CHECK: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sieq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store %p, %desired, align 4
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sine %expected
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
; CHECK-LABEL: @volatile_weak(
; CHECK: #1 predecessors=
; CHECK: si32 %1 = load volatile %p, align 4
; CHECK-NEXT: %1 sieq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store volatile %p, %desired, align 4
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si32 %1 = load volatile %p, align 4
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
; CHECK-LABEL: @weak_acqrel(
; CHECK: #1 predecessors=
; CHECK: si32 %1 = load %p, align 4
; CHECK-NEXT: %1 sieq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store %p, %desired, align 4
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si32 %1 = load %p, align 4
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
; CHECK-LABEL: @weak_i64(
; CHECK: #1 predecessors=
; CHECK: si64 %1 = load %p, align 8
; CHECK-NEXT: %1 sieq %expected
; CHECK-NEXT: si1 %2 = -1
; CHECK-NEXT: store %p, %desired, align 8
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK: #2 predecessors=
; CHECK-NEXT: si64 %1 = load %p, align 8
; CHECK-NEXT: si1 %2 = 0
; CHECK-NEXT: {{.*}} = insertelement undef, 0, %1
; CHECK-NEXT: {{.*}} = insertelement {{.*}}, {{[48]}}, %2
; CHECK-NOT: store
; CHECK: return %cx
