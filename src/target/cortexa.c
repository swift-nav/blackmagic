/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2016  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

/* This file implements debugging functionality specific to ARM
 * the Cortex-A9 core.  This should be generic to ARMv7-A as it is
 * implemented according to the "ARMv7-A Architecture Reference Manual",
 * ARM doc DDI0406C.
 *
 * Cache line length is from Cortex-A9 TRM, may differ for others.
 * Janky reset code is for Zynq-7000 which disconnects the DP from the JTAG
 * scan chain during reset.
 */
#include <assert.h>

#include "general.h"
#include "exception.h"
#include "adiv5.h"
#include "target.h"
#include "target_internal.h"

#define ZYNQ_SLCR_UNLOCK       2
#define ZYNQ_SLCR_UNLOCK_KEY   0xdf0d
#define ZYNQ_SLCR_A9_CPU_RST_CTRL 145
#define ZYNQ_SLCR_A9_CPU_RST_CTRL_A9_RST1 (1<<1)
#define ZYNQ_SLCR_A9_CPU_RST_CTRL_A9_CLKSTOP1 (1<<5)

static const char cortexa_driver_str[] = "ARM Cortex-A";

static bool cortexa_attach(target *t);
static void cortexa_detach(target *t);
static void cortexa_halt_resume(target *t, bool step);

static void cortexa_regs_read(target *t, void *data);
static void cortexa_regs_write(target *t, const void *data);
static void cortexa_regs_read_internal(target *t);
static void cortexa_regs_write_internal(target *t);

static void cortexa_reset(target *t);
static enum target_halt_reason cortexa_halt_poll(target *t, target_addr *watch);
static void cortexa_halt_request(target *t);

static int cortexa_breakwatch_set(target *t, struct breakwatch *);
static int cortexa_breakwatch_clear(target *t, struct breakwatch *);
static uint32_t bp_bas(uint32_t addr, uint8_t len);

static void apb_write(target *t, uint16_t reg, uint32_t val);
static uint32_t apb_read(target *t, uint16_t reg);
static void write_gpreg(target *t, uint8_t regno, uint32_t val);
static uint32_t read_gpreg(target *t, uint8_t regno);

struct cortexa_priv {
	volatile uint32_t *dbg;
	volatile uint32_t *slcr;
	struct {
		uint32_t r[16];
		uint32_t cpsr;
		uint32_t fpscr;
		uint64_t d[16];
	} reg_cache;
	unsigned hw_breakpoint_max;
	uint16_t hw_breakpoint_mask;
	uint32_t bcr0;
	uint32_t bvr0;
	unsigned hw_watchpoint_max;
	uint16_t hw_watchpoint_mask;
	bool mmu_fault;
};

/* This may be specific to Cortex-A9 */
#define CACHE_LINE_LENGTH        (8*4)

/* Debug APB registers */
#define DBGDIDR                  0

#define DBGVCR                   7
#define DBGVCR_R                 (1 << 0)
#define DBGVCR_SU                (1 << 1)
#define DBGVCR_SP                (1 << 3)
#define DBGVCR_SD                (1 << 4)


#define DBGDTRRX                 32 /* DCC: Host to target */
#define DBGITR                   33

#define DBGDSCR                  34
#define DBGDSCR_TXFULL           (1 << 29)
#define DBGDSCR_INSTRCOMPL       (1 << 24)
#define DBGDSCR_EXTDCCMODE_STALL (1 << 20)
#define DBGDSCR_EXTDCCMODE_FAST  (2 << 20)
#define DBGDSCR_EXTDCCMODE_MASK  (3 << 20)
#define DBGDSCR_HDBGEN           (1 << 14)
#define DBGDSCR_ITREN            (1 << 13)
#define DBGDSCR_INTDIS           (1 << 11)
#define DBGDSCR_UND_I            (1 << 8)
#define DBGDSCR_SDABORT_L        (1 << 6)
#define DBGDSCR_MOE_MASK         (0xf << 2)
#define DBGDSCR_MOE_HALT_REQ     (0x0 << 2)
#define DBGDSCR_MOE_WATCH_ASYNC  (0x2 << 2)
#define DBGDSCR_MOE_WATCH_SYNC   (0xa << 2)
#define DBGDSCR_RESTARTED        (1 << 1)
#define DBGDSCR_HALTED           (1 << 0)

