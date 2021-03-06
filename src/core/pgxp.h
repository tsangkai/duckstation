/***************************************************************************
 *   Original copyright notice from PGXP code from Beetle PSX.             *
 *   Copyright (C) 2016 by iCatButler                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#pragma once
#include "types.h"

namespace PGXP {

void Initialize();

// -- GTE functions
// Transforms
void GTE_PushSXYZ2f(float _x, float _y, float _z, unsigned int _v);
void GTE_PushSXYZ2s(s64 _x, s64 _y, s64 _z, u32 v);
int GTE_NCLIP_valid(u32 sxy0, u32 sxy1, u32 sxy2);
float GTE_NCLIP();

// Data transfer tracking
void CPU_MFC2(u32 instr, u32 rtVal, u32 rdVal); // copy GTE data reg to GPR reg (MFC2)
void CPU_MTC2(u32 instr, u32 rdVal, u32 rtVal); // copy GPR reg to GTE data reg (MTC2)
void CPU_CFC2(u32 instr, u32 rtVal, u32 rdVal); // copy GTE ctrl reg to GPR reg (CFC2)
void CPU_CTC2(u32 instr, u32 rdVal, u32 rtVal); // copy GPR reg to GTE ctrl reg (CTC2)
// Memory Access
void CPU_LWC2(u32 instr, u32 rtVal, u32 addr); // copy memory to GTE reg
void CPU_SWC2(u32 instr, u32 rtVal, u32 addr); // copy GTE reg to memory

bool GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y, float* out_w);

// -- CPU functions
void CPU_LW(u32 instr, u32 rtVal, u32 addr);
void CPU_LHx(u32 instr, u32 rtVal, u32 addr);
void CPU_LBx(u32 instr, u32 rtVal, u32 addr);
void CPU_SB(u32 instr, u8 rtVal, u32 addr);
void CPU_SH(u32 instr, u16 rtVal, u32 addr);
void CPU_SW(u32 instr, u32 rtVal, u32 addr);

} // namespace PGXP