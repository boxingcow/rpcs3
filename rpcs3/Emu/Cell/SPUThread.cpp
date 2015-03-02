#include "stdafx.h"
#include "rpcs3/Ini.h"
#include "Utilities/Log.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/Memory/atomic_type.h"

#include "Emu/IdManager.h"
#include "Emu/CPU/CPUThreadManager.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/SysCalls/ErrorCodes.h"
#include "Emu/SysCalls/lv2/sys_spu.h"
#include "Emu/SysCalls/lv2/sys_event_flag.h"
#include "Emu/SysCalls/lv2/sys_time.h"

#include "Emu/Cell/SPUDisAsm.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/SPUDecoder.h"
#include "Emu/Cell/SPUInterpreter.h"
#include "Emu/Cell/SPURecompiler.h"

#include <cfenv>

SPUThread& GetCurrentSPUThread()
{
	CPUThread* thread = GetCurrentCPUThread();

	if(!thread || (thread->GetType() != CPU_THREAD_SPU && thread->GetType() != CPU_THREAD_RAW_SPU))
	{
		throw std::string("GetCurrentSPUThread: bad thread");
	}

	return *(SPUThread*)thread;
}

SPUThread::SPUThread(CPUThreadType type) : CPUThread(type)
{
	assert(type == CPU_THREAD_SPU || type == CPU_THREAD_RAW_SPU);

	group = nullptr;
	for (auto& p : SPUPs)
	{
		p.reset(new EventPort());
	}

	Reset();
}

SPUThread::~SPUThread()
{
}

void SPUThread::Task()
{
	const int round = std::fegetround();
	std::fesetround(FE_TOWARDZERO);

	if (m_custom_task)
	{
		m_custom_task(*this);
	}
	else
	{
		CPUThread::Task();
	}
	
	if (std::fegetround() != FE_TOWARDZERO)
	{
		LOG_ERROR(Log::SPU, "Rounding mode has changed(%d)", std::fegetround());
	}
	std::fesetround(round);
}

void SPUThread::DoReset()
{
	//reset regs
	memset(GPR, 0, sizeof(GPR));
}

void SPUThread::InitRegs()
{
	GPR[1]._u32[3] = 0x3FFF0; // initial stack frame pointer

	cfg.Reset();

	ls_offset = m_offset;

	SPU.Status.SetValue(SPU_STATUS_STOPPED);

	// TODO: check initialization if necessary
	MFC2.QueryType.SetValue(0); // prxy
	MFC1.CMDStatus.SetValue(0);
	MFC2.CMDStatus.SetValue(0);
	MFC1.TagStatus.SetValue(0);
	MFC2.TagStatus.SetValue(0);
	//PC = SPU.NPC.GetValue();

	m_event_mask = 0;
	m_events = 0;
}

void SPUThread::InitStack()
{
	m_stack_size = 0x1000; // this value is wrong
	m_stack_addr = m_offset + 0x40000 - m_stack_size; // stack is the part of SPU Local Storage
}

void SPUThread::CloseStack()
{
	// nothing to do here
}

void SPUThread::DoRun()
{
	switch(Ini.SPUDecoderMode.GetValue())
	{
	case 1:
		m_dec = new SPUDecoder(*new SPUInterpreter(*this));
	break;
	case 2:
		m_dec = new SPURecompilerCore(*this);
	break;

	default:
		LOG_ERROR(Log::SPU, "Invalid SPU decoder mode: %d", Ini.SPUDecoderMode.GetValue());
		Emu.Pause();
	}
}

void SPUThread::DoResume()
{
}

void SPUThread::DoPause()
{
}

void SPUThread::DoStop()
{
	delete m_dec;
	m_dec = nullptr;
}

void SPUThread::DoClose()
{
	// disconnect all event ports
	if (Emu.IsStopped())
	{
		return;
	}
	for (u32 i = 0; i < 64; i++)
	{
		std::shared_ptr<EventPort> port = SPUPs[i];
		std::lock_guard<std::mutex> lock(port->m_mutex);
		if (port->eq)
		{
			port->eq->ports.remove(port);
			port->eq = nullptr;
		}
	}
}

void SPUThread::FastCall(u32 ls_addr)
{
	// can't be called from another thread (because it doesn't make sense)
	WriteLS32(0x0, 2);

	auto old_PC = PC;
	auto old_LR = GPR[0]._u32[3];
	auto old_stack = GPR[1]._u32[3]; // only saved and restored (may be wrong)

	m_status = Running;
	PC = ls_addr;
	GPR[0]._u32[3] = 0x0;

	CPUThread::Task();

	PC = old_PC;
	GPR[0]._u32[3] = old_LR;
	GPR[1]._u32[3] = old_stack;
}

