; RUN: llc < %s -mtriple=nvptx -mcpu=sm_20 -fp-contract=fast | FileCheck %s
; RUN: llc < %s -mtriple=nvptx64 -mcpu=sm_20 -fp-contract=fast | FileCheck %s
; RUN: %if ptxas && !ptxas-12.0 %{ llc < %s -mtriple=nvptx -mcpu=sm_20 -fp-contract=fast | %ptxas-verify %}
; RUN: %if ptxas %{ llc < %s -mtriple=nvptx64 -mcpu=sm_20 -fp-contract=fast | %ptxas-verify %}

;; These tests should run for all targets

;;===-- Basic instruction selection tests ---------------------------------===;;


;;; f64

define double @fadd_f64(double %a, double %b) {
; CHECK: add.f64 %rd{{[0-9]+}}, %rd{{[0-9]+}}, %rd{{[0-9]+}}
; CHECK: ret
  %ret = fadd double %a, %b
  ret double %ret
}

define double @fsub_f64(double %a, double %b) {
; CHECK: sub.f64 %rd{{[0-9]+}}, %rd{{[0-9]+}}, %rd{{[0-9]+}}
; CHECK: ret
  %ret = fsub double %a, %b
  ret double %ret
}

define double @fmul_f64(double %a, double %b) {
; CHECK: mul.f64 %rd{{[0-9]+}}, %rd{{[0-9]+}}, %rd{{[0-9]+}}
; CHECK: ret
  %ret = fmul double %a, %b
  ret double %ret
}

define double @fdiv_f64(double %a, double %b) {
; CHECK: div.rn.f64 %rd{{[0-9]+}}, %rd{{[0-9]+}}, %rd{{[0-9]+}}
; CHECK: ret
  %ret = fdiv double %a, %b
  ret double %ret
}

;; PTX does not have a floating-point rem instruction


;;; f32

define float @fadd_f32(float %a, float %b) {
; CHECK: add.f32 %r{{[0-9]+}}, %r{{[0-9]+}}, %r{{[0-9]+}}
; CHECK: ret
  %ret = fadd float %a, %b
  ret float %ret
}

define float @fsub_f32(float %a, float %b) {
; CHECK: sub.f32 %r{{[0-9]+}}, %r{{[0-9]+}}, %r{{[0-9]+}}
; CHECK: ret
  %ret = fsub float %a, %b
  ret float %ret
}

define float @fmul_f32(float %a, float %b) {
; CHECK: mul.f32 %r{{[0-9]+}}, %r{{[0-9]+}}, %r{{[0-9]+}}
; CHECK: ret
  %ret = fmul float %a, %b
  ret float %ret
}

define float @fdiv_f32(float %a, float %b) {
; CHECK: div.rn.f32 %r{{[0-9]+}}, %r{{[0-9]+}}, %r{{[0-9]+}}
; CHECK: ret
  %ret = fdiv float %a, %b
  ret float %ret
}

;; PTX does not have a floating-point rem instruction