#define DBGDTRTX                 35 /* DCC: Target to host */

#define DBGDRCR                  36
#define DBGDRCR_CSE              (1 << 2)
#define DBGDRCR_RRQ              (1 << 1)
#define DBGDRCR_HRQ              (1 << 0)

#define DBGBVR(i)                (64+(i))
#define DBGBCR(i)                (80+(i))
#define DBGBCR_INST_MISMATCH     (4 << 20)
#define DBGBCR_BAS_ANY           (0xf << 5)
#define DBGBCR_BAS_LOW_HW        (0x3 << 5)
#define DBGBCR_BAS_HIGH_HW       (0xc << 5)
#define DBGBCR_EN                (1 << 0)

#define DBGLAR                   (1004)
#define DBGLAR_KEY               0xC5ACCE55

#define DBGWVR(i)                (96+(i))
#define DBGWCR(i)                (112+(i))
#define DBGWCR_LSC_LOAD          (0b01 << 3)
#define DBGWCR_LSC_STORE         (0b10 << 3)
#define DBGWCR_LSC_ANY           (0b11 << 3)
#define DBGWCR_BAS_BYTE          (0b0001 << 5)
#define DBGWCR_BAS_HALFWORD      (0b0011 << 5)
#define DBGWCR_BAS_WORD          (0b1111 << 5)
#define DBGWCR_PAC_ANY           (0b11 << 1)
#define DBGWCR_EN                (1 << 0)

/* Instruction encodings for accessing the coprocessor interface */
#define MCR 0xee000010
#define MRC 0xee100010
#define CPREG(coproc, opc1, rt, crn, crm, opc2) \
	(((opc1) << 21) | ((crn) << 16) | ((rt) << 12) | \
        ((coproc) << 8) | ((opc2) << 5) | (crm))

/* Debug registers CP14 */
#define DBGDTRRXint CPREG(14, 0, 0, 0, 5, 0)
#define DBGDTRTXint CPREG(14, 0, 0, 0, 5, 0)

/* Address translation registers CP15 */
#define PAR         CPREG(15, 0, 0, 7, 4, 0)
#define ATS1CPR     CPREG(15, 0, 0, 7, 8, 0)

/* Cache management registers CP15 */
#define ICIALLU     CPREG(15, 0, 0, 7, 5, 0)
#define DCCIMVAC    CPREG(15, 0, 0, 7, 14, 1)
#define DCCMVAC     CPREG(15, 0, 0, 7, 10, 1)

/* Thumb mode bit in CPSR */
#define CPSR_THUMB               (1 << 5)

/* GDB register map / target description */
static const char tdesc_cortex_a[] =
	"<?xml version=\"1.0\"?>"
	"<!DOCTYPE feature SYSTEM \"gdb-target.dtd\">"
	"<target>"
	"  <architecture>arm</architecture>"
	"  <feature name=\"org.gnu.gdb.arm.core\">"
	"    <reg name=\"r0\" bitsize=\"32\"/>"
	"    <reg name=\"r1\" bitsize=\"32\"/>"
	"    <reg name=\"r2\" bitsize=\"32\"/>"
	"    <reg name=\"r3\" bitsize=\"32\"/>"
	"    <reg name=\"r4\" bitsize=\"32\"/>"
	"    <reg name=\"r5\" bitsize=\"32\"/>"
	"    <reg name=\"r6\" bitsize=\"32\"/>"
	"    <reg name=\"r7\" bitsize=\"32\"/>"
	"    <reg name=\"r8\" bitsize=\"32\"/>"
	"    <reg name=\"r9\" bitsize=\"32\"/>"
	"    <reg name=\"r10\" bitsize=\"32\"/>"
	"    <reg name=\"r11\" bitsize=\"32\"/>"
	"    <reg name=\"r12\" bitsize=\"32\"/>"
	"    <reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
	"    <reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>"
	"    <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
	"    <reg name=\"cpsr\" bitsize=\"32\"/>"
	"  </feature>"
	"  <feature name=\"org.gnu.gdb.arm.vfp\">"
	"    <reg name=\"fpscr\" bitsize=\"32\"/>"
	"    <reg name=\"d0\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d1\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d2\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d3\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d4\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d5\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d6\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d7\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d8\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d9\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d10\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d11\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d12\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d13\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d14\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d15\" bitsize=\"64\" type=\"float\"/>"
	"  </feature>"
	"</target>";

