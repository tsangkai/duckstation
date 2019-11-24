#include "pad.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "memory_card.h"
#include "pad_device.h"
#include "system.h"
Log_SetChannel(Pad);

Pad::Pad() = default;

Pad::~Pad() = default;

void Pad::Initialize(System* system, InterruptController* interrupt_controller)
{
  m_system = system;
  m_interrupt_controller = interrupt_controller;
}

void Pad::Reset()
{
  SoftReset();

  for (u32 i = 0; i < NUM_SLOTS; i++)
  {
    if (m_controllers[i])
      m_controllers[i]->Reset();

    if (m_memory_cards[i])
      m_memory_cards[i]->Reset();
  }
}

bool Pad::DoState(StateWrapper& sw)
{
  for (u32 i = 0; i < NUM_SLOTS; i++)
  {
    if (m_controllers[i])
    {
      if (!sw.DoMarker("Controller") || !m_controllers[i]->DoState(sw))
        return false;
    }
    else
    {
      if (!sw.DoMarker("NoController"))
        return false;
    }

    bool card_present = static_cast<bool>(m_memory_cards[i]);
    sw.Do(&card_present);

    if (card_present && !m_memory_cards[i])
    {
      const TinyString message = TinyString::FromFormat(
        "Memory card %c present in save state but not in system. Creating temporary card.", 'A' + i);
      m_system->GetHostInterface()->AddOSDMessage(message);
      Log_WarningPrint(message);

      m_memory_cards[i] = MemoryCard::Create(m_system);
    }
    else if (!card_present && m_memory_cards[i])
    {
      const TinyString message =
        TinyString::FromFormat("Memory card %u present system but not save state. Removing card.", 'A' + i);
      m_system->GetHostInterface()->AddOSDMessage(message);
      Log_WarningPrint(message);

      m_memory_cards[i].reset();
    }

    if (m_memory_cards[i])
    {
      if (!sw.DoMarker("MemoryCard") || !m_memory_cards[i]->DoState(sw))
        return false;
    }
    else
    {
      if (!sw.DoMarker("NoController"))
        return false;
    }
  }

  sw.Do(&m_state);
  sw.Do(&m_ticks_remaining);
  sw.Do(&m_JOY_CTRL.bits);
  sw.Do(&m_JOY_STAT.bits);
  sw.Do(&m_JOY_MODE.bits);
  sw.Do(&m_JOY_BAUD);
  sw.Do(&m_receive_buffer);
  sw.Do(&m_transmit_buffer);
  sw.Do(&m_receive_buffer_full);
  sw.Do(&m_transmit_buffer_full);
  return !sw.HasError();
}

