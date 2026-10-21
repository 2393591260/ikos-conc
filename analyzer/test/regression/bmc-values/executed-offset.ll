target datalayout = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:32-n8:16:32-S128"
target triple = "i386-pc-linux-gnu"
declare i32 @pthread_create(i32*, i8*, i8* (i8*)*, i8*)
declare i32 @pthread_join(i32, i8**)
define i8* @reader(i8* %arg) {
entry:
  %base = bitcast i8* %arg to i32*
  ; ADDRESS
  %value = load volatile i32, i32* %base, align 4
  ret i8* null
}
define i32 @main() {
entry:
  %data = alloca [4 x i32], align 4
  %handle = alloca i32, align 4
  %arg = bitcast [4 x i32]* %data to i8*
  %first = getelementptr inbounds [4 x i32], [4 x i32]* %data, i32 0, i32 0
  store i32 0, i32* %first, align 4
  %created = call i32 @pthread_create(i32* %handle, i8* null, i8* (i8*)* @reader, i8* %arg)
  store volatile i32 1, i32* %first, align 4
  %thread = load i32, i32* %handle, align 4
  %joined = call i32 @pthread_join(i32 %thread, i8** null)
  ret i32 0
}