static void apb_write(target *t, uint16_t reg, uint32_t val)
{
	struct cortexa_priv *priv = t->priv;
    priv->dbg[reg] = val;
}

static uint32_t apb_read(target *t, uint16_t reg)
{
	struct cortexa_priv *priv = t->priv;
	return priv->dbg[reg];
}

static uint32_t va_to_pa(target *t, uint32_t va)
{
	struct cortexa_priv *priv = t->priv;
	write_gpreg(t, 0, va);
	apb_write(t, DBGITR, MCR | ATS1CPR);
	apb_write(t, DBGITR, MRC | PAR);
	uint32_t par = read_gpreg(t, 0);
	if (par & 1)
		priv->mmu_fault = true;
	uint32_t pa = (par & ~0xfff) | (va & 0xfff);
	DEBUG("%s: VA = 0x%08"PRIx32", PAR = 0x%08"PRIx32", PA = 0x%08"PRIX32"\n",
              __func__, va, par, pa);
	return pa;
}

void cortexa_cache_clean(target *t, target_addr src, size_t len)
{
	/* Clean cache before reading */
	for (uint32_t cl = src & ~(CACHE_LINE_LENGTH-1);
	     cl < src + len; cl += CACHE_LINE_LENGTH) {
		write_gpreg(t, 0, cl);
		apb_write(t, DBGITR, MCR | DCCMVAC);
	}
}

static void cortexa_slow_mem_read(target *t, void *dest, target_addr src, size_t len)
{
	struct cortexa_priv *priv = t->priv;
	unsigned words = (len + (src & 3) + 3) / 4;
	uint32_t dest32[words];

	/* Set r0 to aligned src address */
	write_gpreg(t, 0, src & ~3);

	/* Switch to fast DCC mode */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_FAST;
	apb_write(t, DBGDSCR, dbgdscr);

	apb_write(t, DBGITR, 0xecb05e01); /* ldc 14, cr5, [r0], #4 */
	/* According to the ARMv7-A ARM, in fast mode, the first read from
	 * DBGDTRTX is  supposed to block until the instruction is complete,
	 * but we see the first read returns junk, so it's read here and
	 * ignored. */
	apb_read(t, DBGDTRTX);

	for (unsigned i = 0; i < words; i++)
		dest32[i] = apb_read(t, DBGDTRTX);

	memcpy(dest, (uint8_t*)dest32 + (src & 3), len);

	/* Switch back to stalling DCC mode */
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_STALL;
	apb_write(t, DBGDSCR, dbgdscr);

	if (apb_read(t, DBGDSCR) & DBGDSCR_SDABORT_L) {
		/* Memory access aborted, flag a fault */
		apb_write(t, DBGDRCR, DBGDRCR_CSE);
		priv->mmu_fault = true;
	} else {
		apb_read(t, DBGDTRTX);
	}
}

static void cortexa_slow_mem_write_bytes(target *t, target_addr dest, const uint8_t *src, size_t len)
{
	struct cortexa_priv *priv = t->priv;

	/* Set r13 to dest address */
	write_gpreg(t, 13, dest);

	while (len--) {
		write_gpreg(t, 0, *src++);
		apb_write(t, DBGITR, 0xe4cd0001); /* strb r0, [sp], #1 */
		if (apb_read(t, DBGDSCR) & DBGDSCR_SDABORT_L) {
			/* Memory access aborted, flag a fault */
			apb_write(t, DBGDRCR, DBGDRCR_CSE);
			priv->mmu_fault = true;
			return;
		}
	}
}