void SPUThread::FastStop()
{
	m_status = Stopped;
}

void SPUThread::WriteSNR(bool number, u32 value)
{
	if (cfg.value & ((u64)1 << (u64)number))
	{
		SPU.SNR[number ? 1 : 0].PushUncond_OR(value); // logical OR
	}
	else
	{
		SPU.SNR[number ? 1 : 0].PushUncond(value); // overwrite
	}
}

#define LOG_DMAC(type, text) type(Log::SPU, "DMAC::ProcessCmd(cmd=0x%x, tag=0x%x, lsa=0x%x, ea=0x%llx, size=0x%x): " text, cmd, tag, lsa, ea, size)

void SPUThread::ProcessCmd(u32 cmd, u32 tag, u32 lsa, u64 ea, u32 size)
{
	if (cmd & (MFC_BARRIER_MASK | MFC_FENCE_MASK))
	{
		_mm_mfence();
	}

	u32 eal = vm::cast(ea, "ea");

	if (eal >= SYS_SPU_THREAD_BASE_LOW && group) // SPU Thread Group MMIO (LS and SNR)
	{
		const u32 num = (eal & SYS_SPU_THREAD_BASE_MASK) / SYS_SPU_THREAD_OFFSET; // thread number in group
		const u32 offset = (eal & SYS_SPU_THREAD_BASE_MASK) % SYS_SPU_THREAD_OFFSET; // LS offset or MMIO register

		std::shared_ptr<CPUThread> t;

		if (num < group->list.size() && group->list[num] && (t = Emu.GetCPU().GetThread(group->list[num])) && t->GetType() == CPU_THREAD_SPU)
		{
			SPUThread& spu = static_cast<SPUThread&>(*t);

			if (offset + size - 1 < 0x40000) // LS access
			{
				eal = spu.ls_offset + offset; // redirect access
			}
			else if ((cmd & MFC_PUT_CMD) && size == 4 && (offset == SYS_SPU_THREAD_SNR1 || offset == SYS_SPU_THREAD_SNR2))
			{
				spu.WriteSNR(SYS_SPU_THREAD_SNR2 == offset, vm::read32(ls_offset + lsa));
				return;
			}
			else
			{
				LOG_DMAC(LOG_ERROR, "Invalid offset (SPU Thread Group MMIO)");
			}
		}
		else
		{
			LOG_DMAC(LOG_ERROR, "Invalid thread (SPU Thread Group MMIO)");
		}
	}

	switch (cmd & ~(MFC_BARRIER_MASK | MFC_FENCE_MASK | MFC_LIST_MASK | MFC_RESULT_MASK))
	{
	case MFC_PUT_CMD:
	{
		memcpy(vm::get_ptr(eal), vm::get_ptr(ls_offset + lsa), size);
		return;
	}

	case MFC_GET_CMD:
	{
		memcpy(vm::get_ptr(ls_offset + lsa), vm::get_ptr(eal), size);
		return;
	}

	default:
	{
		LOG_DMAC(LOG_ERROR, "Unknown DMA command");
		Emu.Pause();
		return;
	}
	}
}

#undef LOG_DMAC

