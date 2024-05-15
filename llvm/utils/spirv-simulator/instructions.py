class Instruction:
  def __init__(self, line):
    self.line = line
    tokens = line.split()
    if len(tokens) > 1 and tokens[1] == '=':
      self.result = tokens[0]
      self.opcode = tokens[2]
      self.operands = tokens[3:] if len(tokens) > 2 else []
    else:
      self.result = None
      self.opcode = tokens[0]
      self.operands = tokens[1:] if len(tokens) > 1 else []

  def __str__(self):
    if self.result is None:
      return f"      {self.opcode} {self.operands}"
    return f"{self.result:3} = {self.opcode} {self.operands}"

  def staticExec(self, thread):
    pass

  def runtimeExec(self, program, thread):
    self._impl(program, thread)
    self._advanceIP(program, thread)

  def _impl(self, program, thread):
    raise RuntimeError(f"Unimplemented instruction {self}")

  def _advanceIP(self, program, thread):
    thread.setIP(thread.ip() + 1)

# Those are parsed, but never executed.
class OpEntryPoint(Instruction):
  pass

class OpFunction(Instruction):
  pass

class OpFunctionEnd(Instruction):
  pass

class OpLabel(Instruction):
  pass

class OpVariable(Instruction):
  pass

class OpName(Instruction):
  pass

# The only decoration we use if the BuilIn one to initialize the values.
class OpDecorate(Instruction):
  def staticExec(self, thread):
    if self.operands[1] == 'LinkageAttributes':
      return

    assert(self.operands[1] == "BuiltIn" and self.operands[2] == "SubgroupLocalInvocationId")
    thread.setRegister(self.operands[0], thread.tid())

# Constants

class OpConstant(Instruction):
  def staticExec(self, thread):
    thread.setRegister(self.result, int(self.operands[1]))

class OpConstantTrue(OpConstant):
  def staticExec(self, thread):
    thread.setRegister(self.result, True)

class OpConstantFalse(OpConstant):
  def staticExec(self, thread):
    print("A")
    thread.setRegister(self.result, False)

class OpConstantComposite(OpConstant):
  def staticExec(self, thread):
    result = []
    length = self.getRegister(self.operands[0])
    for op in self.operands[1:]:
      result.append(self.getRegister(op))
    thread.setRegister(self.result, result)

class OpConstantComposite(OpConstant):
  def staticExec(self, vm, state):
    output = []
    for op in self.operands[1:]:
      output.append(state.getRegister(op))
    state.setRegister(self.result, output)

# Control flow instructions

class OpFunctionCall(Instruction):
  def _impl(self, program, thread):
    pass

  def _advanceIP(self, program, thread):
    entry = program.getFunctionEntry(self.operands[1])
    thread.doCall(entry, self.result)

class OpReturn(Instruction):
  def _impl(self, program, thread):
    pass

  def _advanceIP(self, program, thread):
    thread.doReturn(None)

class OpReturnValue(Instruction):
  def _impl(self, program, thread):
    pass

  def _advanceIP(self, program, thread):
    thread.doReturn(thread.getRegister(self.operands[0]))

class OpBranch(Instruction):
  def _impl(self, program, thread):
    pass

  def _advanceIP(self, program, thread):
    thread.setIP(program.getBBEntry(self.operands[0]))
    pass

class OpBranchConditional(Instruction):
  def _impl(self, program, thread):
    pass

  def _advanceIP(self, program, thread):
    condition = thread.getRegister(self.operands[0])
    if condition:
      thread.setIP(program.getBBEntry(self.operands[1]))
    else:
      thread.setIP(program.getBBEntry(self.operands[2]))

class OpSwitch(Instruction):
  def _impl(self, program, thread):
    pass

  def _advanceIP(self, program, thread):
    value = thread.getRegister(self.operands[0])
    default_label = self.operands[1]
    i = 2
    while i < len(self.operands):
      imm = int(self.operands[i])
      label = self.operands[i + 1]
      if value == imm:
        thread.setIP(program.getBBEntry(label))
        return
      i += 2
    thread.setIP(program.getBBEntry(default_label))

class OpUnreachable(Instruction):
  def _impl(self, program, thread):
    raise RuntimeError("This instruction should never be executed.")

# Convergence instructions

class OpLoopMerge(Instruction):
  def getMergeLocation(self):
    return self.operands[0]

  def getContinueLocation(self):
    return self.operands[1]

  def _impl(self, program, thread):
    thread.handleConvergenceHeader(self)

class OpSelectionMerge(Instruction):
  def getMergeLocation(self):
    return self.operands[0]

  def getContinueLocation(self):
    return None

  def _impl(self, program, thread):
    thread.handleConvergenceHeader(self)

# Other instructions

class OpBitcast(Instruction):
  def _impl(self, program, thread):
    # TODO: find out the type from the defining instruction.
    # This can only work for DXC.
    if self.operands[0] == "%int":
      thread.setRegister(self.result, int(thread.getRegister(self.operands[1])))
    else:
      raise RuntimeError("Unsupported OpBitcast operand")

class OpAccessChain(Instruction):
  def _impl(self, program, thread):
    # Python dynamic types allows me to simplify. As long as the SPIR-V
    # is legal, this should be fine.
    # Note: SPIR-V structs are stored as tuples
    value = thread.getRegister(self.operands[1])
    for operand in self.operands[2:]:
      value = value[thread.getRegister(operand)]
    thread.setRegister(self.result, value)

class OpCompositeConstruct(Instruction):
  def _impl(self, program, thread):
    output = []
    for op in self.operands[1:]:
      output.append(thread.getRegister(op))
    thread.setRegister(self.result, output)

class OpStore(Instruction):
  def _impl(self, program, thread):
    thread.setRegister(self.operands[0], thread.getRegister(self.operands[1]))

class OpLoad(Instruction):
  def _impl(self, program, thread):
    thread.setRegister(self.result, thread.getRegister(self.operands[1]))

class OpIAdd(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS + RHS)

class OpISub(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS - RHS)

class OpIMul(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS * RHS)

class OpLogicalNot(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    thread.setRegister(self.result, not LHS)

class OpSGreaterThan(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS > RHS)

class OpSLessThan(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS < RHS)

class OpIEqual(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS == RHS)

class OpINotEqual(Instruction):
  def _impl(self, program, thread):
    LHS = thread.getRegister(self.operands[1])
    RHS = thread.getRegister(self.operands[2])
    thread.setRegister(self.result, LHS != RHS)

class OpPhi(Instruction):
  def _impl(self, program, thread):
    previousBBName = thread.getPreviousBBName()
    i = 1
    while i < len(self.operands):
      label = self.operands[i + 1]
      if label == previousBBName:
        thread.setRegister(self.result, thread.getRegister(self.operands[i]))
        return
      i += 2
    raise RuntimeError("previousBB not in the OpPhi operands")

class OpSelect(Instruction):
  def _impl(self, program, thread):
    condition = thread.getRegister(self.operands[1])
    value = thread.getRegister(self.operands[2 if condition else 3])
    thread.setRegister(self.result, value)

# Wave intrinsics

class OpGroupNonUniformBroadcastFirst(Instruction):
  def _impl(self, program, thread):
    assert(thread.getRegister(self.operands[1]) == 3)
    if thread.isFirstActiveThread():
      thread.broadcastRegister(self.result, thread.getRegister(self.operands[2]))

class OpGroupNonUniformElect(Instruction):
  def _impl(self, program, thread):
    thread.setRegister(self.result, thread.isFirstActiveThread())
