/*
 *
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dex_file.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "builder.h"
#include "nodes.h"
#include "primitive.h"

namespace art {

void HGraphBuilder::InitializeLocals(uint16_t count) {
  graph_->SetNumberOfVRegs(count);
  locals_.SetSize(count);
  for (int i = 0; i < count; i++) {
    HLocal* local = new (arena_) HLocal(i);
    entry_block_->AddInstruction(local);
    locals_.Put(i, local);
  }
}

bool HGraphBuilder::InitializeParameters(uint16_t number_of_parameters) {
  // dex_compilation_unit_ is null only when unit testing.
  if (dex_compilation_unit_ == nullptr) {
    return true;
  }

  graph_->SetNumberOfInVRegs(number_of_parameters);
  const char* shorty = dex_compilation_unit_->GetShorty();
  int locals_index = locals_.Size() - number_of_parameters;
  int parameter_index = 0;

  if (!dex_compilation_unit_->IsStatic()) {
    // Add the implicit 'this' argument, not expressed in the signature.
    HParameterValue* parameter = new (arena_) HParameterValue(parameter_index++);
    entry_block_->AddInstruction(parameter);
    HLocal* local = GetLocalAt(locals_index++);
    entry_block_->AddInstruction(new (arena_) HStoreLocal(local, parameter));
    number_of_parameters--;
  }

  uint32_t pos = 1;
  for (int i = 0; i < number_of_parameters; i++) {
    switch (shorty[pos++]) {
      case 'F':
      case 'D':
      case 'J': {
        return false;
      }

      default: {
        // integer and reference parameters.
        HParameterValue* parameter = new (arena_) HParameterValue(parameter_index++);
        entry_block_->AddInstruction(parameter);
        HLocal* local = GetLocalAt(locals_index++);
        // Store the parameter value in the local that the dex code will use
        // to reference that parameter.
        entry_block_->AddInstruction(new (arena_) HStoreLocal(local, parameter));
        break;
      }
    }
  }
  return true;
}

static bool CanHandleCodeItem(const DexFile::CodeItem& code_item) {
  if (code_item.tries_size_ > 0) {
    return false;
  }
  return true;
}

template<typename T>
void HGraphBuilder::If_22t(const Instruction& instruction, int32_t dex_offset, bool is_not) {
  HInstruction* first = LoadLocal(instruction.VRegA());
  HInstruction* second = LoadLocal(instruction.VRegB());
  current_block_->AddInstruction(new (arena_) T(first, second));
  if (is_not) {
    current_block_->AddInstruction(new (arena_) HNot(current_block_->GetLastInstruction()));
  }
  current_block_->AddInstruction(new (arena_) HIf(current_block_->GetLastInstruction()));
  HBasicBlock* target = FindBlockStartingAt(instruction.GetTargetOffset() + dex_offset);
  DCHECK(target != nullptr);
  current_block_->AddSuccessor(target);
  target = FindBlockStartingAt(dex_offset + instruction.SizeInCodeUnits());
  DCHECK(target != nullptr);
  current_block_->AddSuccessor(target);
  current_block_ = nullptr;
}

HGraph* HGraphBuilder::BuildGraph(const DexFile::CodeItem& code_item) {
  if (!CanHandleCodeItem(code_item)) {
    return nullptr;
  }

  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;

  // Setup the graph with the entry block and exit block.
  graph_ = new (arena_) HGraph(arena_);
  entry_block_ = new (arena_) HBasicBlock(graph_);
  graph_->AddBlock(entry_block_);
  exit_block_ = new (arena_) HBasicBlock(graph_);
  graph_->SetEntryBlock(entry_block_);
  graph_->SetExitBlock(exit_block_);

  InitializeLocals(code_item.registers_size_);
  graph_->UpdateMaximumNumberOfOutVRegs(code_item.outs_size_);

  // To avoid splitting blocks, we compute ahead of time the instructions that
  // start a new block, and create these blocks.
  ComputeBranchTargets(code_ptr, code_end);

  if (!InitializeParameters(code_item.ins_size_)) {
    return nullptr;
  }

  size_t dex_offset = 0;
  while (code_ptr < code_end) {
    // Update the current block if dex_offset starts a new block.
    MaybeUpdateCurrentBlock(dex_offset);
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (!AnalyzeDexInstruction(instruction, dex_offset)) return nullptr;
    dex_offset += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  // Add the exit block at the end to give it the highest id.
  graph_->AddBlock(exit_block_);
  exit_block_->AddInstruction(new (arena_) HExit());
  entry_block_->AddInstruction(new (arena_) HGoto());
  return graph_;
}

void HGraphBuilder::MaybeUpdateCurrentBlock(size_t index) {
  HBasicBlock* block = FindBlockStartingAt(index);
  if (block == nullptr) {
    return;
  }

  if (current_block_ != nullptr) {
    // Branching instructions clear current_block, so we know
    // the last instruction of the current block is not a branching
    // instruction. We add an unconditional goto to the found block.
    current_block_->AddInstruction(new (arena_) HGoto());
    current_block_->AddSuccessor(block);
  }
  graph_->AddBlock(block);
  current_block_ = block;
}

void HGraphBuilder::ComputeBranchTargets(const uint16_t* code_ptr, const uint16_t* code_end) {
  // TODO: Support switch instructions.
  branch_targets_.SetSize(code_end - code_ptr);

  // Create the first block for the dex instructions, single successor of the entry block.
  HBasicBlock* block = new (arena_) HBasicBlock(graph_);
  branch_targets_.Put(0, block);
  entry_block_->AddSuccessor(block);

  // Iterate over all instructions and find branching instructions. Create blocks for
  // the locations these instructions branch to.
  size_t dex_offset = 0;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (instruction.IsBranch()) {
      int32_t target = instruction.GetTargetOffset() + dex_offset;
      // Create a block for the target instruction.
      if (FindBlockStartingAt(target) == nullptr) {
        block = new (arena_) HBasicBlock(graph_);
        branch_targets_.Put(target, block);
      }
      dex_offset += instruction.SizeInCodeUnits();
      code_ptr += instruction.SizeInCodeUnits();
      if ((code_ptr < code_end) && (FindBlockStartingAt(dex_offset) == nullptr)) {
        block = new (arena_) HBasicBlock(graph_);
        branch_targets_.Put(dex_offset, block);
      }
    } else {
      code_ptr += instruction.SizeInCodeUnits();
      dex_offset += instruction.SizeInCodeUnits();
    }
  }
}

HBasicBlock* HGraphBuilder::FindBlockStartingAt(int32_t index) const {
  DCHECK_GE(index, 0);
  return branch_targets_.Get(index);
}

template<typename T>
void HGraphBuilder::Binop_32x(const Instruction& instruction) {
  HInstruction* first = LoadLocal(instruction.VRegB());
  HInstruction* second = LoadLocal(instruction.VRegC());
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_12x(const Instruction& instruction) {
  HInstruction* first = LoadLocal(instruction.VRegA());
  HInstruction* second = LoadLocal(instruction.VRegB());
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_22s(const Instruction& instruction, bool reverse) {
  HInstruction* first = LoadLocal(instruction.VRegB());
  HInstruction* second = GetConstant(instruction.VRegC_22s());
  if (reverse) {
    std::swap(first, second);
  }
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_22b(const Instruction& instruction, bool reverse) {
  HInstruction* first = LoadLocal(instruction.VRegB());
  HInstruction* second = GetConstant(instruction.VRegC_22b());
  if (reverse) {
    std::swap(first, second);
  }
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

bool HGraphBuilder::AnalyzeDexInstruction(const Instruction& instruction, int32_t dex_offset) {
  if (current_block_ == nullptr) {
    return true;  // Dead code
  }

  switch (instruction.Opcode()) {
    case Instruction::CONST_4: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetConstant(instruction.VRegB_11n());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetConstant(instruction.VRegB_21s());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::MOVE: {
      HInstruction* value = LoadLocal(instruction.VRegB());
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    case Instruction::RETURN_VOID: {
      current_block_->AddInstruction(new (arena_) HReturnVoid());
      current_block_->AddSuccessor(exit_block_);
      current_block_ = nullptr;
      break;
    }

    case Instruction::IF_EQ: {
      If_22t<HEqual>(instruction, dex_offset, false);
      break;
    }

    case Instruction::IF_NE: {
      If_22t<HEqual>(instruction, dex_offset, true);
      break;
    }

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      HBasicBlock* target = FindBlockStartingAt(instruction.GetTargetOffset() + dex_offset);
      DCHECK(target != nullptr);
      current_block_->AddInstruction(new (arena_) HGoto());
      current_block_->AddSuccessor(target);
      current_block_ = nullptr;
      break;
    }

    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT: {
      HInstruction* value = LoadLocal(instruction.VRegA());
      current_block_->AddInstruction(new (arena_) HReturn(value));
      current_block_->AddSuccessor(exit_block_);
      current_block_ = nullptr;
      break;
    }

    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_DIRECT: {
      uint32_t method_idx = instruction.VRegB_35c();
      const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
      uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
      const char* descriptor = dex_file_->StringByTypeIdx(return_type_idx);
      const size_t number_of_arguments = instruction.VRegA_35c();

      if (Primitive::GetType(descriptor[0]) != Primitive::kPrimVoid) {
        return false;
      }

      // Treat invoke-direct like static calls for now.
      HInvokeStatic* invoke = new (arena_) HInvokeStatic(
          arena_, number_of_arguments, dex_offset, method_idx);

      uint32_t args[5];
      instruction.GetArgs(args);

      for (size_t i = 0; i < number_of_arguments; i++) {
        HInstruction* arg = LoadLocal(args[i]);
        HInstruction* push = new (arena_) HPushArgument(arg, i);
        current_block_->AddInstruction(push);
        invoke->SetArgumentAt(i, push);
      }

      current_block_->AddInstruction(invoke);
      break;
    }

    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_DIRECT_RANGE: {
      uint32_t method_idx = instruction.VRegB_3rc();
      const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
      uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
      const char* descriptor = dex_file_->StringByTypeIdx(return_type_idx);
      const size_t number_of_arguments = instruction.VRegA_3rc();

      if (Primitive::GetType(descriptor[0]) != Primitive::kPrimVoid) {
        return false;
      }

      // Treat invoke-direct like static calls for now.
      HInvokeStatic* invoke = new (arena_) HInvokeStatic(
          arena_, number_of_arguments, dex_offset, method_idx);
      int32_t register_index = instruction.VRegC();
      for (size_t i = 0; i < number_of_arguments; i++) {
        HInstruction* arg = LoadLocal(register_index + i);
        HInstruction* push = new (arena_) HPushArgument(arg, i);
        current_block_->AddInstruction(push);
        invoke->SetArgumentAt(i, push);
      }
      current_block_->AddInstruction(invoke);
      break;
    }

    case Instruction::ADD_INT: {
      Binop_32x<HAdd>(instruction);
      break;
    }

    case Instruction::SUB_INT: {
      Binop_32x<HSub>(instruction);
      break;
    }

    case Instruction::ADD_INT_2ADDR: {
      Binop_12x<HAdd>(instruction);
      break;
    }

    case Instruction::SUB_INT_2ADDR: {
      Binop_12x<HSub>(instruction);
      break;
    }

    case Instruction::ADD_INT_LIT16: {
      Binop_22s<HAdd>(instruction, false);
      break;
    }

    case Instruction::RSUB_INT: {
      Binop_22s<HSub>(instruction, true);
      break;
    }

    case Instruction::ADD_INT_LIT8: {
      Binop_22b<HAdd>(instruction, false);
      break;
    }

    case Instruction::RSUB_INT_LIT8: {
      Binop_22b<HSub>(instruction, true);
      break;
    }

    case Instruction::NEW_INSTANCE: {
      current_block_->AddInstruction(
          new (arena_) HNewInstance(dex_offset, instruction.VRegB_21c()));
      UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::NOP:
      break;

    default:
      return false;
  }
  return true;
}

HIntConstant* HGraphBuilder::GetConstant0() {
  if (constant0_ != nullptr) {
    return constant0_;
  }
  constant0_ = new(arena_) HIntConstant(0);
  entry_block_->AddInstruction(constant0_);
  return constant0_;
}

HIntConstant* HGraphBuilder::GetConstant1() {
  if (constant1_ != nullptr) {
    return constant1_;
  }
  constant1_ = new(arena_) HIntConstant(1);
  entry_block_->AddInstruction(constant1_);
  return constant1_;
}

HIntConstant* HGraphBuilder::GetConstant(int constant) {
  switch (constant) {
    case 0: return GetConstant0();
    case 1: return GetConstant1();
    default: {
      HIntConstant* instruction = new (arena_) HIntConstant(constant);
      entry_block_->AddInstruction(instruction);
      return instruction;
    }
  }
}

HLocal* HGraphBuilder::GetLocalAt(int register_index) const {
  return locals_.Get(register_index);
}

void HGraphBuilder::UpdateLocal(int register_index, HInstruction* instruction) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HStoreLocal(local, instruction));
}

HInstruction* HGraphBuilder::LoadLocal(int register_index) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HLoadLocal(local));
  return current_block_->GetLastInstruction();
}

}  // namespace art