void SPUThread::ListCmd(u32 lsa, u64 ea, u16 tag, u16 size, u32 cmd, MFCReg& MFCArgs)
{
	const u32 list_addr = ea & 0x3ffff;
	const u32 list_size = size / 8;
	lsa &= 0x3fff0;

	struct list_element
	{
		be_t<u16> s; // Stall-and-Notify bit (0x8000)
		be_t<u16> ts; // List Transfer Size
		be_t<u32> ea; // External Address Low
	};

	u32 result = MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL;

	for (u32 i = 0; i < list_size; i++)
	{
		auto rec = vm::ptr<list_element>::make(ls_offset + list_addr + i * 8);

		const u32 size = rec->ts;
		if (!(rec->s.data() & se16(0x8000)) && size < 16 && size != 1 && size != 2 && size != 4 && size != 8)
		{
			LOG_ERROR(Log::SPU, "DMA List: invalid transfer size(%d)", size);
			result = MFC_PPU_DMA_CMD_SEQUENCE_ERROR;
			break;
		}

		const u32 addr = rec->ea;
		if (size)
		{
			ProcessCmd(cmd, tag, lsa | (addr & 0xf), addr, size);
		}

		if (Ini.HLELogging.GetValue() || rec->s.data())
		{
			LOG_NOTICE(Log::SPU, "*** list element(%d/%d): s=0x%x, ts=0x%x, eal=0x%x (lsa=0x%x)", i, list_size, rec->s, rec->ts, rec->ea, lsa | (addr & 0xf));
		}

		if (size)
		{
			lsa += std::max<u32>(size, 16);
		}

		if (rec->s.data() & se16(0x8000))
		{
			StallStat.PushUncond_OR(1 << tag);

			if (StallList[tag].MFCArgs)
			{
				LOG_ERROR(Log::SPU, "DMA List: existing stalled list found (tag=%d)", tag);
				result = MFC_PPU_DMA_CMD_SEQUENCE_ERROR;
				break;
			}

			StallList[tag].MFCArgs = &MFCArgs;
			StallList[tag].cmd = cmd;
			StallList[tag].ea = (ea & ~0xffffffff) | (list_addr + (i + 1) * 8);
			StallList[tag].lsa = lsa;
			StallList[tag].size = (list_size - i - 1) * 8;
			break;
		}
	}

	MFCArgs.CMDStatus.SetValue(result);
}

void SPUThread::EnqMfcCmd(MFCReg& MFCArgs)
{
	u32 cmd = MFCArgs.CMDStatus.GetValue();
	u16 op = cmd & MFC_MASK_CMD;

	u32 lsa = MFCArgs.LSA.GetValue();
	u64 ea = (u64)MFCArgs.EAL.GetValue() | ((u64)MFCArgs.EAH.GetValue() << 32);
	u32 size_tag = MFCArgs.Size_Tag.GetValue();
	u16 tag = (u16)size_tag;
	u16 size = size_tag >> 16;

	switch (op & ~(MFC_BARRIER_MASK | MFC_FENCE_MASK))
	{
	case MFC_PUT_CMD:
	case MFC_PUTR_CMD: // ???
	case MFC_GET_CMD:
	{
		if (Ini.HLELogging.GetValue()) LOG_NOTICE(Log::SPU, "DMA %s%s%s%s: lsa=0x%x, ea=0x%llx, tag=0x%x, size=0x%x, cmd=0x%x",
			(op & MFC_PUT_CMD ? "PUT" : "GET"),
			(op & MFC_RESULT_MASK ? "R" : ""),
			(op & MFC_BARRIER_MASK ? "B" : ""),
			(op & MFC_FENCE_MASK ? "F" : ""),
			lsa, ea, tag, size, cmd);

		ProcessCmd(cmd, tag, lsa, ea, size);
		MFCArgs.CMDStatus.SetValue(MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL);
		break;
	}

	case MFC_PUTL_CMD:
	case MFC_PUTRL_CMD: // ???
	case MFC_GETL_CMD:
	{
		if (Ini.HLELogging.GetValue()) LOG_NOTICE(Log::SPU, "DMA %s%s%s%s: lsa=0x%x, list=0x%llx, tag=0x%x, size=0x%x, cmd=0x%x",
			(op & MFC_PUT_CMD ? "PUT" : "GET"),
			(op & MFC_RESULT_MASK ? "RL" : "L"),
			(op & MFC_BARRIER_MASK ? "B" : ""),
			(op & MFC_FENCE_MASK ? "F" : ""),
			lsa, ea, tag, size, cmd);

		ListCmd(lsa, ea, tag, size, cmd, MFCArgs);
		break;
	}

	case MFC_GETLLAR_CMD:
	case MFC_PUTLLC_CMD:
	case MFC_PUTLLUC_CMD:
	case MFC_PUTQLLUC_CMD:
	{
		if (Ini.HLELogging.GetValue() || size != 128) LOG_NOTICE(Log::SPU, "DMA %s: lsa=0x%x, ea=0x%llx, tag=0x%x, size=0x%x, cmd=0x%x",
			(op == MFC_GETLLAR_CMD ? "GETLLAR" :
			op == MFC_PUTLLC_CMD ? "PUTLLC" :
			op == MFC_PUTLLUC_CMD ? "PUTLLUC" : "PUTQLLUC"),
			lsa, ea, tag, size, cmd);

		if (op == MFC_GETLLAR_CMD) // get reservation
		{
			vm::reservation_acquire(vm::get_ptr(ls_offset + lsa), vm::cast(ea), 128, [this]()
			{
				m_events |= SPU_EVENT_LR; // TODO: atomic op
				Notify();
			});

			MFCArgs.AtomicStat.PushUncond(MFC_GETLLAR_SUCCESS);
		}
		else if (op == MFC_PUTLLC_CMD) // store conditional
		{
			if (vm::reservation_update(vm::cast(ea), vm::get_ptr(ls_offset + lsa), 128))
			{
				MFCArgs.AtomicStat.PushUncond(MFC_PUTLLC_SUCCESS);
			}
			else
			{
				MFCArgs.AtomicStat.PushUncond(MFC_PUTLLC_FAILURE);
			}
		}
		else // store unconditional (may be wrong)
		{
			vm::reservation_op(vm::cast(ea), 128, [this, tag, lsa, ea]()
			{
				memcpy(vm::priv_ptr(vm::cast(ea)), vm::get_ptr(ls_offset + lsa), 128);
			});

			if (op == MFC_PUTLLUC_CMD)
			{
				MFCArgs.AtomicStat.PushUncond(MFC_PUTLLUC_SUCCESS);
			}
			else
			{
				// tag may be used here
			}
		}
		break;
	}

	default:
	{
		LOG_ERROR(Log::SPU, "Unknown MFC cmd (opcode=0x%x, cmd=0x%x, lsa=0x%x, ea=0x%llx, tag=0x%x, size=0x%x)", op, cmd, lsa, ea, tag, size);
		Emu.Pause();
		break;
	}
	}
}

