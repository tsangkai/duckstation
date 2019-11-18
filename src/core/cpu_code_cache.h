#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

class JitCodeBuffer;

class Bus;

namespace CPU {
class Core;

namespace Recompiler
{
class ASMFunctions;
}

class CodeCache
{
public:
  CodeCache(Core* core, Bus* bus);
  ~CodeCache();

  void Reset();
  void Execute();

private:
  using BlockMap = std::unordered_map<u32, CodeBlock*>;

  const CodeBlock* GetNextBlock();
  bool CompileBlock(CodeBlock* block);
  void FlushBlocks(u32 page_index);
  void FlushBlock(CodeBlock* block);
  void InterpretCachedBlock(const CodeBlock& block);
  void InterpretUncachedBlock();

  Core* m_core;
  Bus* m_bus;

  const CodeBlock* m_current_block = nullptr;
  bool m_current_block_flushed = false;

  std::unique_ptr<JitCodeBuffer> m_code_buffer;
  std::unique_ptr<Recompiler::ASMFunctions> m_asm_functions;

  BlockMap m_blocks;

  std::array<std::vector<CodeBlock*>, RECOMPILER_CODE_PAGE_COUNT> m_ram_block_map;
};
} // namespace CPU