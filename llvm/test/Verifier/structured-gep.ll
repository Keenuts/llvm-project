; RUN: opt -S -passes=verify %s

%struct.test = type { <1 x double>, <1 x double> }

declare ptr @llvm.structured.gep.inbounds(%struct.test, ptr, ...)
declare ptr addrspace(1) @llvm.structured.gep(%struct.test, ptr addrspace(1), ...)

define void @foo(ptr %p1, ptr addrspace(1) %p2) {
  %c = call ptr (%struct.test, ptr, ...) @llvm.structured.gep.inbounds(%struct.test poison, ptr %p1, i32 1)

  %b = call ptr addrspace(1) (%struct.test, ptr addrspace(1), ...) @llvm.structured.gep(%struct.test poison, ptr addrspace(1) %p2, i32 1)
  ret void
}