bool SPUThread::CheckEvents()
{
	return (m_events & m_event_mask) != 0;
}

u32 SPUThread::GetChannelCount(u32 ch)
{
	u32 res = 0xdeafbeef;

	switch (ch)
	{
	case SPU_WrSRR0:          res = 1; break;
	case SPU_RdSRR0:          res = 1; break;
	case SPU_WrOutMbox:       res = SPU.Out_MBox.GetFreeCount(); break;
	case SPU_WrOutIntrMbox:   res = SPU.Out_IntrMBox.GetFreeCount(); break;
	case SPU_RdInMbox:        res = SPU.In_MBox.GetCount(); break;
	case MFC_RdTagStat:       res = MFC1.TagStatus.GetCount(); break;
	case MFC_RdListStallStat: res = StallStat.GetCount(); break;
	case MFC_WrTagUpdate:     res = MFC1.TagStatus.GetCount(); break;// hack
	case SPU_RdSigNotify1:    res = SPU.SNR[0].GetCount(); break;
	case SPU_RdSigNotify2:    res = SPU.SNR[1].GetCount(); break;
	case MFC_RdAtomicStat:    res = MFC1.AtomicStat.GetCount(); break;
	case SPU_RdEventStat:     res = CheckEvents() ? 1 : 0; break;

	default:
	{
		LOG_ERROR(Log::SPU, "%s error: unknown/illegal channel (%d [%s]).",
			__FUNCTION__, ch, spu_ch_name[ch]);
		return 0;
	}
	}

	//LOG_NOTICE(Log::SPU, "%s(%s) -> 0x%x", __FUNCTION__, spu_ch_name[ch], res);
	return res;
}

