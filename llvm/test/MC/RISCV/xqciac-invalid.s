# Xqciac - Qualcomm uC Load-Store Address Calculation Extension
# RUN: not llvm-mc -triple riscv32 -mattr=+experimental-xqciac < %s 2>&1 \
# RUN:     | FileCheck -check-prefixes=CHECK,CHECK-IMM %s
# RUN: not llvm-mc -triple riscv32 -mattr=-experimental-xqciac < %s 2>&1 \
# RUN:     | FileCheck -check-prefixes=CHECK,CHECK-EXT %s

# CHECK-PLUS: :[[@LINE+2]]:14: error: register must be a GPR excluding zero (x0)
# CHECK-MINUS: :[[@LINE+1]]:14: error: invalid operand for instruction
qc.c.muliadd x5, x10, 4

# CHECK: :[[@LINE+1]]:1: error: too few operands for instruction
qc.c.muliadd x15

# CHECK-IMM: :[[@LINE+1]]:24: error: immediate must be an integer in the range [0, 31]
qc.c.muliadd x10, x15, 32

# CHECK-EXT: :[[@LINE+1]]:1: error: instruction requires the following: 'Xqciac' (Qualcomm uC Load-Store Address Calculation Extension)
qc.c.muliadd x10, x15, 20


# CHECK-PLUS: :[[@LINE+2]]:12: error: register must be a GPR excluding zero (x0)
# CHECK-MINUS: :[[@LINE+1]]:12: error: invalid operand for instruction
qc.muliadd x0, x10, 1048577

# CHECK: :[[@LINE+1]]:1: error: too few operands for instruction
qc.muliadd x10

# CHECK-IMM: :[[@LINE+1]]:22: error: operand must be a symbol with %lo/%pcrel_lo/%tprel_lo specifier or an integer in the range [-2048, 2047]
qc.muliadd x10, x15, 8589934592

# CHECK-EXT: :[[@LINE+1]]:1: error: instruction requires the following: 'Xqciac' (Qualcomm uC Load-Store Address Calculation Extension)
qc.muliadd x10, x15, 577


# CHECK-PLUS: :[[@LINE+2]]:11: error: register must be a GPR excluding zero (x0)
# CHECK-MINUS: :[[@LINE+1]]:11: error: invalid operand for instruction
qc.shladd 0, x10, 1048577

# CHECK: :[[@LINE+1]]:1: error: too few operands for instruction
qc.shladd x10

# CHECK-IMM: :[[@LINE+1]]:26: error: immediate must be an integer in the range [4, 31]
qc.shladd x10, x15, x11, 2

# CHECK-EXT: :[[@LINE+1]]:1: error: instruction requires the following: 'Xqciac' (Qualcomm uC Load-Store Address Calculation Extension)
qc.shladd x10, x15, x11, 5
