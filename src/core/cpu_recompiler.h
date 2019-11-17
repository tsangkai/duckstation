#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

class Bus;

namespace CPU {
class Core;

union CodeBlockKey
{
  u32 bits;

  BitField<u32, bool, 0, 1> user_mode;
  BitField<u32, u32, 2, 30> aligned_pc;

  ALWAYS_INLINE u32 GetPC() const { return aligned_pc << 2; }
  ALWAYS_INLINE void SetPC(u32 pc) { aligned_pc = pc >> 2; }

  ALWAYS_INLINE CodeBlockKey& operator=(const CodeBlockKey& rhs)
  {
    bits = rhs.bits;
    return *this;
  }

  ALWAYS_INLINE bool operator==(const CodeBlockKey& rhs) const { return bits == rhs.bits; }
  ALWAYS_INLINE bool operator!=(const CodeBlockKey& rhs) const { return bits != rhs.bits; }
  ALWAYS_INLINE bool operator<(const CodeBlockKey& rhs) const { return bits < rhs.bits; }
};

struct CodeBlockInstruction
{
  Instruction instruction;
  u32 pc;
  bool is_branch_delay_slot;
  bool is_load_delay_slot;
  bool can_trap;
};

struct CodeBlock
{
  CodeBlockKey key;

  std::vector<CodeBlockInstruction> instructions;

  const u32 GetPC() const { return key.GetPC(); }
  const u32 GetSizeInBytes() const { return static_cast<u32>(instructions.size()) * sizeof(Instruction); }
  const u32 GetStartPageIndex() const { return (key.GetPC() / RECOMPILER_CODE_PAGE_SIZE); }
  const u32 GetEndPageIndex() const
  {
    return ((key.GetPC() + GetSizeInBytes() + (RECOMPILER_CODE_PAGE_SIZE - 1)) / RECOMPILER_CODE_PAGE_SIZE);
  }
};

class Recompiler
{
public:
  Recompiler(Core* core, Bus* bus);
  ~Recompiler();

  // Instruction helpers.
  static bool IsExitBlockInstruction(const Instruction& instruction, bool* is_branch);
  static bool CanInstructionTrap(const Instruction& instruction, bool in_user_mode);
  static bool IsLoadDelayingInstruction(const Instruction& instruction);
  static bool IsInvalidInstruction(const Instruction& instruction);

  void Reset();
  void Execute();

private:
  using BlockMap = std::unordered_map<u32, std::unique_ptr<CodeBlock>>;

  const CodeBlock* GetNextBlock();
  bool CompileBlock(CodeBlock* block);
  void FlushBlocks(u32 page_index);
  void FlushBlock(CodeBlock* block);
  void InterpretCachedBlock(const CodeBlock& block);
  void InterpretUncachedBlock();

  Core* m_core;
  Bus* m_bus;

  BlockMap m_blocks;

  std::array<std::vector<CodeBlock*>, RECOMPILER_CODE_PAGE_COUNT> m_ram_block_map;
};
} // namespace CPU