void SPUThread::WriteChannel(u32 ch, const u128& r)
{
	const u32 v = r._u32[3];

	//LOG_NOTICE(Log::SPU, "%s(%s): v=0x%x", __FUNCTION__, spu_ch_name[ch], v);

	switch (ch)
	{
	case SPU_WrSRR0:
		SRR0 = v & 0x3FFFC;  //LSLR & ~3
		break;
	case SPU_WrOutIntrMbox:
	{
		if (!group) // if RawSPU
		{
			if (Ini.HLELogging.GetValue()) LOG_NOTICE(Log::SPU, "SPU_WrOutIntrMbox: interrupt(v=0x%x)", v);
			while (!SPU.Out_IntrMBox.Push(v))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
				if (Emu.IsStopped())
				{
					LOG_WARNING(Log::SPU, "%s(%s) aborted", __FUNCTION__, spu_ch_name[ch]);
					return;
				}
			}
			m_intrtag[2].stat |= 1;
			if (std::shared_ptr<CPUThread> t = Emu.GetCPU().GetThread(m_intrtag[2].thread))
			{
				if (t->GetType() == CPU_THREAD_PPU)
				{
					if (t->IsAlive())
					{
						LOG_ERROR(Log::SPU, "%s(%s): interrupt thread was alive", __FUNCTION__, spu_ch_name[ch]);
						Emu.Pause();
						return;
					}
					PPUThread& ppu = *(PPUThread*)t.get();
					ppu.GPR[3] = ppu.m_interrupt_arg;
					ppu.FastCall2(vm::read32(ppu.entry), vm::read32(ppu.entry + 4));
				}
			}
		}
		else
		{
			const u8 code = v >> 24;
			if (code < 64)
			{
				/* ===== sys_spu_thread_send_event (used by spu_printf) ===== */

				u8 spup = code & 63;

				u32 data;
				if (!SPU.Out_MBox.Pop(data))
				{
					LOG_ERROR(Log::SPU, "sys_spu_thread_send_event(v=0x%x, spup=%d): Out_MBox is empty", v, spup);
					return;
				}

				if (Ini.HLELogging.GetValue())
				{
					LOG_NOTICE(Log::SPU, "sys_spu_thread_send_event(spup=%d, data0=0x%x, data1=0x%x)", spup, v & 0x00ffffff, data);
				}

				std::shared_ptr<EventPort> port = SPUPs[spup];

				std::lock_guard<std::mutex> lock(port->m_mutex);

				if (!port->eq)
				{
					LOG_WARNING(Log::SPU, "sys_spu_thread_send_event(spup=%d, data0=0x%x, data1=0x%x): event queue not connected", spup, (v & 0x00ffffff), data);
					SPU.In_MBox.PushUncond(CELL_ENOTCONN); // TODO: check error passing
					return;
				}

				if (!port->eq->events.push(SYS_SPU_THREAD_EVENT_USER_KEY, GetId(), ((u64)spup << 32) | (v & 0x00ffffff), data))
				{
					SPU.In_MBox.PushUncond(CELL_EBUSY);
					return;
				}

				SPU.In_MBox.PushUncond(CELL_OK);
				return;
			}
			else if (code < 128)
			{
				/* ===== sys_spu_thread_throw_event ===== */

				const u8 spup = code & 63;

				u32 data;
				if (!SPU.Out_MBox.Pop(data))
				{
					LOG_ERROR(Log::SPU, "sys_spu_thread_throw_event(v=0x%x, spup=%d): Out_MBox is empty", v, spup);
					return;
				}

				if (Ini.HLELogging.GetValue())
				{
					LOG_WARNING(Log::SPU, "sys_spu_thread_throw_event(spup=%d, data0=0x%x, data1=0x%x)", spup, v & 0x00ffffff, data);
				}

				std::shared_ptr<EventPort> port = SPUPs[spup];

				std::lock_guard<std::mutex> lock(port->m_mutex);

				if (!port->eq)
				{
					LOG_WARNING(Log::SPU, "sys_spu_thread_throw_event(spup=%d, data0=0x%x, data1=0x%x): event queue not connected", spup, (v & 0x00ffffff), data);
					return;
				}

				// TODO: check passing spup value
				if (!port->eq->events.push(SYS_SPU_THREAD_EVENT_USER_KEY, GetId(), ((u64)spup << 32) | (v & 0x00ffffff), data))
				{
					LOG_WARNING(Log::SPU, "sys_spu_thread_throw_event(spup=%d, data0=0x%x, data1=0x%x) failed (queue is full)", spup, (v & 0x00ffffff), data);
					return;
				}

				return;
			}
			else if (code == 128)
			{
				/* ===== sys_event_flag_set_bit ===== */
				u32 flag = v & 0xffffff;

				u32 data;
				if (!SPU.Out_MBox.Pop(data))
				{
					LOG_ERROR(Log::SPU, "sys_event_flag_set_bit(v=0x%x (flag=%d)): Out_MBox is empty", v, flag);
					return;
				}

				if (flag > 63)
				{
					LOG_ERROR(Log::SPU, "sys_event_flag_set_bit(id=%d, v=0x%x): flag > 63", data, v, flag);
					return;
				}

				//if (Ini.HLELogging.GetValue())
				{
					LOG_WARNING(Log::SPU, "sys_event_flag_set_bit(id=%d, v=0x%x (flag=%d))", data, v, flag);
				}

				std::shared_ptr<EventFlag> ef;
				if (!Emu.GetIdManager().GetIDData(data, ef))
				{
					LOG_ERROR(Log::SPU, "sys_event_flag_set_bit(id=%d, v=0x%x (flag=%d)): EventFlag not found", data, v, flag);
					SPU.In_MBox.PushUncond(CELL_ESRCH);
					return;
				}

				std::lock_guard<std::mutex> lock(ef->mutex);

				ef->flags |= (u64)1 << flag;
				if (u32 target = ef->check())
				{
					ef->signal.push(target);
				}
				SPU.In_MBox.PushUncond(CELL_OK);
				return;
			}
			else if (code == 192)
			{
				/* ===== sys_event_flag_set_bit_impatient ===== */
				u32 flag = v & 0xffffff;

				u32 data;
				if (!SPU.Out_MBox.Pop(data))
				{
					LOG_ERROR(Log::SPU, "sys_event_flag_set_bit_impatient(v=0x%x (flag=%d)): Out_MBox is empty", v, flag);
					return;
				}

				if (flag > 63)
				{
					LOG_ERROR(Log::SPU, "sys_event_flag_set_bit_impatient(id=%d, v=0x%x): flag > 63", data, v, flag);
					return;
				}

				//if (Ini.HLELogging.GetValue())
				{
					LOG_WARNING(Log::SPU, "sys_event_flag_set_bit_impatient(id=%d, v=0x%x (flag=%d))", data, v, flag);
				}

				std::shared_ptr<EventFlag> ef;
				if (!Emu.GetIdManager().GetIDData(data, ef))
				{
					LOG_WARNING(Log::SPU, "sys_event_flag_set_bit_impatient(id=%d, v=0x%x (flag=%d)): EventFlag not found", data, v, flag);
					return;
				}

				std::lock_guard<std::mutex> lock(ef->mutex);

				ef->flags |= (u64)1 << flag;
				if (u32 target = ef->check())
				{
					ef->signal.push(target);
				}
				return;
			}
			else
			{
				u32 data;
				if (SPU.Out_MBox.Pop(data))
				{
					LOG_ERROR(Log::SPU, "SPU_WrOutIntrMbox: unknown data (v=0x%x); Out_MBox = 0x%x", v, data);
				}
				else
				{
					LOG_ERROR(Log::SPU, "SPU_WrOutIntrMbox: unknown data (v=0x%x)", v);
				}
				SPU.In_MBox.PushUncond(CELL_EINVAL); // ???
				return;
			}
		}
		break;
	}

	case SPU_WrOutMbox:
	{
		while (!SPU.Out_MBox.Push(v) && !Emu.IsStopped())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		}
		break;
	}

	case MFC_WrTagMask:
	{
		MFC1.QueryMask.SetValue(v);
		break;
	}

	case MFC_WrTagUpdate:
	{
		MFC1.TagStatus.PushUncond(MFC1.QueryMask.GetValue());
		break;
	}

	case MFC_LSA:
	{
		MFC1.LSA.SetValue(v);
		break;
	}

	case MFC_EAH:
	{
		MFC1.EAH.SetValue(v);
		break;
	}

	case MFC_EAL:
	{
		MFC1.EAL.SetValue(v);
		break;
	}

	case MFC_Size:
	{
		MFC1.Size_Tag.SetValue((MFC1.Size_Tag.GetValue() & 0xffff) | (v << 16));
		break;
	}

	case MFC_TagID:
	{
		MFC1.Size_Tag.SetValue((MFC1.Size_Tag.GetValue() & ~0xffff) | (v & 0xffff));
		break;
	}


	case MFC_Cmd:
	{
		MFC1.CMDStatus.SetValue(v);
		EnqMfcCmd(MFC1);
		break;
	}

	case MFC_WrListStallAck:
	{
		if (v >= 32)
		{
			LOG_ERROR(Log::SPU, "MFC_WrListStallAck error: invalid tag(%d)", v);
			return;
		}
		StalledList temp = StallList[v];
		if (!temp.MFCArgs)
		{
			LOG_ERROR(Log::SPU, "MFC_WrListStallAck error: empty tag(%d)", v);
			return;
		}
		StallList[v].MFCArgs = nullptr;
		ListCmd(temp.lsa, temp.ea, temp.tag, temp.size, temp.cmd, *temp.MFCArgs);
		break;
	}

	case SPU_WrDec:
	{
		m_dec_start = get_time();
		m_dec_value = v;
		break;
	}

	case SPU_WrEventMask:
	{
		m_event_mask = v;
		if (v & ~(SPU_EVENT_IMPLEMENTED)) LOG_ERROR(Log::SPU, "SPU_WrEventMask: unsupported event masked (0x%x)");
		break;
	}

	case SPU_WrEventAck:
	{
		m_events &= ~v;
		break;
	}

	default:
	{
		LOG_ERROR(Log::SPU, "%s error (v=0x%x): unknown/illegal channel (%d [%s]).", __FUNCTION__, v, ch, spu_ch_name[ch]);
		break;
	}
	}

	if (Emu.IsStopped()) LOG_WARNING(Log::SPU, "%s(%s) aborted", __FUNCTION__, spu_ch_name[ch]);
}

