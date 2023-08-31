; ModuleID = 'compute4.hlsl'
source_filename = "compute4.hlsl"
target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024"
target triple = "spirv1.5-unknown-shadermodel6.3-compute"

; Function Attrs: norecurse
define void @main.1() #1 {
entry:
  %addr = alloca i64, align 8
  %o = alloca i8, align 1

  store i64 16045690984503115501, ptr %addr, align 8
  %0 = load i64, ptr %addr, align 8
  %1 = inttoptr i64 %0 to ptr
  %2 = load i8, ptr %1, align 1
  store i8 %2, ptr %o, align 1

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
