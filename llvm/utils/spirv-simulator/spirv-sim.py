#!/usr/bin/env python3

import fileinput
import inspect
from dataclasses import dataclass
import sys
from instructions import *
import argparse

def parseInstruction(i):
  IGNORED = set([ 'OpCapability', 'OpMemoryModel', 'OpExecutionMode', 'OpExtension',
                  'OpSource', 'OpTypeInt', 'OpTypeFloat',
                  'OpTypeBool', 'OpTypeVoid', 'OpTypeFunction', 'OpTypePointer', 'OpTypeArray' ])
  if i.opcode in IGNORED:
    return None

  try:
    Type = getattr(sys.modules['instructions'], i.opcode)
  except AttributeError:
    raise RuntimeError(f"Unsupported instruction {i}")
  if not inspect.isclass(Type):
    raise RuntimeError(f"{i} instruction definition is not a class. Did you used 'def' instead of 'class'?")
  return Type(i.line)

def splitInstructions(splitType, instructions):
  blocks = [ [] ]
  for instruction in instructions:
    if type(instruction) is splitType and len(blocks[-1]) > 0:
      blocks.append([])
    blocks[-1].append(instruction)
  return blocks

class BasicBlock:
  def __init__(self, instructions):
    assert(type(instructions[0]) is OpLabel)
    self._name = instructions[0].result
    self._instructions = instructions[1:]

  def name(self):
    return self._name

  def __getitem__(self, index : int) -> Instruction:
    return self._instructions[index]

  def __len__(self):
    return len(self._instructions)

  def dump(self):
    print(f"        {self._name}:")
    for instruction in self._instructions:
      print(f"        {instruction}")

class Function:
  def __init__(self, instructions):
    assert(type(instructions[0]) is OpFunction)
    self._name = instructions[0].result
    self._basic_blocks = []
    self._variables = [ x for x in instructions if type(x) is OpVariable ]

    assert(type(instructions[0]) is OpFunction)
    assert(type(instructions[-1]) is OpFunctionEnd)
    body = filter(lambda x: type(x) != OpVariable, instructions[1:-1])

    for block in splitInstructions(OpLabel, body):
      self._basic_blocks.append(BasicBlock(block))

  def name(self):
    return self._name

  def __getitem__(self, index : int) -> BasicBlock:
    return self._basic_blocks[index]

  def bb(self, index : int) -> BasicBlock:
    return self._basic_blocks[index]

  def getBasicBlockIndex(self, name):
    for i in range(len(self._basic_blocks)):
      if self._basic_blocks[i].name() == name:
        return i
    return None

  def dump(self):
    print("      Variables:")
    for var in self._variables:
      print(f"        {var}")
    print("      Blocks:")
    for bb in self._basic_blocks:
      bb.dump()

@dataclass
class InstructionPointer:
  function : Function
  basic_block : int
  instruction_index : int

  def __str__(self):
    bb = self.function[self.basic_block]
    i = bb[self.instruction_index]
    return f"{bb.name()}:{self.instruction_index} in {self.function.name()} | {i}"

  def __hash__(self):
    return hash((self.function.name(), self.basic_block, self.instruction_index))

  def bb(self) -> BasicBlock:
    return self.function[self.basic_block]

  def instruction(self):
    return self.function[self.basic_block][self.instruction_index]

  def runtimeExec(self, program, thread):
    self.function[self.basic_block][self.instruction_index].runtimeExec(program, thread)

  def __add__(self, value : int):
    bb = self.function[self.basic_block]
    assert(len(bb) > self.instruction_index + value)
    return InstructionPointer(self.function, self.basic_block, self.instruction_index + 1)


class Program:
  def __init__(self, instructions):

    chunks = splitInstructions(OpFunction, instructions)
    self._prolog = chunks[0]
    self._functions = {}
    self._globals = [ x for x in instructions if type(x) is OpVariable or issubclass(type(x), OpConstant) ]

    self._name2reg = {}
    self._reg2name = {}
    for instruction in instructions:
      if type(instruction) is OpName:
        name = instruction.operands[1][1:-1]
        reg = instruction.operands[0]
        self._name2reg[name] = reg
        self._reg2name[reg] = name

    for chunk in chunks[1:]:
      function = Function(chunk)
      assert(function.name() not in self._functions)
      self._functions[function.name()] = function

    self._entrypoint = None
    for instruction in self._prolog:
      if type(instruction) is OpEntryPoint:
        name = instruction.operands[1]
        self._entrypoint = self.getFunctionEntry(name)
    assert(self._entrypoint is not None)

  def entrypoint(self):
    return self._entrypoint

  def getRegisterFromName(self, name):
    if name in self._name2reg:
      return self._name2reg[name]
    return None

  def getNameFromRegister(self, register):
    if register in self._reg2name:
      return self._reg2name[register]
    return None

  def initialize(self, thread):
    for instruction in self._globals:
      instruction.staticExec(thread)

    # Initialize builtins
    for instruction in self._prolog:
      if type(instruction) is OpDecorate:
        instruction.staticExec(thread)

  def runInstruction(self, thread, ip):
    ip.runtimeExec(self, thread)

  def getFunctionEntry(self, name : str) -> InstructionPointer:
    return InstructionPointer(self._functions[name], 0, 0)

  def getBBEntry(self, basic_block_name : str) -> InstructionPointer:
    for name, function in self._functions.items():
      index = function.getBasicBlockIndex(basic_block_name)
      if index != None:
        return InstructionPointer(function, index, 0)
    raise RuntimeError(f"Instruction defining {basic_block_name} not found.")

  def dump(self, function_name : str = None):
    print("Program:")
    print("  globals:")
    for instruction in self._globals:
      print(f"    {instruction}")
    print(f"  entrypoint: {self._entrypoint}")

    if function_name is None:
      print("  functions:")
      for register, function in self._functions.items():
        name = self.getNameFromRegister(register)
        print(f"  Function {register} ({name})")
        function.dump()
      return

    register = self.getRegisterFromName(function_name)
    print(f"  function {register} ({function_name}):")
    self._functions[register].dump()

  def variables(self) -> iter:
    return [ x.result for x in self._globals ]