static void cortexa_slow_mem_write(target *t, target_addr dest, const void *src, size_t len)
{
	struct cortexa_priv *priv = t->priv;
	if (len == 0)
		return;

	if ((dest & 3) || (len & 3)) {
		cortexa_slow_mem_write_bytes(t, dest, src, len);
		return;
	}

	write_gpreg(t, 0, dest);
	const uint32_t *src32 = src;

	/* Switch to fast DCC mode */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_FAST;
	apb_write(t, DBGDSCR, dbgdscr);

	apb_write(t, DBGITR, 0xeca05e01); /* stc 14, cr5, [r0], #4 */

	for (; len; len -= 4)
		apb_write(t, DBGDTRRX, *src32++);

	/* Switch back to stalling DCC mode */
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_STALL;
	apb_write(t, DBGDSCR, dbgdscr);

	if (apb_read(t, DBGDSCR) & DBGDSCR_SDABORT_L) {
		/* Memory access aborted, flag a fault */
		apb_write(t, DBGDRCR, DBGDRCR_CSE);
		priv->mmu_fault = true;
	}
}

static bool cortexa_check_error(target *t)
{
	struct cortexa_priv *priv = t->priv;
	bool err = priv->mmu_fault;
	priv->mmu_fault = false;
	return err;
}

void zynq_amp_clock_wait(target *t)
{
	struct cortexa_priv *priv = t->priv;
	/* Don't touch the CPU while it's clock gated.  This locks up the
	 * bus and is unrecoverable.  The potential problem here is that
	 * the Linux system could gate the clock after we've passed this
	 * check if, for example, the remoteproc driver is unloaded.
	 */
	while (priv->slcr[ZYNQ_SLCR_A9_CPU_RST_CTRL] &
               ZYNQ_SLCR_A9_CPU_RST_CTRL_A9_CLKSTOP1) {
		platform_delay(1);
	}
}

bool cortexa_probe(volatile uint32_t *dbg, volatile uint32_t *slcr)
{
	target *t;

	t = target_new();
	struct cortexa_priv *priv = calloc(1, sizeof(*priv));
	t->priv = priv;
	t->priv_free = free;

	priv->dbg = dbg;
	priv->slcr = slcr;
	t->mem_read = cortexa_slow_mem_read;
	t->mem_write = cortexa_slow_mem_write;

	zynq_amp_clock_wait(t);

	/* Set up APB CSW, we won't touch this again */
	//uint32_t csw = apb->csw | ADIV5_AP_CSW_SIZE_WORD;
	//adiv5_ap_write(apb, ADIV5_AP_CSW, csw);
	uint32_t dbgdidr = apb_read(t, DBGDIDR);
	priv->hw_breakpoint_max = ((dbgdidr >> 24) & 15)+1;
	priv->hw_watchpoint_max = ((dbgdidr >> 28) & 15)+1;

	t->check_error = cortexa_check_error;

	t->driver = cortexa_driver_str;

	t->attach = cortexa_attach;
	t->detach = cortexa_detach;

	t->tdesc = tdesc_cortex_a;
	t->regs_read = cortexa_regs_read;
	t->regs_write = cortexa_regs_write;

	t->reset = cortexa_reset;
	t->halt_request = cortexa_halt_request;
	t->halt_poll = cortexa_halt_poll;
	t->halt_resume = cortexa_halt_resume;
	t->regs_size = sizeof(priv->reg_cache);

	t->breakwatch_set = cortexa_breakwatch_set;
	t->breakwatch_clear = cortexa_breakwatch_clear;

	return true;
}

bool cortexa_attach(target *t)
{
	struct cortexa_priv *priv = t->priv;
	int tries;

	/* Clear any pending fault condition */
	target_check_error(t);

	zynq_amp_clock_wait(t);

	/* Unlock access to MMIO interface */
	apb_write(t, DBGLAR, DBGLAR_KEY);

	/* Enable halting debug mode */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	dbgdscr |= DBGDSCR_HDBGEN | DBGDSCR_ITREN;
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_STALL;
	apb_write(t, DBGDSCR, dbgdscr);
	DEBUG("DBGDSCR = 0x%08"PRIx32"\n", dbgdscr);

	target_halt_request(t);
	tries = 10;
	while(!platform_srst_get_val() && !target_halt_poll(t, NULL) && --tries)
		platform_delay(200);
	if(!tries)
		return false;

	/* Enable vector catch on Undefined, Prefetch abort, Data abort */
	apb_write(t, DBGVCR, DBGVCR_SU | DBGVCR_SP | DBGVCR_SD);

	/* Clear any stale breakpoints */
	for(unsigned i = 0; i < priv->hw_breakpoint_max; i++) {
		apb_write(t, DBGBCR(i), 0);
	}
	priv->hw_breakpoint_mask = 0;
	priv->bcr0 = 0;

	platform_srst_set_val(false);

	return true;
}