void SPUThread::ReadChannel(u128& r, u32 ch)
{
	r.clear();
	u32& v = r._u32[3];

	switch (ch)
	{
	case SPU_RdSRR0:
		v = SRR0;
		break;
	case SPU_RdInMbox:
	{
		while (!SPU.In_MBox.Pop(v) && !Emu.IsStopped())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		}
		break;
	}

	case MFC_RdTagStat:
	{
		while (!MFC1.TagStatus.Pop(v) && !Emu.IsStopped())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		}
		break;
	}

	case MFC_RdTagMask:
	{
		v = MFC1.QueryMask.GetValue();
		break;
	}

	case SPU_RdSigNotify1:
	{
		if (cfg.value & 1)
		{
			while (!SPU.SNR[0].Pop_XCHG(v) && !Emu.IsStopped())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			}
		}
		else
		{
			while (!SPU.SNR[0].Pop(v) && !Emu.IsStopped())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			}
		}
		break;
	}

	case SPU_RdSigNotify2:
	{
		if (cfg.value & 2)
		{
			while (!SPU.SNR[1].Pop_XCHG(v) && !Emu.IsStopped())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			}
		}
		else
		{
			while (!SPU.SNR[1].Pop(v) && !Emu.IsStopped())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			}
		}
		break;
	}

	case MFC_RdAtomicStat:
	{
		while (!MFC1.AtomicStat.Pop(v) && !Emu.IsStopped())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		}
		break;
	}

	case MFC_RdListStallStat:
	{
		while (!StallStat.Pop(v) && !Emu.IsStopped())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		}
		break;
	}

	case SPU_RdDec:
	{
		v = m_dec_value - (u32)(get_time() - m_dec_start);
		break;
	}

	case SPU_RdEventMask:
	{
		v = m_event_mask;
		break;
	}

	case SPU_RdEventStat:
	{
		while (!CheckEvents() && !Emu.IsStopped())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		}
		v = m_events & m_event_mask;
		break;
	}

	case SPU_RdMachStat:
	{
		v = 1; // hack (not isolated, interrupts enabled)
		// TODO: check value
		break;
	}

	default:
	{
		LOG_ERROR(Log::SPU, "%s error: unknown/illegal channel (%d [%s]).", __FUNCTION__, ch, spu_ch_name[ch]);
		break;
	}
	}

	if (Emu.IsStopped()) LOG_WARNING(Log::SPU, "%s(%s) aborted", __FUNCTION__, spu_ch_name[ch]);

	//LOG_NOTICE(Log::SPU, "%s(%s) -> 0x%x", __FUNCTION__, spu_ch_name[ch], v);
}

