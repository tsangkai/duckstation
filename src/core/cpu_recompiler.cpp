#include "cpu_recompiler.h"
#include "YBaseLib/Log.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
Log_SetChannel(CPU::Recompiler);

namespace CPU {

extern bool TRACE_EXECUTION;
extern bool LOG_EXECUTION;

Recompiler::Recompiler(Core* core, Bus* bus) : m_core(core), m_bus(bus)
{
  bus->SetCodeInvalidateCallback(std::bind(&Recompiler::FlushBlocks, this, std::placeholders::_1));
}

Recompiler::~Recompiler() = default;

bool Recompiler::IsExitBlockInstruction(const Instruction& instruction, bool* is_branch)
{
  switch (instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    case InstructionOp::b:
    case InstructionOp::beq:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
    case InstructionOp::bne:
      *is_branch = true;
      return true;

    case InstructionOp::funct:
    {
      switch (instruction.r.funct)
      {
        case InstructionFunct::jr:
        case InstructionFunct::jalr:
          *is_branch = true;
          return true;

        case InstructionFunct::syscall:
        case InstructionFunct::break_:
          *is_branch = false;
          return true;

        default:
          *is_branch = false;
          return false;
      }
    }

    default:
      *is_branch = false;
      return false;
  }
}

bool Recompiler::CanInstructionTrap(const Instruction& instruction, bool in_user_mode)
{
  switch (instruction.op)
  {
    case InstructionOp::lui:
    case InstructionOp::andi:
    case InstructionOp::ori:
    case InstructionOp::xori:
    case InstructionOp::addiu:
    case InstructionOp::slti:
    case InstructionOp::sltiu:
    case InstructionOp::j:
    case InstructionOp::jal:
    case InstructionOp::beq:
    case InstructionOp::bne:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
    case InstructionOp::b:
      return false;

    case InstructionOp::cop0:
    case InstructionOp::cop2:
    case InstructionOp::lwc2:
    case InstructionOp::swc2:
      return in_user_mode;

      // swc0/lwc0/cop1/cop3 are essentially no-ops
    case InstructionOp::cop1:
    case InstructionOp::cop3:
    case InstructionOp::lwc0:
    case InstructionOp::lwc1:
    case InstructionOp::lwc3:
    case InstructionOp::swc0:
    case InstructionOp::swc1:
    case InstructionOp::swc3:
      return false;

    case InstructionOp::addi:
    case InstructionOp::lb:
    case InstructionOp::lh:
    case InstructionOp::lw:
    case InstructionOp::lbu:
    case InstructionOp::lhu:
    case InstructionOp::lwl:
    case InstructionOp::lwr:
    case InstructionOp::sb:
    case InstructionOp::sh:
    case InstructionOp::sw:
    case InstructionOp::swl:
    case InstructionOp::swr:
      return true;

    case InstructionOp::funct:
    {
      switch (instruction.r.funct)
      {
        case InstructionFunct::sll:
        case InstructionFunct::srl:
        case InstructionFunct::sra:
        case InstructionFunct::sllv:
        case InstructionFunct::srlv:
        case InstructionFunct::srav:
        case InstructionFunct::and_:
        case InstructionFunct::or_:
        case InstructionFunct::xor_:
        case InstructionFunct::nor:
        case InstructionFunct::addu:
        case InstructionFunct::subu:
        case InstructionFunct::slt:
        case InstructionFunct::sltu:
        case InstructionFunct::mfhi:
        case InstructionFunct::mthi:
        case InstructionFunct::mflo:
        case InstructionFunct::mtlo:
        case InstructionFunct::mult:
        case InstructionFunct::multu:
        case InstructionFunct::div:
        case InstructionFunct::divu:
        case InstructionFunct::jr:
        case InstructionFunct::jalr:
          return false;

        case InstructionFunct::add:
        case InstructionFunct::sub:
        case InstructionFunct::syscall:
        case InstructionFunct::break_:
        default:
          return true;
      }
    }

    default:
      return true;
  }
}

bool Recompiler::IsLoadDelayingInstruction(const Instruction& instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::lb:
    case InstructionOp::lh:
    case InstructionOp::lw:
    case InstructionOp::lbu:
    case InstructionOp::lhu:
      return true;

    case InstructionOp::lwl:
    case InstructionOp::lwr:
      return false;

    default:
      return false;
  }
}