void cortexa_detach(target *t)
{
	struct cortexa_priv *priv = t->priv;

	/* Clear any stale breakpoints */
	for(unsigned i = 0; i < priv->hw_breakpoint_max; i++) {
		apb_write(t, DBGBCR(i), 0);
	}

	/* Disable vector catch */
	apb_write(t, DBGVCR, 0);

	/* Restore any clobbered registers */
	cortexa_regs_write_internal(t);
	/* Invalidate cache */
	apb_write(t, DBGITR, MCR | ICIALLU);

	platform_timeout to;
	platform_timeout_set(&to, 200);

	/* Wait for instruction to complete */
	uint32_t dbgdscr;
	do {
		dbgdscr = apb_read(t, DBGDSCR);
	} while (!(dbgdscr & DBGDSCR_INSTRCOMPL) &&
	         !platform_timeout_is_expired(&to));

	/* Disable halting debug mode */
	dbgdscr &= ~(DBGDSCR_HDBGEN | DBGDSCR_ITREN);
	apb_write(t, DBGDSCR, dbgdscr);
	/* Clear sticky error and resume */
	apb_write(t, DBGDRCR, DBGDRCR_CSE | DBGDRCR_RRQ);
}


static uint32_t read_gpreg(target *t, uint8_t regno)
{
	/* To read a register we use DBGITR to load an MCR instruction
	 * that sends the value via DCC DBGDTRTX using the CP14 interface.
	 */
	uint32_t instr = MCR | DBGDTRTXint | ((regno & 0xf) << 12);
	apb_write(t, DBGITR, instr);
	/* Return value read from DCC channel */
	return apb_read(t, DBGDTRTX);
}

static void write_gpreg(target *t, uint8_t regno, uint32_t val)
{
	/* Write value to DCC channel */
	apb_write(t, DBGDTRRX, val);
	/* Run instruction to load register */
	uint32_t instr = MRC | DBGDTRRXint | ((regno & 0xf) << 12);
	apb_write(t, DBGITR, instr);
}

static void cortexa_regs_read(target *t, void *data)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	memcpy(data, &priv->reg_cache, t->regs_size);
}

static void cortexa_regs_write(target *t, const void *data)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	memcpy(&priv->reg_cache, data, t->regs_size);
}

static void cortexa_regs_read_internal(target *t)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	/* Read general purpose registers */
	for (int i = 0; i < 15; i++) {
		priv->reg_cache.r[i] = read_gpreg(t, i);
	}
	/* Read PC, via r0.  MCR is UNPREDICTABLE for Rt = r15. */
	apb_write(t, DBGITR, 0xe1a0000f); /* mov r0, pc */
	priv->reg_cache.r[15] = read_gpreg(t, 0);
	/* Read CPSR */
	apb_write(t, DBGITR, 0xE10F0000); /* mrs r0, CPSR */
	priv->reg_cache.cpsr = read_gpreg(t, 0);
	/* Read FPSCR */
	apb_write(t, DBGITR, 0xeef10a10); /* vmrs r0, fpscr */
	priv->reg_cache.fpscr = read_gpreg(t, 0);
	/* Read out VFP registers */
	for (int i = 0; i < 16; i++) {
		/* Read D[i] to R0/R1 */
		apb_write(t, DBGITR, 0xEC510B10 | i); /* vmov r0, r1, d0 */
		priv->reg_cache.d[i] = ((uint64_t)read_gpreg(t, 1) << 32) | read_gpreg(t, 0);
	}
	priv->reg_cache.r[15] -= (priv->reg_cache.cpsr & CPSR_THUMB) ? 4 : 8;
}