class Thread:
  def __init__(self, wave, tid):
    self._registers = {}
    self._ip = None
    self._running = True
    self._wave = wave
    self._stack = []
    self._tid = tid
    self._previous_bb = None
    self._current_bb = None

  def tid(self):
    return self._tid

  def isFirstActiveThread(self):
    return self._tid == self._wave.getFirstActiveThreadIndex()

  def broadcastRegister(self, register, value):
    self._wave.broadcastRegister(register, value)

  def ip(self):
    return self._ip

  def running(self):
    return self._running

  def setRegister(self, name, value):
    self._registers[name] = value

  def getRegister(self, name, allow_undef = False):
    if allow_undef and name not in self._registers:
      return None
    return self._registers[name]

  def setIP(self, ip):
    if ip.bb() != self._current_bb:
      self._previous_bb = self._current_bb
      self._current_bb = ip.bb()
    self._ip = ip

  def getPreviousBBName(self):
    return self._previous_bb.name()

  def handleConvergenceHeader(self, instruction):
    self._wave.handleConvergenceHeader(self, instruction)

  def doCall(self, ip, output_register):
    return_ip = None if self._ip is None else self._ip + 1
    self._stack.append((return_ip, lambda value: self.setRegister(output_register, value)));
    self.setIP(ip)

  def doReturn(self, value):
    ip, callback = self._stack[-1]
    self._stack.pop()

    callback(value)
    if len(self._stack) == 0:
      self._running = False
    else:
      self.setIP(ip);

class Group:
  def __init__(self, threads):
    self._threads = threads

@dataclass
class ConvergenceRequirement:
  mergeTarget : InstructionPointer
  continueTarget : InstructionPointer
  impactedLanes : set[int]

