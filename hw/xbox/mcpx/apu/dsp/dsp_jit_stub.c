/*
 * MCPX DSP emulator - stand-in for the JIT backend
 *
 * The JIT is a prebuilt library that is not available for every host. Fall
 * back to the interpreter where it is missing.
 *
 * Copyright (c) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "dsp_internal.h"

/* Never installed; only its address is compared against */
const DSPOps jit_dsp_ops;

void dsp_jit_init(DSPState *dsp)
{
    dsp_c_init(dsp);
}