static void cortexa_regs_write_internal(target *t)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	/* First write back floats */
	for (int i = 0; i < 16; i++) {
		write_gpreg(t, 1, priv->reg_cache.d[i] >> 32);
		write_gpreg(t, 0, priv->reg_cache.d[i]);
		apb_write(t, DBGITR, 0xec410b10 | i); /* vmov d[i], r0, r1 */
	}
	/* Write back FPSCR */
	write_gpreg(t, 0, priv->reg_cache.fpscr);
	apb_write(t, DBGITR, 0xeee10a10); /* vmsr fpscr, r0 */
	/* Write back the CPSR */
	write_gpreg(t, 0, priv->reg_cache.cpsr);
	apb_write(t, DBGITR, 0xe12ff000); /* msr CPSR_fsxc, r0 */
	/* Write back PC, via r0.  MRC clobbers CPSR instead */
	write_gpreg(t, 0, priv->reg_cache.r[15]);
	apb_write(t, DBGITR, 0xe1a0f000); /* mov pc, r0 */
	/* Finally the GP registers now that we're done using them */
	for (int i = 0; i < 15; i++) {
		write_gpreg(t, i, priv->reg_cache.r[i]);
	}
}

static bool step_one_instruction(target *t)
{
	cortexa_halt_resume(t, true);
	enum target_halt_reason r;
	do {
		r = cortexa_halt_poll(t, NULL);
	} while(r == TARGET_HALT_RUNNING);
	return r == TARGET_HALT_BREAKPOINT;
}

static void cortexa_reset(target *t)
{
	uint32_t dbgvcr = apb_read(t, DBGVCR);

	/* Disable watchdog */
	target_mem_write32(t, 0xf8f00634, 0x12345678);
	target_mem_write32(t, 0xf8f00634, 0x87654321);

	/* Trap on reset only */
	apb_write(t, DBGVCR, DBGVCR_R);

	/* Unload all Linux drivers to reset slave core */
	system("/etc/init.d/S83endpoint_adapter_rpmsg_piksi101 stop");
	system("/etc/init.d/S83endpoint_adapter_rpmsg_piksi100 stop");
	platform_delay(500);
	system("modprobe -r rpmsg_piksi");
	system("modprobe -r zynq_remoteproc");
	platform_delay(500);

	/* Reload Linux driver to load firmware and release from reset.
	 * DBGVCR will trap us on the reset vector containing the
	 * boot trampoline. */
	system("modprobe rpmsg_piksi");
	system("/etc/init.d/S83endpoint_adapter_rpmsg_piksi100 start");
	system("/etc/init.d/S83endpoint_adapter_rpmsg_piksi101 start");
	system("modprobe zynq_remoteproc");
	platform_delay(1000);

	/* Ensure we're not clock gated before we talk */
	zynq_amp_clock_wait(t);

	/* Update our register cache with the newly reset values */
	cortexa_regs_read_internal(t);

	/* Step through Linux's boot trampoline */
	/* From Linux kernel, arch/arm/mach-zynq/platsmp.c:62-67
	 *   This is elegant way how to jump to any address
	 *   0x0: Load address at 0x8 to r0
	 *   0x4: Jump by mov instruction
	 *   0x8: Jumping address
	 * To get to the first firmware instruction, we need to disable traps,
	 * and step over 2 instructions.
	 */
	apb_write(t, DBGVCR, 0);
	assert(step_one_instruction(t));
	assert(step_one_instruction(t));

	/* Restore traps */
	apb_write(t, DBGVCR, dbgvcr);
}

static void cortexa_halt_request(target *t)
{
	volatile struct exception e;
	TRY_CATCH (e, EXCEPTION_TIMEOUT) {
		apb_write(t, DBGDRCR, DBGDRCR_HRQ);
	}
	if (e.type) {
		tc_printf(t, "Timeout sending interrupt, is target in WFI?\n");
	}
}

static enum target_halt_reason cortexa_halt_poll(target *t, target_addr *watch)
{
	volatile uint32_t dbgdscr = 0;
	volatile struct exception e;
	TRY_CATCH (e, EXCEPTION_ALL) {
		/* If this times out because the target is in WFI then
		 * the target is still running. */
		dbgdscr = apb_read(t, DBGDSCR);
	}
	switch (e.type) {
	case EXCEPTION_ERROR:
		/* Oh crap, there's no recovery from this... */
		target_list_free();
		return TARGET_HALT_ERROR;
	case EXCEPTION_TIMEOUT:
		/* Timeout isn't a problem, target could be in WFI */
		return TARGET_HALT_RUNNING;
	}