class Wave:
  def __init__(self, program, wave_size : int):
    assert(wave_size > 0)

    self._program = program

    self._threads = []
    for i in range(wave_size):
      self._threads.append(Thread(self, i))

    self._tasks = {}
    self._convergence_requirements = []
    self._active_thread_indices = set()

  def _isTaskCandidate(self, ip, threads):
    #print(f"   candidate: {[ x.tid() for x in threads ]} | {ip.instruction()}")
    merged_threads = set()
    for thread in self._threads:
      if not thread.running():
        merged_threads.add(thread)

    for requirement in self._convergence_requirements:
      #print(f"       requirement:")
      #print(f"         - threads: {requirement.impactedLanes}")
      #print(f"         - merge: {requirement.mergeTarget.instruction()}")

      # This task is not executing a merge or continue target.
      # Adding all threads at those points into the ignore list.
      if requirement.mergeTarget != ip and requirement.continueTarget != ip:
        for tid in requirement.impactedLanes:
          if self._threads[tid].ip() == requirement.mergeTarget:
            merged_threads.add(tid)
          if self._threads[tid].ip() == requirement.continueTarget:
            merged_threads.add(tid)
        continue

      #print(f"          merged threads: {merged_threads}")
      #print(f"          requirement is a merge target")
      # This task is executing the current requirement continue/merge
      # target.
      for tid in requirement.impactedLanes:
        #print(f"           checking thread {thread.tid()}... ", end="")
        thread = self._threads[tid]
        if not thread.running():
          #print("Dead. Ignoring")
          continue

        if thread.tid() in merged_threads:
          #print("merged")
          continue

        if ip == requirement.mergeTarget:
          #print(f"task is merge target... thread.ip() = {thread.ip().instruction()}")
          if thread.ip() != requirement.mergeTarget:
            #print("blocked because thread is not yet at the merge")
            return False
        else:
          if thread.ip() != requirement.mergeTarget and \
             thread.ip() != requirement.continueTarget:
            #print("blocked because thread not yet at the continue nor merge")
            return False
        #print("fine")

    #print("    Elected")
    return True

  def _getNextRunnableTask(self):
    candidate = None
    for ip, threads in self._tasks.items():
      if len(threads) == 0:
        continue
      if self._isTaskCandidate(ip, threads):
        candidate = ip
        break

    if candidate:
      threads = self._tasks[candidate]
      del self._tasks[ip]
      return (candidate, threads)

    #print("Tasks:")
    #for ip, threads in self._tasks.items():
    #  print(f"- {[ x.tid() for x in threads ]} | {ip.instruction()}")
    #print("Requirements:")
    #for requirement in self._convergence_requirements:
    #  print(f"- {requirement.impactedLanes} | {requirement.mergeTarget.instruction()}")
    raise RuntimeError("No task to execute. Deadlock?")

  def handleConvergenceHeader(self, thread : Thread, instruction : Instruction):
    mergeTarget = self._program.getBBEntry(instruction.getMergeLocation())
    for requirement in self._convergence_requirements:
      if requirement.mergeTarget == mergeTarget:
        requirement.impactedLanes.add(thread.tid())
        return

    continueTarget = None
    if instruction.getContinueLocation():
      continueTarget = self._program.getBBEntry(instruction.getContinueLocation())
    requirement = ConvergenceRequirement(mergeTarget, continueTarget, set([ thread.tid() ]))
    self._convergence_requirements.append(requirement)

  def _hasTasks(self):
    return len(self._tasks) > 0

  def getFirstActiveThreadIndex(self) -> int:
    return min(self._active_thread_indices)

  def broadcastRegister(self, register, value) -> int:
    for tid in self._active_thread_indices:
      self._threads[tid].setRegister(register, value)

  def _getFunctionFromName(self, name : str) -> Function:
    register = self._program.getRegisterFromName(name)
    assert(register is not None)
    return self._program.getFunctionEntry(register)

  def run(self, function_name : str, verbose : bool = False) -> list[int]:
    for t in self._threads:
      self._program.initialize(t)

    function = self._getFunctionFromName(function_name)
    for t in self._threads:
      t.doCall(function, "__shader_output__")

    #step = 0
    self._tasks[self._threads[0].ip()] = self._threads
    while self._hasTasks():
      ip, threads = self._getNextRunnableTask()
      self._active_thread_indices = set([ x.tid() for x in threads ])
      if verbose:
        print(f"Executing with threads {self._active_thread_indices}: {ip.instruction()}")

      for thread in threads:
        self._program.runInstruction(thread, ip)
        if not thread.running():
          continue

        if thread.ip() in self._tasks:
          self._tasks[thread.ip()].append(thread)
        else:
          self._tasks[thread.ip()] = [ thread ]

      if verbose and ip.instruction().result != None:
        register = ip.instruction().result
        print(f"   {register:3} = {[ x.getRegister(register, allow_undef=True) for x in threads ]}")
      #step += 1
      #if step > 100:
      #  return [ 0 for x in self._threads ]

    output = []
    for thread in self._threads:
      output.append(thread.getRegister("__shader_output__"))
    return output

  def dump_register(self, register):
    for thread in self._threads:
      print(f" Thread {thread.tid():2} | {register:3} = {thread.getRegister(register)}")

parser = argparse.ArgumentParser(description="simulator", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument('-i', "--input", help="Text SPIR-V to read from", required=False, default='-')
parser.add_argument('-f', "--function", help="Function to execute")
parser.add_argument('-w', "--wave", help="Wave size", default=32, required=False)
parser.add_argument('-e', "--expects", help="Expected results per threads, expects a list of values. Ex: '1, 2, 3'.")
parser.add_argument("-v", "--verbose", help="verbose", action='store_true')
args = parser.parse_args()

def load_instructions(filename):
  if filename.lstrip().rstrip() != '-':
    with open(filename, "r") as f:
      lines = f.read().split("\n")
  else:
    lines = sys.stdin.readlines()

  lines = [ x.rstrip().lstrip() for x in lines ]
  lines = [ x for x in filter(lambda x: len(x) != 0 and x[0] != ';', lines) ]

  instructions = []
  for i in [ Instruction(x) for x in lines ]:
    out = parseInstruction(i)
    if out != None:
      instructions.append(out)
  return instructions

def main():
  expected_results = [ int(x.rstrip().lstrip()) for x in args.expects.split(',') ]
  wave_size = int(args.wave)

  if len(expected_results) != wave_size:
    print('Wave size != expected result array size', file=sys.stderr)
    sys.exit(1)

  instructions = load_instructions(args.input)
  program = Program(instructions)
  if args.verbose:
    program.dump()
    program.dump(args.function)

  wave = Wave(program, wave_size)
  results = wave.run(args.function, verbose=args.verbose)

  if expected_results != results:
    print("Expected != Observed", file=sys.stderr)
    print(f"{expected_results} != {results}", file=sys.stderr)
    sys.exit(1)
  sys.exit(0)

main()
