; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -verify-machineinstrs -debug-entry-values < %s | FileCheck %s
; Check that we can lower a use of an alloca both as a deopt value (where the
; exact meaning is up to the consumer of the stackmap) and as an explicit spill
; slot used for GC.

target datalayout = "e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

declare zeroext i1 @return_i1()

; Can we handle an explicit relocation slot (in the form of an alloca) given
; to the statepoint?
define ptr addrspace(1) @test(ptr addrspace(1) %ptr) gc "statepoint-example" {
; CHECK-LABEL: test:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    movq %rdi, (%rsp)
; CHECK-NEXT:    callq return_i1@PLT
; CHECK-NEXT:  .Ltmp0:
; CHECK-NEXT:    movq (%rsp), %rax
; CHECK-NEXT:    popq %rcx
; CHECK-NEXT:    .cfi_def_cfa_offset 8
; CHECK-NEXT:    retq
entry:
  %alloca = alloca ptr addrspace(1), align 8
  store ptr addrspace(1) %ptr, ptr %alloca
  call token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 0, ptr elementtype(i1 ()) @return_i1, i32 0, i32 0, i32 0, i32 0) ["gc-live" (ptr %alloca)]
  %rel = load ptr addrspace(1), ptr %alloca
  ret ptr addrspace(1) %rel
}

; Can we handle an alloca as a deopt value?
define ptr addrspace(1) @test2(ptr addrspace(1) %ptr) gc "statepoint-example" {
; CHECK-LABEL: test2:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    movq %rdi, (%rsp)
; CHECK-NEXT:    callq return_i1@PLT
; CHECK-NEXT:  .Ltmp1:
; CHECK-NEXT:    xorl %eax, %eax
; CHECK-NEXT:    popq %rcx
; CHECK-NEXT:    .cfi_def_cfa_offset 8
; CHECK-NEXT:    retq
entry:
  %alloca = alloca ptr addrspace(1), align 8
  store ptr addrspace(1) %ptr, ptr %alloca
  call token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 0, ptr elementtype(i1 ()) @return_i1, i32 0, i32 0, i32 0, i32 0) ["deopt" (ptr %alloca)]
  ret ptr addrspace(1) null
}

declare token @llvm.experimental.gc.statepoint.p0(i64, i32, ptr, i32, i32, ...)


; CHECK-LABEL: .section .llvm_stackmaps
; CHECK-NEXT:  __LLVM_StackMaps:
; Header
; CHECK-NEXT:   .byte 3
; CHECK-NEXT:   .byte 0
; CHECK-NEXT:   .short 0
; Num Functions
; CHECK-NEXT:   .long 2
; Num LargeConstants
; CHECK-NEXT:   .long 0
; Num Callsites
; CHECK-NEXT:   .long 2

; Functions and stack size
; CHECK-NEXT:   .quad test
; CHECK-NEXT:   .quad 8
; CHECK-NEXT:   .quad 1
; CHECK-NEXT:   .quad test2
; CHECK-NEXT:   .quad 8
; CHECK-NEXT:   .quad 1

; Large Constants
; Statepoint ID only
; CHECK: .quad	0

; Callsites
; The GC one
; CHECK: .long	.Ltmp0-test
; CHECK: .short	0
; CHECK: .short	2
; LiveVar[0]
; CHECK: .byte	3
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .long	0
; LiveVar[1]
; CHECK: .byte	1
; Direct Spill Slot [rsp+0]
; CHECK: .byte	2
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	7
; CHECK: .short	0
; CHECK: .long	0
; No Padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .p2align	3

; The Deopt one
; CHECK: .long	.Ltmp1-test2
; CHECK: .short	0
; CHECK: .short	2
; LiveVar[0]
; CHECK: .byte	3
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (1)
; CHECK: .byte	4
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .long	1
; LiveVar[1]
; CHECK: .byte	1
; Direct Spill Slot [rsp+0]
; CHECK: .byte	2
; CHECK: .byte	0
; CHECK: .short 8
; CHECK: .short	7
; CHECK: .short	0
; CHECK: .long	0

; No Padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .p2align	3