	if (!(dbgdscr & DBGDSCR_HALTED)) /* Not halted */
		return TARGET_HALT_RUNNING;

	DEBUG("%s: DBGDSCR = 0x%08"PRIx32"\n", __func__, dbgdscr);
	/* Reenable DBGITR */
	dbgdscr |= DBGDSCR_ITREN;
	apb_write(t, DBGDSCR, dbgdscr);

	/* Find out why we halted */
	enum target_halt_reason reason = TARGET_HALT_BREAKPOINT;
	switch (dbgdscr & DBGDSCR_MOE_MASK) {
	case DBGDSCR_MOE_HALT_REQ:
		reason = TARGET_HALT_REQUEST;
		break;
	case DBGDSCR_MOE_WATCH_ASYNC:
	case DBGDSCR_MOE_WATCH_SYNC:
		/* How do we know which watchpoint was hit? */
		/* If there is only one set, it's that */
		for (struct breakwatch *bw = t->bw_list; bw; bw = bw->next) {
			if ((bw->type != TARGET_WATCH_READ) &&
			    (bw->type != TARGET_WATCH_WRITE) &&
			    (bw->type != TARGET_WATCH_ACCESS))
				continue;
			if (reason == TARGET_HALT_WATCHPOINT) {
				/* More than one watchpoint set,
				 * we can't tell which triggered. */
				reason = TARGET_HALT_BREAKPOINT;
				break;
			}
			*watch = bw->addr;
			reason = TARGET_HALT_WATCHPOINT;
		}
		break;
	default:
		reason = TARGET_HALT_BREAKPOINT;
	}

	cortexa_regs_read_internal(t);

	return reason;
}

void cortexa_halt_resume(target *t, bool step)
{
	struct cortexa_priv *priv = t->priv;
	/* Set breakpoint comarator for single stepping if needed */
	if (step) {
		uint32_t addr = priv->reg_cache.r[15];
		uint32_t bas = bp_bas(addr, (priv->reg_cache.cpsr & CPSR_THUMB) ? 2 : 4);
		DEBUG("step 0x%08"PRIx32"  %"PRIx32"\n", addr, bas);
		/* Set match any breakpoint */
		apb_write(t, DBGBVR(0), priv->reg_cache.r[15] & ~3);
		apb_write(t, DBGBCR(0), DBGBCR_INST_MISMATCH | bas |
		                             DBGBCR_EN);
	} else {
		apb_write(t, DBGBVR(0), priv->bvr0);
		apb_write(t, DBGBCR(0), priv->bcr0);
	}

	/* Write back register cache */
	cortexa_regs_write_internal(t);

	apb_write(t, DBGITR, MCR | ICIALLU); /* invalidate cache */

	platform_timeout to;
	platform_timeout_set(&to, 200);

	/* Wait for instruction to complete */
	uint32_t dbgdscr;
	do {
		dbgdscr = apb_read(t, DBGDSCR);
	} while (!(dbgdscr & DBGDSCR_INSTRCOMPL) &&
	         !platform_timeout_is_expired(&to));

	 /* Disable DBGITR.  Not sure why, but RRQ is ignored otherwise. */
	if (step)
		dbgdscr |= DBGDSCR_INTDIS;
	else
		dbgdscr &= ~DBGDSCR_INTDIS;
	dbgdscr &= ~DBGDSCR_ITREN;
	apb_write(t, DBGDSCR, dbgdscr);

	do {
		apb_write(t, DBGDRCR, DBGDRCR_CSE | DBGDRCR_RRQ);
		dbgdscr = apb_read(t, DBGDSCR);
		DEBUG("%s: DBGDSCR = 0x%08"PRIx32"\n", __func__, dbgdscr);
	} while (!(dbgdscr & DBGDSCR_RESTARTED) &&
	         !platform_timeout_is_expired(&to));
}

/* Breakpoints */
static uint32_t bp_bas(uint32_t addr, uint8_t len)
{
	if (len == 4)
		return DBGBCR_BAS_ANY;
	else if (addr & 2)
		return DBGBCR_BAS_HIGH_HW;
	else
		return DBGBCR_BAS_LOW_HW;
}

