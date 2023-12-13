; TODO:  cd ./ykllvm/ && ./build/bin/llvm-lit -v llvm/test/Transforms/Yk/SoftwareTracing.ll
; ./bin/opt -passes=software-tracer /home/pd/ykjit/2023-12-01/ykllvm-fork/llvm/test/Transforms/Yk/SoftwareTracer.ll
; RUN: opt --yk-software-tracer -o - < %s | FileCheck %s
; RUN: opt -o - < %s | FileCheck --check-prefix CHECK-NOPASS %s

define void @bar() {
  entry:
    ; CHECK: call void @YK_TRACE_FUNCTION()
    ret void
}
