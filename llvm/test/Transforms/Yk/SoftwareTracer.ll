; TODO:  ./build/bin/llvm-lit -v llvm/test/Transforms/Yk/SoftwareTracing.ll
; Checks that the yk-software-tracer pass injects tracing call at the begining of each basic block
;
; RUN: llc --yk-software-tracer -o - < %s | FileCheck %s
; RUN: llc -o - < %s | FileCheck --check-prefix CHECK-NOPASS %s

; CHECK: call void @yk_trace_basic_block()
define internal void @myfunc() noinline {
	ret void
}