static int cortexa_breakwatch_set(target *t, struct breakwatch *bw)
{
	struct cortexa_priv *priv = t->priv;
	unsigned i;

	switch (bw->type) {
	case TARGET_BREAK_SOFT:/*
		switch (bw->size) {
		case 2:
			bw->reserved[0] = target_mem_read16(t, bw->addr);
			target_mem_write16(t, bw->addr, 0xBE00);
			return target_check_error(t);
		case 4:
			bw->reserved[0] = target_mem_read32(t, bw->addr);
			target_mem_write32(t, bw->addr, 0xE1200070);
			return target_check_error(t);
		default:
			return -1;
		}*/
	case TARGET_BREAK_HARD:
		if ((bw->size != 4) && (bw->size != 2))
			return -1;

		for (i = 0; i < priv->hw_breakpoint_max; i++)
			if ((priv->hw_breakpoint_mask & (1 << i)) == 0)
				break;

		if (i == priv->hw_breakpoint_max)
			return -1;

		bw->reserved[0] = i;
		priv->hw_breakpoint_mask |= (1 << i);

		uint32_t addr = va_to_pa(t, bw->addr);
		uint32_t bcr =  bp_bas(addr, bw->size) | DBGBCR_EN;
		apb_write(t, DBGBVR(i), addr & ~3);
		apb_write(t, DBGBCR(i), bcr);
		if (i == 0) {
			priv->bcr0 = bcr;
			priv->bvr0 = addr & ~3;
		}

		return 0;

	case TARGET_WATCH_WRITE:
	case TARGET_WATCH_READ:
	case TARGET_WATCH_ACCESS:
		for (i = 0; i < priv->hw_watchpoint_max; i++)
			if ((priv->hw_watchpoint_mask & (1 << i)) == 0)
				break;

		if (i == priv->hw_watchpoint_max)
			return -1;

		bw->reserved[0] = i;
		priv->hw_watchpoint_mask |= (1 << i);

		{
			uint32_t wcr = DBGWCR_PAC_ANY | DBGWCR_EN;
			uint32_t bas = 0;
			switch(bw->size) { /* Convert bytes size to BAS bits */
				case 1: bas = DBGWCR_BAS_BYTE; break;
				case 2: bas = DBGWCR_BAS_HALFWORD; break;
				case 4: bas = DBGWCR_BAS_WORD; break;
				default:
					return -1;
			}
			/* Apply shift based on address LSBs */
			wcr |= bas << (bw->addr & 3);

			switch (bw->type) { /* Convert gdb type */
				case TARGET_WATCH_WRITE: wcr |= DBGWCR_LSC_STORE; break;
				case TARGET_WATCH_READ: wcr |= DBGWCR_LSC_LOAD; break;
				case TARGET_WATCH_ACCESS: wcr |= DBGWCR_LSC_ANY; break;
				default:
					return -1;
			}

			apb_write(t, DBGWCR(i), wcr);
			apb_write(t, DBGWVR(i), bw->addr & ~3);
			DEBUG("Watchpoint set WCR = 0x%08x, WVR = %08x\n",
				apb_read(t, DBGWCR(i)),
				apb_read(t, DBGWVR(i)));
		}
		return 0;

	default:
		return 1;
	}
}

static int cortexa_breakwatch_clear(target *t, struct breakwatch *bw)
{
	struct cortexa_priv *priv = t->priv;
	unsigned i = bw->reserved[0];
	switch (bw->type) {
	case TARGET_BREAK_SOFT:/*
		switch (bw->size) {
		case 2:
			target_mem_write16(t, bw->addr, i);
			return target_check_error(t);
		case 4:
			target_mem_write32(t, bw->addr, i);
			return target_check_error(t);
		default:
			return -1;
		}*/
	case TARGET_BREAK_HARD:
		priv->hw_breakpoint_mask &= ~(1 << i);
		apb_write(t, DBGBCR(i), 0);
		if (i == 0)
			priv->bcr0 = 0;
		return 0;
	case TARGET_WATCH_WRITE:
	case TARGET_WATCH_READ:
	case TARGET_WATCH_ACCESS:
		priv->hw_watchpoint_mask &= ~(1 << i);
		apb_write(t, DBGWCR(i), 0);
		return 0;
	default:
		return 1;
	}
}
