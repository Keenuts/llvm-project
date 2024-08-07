; RUN: opt %loadNPMPolly -polly-parallel -polly-parallel-force '-passes=print<polly-ast>' -disable-output < %s | FileCheck %s -check-prefix=AST
; RUN: opt %loadNPMPolly -polly-parallel -polly-parallel-force -passes=polly-codegen -S < %s | FileCheck %s -check-prefix=IR

; Make sure we correctly forward the reference to 'A' to the OpenMP subfunction.
;
; void loop_references_outer_ids(float *A) {
;   for (long i = 0; i < 100; i++)
;     A[i] = i;
; }


; AST: #pragma simd
; AST: #pragma omp parallel for
; AST: for (int c0 = 0; c0 <= 99; c0 += 1)
; AST:   Stmt_for_body(c0);

; IR-LABEL: polly.parallel.for:
; IR-NEXT:  %polly.subfn.storeaddr.A = getelementptr inbounds nuw { ptr }, ptr %polly.par.userContext, i32 0, i32 0
; IR-NEXT:  store ptr %A, ptr %polly.subfn.storeaddr.A

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @loop_references_outer_ids(ptr %A) {
entry:
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %i.0 = phi i64 [ 0, %entry ], [ %inc, %for.inc ]
  %exitcond = icmp ne i64 %i.0, 100
  br i1 %exitcond, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %conv = sitofp i64 %i.0 to float
  %arrayidx = getelementptr inbounds float, ptr %A, i64 %i.0
  store float %conv, ptr %arrayidx, align 4
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %inc = add nsw i64 %i.0, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}
