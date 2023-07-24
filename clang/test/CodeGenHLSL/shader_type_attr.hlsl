// RUN: %clang_cc1 -std=hlsl2021 -finclude-default-header -x hlsl -triple \
// RUN:   dxil-pc-shadermodel6.3-library %s -fnative-half-type \
// RUN:   -emit-llvm -disable-llvm-passes -o - \
// RUN:   | FileCheck %s --check-prefixes=DXIL-CHECK

// RUN: %clang_cc1 -std=hlsl2021 -finclude-default-header -x hlsl -triple \
// RUN:   spirv-unknown-shadermodel6.3-compute %s \
// RUN:   -emit-llvm -disable-llvm-passes -o - \
// RUN:   | FileCheck %s --check-prefixes=SPIRV-CHECK

// Make sure not mangle entry.
// DXIL-CHECK:define void @foo()
// Make sure add function attribute.
// DXIL-CHECK:"hlsl.shader"="compute"

// Make sure not mangle entry and workgroup attribute.
// SPIRV-CHECK: define void @foo() #[[ATTR:[0-9]+]] !reqd_work_group_size !{{[0-9]+}}
// SPIRV-CHECK: attributes #[[ATTR]] = { "hlsl.shader"="compute" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }

[shader("compute")]
[numthreads(1,1,1)]
void foo() {

}