bool Recompiler::IsInvalidInstruction(const Instruction& instruction)
{
  // TODO
  return true;
}

void Recompiler::Execute()
{
  while (m_core->m_downcount >= 0)
  {
    // fetch the next instruction
    m_core->DispatchInterrupts();

    const CodeBlock* block = GetNextBlock();
    if (!block)
    {
      Log_WarningPrintf("Falling back to uncached interpreter at 0x%08X", m_core->GetRegs().pc);
      InterpretUncachedBlock();
      continue;
    }

    InterpretCachedBlock(*block);
  }
}

void Recompiler::Reset()
{
  m_bus->ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();

  m_blocks.clear();
}

const CPU::CodeBlock* Recompiler::GetNextBlock()
{
  const u32 address = m_bus->UnmirrorAddress(m_core->m_regs.pc & UINT32_C(0x1FFFFFFF));

  CodeBlockKey key = {};
  key.SetPC(address);
  key.user_mode = m_core->InUserMode();

  BlockMap::iterator iter = m_blocks.find(key.bits);
  if (iter != m_blocks.end())
    return iter->second.get();

  std::unique_ptr<CodeBlock> block = std::make_unique<CodeBlock>();
  block->key = key;
  if (CompileBlock(block.get()))
  {
    // insert into the page map
    if (m_bus->IsRAMAddress(address))
    {
      const u32 start_page = block->GetStartPageIndex();
      const u32 end_page = block->GetEndPageIndex();
      for (u32 page = start_page; page < end_page; page++)
      {
        m_ram_block_map[page].push_back(block.get());
        m_bus->SetRAMCodePage(page);
      }
    }
  }
  else
  {
    Log_ErrorPrintf("Failed to compile block at PC=0x%08X", address);
  }

  iter = m_blocks.emplace(key.bits, std::move(block)).first;
  return iter->second.get();
}

bool Recompiler::CompileBlock(CodeBlock* block)
{
  u32 pc = block->GetPC();
  bool is_branch_delay_slot = false;
  bool is_load_delay_slot = false;

  for (;;)
  {
    CodeBlockInstruction cbi = {};
    if (!m_bus->IsCacheableAddress(pc) ||
        m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(pc, cbi.instruction.bits) < 0 ||
        !IsInvalidInstruction(cbi.instruction))
    {
      break;
    }

    cbi.pc = pc;
    cbi.is_branch_delay_slot = is_branch_delay_slot;
    cbi.is_load_delay_slot = is_load_delay_slot;
    cbi.can_trap = CanInstructionTrap(cbi.instruction, m_core->InUserMode());

    // load delay is done now
    is_load_delay_slot = IsLoadDelayingInstruction(cbi.instruction);

    // instruction is decoded now
    block->instructions.push_back(cbi);
    pc += sizeof(cbi.instruction.bits);

    // if we're in a branch delay slot, the block is now done
    if (is_branch_delay_slot)
      break;

    // if there is a branch delay slot, grab the instruction for that too, otherwise we're done
    if (IsExitBlockInstruction(cbi.instruction, &is_branch_delay_slot) && !is_branch_delay_slot)
      break;
  }

  if (!block->instructions.empty())
  {
#ifdef _DEBUG
    SmallString disasm;
    Log_DebugPrintf("Block at 0x%08X", block->GetPC());
    for (const CodeBlockInstruction& cbi : block->instructions)
    {
      CPU::DisassembleInstruction(&disasm, cbi.pc, cbi.instruction.bits, nullptr);
      Log_DebugPrintf("[%s %s 0x%08X] %s", cbi.is_branch_delay_slot ? "BD" : "  ", cbi.is_load_delay_slot ? "LD" : "  ",
                      cbi.pc, disasm.GetCharArray());
    }
#endif
  }
  else
  {
    Log_WarningPrintf("Empty block compiled at 0x%08X", block->key.GetPC());
    return false;
  }

  return true;
}

