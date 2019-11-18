#include "cpu_code_cache.h"
#include "YBaseLib/Log.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
Log_SetChannel(CPU::CodeCache);

namespace CPU {

extern bool TRACE_EXECUTION;
extern bool LOG_EXECUTION;
static bool USE_RECOMPILER = true;

CodeCache::CodeCache(Core* core, Bus* bus) : m_core(core), m_bus(bus)
{
  bus->SetCodeInvalidateCallback(std::bind(&CodeCache::FlushBlocks, this, std::placeholders::_1));

  m_code_buffer = std::make_unique<JitCodeBuffer>();
  m_asm_functions = std::make_unique<Recompiler::ASMFunctions>();
  m_asm_functions->Generate(m_code_buffer.get());
}

CodeCache::~CodeCache() = default;

void CodeCache::Execute()
{
  while (m_core->m_downcount >= 0)
  {
    if (m_core->HasPendingInterrupt())
    {
      // TODO: Fill in m_next_instruction...
      m_core->DispatchInterrupt();
    }

    m_current_block = GetNextBlock();
    if (!m_current_block)
    {
      Log_WarningPrintf("Falling back to uncached interpreter at 0x%08X", m_core->GetRegs().pc);
      InterpretUncachedBlock();
      continue;
    }

    if (USE_RECOMPILER)
      static_cast<Recompiler::BlockFunctionType>(m_current_block->code)(m_core);
    else
      InterpretCachedBlock(*m_current_block);

    if (m_current_block_flushed)
    {
      m_current_block_flushed = false;
      delete m_current_block;
    }

    m_current_block = nullptr;
  }
}

void CodeCache::Reset()
{
  m_bus->ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();

  m_blocks.clear();
}

const CPU::CodeBlock* CodeCache::GetNextBlock()
{
  const u32 address = m_bus->UnmirrorAddress(m_core->m_regs.pc & UINT32_C(0x1FFFFFFF));

  CodeBlockKey key = {};
  key.SetPC(address);
  key.user_mode = m_core->InUserMode();

  BlockMap::iterator iter = m_blocks.find(key.bits);
  if (iter != m_blocks.end())
    return iter->second;

  CodeBlock* block = new CodeBlock();
  block->key = key;
  if (CompileBlock(block))
  {
    // insert into the page map
    if (m_bus->IsRAMAddress(address))
    {
      const u32 start_page = block->GetStartPageIndex();
      const u32 end_page = block->GetEndPageIndex();
      for (u32 page = start_page; page < end_page; page++)
      {
        m_ram_block_map[page].push_back(block);
        m_bus->SetRAMCodePage(page);
      }
    }
  }
  else
  {
    Log_ErrorPrintf("Failed to compile block at PC=0x%08X", address);
  }

  iter = m_blocks.emplace(key.bits, block).first;
  return block;
}

bool CodeCache::CompileBlock(CodeBlock* block)
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

  if (USE_RECOMPILER)
  {
    Recompiler::CodeGenerator codegen(m_core, m_code_buffer.get(), *m_asm_functions.get());
    if (!codegen.CompileBlock(block, &block->code, &block->code_size))
    {
      Log_ErrorPrintf("Failed to compile host code for block at 0x%08X", block->key.GetPC());
      return false;
    }
  }

  return true;
}

void CodeCache::FlushBlocks(u32 page_index)
{
  DebugAssert(page_index < RECOMPILER_CODE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  while (!blocks.empty())
    FlushBlock(blocks.back());
}

void CodeCache::FlushBlock(CodeBlock* block)
{
  BlockMap::iterator iter = m_blocks.find(block->key.GetPC());
  Assert(iter != m_blocks.end() && iter->second == block);
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

  // flushing block currently executing?
  if (m_current_block == block)
  {
    Log_WarningPrintf("Flushing currently-executing block 0x%08X", block->GetPC());
    m_current_block_flushed = true;
  }
  else
  {
    delete block;
  }
}

void CodeCache::InterpretCachedBlock(const CodeBlock& block)
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

void CodeCache::InterpretUncachedBlock()
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