void SPUThread::StopAndSignal(u32 code)
{
	SetExitStatus(code); // exit code (not status)
	// TODO: process interrupts for RawSPU

	switch (code)
	{
	case 0x001:
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		break;
	}

	case 0x002:
	{
		FastStop();
		break;
	}

	case 0x003:
	{
		auto iter = m_addr_to_hle_function_map.find(PC);
		assert(iter != m_addr_to_hle_function_map.end());

		auto return_to_caller = iter->second(*this);
		if (return_to_caller)
		{
			SetBranch(GPR[0]._u32[3] & 0x3fffc);
		}
		break;
	}

	case 0x110:
	{
		/* ===== sys_spu_thread_receive_event ===== */

		u32 spuq = 0;
		if (!SPU.Out_MBox.Pop(spuq))
		{
			LOG_ERROR(Log::SPU, "sys_spu_thread_receive_event: cannot read Out_MBox");
			SPU.In_MBox.PushUncond(CELL_EINVAL); // ???
			return;
		}

		if (SPU.In_MBox.GetCount())
		{
			LOG_ERROR(Log::SPU, "sys_spu_thread_receive_event(spuq=0x%x): In_MBox is not empty", spuq);
			SPU.In_MBox.PushUncond(CELL_EBUSY); // ???
			return;
		}

		if (Ini.HLELogging.GetValue())
		{
			LOG_NOTICE(Log::SPU, "sys_spu_thread_receive_event(spuq=0x%x)", spuq);
		}

		std::shared_ptr<EventQueue> eq;
		if (!SPUQs.GetEventQueue(FIX_SPUQ(spuq), eq))
		{
			SPU.In_MBox.PushUncond(CELL_EINVAL); // TODO: check error value
			return;
		}

		u32 tid = GetId();

		eq->sq.push(tid, eq->protocol); // add thread to sleep queue

		while (true)
		{
			u32 old_owner = eq->owner.compare_and_swap(0, tid);

			switch (s32 res = old_owner ? (old_owner == tid ? 1 : 2) : 0)
			{
			case 0:
			{
				const u32 next = eq->events.count() ? eq->sq.signal(eq->protocol) : 0;
				if (next != tid)
				{
					if (!eq->owner.compare_and_swap_test(tid, next))
					{
						assert(!"sys_spu_thread_receive_event() failed (I)");
					}
					break;
				}
				// fallthrough
			}
			case 1:
			{
				sys_event_data event;
				eq->events.pop(event);
				if (!eq->owner.compare_and_swap_test(tid, 0))
				{
					assert(!"sys_spu_thread_receive_event() failed (II)");
				}
				SPU.In_MBox.PushUncond(CELL_OK);
				SPU.In_MBox.PushUncond((u32)event.data1);
				SPU.In_MBox.PushUncond((u32)event.data2);
				SPU.In_MBox.PushUncond((u32)event.data3);
				if (!eq->sq.invalidate(tid, eq->protocol) && !eq->sq.pop(tid, eq->protocol))
				{
					assert(!"sys_spu_thread_receive_event() failed (receiving)");
				}
				return;
			}
			}

			if (!~old_owner)
			{
				if (!eq->sq.invalidate(tid, eq->protocol))
				{
					assert(!"sys_spu_thread_receive_event() failed (cancelling)");
				}
				SPU.In_MBox.PushUncond(CELL_ECANCELED);
				return;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				LOG_WARNING(Log::SPU, "sys_spu_thread_receive_event(spuq=0x%x) aborted", spuq);
				return;
			}
		}
		break;
	}

	case 0x101:
	{
		/* ===== sys_spu_thread_group_exit ===== */

		if (!group)
		{
			LOG_ERROR(Log::SPU, "sys_spu_thread_group_exit(): group not set");
			break;
		}
		else if (!SPU.Out_MBox.GetCount())
		{
			LOG_ERROR(Log::SPU, "sys_spu_thread_group_exit(): Out_MBox is empty");
		}
		else if (Ini.HLELogging.GetValue())
		{
			LOG_NOTICE(Log::SPU, "sys_spu_thread_group_exit(status=0x%x)", SPU.Out_MBox.GetValue());
		}
		
		group->m_group_exit = true;
		group->m_exit_status = SPU.Out_MBox.GetValue();
		for (auto& v : group->list)
		{
			if (std::shared_ptr<CPUThread> t = Emu.GetCPU().GetThread(v))
			{
				t->Stop();
			}
		}

		break;
	}

	case 0x102:
	{
		/* ===== sys_spu_thread_exit ===== */

		if (!SPU.Out_MBox.GetCount())
		{
			LOG_ERROR(Log::SPU, "sys_spu_thread_exit(): Out_MBox is empty");
		}
		else if (Ini.HLELogging.GetValue())
		{
			// the real exit status
			LOG_NOTICE(Log::SPU, "sys_spu_thread_exit(status=0x%x)", SPU.Out_MBox.GetValue());
		}
		SPU.Status.SetValue(SPU_STATUS_STOPPED_BY_STOP);
		Stop();
		break;
	}

	default:
		if (!SPU.Out_MBox.GetCount())
		{
			LOG_ERROR(Log::SPU, "Unknown STOP code: 0x%x (no message)", code);
		}
		else
		{
			LOG_ERROR(Log::SPU, "Unknown STOP code: 0x%x (message=0x%x)", code, SPU.Out_MBox.GetValue());
		}
		Emu.Pause();
		break;
	}
}

spu_thread::spu_thread(u32 entry, const std::string& name, u32 stack_size, u32 prio)
{
	thread = &Emu.GetCPU().AddThread(CPU_THREAD_SPU);

	thread->SetName(name);
	thread->SetEntry(entry);
	thread->SetStackSize(stack_size ? stack_size : Emu.GetInfo().GetProcParam().primary_stacksize);
	thread->SetPrio(prio ? prio : Emu.GetInfo().GetProcParam().primary_prio);

	argc = 0;
}
