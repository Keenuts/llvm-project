// Test device for mapping codegen.
///==========================================================================///

// RUN: %clang_cc1 -DCK1 -verify -fopenmp -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -o - 2>&1 | FileCheck -check-prefix=CK1 %s

// RUN: %clang_cc1 -DCK1 -verify -fopenmp-simd -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -o - 2>&1 | FileCheck --check-prefix SIMD-ONLY0 %s
// SIMD-ONLY0-NOT: {{__kmpc|__tgt}}

// RUN: %clang_cc1 -DCK1 -verify -fopenmp -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc
// RUN: %clang_cc1 -DCK1 -verify -fopenmp -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - | FileCheck %s --check-prefix CK1-DEVICE

// RUN: %clang_cc1 -DCK1 -verify -fopenmp-simd -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc
// RUN: %clang_cc1 -DCK1 -verify -fopenmp-simd -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - | FileCheck --check-prefix SIMD-ONLY1 %s
// SIMD-ONLY1-NOT: {{__kmpc|__tgt}}

// expected-no-diagnostics

#ifdef CK1

void target_maps_parallel_integer(int a){
  int ParamToKernel = a;
#pragma omp target map(tofrom: ParamToKernel)
  {
    ParamToKernel += 1;
  }
}

// CK1-DEVICE: {{.*}}void @__omp_offloading_{{.*}}(ptr {{[^,]+}}, ptr noundef nonnull align 4 dereferenceable(4){{.*}}

// CK1: {{.*}}void {{.*}}target_maps_parallel_integer{{.*}} {

// CK1: [[GEPOBP:%.+]] = getelementptr inbounds {{.*}}
// CK1: store ptr %ParamToKernel, ptr [[GEPOBP]]
// CK1: [[GEPOP:%.+]] = getelementptr inbounds {{.*}}
// CK1: store ptr %ParamToKernel, ptr [[GEPOP]]
// CK1: [[GEPOBPARG:%.+]] = getelementptr inbounds {{.*}} %.offload_baseptrs, i32 0, i32 0
// CK1: [[GEPOPARG:%.+]] = getelementptr inbounds {{.*}} %.offload_ptrs, i32 0, i32 0
// CK1: [[ARGBP:%.+]] = getelementptr inbounds nuw %struct.__tgt_kernel_arguments, ptr %kernel_args, i32 0, i32 2
// CK1: store ptr [[GEPOBPARG]], ptr [[ARGBP]], align 8
// CK1: [[ARGP:%.+]] = getelementptr inbounds nuw %struct.__tgt_kernel_arguments, ptr %kernel_args, i32 0, i32 3
// CK1: store ptr [[GEPOPARG]], ptr [[ARGP]], align 8
// CK1: call {{.*}}tgt_target_kernel({{.*}})

#endif