void Recompiler::FlushBlocks(u32 page_index)
{
  DebugAssert(page_index < RECOMPILER_CODE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  while (!blocks.empty())
    FlushBlock(blocks.back());
}

void Recompiler::FlushBlock(CodeBlock* block)
{
  BlockMap::iterator iter = m_blocks.find(block->key.GetPC());
  Assert(iter != m_blocks.end() && iter->second.get() == block);
  Log_DevPrintf("Flushing block at address 0x%08X", block->GetPC());

  // remove from the page map
  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page < end_page; page++)
  {
    auto& page_blocks = m_ram_block_map[page];
    auto page_block_iter = std::find(page_blocks.begin(), page_blocks.end(), block);
    Assert(page_block_iter != page_blocks.end());
    page_blocks.erase(page_block_iter);
  }

  // remove from block map
  m_blocks.erase(iter);
}

void Recompiler::InterpretCachedBlock(const CodeBlock& block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert((m_core->m_regs.pc & PHYSICAL_MEMORY_ADDRESS_MASK) == block.GetPC());

  for (const CodeBlockInstruction& cbi : block.instructions)
  {
    m_core->m_pending_ticks += 1;
    m_core->m_downcount -= 1;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = cbi.instruction.bits;
    m_core->m_current_instruction_pc = m_core->m_regs.pc;
    m_core->m_current_instruction_in_branch_delay_slot = cbi.is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // update pc
    DebugAssert((m_core->m_regs.pc & PHYSICAL_MEMORY_ADDRESS_MASK) == cbi.pc);
    m_core->m_regs.pc = m_core->m_regs.npc;
    m_core->m_regs.npc += 4;

    // execute the instruction we previously fetched
    m_core->ExecuteInstruction();

    // next load delay
    m_core->m_load_delay_reg = m_core->m_next_load_delay_reg;
    m_core->m_next_load_delay_reg = Reg::count;
    m_core->m_load_delay_old_value = m_core->m_next_load_delay_old_value;
    m_core->m_next_load_delay_old_value = 0;

    if (m_core->m_exception_raised)
      break;
  }

  // cleanup so the interpreter can kick in if needed
  m_core->m_next_instruction_is_branch_delay_slot = false;
}

void Recompiler::InterpretUncachedBlock()
{
  // At this point, pc contains the last address executed (in the previous block). The instruction has not been fetched
  // yet. pc shouldn't be updated until the fetch occurs, that way the exception occurs in the delay slot.
  bool in_branch_delay_slot = false;
  for (;;)
  {
    m_core->m_pending_ticks += 1;
    m_core->m_downcount -= 1;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = m_core->m_next_instruction.bits;
    m_core->m_current_instruction_pc = m_core->m_regs.pc;
    m_core->m_current_instruction_in_branch_delay_slot = m_core->m_next_instruction_is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_next_instruction_is_branch_delay_slot = false;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // Fetch the next instruction, except if we're in a branch delay slot. The "fetch" is done in the next block.
    if (!m_core->FetchInstruction())
      break;

    // execute the instruction we previously fetched
    m_core->ExecuteInstruction();

    // next load delay
    m_core->m_load_delay_reg = m_core->m_next_load_delay_reg;
    m_core->m_next_load_delay_reg = Reg::count;
    m_core->m_load_delay_old_value = m_core->m_next_load_delay_old_value;
    m_core->m_next_load_delay_old_value = 0;

    if (m_core->m_exception_raised || in_branch_delay_slot ||
        (IsExitBlockInstruction(m_core->m_current_instruction, &in_branch_delay_slot) && !in_branch_delay_slot))
    {
      break;
    }
  }
}

} // namespace CPU