; ModuleID = 'compute2.hlsl'
source_filename = "compute2.hlsl"
target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024"
target triple = "spirv1.5-unknown-shadermodel6.3-compute"

%struct = type {
  float,
  float,
  float
}

; %MyCBuffer = type { %struct, %struct }
%MyCBuffer = type { %struct, %struct }
@MyCBuffer = external addrspace(2) constant %MyCBuffer

; Function Attrs: norecurse
define void @main.1() #1 {
entry:
  %0 = alloca { float, float }, align 4

  ; %in0 = getelementptr %MyCBuffer, ptr addrspace(2) @MyCBuffer, i32 0, i32 0, i32 1, align 4
  ; %in1 = getelementptr %MyCBuffer, ptr addrspace(2) @MyCBuffer, i32 0, i32 1, i32 2, align 4
  %in0 = getelementptr inbounds %MyCBuffer, ptr addrspace(2) @MyCBuffer, i32 0, i32 0, i32 0
  %in1 = getelementptr inbounds %MyCBuffer, ptr addrspace(2) @MyCBuffer, i32 0, i32 1, i32 2

  %out0 = getelementptr inbounds { float, float }, ptr %0, i32 0, i32 0
  %out1 = getelementptr inbounds { float, float }, ptr %0, i32 0, i32 1

  %1 = load float, ptr addrspace(2) %in0, align 4
  %2 = load float, ptr addrspace(2) %in1, align 4

  store float %1, ptr %out0, align 4
  store float %2, ptr %out1, align 4

  ret void
}

attributes #0 = { noinline norecurse nounwind optnone "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { norecurse "hlsl.shader"="compute" "hlsl.numthreads"="4,8,16" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }

!llvm.module.flags = !{!0, !1}
!llvm.ident = !{!2}

!hlsl.cbufs = !{!1}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 4, !"dx.disable_optimizations", i32 1}
!2 = !{!"clang version 18.0.0 (/usr/local/google/home/nathangauer/projects/llvm-project/clang 5f7260cdbfe7edc30e8b02ddb4424f282fbf7d73)"}
; !3 = !{ptr addrspace(2) @A.cb., !"A.cb.ty", i32 13, i32 2, i32 1}
