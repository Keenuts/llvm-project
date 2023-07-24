; RUN: llc -O0 -mtriple=spirv-unknown-unknown %s -o - | FileCheck %s

; CHECK-DAG: OpEntryPoint GLCompute %[[#entry:]] "main"
; CHECK-DAG: OpExecutionMode %[[#entry]] LocalSize 4 8 16

define void @main() #0 !reqd_work_group_size !3 {
entry:
  ret void
}

attributes #0 = { norecurse "hlsl.shader"="compute" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
!3 = !{i32 4, i32 8, i32 16}