u32 Pad::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      if (!m_transmit_buffer_full)
        Log_DevPrintf("Read from RX fifo when empty");

      const u8 value = m_receive_buffer;
      m_receive_buffer_full = false;

      UpdateJoyStat();
      Log_DebugPrintf("JOY_DATA (R) -> 0x%02X", ZeroExtend32(value));
      return (ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
              (ZeroExtend32(value) << 24));
    }

    case 0x04: // JOY_STAT
    {
      const u32 bits = m_JOY_STAT.bits;
      m_JOY_STAT.ACKINPUT = false;
      return bits;
    }

    case 0x08: // JOY_MODE
      return ZeroExtend32(m_JOY_MODE.bits);

    case 0x0A: // JOY_CTRL
      return ZeroExtend32(m_JOY_CTRL.bits);

    case 0x0E: // JOY_BAUD
      return ZeroExtend32(m_JOY_BAUD);

    default:
      Log_ErrorPrintf("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void Pad::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      Log_DebugPrintf("JOY_DATA (W) <- 0x%02X", value);

      if (m_transmit_buffer_full)
        Log_WarningPrint("TX FIFO overrun");

      m_transmit_buffer = Truncate8(value);
      m_transmit_buffer_full = true;

      if (!IsTransmitting() && CanTransfer())
        BeginTransfer();

      return;
    }

    case 0x0A: // JOY_CTRL
    {
      Log_DebugPrintf("JOY_CTRL <- 0x%04X", value);
      const bool old_select = m_JOY_CTRL.SELECT;

      m_JOY_CTRL.bits = Truncate16(value);
      if (m_JOY_CTRL.RESET)
        SoftReset();

      if (m_JOY_CTRL.ACK)
      {
        // reset stat bits
        m_JOY_STAT.INTR = false;
      }

      if (!m_JOY_CTRL.SELECT)
        ResetDeviceTransferState();

      if (!m_JOY_CTRL.SELECT || !m_JOY_CTRL.TXEN)
      {
        if (IsTransmitting())
          EndTransfer();
      }
      else
      {
        if (!IsTransmitting() && CanTransfer())
          BeginTransfer();
      }

      UpdateJoyStat();
      return;
    }

    case 0x08: // JOY_MODE
    {
      Log_DebugPrintf("JOY_MODE <- 0x%08X", value);
      m_JOY_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_DebugPrintf("JOY_BAUD <- 0x%08X", value);
      m_JOY_BAUD = Truncate16(value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

void Pad::Execute(TickCount ticks)
{
  switch (m_state)
  {
    case State::Idle:
      break;

    case State::Transmitting:
    {
      m_ticks_remaining -= ticks;
      if (m_ticks_remaining <= 0)
        DoTransfer();
      else
        m_system->SetDowncount(m_ticks_remaining);
    }
    break;
  }
}

void Pad::SoftReset()
{
  if (IsTransmitting())
    EndTransfer();

  m_JOY_CTRL.bits = 0;
  m_JOY_STAT.bits = 0;
  m_JOY_MODE.bits = 0;
  m_receive_buffer = 0;
  m_receive_buffer_full = false;
  m_transmit_buffer = 0;
  m_transmit_buffer_full = false;
  ResetDeviceTransferState();
  UpdateJoyStat();
}

void Pad::UpdateJoyStat()
{
  m_JOY_STAT.RXFIFONEMPTY = m_receive_buffer_full;
  m_JOY_STAT.TXDONE = !m_transmit_buffer_full && m_state == State::Idle;
  m_JOY_STAT.TXRDY = !m_transmit_buffer_full;
}

void Pad::BeginTransfer()
{
  DebugAssert(m_state == State::Idle && CanTransfer());
  Log_DebugPrintf("Starting transfer");

  m_JOY_CTRL.RXEN = true;
  m_transmit_value = m_transmit_buffer;
  m_transmit_buffer_full = false;

  // The transfer or the interrupt must be delayed, otherwise the BIOS thinks there's no device detected.
  // It seems to do something resembling the following:
  //  1) Sets the control register up for transmitting, interrupt on ACK.
  //  2) Writes 0x01 to the TX FIFO.
  //  3) Delays for a bit.
  //  4) Writes ACK to the control register, clearing the interrupt flag.
  //  5) Clears IRQ7 in the interrupt controller.
  //  6) Waits until the RX FIFO is not empty, reads the first byte to $zero.
  //  7) Checks if the interrupt status register had IRQ7 set. If not, no device connected.
  //
  // Performing the transfer immediately will result in both the INTR bit and the bit in the interrupt
  // controller being discarded in (4)/(5), but this bit was set by the *new* transfer. Therefore, the
  // test in (7) will fail, and it won't send any more data. So, the transfer/interrupt must be delayed
  // until after (4) and (5) have been completed.

  m_system->Synchronize();
  m_state = State::Transmitting;
  m_ticks_remaining = GetTransferTicks();
  m_system->SetDowncount(m_ticks_remaining);
}

void Pad::DoTransfer()
{
  Log_DebugPrintf("Transferring slot %d", m_JOY_CTRL.SLOT.GetValue());

  const std::shared_ptr<PadDevice>& controller = m_controllers[m_JOY_CTRL.SLOT];
  const std::shared_ptr<PadDevice>& memory_card = m_memory_cards[m_JOY_CTRL.SLOT];

  // set rx?
  m_JOY_CTRL.RXEN = true;

  const u8 data_out = m_transmit_value;

  u8 data_in = 0xFF;
  bool ack = false;

  switch (m_active_device)
  {
    case ActiveDevice::None:
    {
      if (!controller || (ack = controller->Transfer(data_out, &data_in)) == false)
      {
        if (!memory_card || (ack = memory_card->Transfer(data_out, &data_in)) == false)
        {
          // nothing connected to this port
          Log_TracePrintf("Nothing connected or ACK'ed");
        }
        else
        {
          // memory card responded, make it the active device until non-ack
          Log_TracePrintf("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
          m_active_device = ActiveDevice::MemoryCard;
        }
      }
      else
      {
        // controller responded, make it the active device until non-ack
        Log_TracePrintf("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
        m_active_device = ActiveDevice::Controller;
      }
    }
    break;

    case ActiveDevice::Controller:
    {
      if (controller)
      {
        ack = controller->Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
    }
    break;

    case ActiveDevice::MemoryCard:
    {
      if (memory_card)
      {
        ack = memory_card->Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
    }
    break;
  }

  m_receive_buffer = data_in;
  m_receive_buffer_full = true;
  m_JOY_STAT.ACKINPUT |= ack;

  // device no longer active?
  if (!ack)
    m_active_device = ActiveDevice::None;

  if (m_JOY_STAT.ACKINPUT && m_JOY_CTRL.ACKINTEN)
  {
    Log_DebugPrintf("Triggering interrupt");
    m_JOY_STAT.INTR = true;
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::IRQ7);
  }

  EndTransfer();
  UpdateJoyStat();
}

void Pad::EndTransfer()
{
  DebugAssert(m_state == State::Transmitting);
  Log_DebugPrintf("Ending transfer");

  m_state = State::Idle;
  m_ticks_remaining = 0;
}

void Pad::ResetDeviceTransferState()
{
  for (u32 i = 0; i < NUM_SLOTS; i++)
  {
    if (m_controllers[i])
      m_controllers[i]->ResetTransferState();
    if (m_memory_cards[i])
      m_memory_cards[i]->ResetTransferState();

    m_active_device = ActiveDevice::None;
  }
}