// SPDX-License-Identifier: GPL-2.0-or-later

/**************************************************************************
*   Copyright (C) 2005 by Dominic Rath                                    *
*   Dominic.Rath@gmx.de                                                   *
*                                                                         *
*   Copyright (C) 2006 by Magnus Lundin                                   *
*   lundin@mlu.mine.nu                                                    *
*                                                                         *
*   Copyright (C) 2008 by Spencer Oliver                                  *
*   spen@spen-soft.co.uk                                                  *
*                                                                         *
*                                                                         *
*   Cortex-M3(tm) TRM, ARM DDI 0337E (r1p1) and 0337G (r2p0)              *
*                                                                         *
*-------------------------------------------------------------------------*
*                                                                         *
*   This file is based on cortex_m.c and adds functionality for the       *
*   Nuvoton NUMICRO series.This file was created based on cortex_m.c.     *
*                                                                         *
*   Copyright (C) 2023 by Nuvoton Technology Corporation                  *
*   ccli0 <ccli0@nuvoton.com>                                             *
*                                                                         *
**************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag/interface.h"
#include "breakpoints.h"
#include "cortex_m.h"
#include "target_request.h"
#include "target_type.h"
#include "arm_adi_v5.h"
#include "arm_disassembler.h"
#include "register.h"
#include "arm_opcodes.h"
#include "arm_semihosting.h"
#include "smp.h"
#include <helper/time_support.h>
#include <rtt/rtt.h>

/* NOTE:  most of this should work fine for the Cortex-M1 and
 * Cortex-M0 cores too, although they're ARMv6-M not ARMv7-M.
 * Some differences:  M0/M1 doesn't have FPB remapping or the
 * DWT tracing/profiling support.  (So the cycle counter will
 * not be usable; the other stuff isn't currently used here.)
 *
 * Although there are some workarounds for errata seen only in r0p0
 * silicon, such old parts are hard to find and thus not much tested
 * any longer.
 */

/* Timeout for register r/w */
#define DHCSR_S_REGRDY_TIMEOUT (500)

/* forward declarations */
static int cortex_m_store_core_reg_u32(struct target *target,
		uint32_t num, uint32_t value);

/** DCB DHCSR register contains S_RETIRE_ST and S_RESET_ST bits cleared
 *  on a read. Call this helper function each time DHCSR is read
 *  to preserve S_RESET_ST state in case of a reset event was detected.
 */
static inline void cortex_m_cumulate_dhcsr_sticky(struct cortex_m_common *cortex_m,
		uint32_t dhcsr)
{
	cortex_m->dcb_dhcsr_cumulated_sticky |= dhcsr;
}

/** Read DCB DHCSR register to cortex_m->dcb_dhcsr and cumulate
 * sticky bits in cortex_m->dcb_dhcsr_cumulated_sticky
 */
static int cortex_m_read_dhcsr_atomic_sticky(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);

	int retval = mem_ap_read_atomic_u32(armv7m->debug_ap, DCB_DHCSR,
				&cortex_m->dcb_dhcsr);
	if (retval != ERROR_OK)
		return retval;

	cortex_m_cumulate_dhcsr_sticky(cortex_m, cortex_m->dcb_dhcsr);
	return ERROR_OK;
}

static int cortex_m_load_core_reg_u32(struct target *target,
		uint32_t regsel, uint32_t *value)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	int retval;
	uint32_t dcrdr, tmp_value;
	int64_t then;

	/* because the DCB_DCRDR is used for the emulated dcc channel
	 * we have to save/restore the DCB_DCRDR when used */
	if (target->dbg_msg_enabled) {
		retval = mem_ap_read_u32(armv7m->debug_ap, DCB_DCRDR, &dcrdr);
		if (retval != ERROR_OK)
			return retval;
	}

	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DCRSR, regsel);
	if (retval != ERROR_OK)
		return retval;

	/* check if value from register is ready and pre-read it */
	then = timeval_ms();
	while (1) {
		retval = mem_ap_read_u32(armv7m->debug_ap, DCB_DHCSR,
								 &cortex_m->dcb_dhcsr);
		if (retval != ERROR_OK)
			return retval;
		retval = mem_ap_read_atomic_u32(armv7m->debug_ap, DCB_DCRDR,
										&tmp_value);
		if (retval != ERROR_OK)
			return retval;
		cortex_m_cumulate_dhcsr_sticky(cortex_m, cortex_m->dcb_dhcsr);
		if (cortex_m->dcb_dhcsr & S_REGRDY)
			break;
		cortex_m->slow_register_read = true; /* Polling (still) needed. */
		if (timeval_ms() > then + DHCSR_S_REGRDY_TIMEOUT) {
			LOG_TARGET_ERROR(target, "Timeout waiting for DCRDR transfer ready");
			return ERROR_TIMEOUT_REACHED;
		}
		keep_alive();
	}

	*value = tmp_value;

	if (target->dbg_msg_enabled) {
		/* restore DCB_DCRDR - this needs to be in a separate
		 * transaction otherwise the emulated DCC channel breaks */
		if (retval == ERROR_OK)
			retval = mem_ap_write_atomic_u32(armv7m->debug_ap, DCB_DCRDR, dcrdr);
	}

	return retval;
}

static int cortex_m_slow_read_all_regs(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	const unsigned int num_regs = armv7m->arm.core_cache->num_regs;

	/* Opportunistically restore fast read, it'll revert to slow
	 * if any register needed polling in cortex_m_load_core_reg_u32(). */
	cortex_m->slow_register_read = false;

	for (unsigned int reg_id = 0; reg_id < num_regs; reg_id++) {
		struct reg *r = &armv7m->arm.core_cache->reg_list[reg_id];
		if (r->exist) {
			int retval = armv7m->arm.read_core_reg(target, r, reg_id, ARM_MODE_ANY);
			if (retval != ERROR_OK)
				return retval;
		}
	}

	if (!cortex_m->slow_register_read)
		LOG_TARGET_DEBUG(target, "Switching back to fast register reads");

	return ERROR_OK;
}

static int cortex_m_queue_reg_read(struct target *target, uint32_t regsel,
		uint32_t *reg_value, uint32_t *dhcsr)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	int retval;

	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DCRSR, regsel);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_read_u32(armv7m->debug_ap, DCB_DHCSR, dhcsr);
	if (retval != ERROR_OK)
		return retval;

	return mem_ap_read_u32(armv7m->debug_ap, DCB_DCRDR, reg_value);
}

static int cortex_m_fast_read_all_regs(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	int retval;
	uint32_t dcrdr;

	/* because the DCB_DCRDR is used for the emulated dcc channel
	 * we have to save/restore the DCB_DCRDR when used */
	if (target->dbg_msg_enabled) {
		retval = mem_ap_read_u32(armv7m->debug_ap, DCB_DCRDR, &dcrdr);
		if (retval != ERROR_OK)
			return retval;
	}

	const unsigned int num_regs = armv7m->arm.core_cache->num_regs;
	const unsigned int n_r32 = ARMV7M_LAST_REG - ARMV7M_CORE_FIRST_REG + 1
							   + ARMV7M_FPU_LAST_REG - ARMV7M_FPU_FIRST_REG + 1;
	/* we need one 32-bit word for each register except FP D0..D15, which
	 * need two words */
	uint32_t r_vals[n_r32];
	uint32_t dhcsr[n_r32];

	unsigned int wi = 0; /* write index to r_vals and dhcsr arrays */
	unsigned int reg_id; /* register index in the reg_list, ARMV7M_R0... */
	for (reg_id = 0; reg_id < num_regs; reg_id++) {
		struct reg *r = &armv7m->arm.core_cache->reg_list[reg_id];
		if (!r->exist)
			continue;	/* skip non existent registers */

		if (r->size <= 8) {
			/* Any 8-bit or shorter register is unpacked from a 32-bit
			 * container register. Skip it now. */
			continue;
		}

		uint32_t regsel = armv7m_map_id_to_regsel(reg_id);
		retval = cortex_m_queue_reg_read(target, regsel, &r_vals[wi],
										 &dhcsr[wi]);
		if (retval != ERROR_OK)
			return retval;
		wi++;

		assert(r->size == 32 || r->size == 64);
		if (r->size == 32)
			continue;	/* done with 32-bit register */

		assert(reg_id >= ARMV7M_FPU_FIRST_REG && reg_id <= ARMV7M_FPU_LAST_REG);
		/* the odd part of FP register (S1, S3...) */
		retval = cortex_m_queue_reg_read(target, regsel + 1, &r_vals[wi],
											 &dhcsr[wi]);
		if (retval != ERROR_OK)
			return retval;
		wi++;
	}

	assert(wi <= n_r32);

	retval = dap_run(armv7m->debug_ap->dap);
	if (retval != ERROR_OK)
		return retval;

	if (target->dbg_msg_enabled) {
		/* restore DCB_DCRDR - this needs to be in a separate
		 * transaction otherwise the emulated DCC channel breaks */
		retval = mem_ap_write_atomic_u32(armv7m->debug_ap, DCB_DCRDR, dcrdr);
		if (retval != ERROR_OK)
			return retval;
	}

	bool not_ready = false;
	for (unsigned int i = 0; i < wi; i++) {
		if ((dhcsr[i] & S_REGRDY) == 0) {
			not_ready = true;
			LOG_TARGET_DEBUG(target, "Register %u was not ready during fast read", i);
		}
		cortex_m_cumulate_dhcsr_sticky(cortex_m, dhcsr[i]);
	}

	if (not_ready) {
		/* Any register was not ready,
		 * fall back to slow read with S_REGRDY polling */
		return ERROR_TIMEOUT_REACHED;
	}

	LOG_TARGET_DEBUG(target, "read %u 32-bit registers", wi);

	unsigned int ri = 0; /* read index from r_vals array */
	for (reg_id = 0; reg_id < num_regs; reg_id++) {
		struct reg *r = &armv7m->arm.core_cache->reg_list[reg_id];
		if (!r->exist)
			continue;	/* skip non existent registers */

		r->dirty = false;

		unsigned int reg32_id;
		uint32_t offset;
		if (armv7m_map_reg_packing(reg_id, &reg32_id, &offset)) {
			/* Unpack a partial register from 32-bit container register */
			struct reg *r32 = &armv7m->arm.core_cache->reg_list[reg32_id];

			/* The container register ought to precede all regs unpacked
			 * from it in the reg_list. So the value should be ready
			 * to unpack */
			assert(r32->valid);
			buf_cpy(r32->value + offset, r->value, r->size);

		} else {
			assert(r->size == 32 || r->size == 64);
			buf_set_u32(r->value, 0, 32, r_vals[ri++]);

			if (r->size == 64) {
				assert(reg_id >= ARMV7M_FPU_FIRST_REG && reg_id <= ARMV7M_FPU_LAST_REG);
				/* the odd part of FP register (S1, S3...) */
				buf_set_u32(r->value + 4, 0, 32, r_vals[ri++]);
			}
		}
		r->valid = true;
	}
	assert(ri == wi);

	return retval;
}

static int cortex_m_store_core_reg_u32(struct target *target,
		uint32_t regsel, uint32_t value)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	int retval;
	uint32_t dcrdr;
	int64_t then;

	/* because the DCB_DCRDR is used for the emulated dcc channel
	 * we have to save/restore the DCB_DCRDR when used */
	if (target->dbg_msg_enabled) {
		retval = mem_ap_read_u32(armv7m->debug_ap, DCB_DCRDR, &dcrdr);
		if (retval != ERROR_OK)
			return retval;
	}

	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DCRDR, value);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DCRSR, regsel | DCRSR_WNR);
	if (retval != ERROR_OK)
		return retval;

	/* check if value is written into register */
	then = timeval_ms();
	while (1) {
		retval = cortex_m_read_dhcsr_atomic_sticky(target);
		if (retval != ERROR_OK)
			return retval;
		if (cortex_m->dcb_dhcsr & S_REGRDY)
			break;
		if (timeval_ms() > then + DHCSR_S_REGRDY_TIMEOUT) {
			LOG_TARGET_ERROR(target, "Timeout waiting for DCRDR transfer ready");
			return ERROR_TIMEOUT_REACHED;
		}
		keep_alive();
	}

	if (target->dbg_msg_enabled) {
		/* restore DCB_DCRDR - this needs to be in a separate
		 * transaction otherwise the emulated DCC channel breaks */
		if (retval == ERROR_OK)
			retval = mem_ap_write_atomic_u32(armv7m->debug_ap, DCB_DCRDR, dcrdr);
	}

	return retval;
}

static int cortex_m_write_debug_halt_mask(struct target *target,
	uint32_t mask_on, uint32_t mask_off)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;

	/* mask off status bits */
	cortex_m->dcb_dhcsr &= ~((0xFFFFul << 16) | mask_off);
	/* create new register mask */
	cortex_m->dcb_dhcsr |= DBGKEY | C_DEBUGEN | mask_on;

	return mem_ap_write_atomic_u32(armv7m->debug_ap, DCB_DHCSR, cortex_m->dcb_dhcsr);
}

static int cortex_m_set_maskints(struct target *target, bool mask)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	if (!!(cortex_m->dcb_dhcsr & C_MASKINTS) != mask)
		return cortex_m_write_debug_halt_mask(target, mask ? C_MASKINTS : 0, mask ? 0 : C_MASKINTS);
	else
		return ERROR_OK;
}

static int cortex_m_set_maskints_for_halt(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	switch (cortex_m->isrmasking_mode) {
		case CORTEX_M_ISRMASK_AUTO:
			/* interrupts taken at resume, whether for step or run -> no mask */
			return cortex_m_set_maskints(target, false);

		case CORTEX_M_ISRMASK_OFF:
			/* interrupts never masked */
			return cortex_m_set_maskints(target, false);

		case CORTEX_M_ISRMASK_ON:
			/* interrupts always masked */
			return cortex_m_set_maskints(target, true);

		case CORTEX_M_ISRMASK_STEPONLY:
			/* interrupts masked for single step only -> mask now if MASKINTS
			 * erratum, otherwise only mask before stepping */
			return cortex_m_set_maskints(target, cortex_m->maskints_erratum);
	}
	return ERROR_OK;
}

static int cortex_m_set_maskints_for_run(struct target *target)
{
	switch (target_to_cm(target)->isrmasking_mode) {
		case CORTEX_M_ISRMASK_AUTO:
			/* interrupts taken at resume, whether for step or run -> no mask */
			return cortex_m_set_maskints(target, false);

		case CORTEX_M_ISRMASK_OFF:
			/* interrupts never masked */
			return cortex_m_set_maskints(target, false);

		case CORTEX_M_ISRMASK_ON:
			/* interrupts always masked */
			return cortex_m_set_maskints(target, true);

		case CORTEX_M_ISRMASK_STEPONLY:
			/* interrupts masked for single step only -> no mask */
			return cortex_m_set_maskints(target, false);
	}
	return ERROR_OK;
}

static int cortex_m_set_maskints_for_step(struct target *target)
{
	switch (target_to_cm(target)->isrmasking_mode) {
		case CORTEX_M_ISRMASK_AUTO:
			/* the auto-interrupt should already be done -> mask */
			return cortex_m_set_maskints(target, true);

		case CORTEX_M_ISRMASK_OFF:
			/* interrupts never masked */
			return cortex_m_set_maskints(target, false);

		case CORTEX_M_ISRMASK_ON:
			/* interrupts always masked */
			return cortex_m_set_maskints(target, true);

		case CORTEX_M_ISRMASK_STEPONLY:
			/* interrupts masked for single step only -> mask */
			return cortex_m_set_maskints(target, true);
	}
	return ERROR_OK;
}

static int cortex_m_clear_halt(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;
	int retval;

	/* clear step if any */
	cortex_m_write_debug_halt_mask(target, C_HALT, C_STEP);

	/* Read Debug Fault Status Register */
	retval = mem_ap_read_atomic_u32(armv7m->debug_ap, NVIC_DFSR, &cortex_m->nvic_dfsr);
	if (retval != ERROR_OK)
		return retval;

	/* Clear Debug Fault Status */
	retval = mem_ap_write_atomic_u32(armv7m->debug_ap, NVIC_DFSR, cortex_m->nvic_dfsr);
	if (retval != ERROR_OK)
		return retval;
	LOG_TARGET_DEBUG(target, "NVIC_DFSR 0x%" PRIx32 "", cortex_m->nvic_dfsr);

	return ERROR_OK;
}

static int cortex_m_single_step_core(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	int retval;

	/* Mask interrupts before clearing halt, if not done already.  This avoids
	 * Erratum 377497 (fixed in r1p0) where setting MASKINTS while clearing
	 * HALT can put the core into an unknown state.
	 */
	if (!(cortex_m->dcb_dhcsr & C_MASKINTS)) {
		retval = cortex_m_write_debug_halt_mask(target, C_MASKINTS, 0);
		if (retval != ERROR_OK)
			return retval;
	}
	retval = cortex_m_write_debug_halt_mask(target, C_STEP, C_HALT);
	if (retval != ERROR_OK)
		return retval;
	LOG_TARGET_DEBUG(target, "single step");

	/* restore dhcsr reg */
	cortex_m_clear_halt(target);

	return ERROR_OK;
}

static int cortex_m_enable_fpb(struct target *target)
{
	int retval = target_write_u32(target, FP_CTRL, 3);
	if (retval != ERROR_OK)
		return retval;

	/* check the fpb is actually enabled */
	uint32_t fpctrl;
	retval = target_read_u32(target, FP_CTRL, &fpctrl);
	if (retval != ERROR_OK)
		return retval;

	if (fpctrl & 1)
		return ERROR_OK;

	return ERROR_FAIL;
}

static int cortex_m_endreset_event(struct target *target)
{
	int retval;
	uint32_t dcb_demcr;
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;
	struct adiv5_dap *swjdp = cortex_m->armv7m.arm.dap;
	struct cortex_m_fp_comparator *fp_list = cortex_m->fp_comparator_list;
	struct cortex_m_dwt_comparator *dwt_list = cortex_m->dwt_comparator_list;

	/* REVISIT The four debug monitor bits are currently ignored... */
	retval = mem_ap_read_atomic_u32(armv7m->debug_ap, DCB_DEMCR, &dcb_demcr);
	if (retval != ERROR_OK)
		return retval;
	LOG_TARGET_DEBUG(target, "DCB_DEMCR = 0x%8.8" PRIx32 "", dcb_demcr);

	/* this register is used for emulated dcc channel */
	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DCRDR, 0);
	if (retval != ERROR_OK)
		return retval;

	retval = cortex_m_read_dhcsr_atomic_sticky(target);
	if (retval != ERROR_OK)
		return retval;

	if (!(cortex_m->dcb_dhcsr & C_DEBUGEN)) {
		/* Enable debug requests */
		retval = cortex_m_write_debug_halt_mask(target, 0, C_HALT | C_STEP | C_MASKINTS);
		if (retval != ERROR_OK)
			return retval;
	}

	/* Restore proper interrupt masking setting for running CPU. */
	cortex_m_set_maskints_for_run(target);

	/* Enable features controlled by ITM and DWT blocks, and catch only
	 * the vectors we were told to pay attention to.
	 *
	 * Target firmware is responsible for all fault handling policy
	 * choices *EXCEPT* explicitly scripted overrides like "vector_catch"
	 * or manual updates to the NVIC SHCSR and CCR registers.
	 */
	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DEMCR, TRCENA | armv7m->demcr);
	if (retval != ERROR_OK)
		return retval;

	/* Paranoia: evidently some (early?) chips don't preserve all the
	 * debug state (including FPB, DWT, etc) across reset...
	 */

	/* Enable FPB */
	retval = cortex_m_enable_fpb(target);
	if (retval != ERROR_OK) {
		LOG_TARGET_ERROR(target, "Failed to enable the FPB");
		return retval;
	}

	cortex_m->fpb_enabled = true;

	/* Restore FPB registers */
	for (unsigned int i = 0; i < cortex_m->fp_num_code + cortex_m->fp_num_lit; i++) {
		retval = target_write_u32(target, fp_list[i].fpcr_address, fp_list[i].fpcr_value);
		if (retval != ERROR_OK)
			return retval;
	}

	/* Restore DWT registers */
	for (unsigned int i = 0; i < cortex_m->dwt_num_comp; i++) {
		retval = target_write_u32(target, dwt_list[i].dwt_comparator_address + 0,
				dwt_list[i].comp);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, dwt_list[i].dwt_comparator_address + 4,
				dwt_list[i].mask);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, dwt_list[i].dwt_comparator_address + 8,
				dwt_list[i].function);
		if (retval != ERROR_OK)
			return retval;
	}
	retval = dap_run(swjdp);
	if (retval != ERROR_OK)
		return retval;

	register_cache_invalidate(armv7m->arm.core_cache);

	/* TODO: invalidate also working areas (needed in the case of detected reset).
	 * Doing so will require flash drivers to test if working area
	 * is still valid in all target algo calling loops.
	 */

	/* make sure we have latest dhcsr flags */
	retval = cortex_m_read_dhcsr_atomic_sticky(target);
	if (retval != ERROR_OK)
		return retval;

	return retval;
}

static int cortex_m_examine_debug_reason(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);

	/* THIS IS NOT GOOD, TODO - better logic for detection of debug state reason
	 * only check the debug reason if we don't know it already */

	if (target->debug_reason != DBG_REASON_DBGRQ
		&& target->debug_reason != DBG_REASON_SINGLESTEP) {
		if (cortex_m->nvic_dfsr & DFSR_BKPT) {
			target->debug_reason = DBG_REASON_BREAKPOINT;
			if (cortex_m->nvic_dfsr & DFSR_DWTTRAP)
				target->debug_reason = DBG_REASON_WPTANDBKPT;
		} else if (cortex_m->nvic_dfsr & DFSR_DWTTRAP) {
			target->debug_reason = DBG_REASON_WATCHPOINT;
		} else if (cortex_m->nvic_dfsr & DFSR_VCATCH) {
			target->debug_reason = DBG_REASON_BREAKPOINT;
		} else if (cortex_m->nvic_dfsr & DFSR_EXTERNAL) {
			target->debug_reason = DBG_REASON_DBGRQ;
		} else {	/* HALTED */
			target->debug_reason = DBG_REASON_UNDEFINED;
		}
	}

	return ERROR_OK;
}

static int cortex_m_examine_exception_reason(struct target *target)
{
	uint32_t shcsr = 0, except_sr = 0, cfsr = -1, except_ar = -1;
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct adiv5_dap *swjdp = armv7m->arm.dap;
	int retval;

	retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_SHCSR, &shcsr);
	if (retval != ERROR_OK)
		return retval;
	switch (armv7m->exception_number) {
		case 2:	/* NMI */
			break;
		case 3:	/* Hard Fault */
			retval = mem_ap_read_atomic_u32(armv7m->debug_ap, NVIC_HFSR, &except_sr);
			if (retval != ERROR_OK)
				return retval;
			if (except_sr & 0x40000000) {
				retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_CFSR, &cfsr);
				if (retval != ERROR_OK)
					return retval;
			}
			break;
		case 4:	/* Memory Management */
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_CFSR, &except_sr);
			if (retval != ERROR_OK)
				return retval;
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_MMFAR, &except_ar);
			if (retval != ERROR_OK)
				return retval;
			break;
		case 5:	/* Bus Fault */
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_CFSR, &except_sr);
			if (retval != ERROR_OK)
				return retval;
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_BFAR, &except_ar);
			if (retval != ERROR_OK)
				return retval;
			break;
		case 6:	/* Usage Fault */
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_CFSR, &except_sr);
			if (retval != ERROR_OK)
				return retval;
			break;
		case 7:	/* Secure Fault */
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_SFSR, &except_sr);
			if (retval != ERROR_OK)
				return retval;
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_SFAR, &except_ar);
			if (retval != ERROR_OK)
				return retval;
			break;
		case 11:	/* SVCall */
			break;
		case 12:	/* Debug Monitor */
			retval = mem_ap_read_u32(armv7m->debug_ap, NVIC_DFSR, &except_sr);
			if (retval != ERROR_OK)
				return retval;
			break;
		case 14:	/* PendSV */
			break;
		case 15:	/* SysTick */
			break;
		default:
			except_sr = 0;
			break;
	}
	retval = dap_run(swjdp);
	if (retval == ERROR_OK)
		LOG_TARGET_DEBUG(target, "%s SHCSR 0x%" PRIx32 ", SR 0x%" PRIx32
			", CFSR 0x%" PRIx32 ", AR 0x%" PRIx32,
			armv7m_exception_string(armv7m->exception_number),
			shcsr, except_sr, cfsr, except_ar);
	return retval;
}

static int cortex_m_debug_entry(struct target *target)
{
	uint32_t xpsr;
	int retval;
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;
	struct arm *arm = &armv7m->arm;
	struct reg *r;

	LOG_TARGET_DEBUG(target, " ");

	/* Do this really early to minimize the window where the MASKINTS erratum
	 * can pile up pending interrupts. */
	cortex_m_set_maskints_for_halt(target);

	cortex_m_clear_halt(target);

	retval = cortex_m_read_dhcsr_atomic_sticky(target);
	if (retval != ERROR_OK)
		return retval;

	retval = armv7m->examine_debug_reason(target);
	if (retval != ERROR_OK)
		return retval;

	/* examine PE security state */
	bool secure_state = false;
	if (armv7m->arm.arch == ARM_ARCH_V8M) {
		uint32_t dscsr;

		retval = mem_ap_read_u32(armv7m->debug_ap, DCB_DSCSR, &dscsr);
		if (retval != ERROR_OK)
			return retval;

		secure_state = (dscsr & DSCSR_CDS) == DSCSR_CDS;
	}

	/* Load all registers to arm.core_cache */
	if (!cortex_m->slow_register_read) {
		retval = cortex_m_fast_read_all_regs(target);
		if (retval == ERROR_TIMEOUT_REACHED) {
			cortex_m->slow_register_read = true;
			LOG_TARGET_DEBUG(target, "Switched to slow register read");
		}
	}

	if (cortex_m->slow_register_read)
		retval = cortex_m_slow_read_all_regs(target);

	if (retval != ERROR_OK)
		return retval;

	r = arm->cpsr;
	xpsr = buf_get_u32(r->value, 0, 32);

	/* Are we in an exception handler */
	if (xpsr & 0x1FF) {
		armv7m->exception_number = (xpsr & 0x1FF);

		arm->core_mode = ARM_MODE_HANDLER;
		arm->map = armv7m_msp_reg_map;
	} else {
		unsigned int control = buf_get_u32(arm->core_cache
				->reg_list[ARMV7M_CONTROL].value, 0, 3);

		/* is this thread privileged? */
		arm->core_mode = control & 1
			? ARM_MODE_USER_THREAD
			: ARM_MODE_THREAD;

		/* which stack is it using? */
		if (control & 2)
			arm->map = armv7m_psp_reg_map;
		else
			arm->map = armv7m_msp_reg_map;

		armv7m->exception_number = 0;
	}

	if (armv7m->exception_number)
		cortex_m_examine_exception_reason(target);

	LOG_TARGET_DEBUG(target, "entered debug state in core mode: %s at PC 0x%" PRIx32
			", cpu in %s state, target->state: %s",
		arm_mode_name(arm->core_mode),
		buf_get_u32(arm->pc->value, 0, 32),
		secure_state ? "Secure" : "Non-Secure",
		target_state_name(target));

	if (armv7m->post_debug_entry) {
		retval = armv7m->post_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int cortex_m_poll_one(struct target *target)
{
	int detected_failure = ERROR_OK;
	int retval = ERROR_OK;
	enum target_state prev_target_state = target->state;
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;

	/* Read from Debug Halting Control and Status Register */
	retval = cortex_m_read_dhcsr_atomic_sticky(target);
	if (retval != ERROR_OK) {
		target->state = TARGET_UNKNOWN;
		return retval;
	}

	/* Recover from lockup.  See ARMv7-M architecture spec,
	 * section B1.5.15 "Unrecoverable exception cases".
	 */
	if (cortex_m->dcb_dhcsr & S_LOCKUP) {
		LOG_TARGET_ERROR(target, "clearing lockup after double fault");
		cortex_m_write_debug_halt_mask(target, C_HALT, 0);
		target->debug_reason = DBG_REASON_DBGRQ;

		/* We have to execute the rest (the "finally" equivalent, but
		 * still throw this exception again).
		 */
		detected_failure = ERROR_FAIL;

		/* refresh status bits */
		retval = cortex_m_read_dhcsr_atomic_sticky(target);
		if (retval != ERROR_OK)
			return retval;
	}

	if (cortex_m->dcb_dhcsr_cumulated_sticky & S_RESET_ST) {
		cortex_m->dcb_dhcsr_cumulated_sticky &= ~S_RESET_ST;
		if (target->state != TARGET_RESET) {
			target->state = TARGET_RESET;
			LOG_TARGET_INFO(target, "external reset detected");
		}
		return ERROR_OK;
	}

	if (target->state == TARGET_RESET) {
		/* Cannot switch context while running so endreset is
		 * called with target->state == TARGET_RESET
		 */
		LOG_TARGET_DEBUG(target, "Exit from reset with dcb_dhcsr 0x%" PRIx32,
			cortex_m->dcb_dhcsr);
		retval = cortex_m_endreset_event(target);
		if (retval != ERROR_OK) {
			target->state = TARGET_UNKNOWN;
			return retval;
		}
		target->state = TARGET_RUNNING;
		prev_target_state = TARGET_RUNNING;
	}

	if (cortex_m->dcb_dhcsr & S_HALT) {
		target->state = TARGET_HALTED;

		if (prev_target_state == TARGET_RUNNING || prev_target_state == TARGET_RESET) {
			retval = cortex_m_debug_entry(target);

			/* arm_semihosting needs to know registers, don't run if debug entry returned error */
			if (retval == ERROR_OK && arm_semihosting(target, &retval) != 0)
				return retval;

			if (target->smp) {
				LOG_TARGET_DEBUG(target, "postpone target event 'halted'");
				target->smp_halt_event_postponed = true;
			} else {
				/* regardless of errors returned in previous code update state */
				target_call_event_callbacks(target, TARGET_EVENT_HALTED);
			}
		}
		if (prev_target_state == TARGET_DEBUG_RUNNING) {
			retval = cortex_m_debug_entry(target);

			target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED);
		}
		if (retval != ERROR_OK)
			return retval;
	}

	if (target->state == TARGET_UNKNOWN) {
		/* Check if processor is retiring instructions or sleeping.
		 * Unlike S_RESET_ST here we test if the target *is* running now,
		 * not if it has been running (possibly in the past). Instructions are
		 * typically processed much faster than OpenOCD polls DHCSR so S_RETIRE_ST
		 * is read always 1. That's the reason not to use dcb_dhcsr_cumulated_sticky.
		 */
		if (cortex_m->dcb_dhcsr & S_RETIRE_ST || cortex_m->dcb_dhcsr & S_SLEEP) {
			target->state = TARGET_RUNNING;
			retval = ERROR_OK;
		}
	}

	/* Check that target is truly halted, since the target could be resumed externally */
	if (prev_target_state == TARGET_HALTED && !(cortex_m->dcb_dhcsr & S_HALT)) {
		/* registers are now invalid */
		register_cache_invalidate(armv7m->arm.core_cache);

		target->state = TARGET_RUNNING;
		LOG_TARGET_WARNING(target, "external resume detected");
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		retval = ERROR_OK;
	}

	/* Did we detect a failure condition that we cleared? */
	if (detected_failure != ERROR_OK)
		retval = detected_failure;
	return retval;
}

static int cortex_m_halt_one(struct target *target);

static int cortex_m_smp_halt_all(struct list_head *smp_targets)
{
	int retval = ERROR_OK;
	struct target_list *head;

	foreach_smp_target(head, smp_targets) {
		struct target *curr = head->target;
		if (!target_was_examined(curr))
			continue;
		if (curr->state == TARGET_HALTED)
			continue;

		int ret2 = cortex_m_halt_one(curr);
		if (retval == ERROR_OK)
			retval = ret2;	/* store the first error code ignore others */
	}
	return retval;
}

static int cortex_m_smp_post_halt_poll(struct list_head *smp_targets)
{
	int retval = ERROR_OK;
	struct target_list *head;

	foreach_smp_target(head, smp_targets) {
		struct target *curr = head->target;
		if (!target_was_examined(curr))
			continue;
		/* skip targets that were already halted */
		if (curr->state == TARGET_HALTED)
			continue;

		int ret2 = cortex_m_poll_one(curr);
		if (retval == ERROR_OK)
			retval = ret2;	/* store the first error code ignore others */
	}
	return retval;
}

static int cortex_m_poll_smp(struct list_head *smp_targets)
{
	int retval = ERROR_OK;
	struct target_list *head;
	bool halted = false;

	foreach_smp_target(head, smp_targets) {
		struct target *curr = head->target;
		if (curr->smp_halt_event_postponed) {
			halted = true;
			break;
		}
	}

	if (halted) {
		retval = cortex_m_smp_halt_all(smp_targets);

		int ret2 = cortex_m_smp_post_halt_poll(smp_targets);
		if (retval == ERROR_OK)
			retval = ret2;	/* store the first error code ignore others */

		foreach_smp_target(head, smp_targets) {
			struct target *curr = head->target;
			if (!curr->smp_halt_event_postponed)
				continue;

			curr->smp_halt_event_postponed = false;
			if (curr->state == TARGET_HALTED) {
				LOG_TARGET_DEBUG(curr, "sending postponed target event 'halted'");
				target_call_event_callbacks(curr, TARGET_EVENT_HALTED);
			}
		}
		/* There is no need to set gdb_service->target
		 * as hwthread_update_threads() selects an interesting thread
		 * by its own
		 */
	}
	return retval;
}

static int cortex_m_poll(struct target *target)
{
	int retval = cortex_m_poll_one(target);

	if (target->smp) {
		struct target_list *last;
		last = list_last_entry(target->smp_targets, struct target_list, lh);
		if (target == last->target)
			/* After the last target in SMP group has been polled
			 * check for postponed halted events and eventually halt and re-poll
			 * other targets */
			cortex_m_poll_smp(target->smp_targets);
	}
	return retval;
}

static int cortex_m_halt_one(struct target *target)
{
	LOG_TARGET_DEBUG(target, "target->state: %s", target_state_name(target));

	if (target->state == TARGET_HALTED) {
		LOG_TARGET_DEBUG(target, "target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
		LOG_TARGET_WARNING(target, "target was in unknown state when halt was requested");

	if (target->state == TARGET_RESET) {
		if ((jtag_get_reset_config() & RESET_SRST_PULLS_TRST) && jtag_get_srst()) {
			LOG_TARGET_ERROR(target, "can't request a halt while in reset if nSRST pulls nTRST");
			return ERROR_TARGET_FAILURE;
		}
		/* we came here in a reset_halt or reset_init sequence
		 * debug entry was already prepared in cortex_m3_assert_reset()
		 */
		target->debug_reason = DBG_REASON_DBGRQ;

		return ERROR_OK;
	}

	/* Write to Debug Halting Control and Status Register */
	cortex_m_write_debug_halt_mask(target, C_HALT, 0);

	/* Do this really early to minimize the window where the MASKINTS erratum
	 * can pile up pending interrupts. */
	cortex_m_set_maskints_for_halt(target);

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

static int cortex_m_halt(struct target *target)
{
	if (target->smp)
		return cortex_m_smp_halt_all(target->smp_targets);
	else
		return cortex_m_halt_one(target);
}

static int cortex_m_soft_reset_halt(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;
	int retval, timeout = 0;

	/* on single cortex_m MCU soft_reset_halt should be avoided as same functionality
	 * can be obtained by using 'reset halt' and 'cortex_m reset_config vectreset'.
	 * As this reset only uses VC_CORERESET it would only ever reset the cortex_m
	 * core, not the peripherals */
	LOG_TARGET_DEBUG(target, "soft_reset_halt is discouraged, please use 'reset halt' instead.");

	if (!cortex_m->vectreset_supported) {
		LOG_TARGET_ERROR(target, "VECTRESET is not supported on this Cortex-M core");
		return ERROR_FAIL;
	}

	/* Set C_DEBUGEN */
	retval = cortex_m_write_debug_halt_mask(target, 0, C_STEP | C_MASKINTS);
	if (retval != ERROR_OK)
		return retval;

	/* Enter debug state on reset; restore DEMCR in endreset_event() */
	retval = mem_ap_write_u32(armv7m->debug_ap, DCB_DEMCR,
			TRCENA | VC_HARDERR | VC_BUSERR | VC_CORERESET);
	if (retval != ERROR_OK)
		return retval;

	/* Request a core-only reset */
	retval = mem_ap_write_atomic_u32(armv7m->debug_ap, NVIC_AIRCR,
			AIRCR_VECTKEY | AIRCR_VECTRESET);
	if (retval != ERROR_OK)
		return retval;
	target->state = TARGET_RESET;

	/* registers are now invalid */
	register_cache_invalidate(cortex_m->armv7m.arm.core_cache);

	while (timeout < 100) {
		retval = cortex_m_read_dhcsr_atomic_sticky(target);
		if (retval == ERROR_OK) {
			retval = mem_ap_read_atomic_u32(armv7m->debug_ap, NVIC_DFSR,
					&cortex_m->nvic_dfsr);
			if (retval != ERROR_OK)
				return retval;
			if ((cortex_m->dcb_dhcsr & S_HALT)
				&& (cortex_m->nvic_dfsr & DFSR_VCATCH)) {
				LOG_TARGET_DEBUG(target, "system reset-halted, DHCSR 0x%08" PRIx32 ", DFSR 0x%08" PRIx32,
						cortex_m->dcb_dhcsr, cortex_m->nvic_dfsr);
				cortex_m_poll(target);
				/* FIXME restore user's vector catch config */
				return ERROR_OK;
			}
			LOG_TARGET_DEBUG(target, "waiting for system reset-halt, "
				"DHCSR 0x%08" PRIx32 ", %d ms",
				cortex_m->dcb_dhcsr, timeout);
		}
		timeout++;
		alive_sleep(1);
	}

	return ERROR_OK;
}

static int cortex_m_restore_one(struct target *target, bool current,
	target_addr_t *address, bool handle_breakpoints, bool debug_execution)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct breakpoint *breakpoint = NULL;
	uint32_t resume_pc;
	struct reg *r;

	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!debug_execution) {
		target_free_all_working_areas(target);
		cortex_m_enable_breakpoints(target);
		cortex_m_enable_watchpoints(target);
	}

	if (debug_execution) {
		r = armv7m->arm.core_cache->reg_list + ARMV7M_PRIMASK;

		/* Disable interrupts */
		/* We disable interrupts in the PRIMASK register instead of
		 * masking with C_MASKINTS.  This is probably the same issue
		 * as Cortex-M3 Erratum 377493 (fixed in r1p0):  C_MASKINTS
		 * in parallel with disabled interrupts can cause local faults
		 * to not be taken.
		 *
		 * This breaks non-debug (application) execution if not
		 * called from armv7m_start_algorithm() which saves registers.
		 */
		buf_set_u32(r->value, 0, 1, 1);
		r->dirty = true;
		r->valid = true;

		/* Make sure we are in Thumb mode, set xPSR.T bit */
		/* armv7m_start_algorithm() initializes entire xPSR register.
		 * This duplicity handles the case when cortex_m_resume()
		 * is used with the debug_execution flag directly,
		 * not called through armv7m_start_algorithm().
		 */
		r = armv7m->arm.cpsr;
		buf_set_u32(r->value, 24, 1, 1);
		r->dirty = true;
		r->valid = true;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	r = armv7m->arm.pc;
	if (!current) {
		buf_set_u32(r->value, 0, 32, *address);
		r->dirty = true;
		r->valid = true;
	}

	/* if we halted last time due to a bkpt instruction
	 * then we have to manually step over it, otherwise
	 * the core will break again */

	if (!breakpoint_find(target, buf_get_u32(r->value, 0, 32))
		&& !debug_execution)
		armv7m_maybe_skip_bkpt_inst(target, NULL);

	resume_pc = buf_get_u32(r->value, 0, 32);
	if (current)
		*address = resume_pc;

	int retval = armv7m_restore_context(target);
	if (retval != ERROR_OK)
		return retval;

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_TARGET_DEBUG(target, "unset breakpoint at " TARGET_ADDR_FMT " (ID: %" PRIu32 ")",
				breakpoint->address,
				breakpoint->unique_id);
			retval = cortex_m_unset_breakpoint(target, breakpoint);
			if (retval == ERROR_OK)
				retval = cortex_m_single_step_core(target);
			int ret2 = cortex_m_set_breakpoint(target, breakpoint);
			if (retval != ERROR_OK)
				return retval;
			if (ret2 != ERROR_OK)
				return ret2;
		}
	}

	return ERROR_OK;
}

static int cortex_m_restart_one(struct target *target, bool debug_execution)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);

	/* Restart core */
	cortex_m_set_maskints_for_run(target);
	cortex_m_write_debug_halt_mask(target, 0, C_HALT);

	target->debug_reason = DBG_REASON_NOTHALTED;
	/* registers are now invalid */
	register_cache_invalidate(armv7m->arm.core_cache);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
	}

	return ERROR_OK;
}

static int cortex_m_restore_smp(struct target *target, bool handle_breakpoints)
{
	struct target_list *head;
	target_addr_t address;
	foreach_smp_target(head, target->smp_targets) {
		struct target *curr = head->target;
		/* skip calling target */
		if (curr == target)
			continue;
		if (!target_was_examined(curr))
			continue;
		/* skip running targets */
		if (curr->state == TARGET_RUNNING)
			continue;

		int retval = cortex_m_restore_one(curr, true, &address,
										handle_breakpoints, false);
		if (retval != ERROR_OK)
			return retval;

		retval = cortex_m_restart_one(curr, false);
		if (retval != ERROR_OK)
			return retval;

		LOG_TARGET_DEBUG(curr, "SMP resumed at " TARGET_ADDR_FMT, address);
	}
	return ERROR_OK;
}

static int cortex_m_resume(struct target *target, int current,
						   target_addr_t address, int handle_breakpoints, int debug_execution)
{
	int retval = cortex_m_restore_one(target, !!current, &address, !!handle_breakpoints, !!debug_execution);
	if (retval != ERROR_OK) {
		LOG_TARGET_ERROR(target, "context restore failed, aborting resume");
		return retval;
	}

	if (target->smp && !debug_execution) {
		retval = cortex_m_restore_smp(target, !!handle_breakpoints);
		if (retval != ERROR_OK)
			LOG_WARNING("resume of a SMP target failed, trying to resume current one");
	}

	cortex_m_restart_one(target, !!debug_execution);
	if (retval != ERROR_OK) {
		LOG_TARGET_ERROR(target, "resume failed");
		return retval;
	}

	LOG_TARGET_DEBUG(target, "%sresumed at " TARGET_ADDR_FMT,
					debug_execution ? "debug " : "", address);

	return ERROR_OK;
}

/* int irqstepcount = 0; */
static int cortex_m_step(struct target *target, int current,
	target_addr_t address, int handle_breakpoints)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;
	struct breakpoint *breakpoint = NULL;
	struct reg *pc = armv7m->arm.pc;
	bool bkpt_inst_found = false;
	int retval;
	bool isr_timed_out = false;

	if (target->state != TARGET_HALTED) {
		LOG_TARGET_WARNING(target, "target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Just one of SMP cores will step. Set the gdb control
	 * target to current one or gdb miss gdb-end event */
	if (target->smp && target->gdb_service)
		target->gdb_service->target = target;

	/* current = 1: continue on current pc, otherwise continue at <address> */
	if (!current) {
		buf_set_u32(pc->value, 0, 32, address);
		pc->dirty = true;
		pc->valid = true;
	}

	uint32_t pc_value = buf_get_u32(pc->value, 0, 32);

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target, pc_value);
		if (breakpoint)
			cortex_m_unset_breakpoint(target, breakpoint);
	}

	armv7m_maybe_skip_bkpt_inst(target, &bkpt_inst_found);

	target->debug_reason = DBG_REASON_SINGLESTEP;

	armv7m_restore_context(target);

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);

	/* if no bkpt instruction is found at pc then we can perform
	 * a normal step, otherwise we have to manually step over the bkpt
	 * instruction - as such simulate a step */
	if (!bkpt_inst_found) {
		if (cortex_m->isrmasking_mode != CORTEX_M_ISRMASK_AUTO) {
			/* Automatic ISR masking mode off: Just step over the next
			 * instruction, with interrupts on or off as appropriate. */
			cortex_m_set_maskints_for_step(target);
			cortex_m_write_debug_halt_mask(target, C_STEP, C_HALT);
		} else {
			/* Process interrupts during stepping in a way they don't interfere
			 * debugging.
			 *
			 * Principle:
			 *
			 * Set a temporary break point at the current pc and let the core run
			 * with interrupts enabled. Pending interrupts get served and we run
			 * into the breakpoint again afterwards. Then we step over the next
			 * instruction with interrupts disabled.
			 *
			 * If the pending interrupts don't complete within time, we leave the
			 * core running. This may happen if the interrupts trigger faster
			 * than the core can process them or the handler doesn't return.
			 *
			 * If no more breakpoints are available we simply do a step with
			 * interrupts enabled.
			 *
			 */

			/* 2012-09-29 ph
			 *
			 * If a break point is already set on the lower half word then a break point on
			 * the upper half word will not break again when the core is restarted. So we
			 * just step over the instruction with interrupts disabled.
			 *
			 * The documentation has no information about this, it was found by observation
			 * on STM32F1 and STM32F2. Proper explanation welcome. STM32F0 doesn't seem to
			 * suffer from this problem.
			 *
			 * To add some confusion: pc_value has bit 0 always set, while the breakpoint
			 * address has it always cleared. The former is done to indicate thumb mode
			 * to gdb.
			 *
			 */
			if ((pc_value & 0x02) && breakpoint_find(target, pc_value & ~0x03)) {
				LOG_TARGET_DEBUG(target, "Stepping over next instruction with interrupts disabled");
				cortex_m_write_debug_halt_mask(target, C_HALT | C_MASKINTS, 0);
				cortex_m_write_debug_halt_mask(target, C_STEP, C_HALT);
				/* Re-enable interrupts if appropriate */
				cortex_m_write_debug_halt_mask(target, C_HALT, 0);
				cortex_m_set_maskints_for_halt(target);
			} else {
				/* Set a temporary break point */
				if (breakpoint) {
					retval = cortex_m_set_breakpoint(target, breakpoint);
				} else {
					enum breakpoint_type type = BKPT_HARD;
					if (cortex_m->fp_rev == 0 && pc_value > 0x1FFFFFFF) {
						/* FPB rev.1 cannot handle such addr, try BKPT instr */
						type = BKPT_SOFT;
					}
					retval = breakpoint_add(target, pc_value, 2, type);
				}

				bool tmp_bp_set = (retval == ERROR_OK);

				/* No more breakpoints left, just do a step */
				if (!tmp_bp_set) {
					cortex_m_set_maskints_for_step(target);
					cortex_m_write_debug_halt_mask(target, C_STEP, C_HALT);
					/* Re-enable interrupts if appropriate */
					cortex_m_write_debug_halt_mask(target, C_HALT, 0);
					cortex_m_set_maskints_for_halt(target);
				} else {
					/* Start the core */
					LOG_TARGET_DEBUG(target, "Starting core to serve pending interrupts");
					int64_t t_start = timeval_ms();
					cortex_m_set_maskints_for_run(target);
					cortex_m_write_debug_halt_mask(target, 0, C_HALT | C_STEP);

					/* Wait for pending handlers to complete or timeout */
					do {
						retval = cortex_m_read_dhcsr_atomic_sticky(target);
						if (retval != ERROR_OK) {
							target->state = TARGET_UNKNOWN;
							return retval;
						}
						isr_timed_out = ((timeval_ms() - t_start) > 500);
					} while (!((cortex_m->dcb_dhcsr & S_HALT) || isr_timed_out));

					/* only remove breakpoint if we created it */
					if (breakpoint) {
						cortex_m_unset_breakpoint(target, breakpoint);
					} else {
						/* Remove the temporary breakpoint */
						breakpoint_remove(target, pc_value);
					}

					if (isr_timed_out) {
						LOG_TARGET_DEBUG(target, "Interrupt handlers didn't complete within time, "
							"leaving target running");
					} else {
						/* Step over next instruction with interrupts disabled */
						cortex_m_set_maskints_for_step(target);
						cortex_m_write_debug_halt_mask(target,
							C_HALT | C_MASKINTS,
							0);
						cortex_m_write_debug_halt_mask(target, C_STEP, C_HALT);
						/* Re-enable interrupts if appropriate */
						cortex_m_write_debug_halt_mask(target, C_HALT, 0);
						cortex_m_set_maskints_for_halt(target);
					}
				}
			}
		}
	}

	retval = cortex_m_read_dhcsr_atomic_sticky(target);
	if (retval != ERROR_OK)
		return retval;

	/* registers are now invalid */
	register_cache_invalidate(armv7m->arm.core_cache);

	if (breakpoint)
		cortex_m_set_breakpoint(target, breakpoint);

	if (isr_timed_out) {
		/* Leave the core running. The user has to stop execution manually. */
		target->debug_reason = DBG_REASON_NOTHALTED;
		target->state = TARGET_RUNNING;
		return ERROR_OK;
	}

	LOG_TARGET_DEBUG(target, "target stepped dcb_dhcsr = 0x%" PRIx32
		" nvic_icsr = 0x%" PRIx32,
		cortex_m->dcb_dhcsr, cortex_m->nvic_icsr);

	retval = cortex_m_debug_entry(target);
	if (retval != ERROR_OK)
		return retval;
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	LOG_TARGET_DEBUG(target, "target stepped dcb_dhcsr = 0x%" PRIx32
		" nvic_icsr = 0x%" PRIx32,
		cortex_m->dcb_dhcsr, cortex_m->nvic_icsr);

	return ERROR_OK;
}

static int cortex_m_assert_reset(struct target *target)
{
	struct cortex_m_common *cortex_m = target_to_cm(target);
	struct armv7m_common *armv7m = &cortex_m->armv7m;
	enum cortex_m_soft_reset_config reset_config = cortex_m->soft_reset_config;

	LOG_TARGET_DEBUG(target, "target->state: %s,%s examined",
		target_state_name(target),
		target_was_examined(target) ? "" : " not");

	enum reset_types jtag_reset_config = jtag_get_reset_config();

	if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT)) {
		/* allow scripts to override the reset event */

		target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
		register_cache_invalidate(cortex_m->armv7m.arm.core_cache);
		target->state = TARGET_RESET;

		return ERROR_OK;
	}

	/* some cores support connecting while srst is asserted
	 * use that mode is it has been configured */

	bool srst_asserted = false;

	if ((jtag_reset_config & RESET_HAS_SRST) &&
		((jtag_reset_config & RESET_SRST_NO_GATING) || !armv7m->debug_ap)) {
		/* If we have no debug_ap, asserting SRST is the only thing
		 * we can do now */
		adapter_assert_reset();
		srst_asserted = true;
	}

	/* TODO: replace the hack calling target_examine_one()
	 * as soon as a better reset framework is available */
	if (!target_was_examined(target) && !target->defer_examine
		&& srst_asserted && (jtag_reset_config & RESET_SRST_NO_GATING)) {
		LOG_TARGET_DEBUG(target, "Trying to re-examine under reset");
		target_examine_one(target);
	}

	/* We need at least debug_ap to go further.
	 * Inform user and bail out if we don't have one. */
	if (!armv7m->debug_ap) {
		if (srst_asserted) {
			if (target->reset_halt)
				LOG_TARGET_ERROR(target, "Debug AP not available, will not halt after reset!");

			/* Do not propagate error: reset was asserted, proceed to deassert! */
			target->state = TARGET_RESET;
			register_cache_invalidate(cortex_m->armv7m.arm.core_cache);
			return ERROR_OK;

		} else {
			LOG_TARGET_ERROR(target, "Debug AP not available, reset NOT asserted!");
			return ERROR_FAIL;
		}
	}

	/* Enable debug requests */
	int retval = cortex_m_read_dhcsr_atomic_sticky(target);

	/* Store important errors instead of failing and proceed to reset assert */

	if (retval != ERROR_OK || !(cortex_m->dcb_dhcsr & C_DEBUGEN))
		retval = cortex_m_write_debug_halt_mask(target, 0, C_HALT | C_STEP | C_MASKINTS);

	/* If the processor is sleeping in a WFI or WFE instruction, the
	 * C_HALT bit must be asserted to regain control */
	if (retval == ERROR_OK && (cortex_m->dcb_dhcsr & S_SLEEP))
		retval = cortex_m_write_debug_halt_mask(target, C_HALT, 0);

	mem_ap_write_u32(armv7m->debug_ap, DCB_DCRDR, 0);
	/* Ignore less important errors */

	if (!target->reset_halt) {
		/* Set/Clear C_MASKINTS in a separate operation */
		cortex_m_set_maskints_for_run(target);

		/* clear any debug flags before resuming */
		cortex_m_clear_halt(target);

		/* clear C_HALT in dhcsr reg */
		cortex_m_write_debug_halt_mask(target, 0, C_HALT);
	} else {
		/* Halt in debug on reset; endreset_event() restores DEMCR.
		 *
		 * REVISIT catching BUSERR presumably helps to defend against
		 * bad vector table entries.  Should this include MMERR or
		 * other flags too?
		 */
		int retval2;
		retval2 = mem_ap_write_atomic_u32(armv7m->debug_ap, DCB_DEMCR,
				TRCENA | VC_HARDERR | VC_BUSERR | VC_CORERESET);
		if (retval != ERROR_OK || retval2 != ERROR_OK)
			LOG_TARGET_INFO(target, "AP write error, reset will not halt");
	}

	if (jtag_reset_config & RESET_HAS_SRST) {
		/* default to asserting srst */
		if (!srst_asserted)
			adapter_assert_reset();

		/* srst is asserted, ignore AP access errors */
		retval = ERROR_OK;
	} else {
		/* Use a standard Cortex-M3 software reset mechanism.
		 * We default to using VECTRESET as it is supported on all current cores
		 * (except Cortex-M0, M0+ and M1 which support SYSRESETREQ only!)
		 * This has the disadvantage of not resetting the peripherals, so a
		 * reset-init event handler is needed to perform any peripheral resets.
		 */
		if (!cortex_m->vectreset_supported
				&& reset_config == CORTEX_M_RESET_VECTRESET) {
			reset_config = CORTEX_M_RESET_SYSRESETREQ;
			LOG_TARGET_WARNING(target, "VECTRESET is not supported on this Cortex-M core, using SYSRESETREQ instead.");
			LOG_TARGET_WARNING(target, "Set 'cortex_m reset_config sysresetreq'.");
		}

		LOG_TARGET_DEBUG(target, "Using Cortex-M %s", (reset_config == CORTEX_M_RESET_SYSRESETREQ)
			? "SYSRESETREQ" : "VECTRESET");

		if (reset_config == CORTEX_M_RESET_VECTRESET) {
			LOG_TARGET_WARNING(target, "Only resetting the Cortex-M core, use a reset-init event "
				"handler to reset any peripherals or configure hardware srst support.");
		}

		int retval3;
		retval3 = mem_ap_write_atomic_u32(armv7m->debug_ap, NVIC_AIRCR,
				AIRCR_VECTKEY | ((reset_config == CORTEX_M_RESET_SYSRESETREQ)
				? AIRCR_SYSRESETREQ : AIRCR_VECTRESET));
		if (retval3 != ERROR_OK)
			LOG_TARGET_DEBUG(target, "Ignoring AP write error right after reset");

		retval3 = dap_dp_init_or_reconnect(armv7m->debug_ap->dap);
		if (retval3 != ERROR_OK) {
			LOG_TARGET_ERROR(target, "DP initialisation failed");
			/* The error return value must not be propagated in this case.
			 * SYSRESETREQ or VECTRESET have been possibly triggered
			 * so reset processing should continue */
		} else {
			/* I do not know why this is necessary, but it
			 * fixes strange effects (step/resume cause NMI
			 * after reset) on LM3S6918 -- Michael Schwingen
			 */
			uint32_t tmp;
			mem_ap_read_atomic_u32(armv7m->debug_ap, NVIC_AIRCR, &tmp);
		}
	}

	target->state = TARGET_RESET;
	jtag_sleep(50000);

	register_cache_invalidate(cortex_m->armv7m.arm.core_cache);

	/* now return stored error code if any */
	if (retval != ERROR_OK)
		return retval;

	if (target->reset_halt && target_was_examined(target)) {
		retval = target_halt(target);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int cortex_m_deassert_reset(struct target *target)
{
	struct armv7m_common *armv7m = &target_to_cm(target)->armv7m;

	LOG_TARGET_DEBUG(target, "target->state: %s,%s examined",
		target_state_name(target),
		target_was_examined(target) ? "" : " not");

	/* deassert reset lines */
	adapter_deassert_reset();

	enum reset_types jtag_reset_config = jtag_get_reset_config();

	if ((jtag_reset_config & RESET_HAS_SRST) &&
		!(jtag_reset_config & RESET_SRST_NO_GATING) &&
		armv7m->debug_ap) {
		int retval = dap_dp_init_or_reconnect(armv7m->debug_ap->dap);
		if (retval != ERROR_OK) {
			LOG_TARGET_ERROR(target, "DP initialisation failed");
			return retval;
		}
	}

	return ERROR_OK;
}

static int cortex_m_hit_watchpoint(struct target *target, struct watchpoint **hit_watchpoint)
{
	if (target->debug_reason != DBG_REASON_WATCHPOINT)
		return ERROR_FAIL;

	struct cortex_m_common *cortex_m = target_to_cm(target);

	for (struct watchpoint *wp = target->watchpoints; wp; wp = wp->next) {
		if (!wp->is_set)
			continue;

		unsigned int dwt_num = wp->number;
		struct cortex_m_dwt_comparator *comparator = cortex_m->dwt_comparator_list + dwt_num;

		uint32_t dwt_function;
		int retval = target_read_u32(target, comparator->dwt_comparator_address + 8, &dwt_function);
		if (retval != ERROR_OK)
			return ERROR_FAIL;

		/* check the MATCHED bit */
		if (dwt_function & BIT(24)) {
			*hit_watchpoint = wp;
			return ERROR_OK;
		}
	}

	return ERROR_FAIL;
}

static int cortex_m_read_memory(struct target *target, target_addr_t address,
	uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);

	if (armv7m->arm.arch == ARM_ARCH_V6M) {
		/* armv6m does not handle unaligned memory access */
		if ((size == 4 && (address & 0x3u)) || (size == 2 && (address & 0x1u)))
			return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	return mem_ap_read_buf(armv7m->debug_ap, buffer, size, count, address);
}

static int cortex_m_write_memory(struct target *target, target_addr_t address,
	uint32_t size, uint32_t count, const uint8_t *buffer)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);

	if (armv7m->arm.arch == ARM_ARCH_V6M) {
		/* armv6m does not handle unaligned memory access */
		if ((size == 4 && (address & 0x3u)) || (size == 2 && (address & 0x1u)))
			return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	return mem_ap_write_buf(armv7m->debug_ap, buffer, size, count, address);
}

static int cortex_m_init_target(struct command_context *cmd_ctx,
	struct target *target)
{
	armv7m_build_reg_cache(target);
	arm_semihosting_init(target);
	return ERROR_OK;
}

static int cortex_m_dcc_read(struct target *target, uint8_t *value, uint8_t *ctrl)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	uint16_t dcrdr;
	uint8_t buf[2];
	int retval;

	retval = mem_ap_read_buf_noincr(armv7m->debug_ap, buf, 2, 1, DCB_DCRDR);
	if (retval != ERROR_OK)
		return retval;

	dcrdr = target_buffer_get_u16(target, buf);
	*ctrl = (uint8_t)dcrdr;
	*value = (uint8_t)(dcrdr >> 8);

	LOG_TARGET_DEBUG(target, "data 0x%x ctrl 0x%x", *value, *ctrl);

	/* write ack back to software dcc register
	 * signify we have read data */
	if (dcrdr & (1 << 0)) {
		target_buffer_set_u16(target, buf, 0);
		retval = mem_ap_write_buf_noincr(armv7m->debug_ap, buf, 2, 1, DCB_DCRDR);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int cortex_m_target_request_data(struct target *target,
	uint32_t size, uint8_t *buffer)
{
	uint8_t data;
	uint8_t ctrl;
	uint32_t i;

	for (i = 0; i < (size * 4); i++) {
		int retval = cortex_m_dcc_read(target, &data, &ctrl);
		if (retval != ERROR_OK)
			return retval;
		buffer[i] = data;
	}

	return ERROR_OK;
}

static int cortex_m_handle_target_request(void *priv)
{
	struct target *target = priv;
	if (!target_was_examined(target))
		return ERROR_OK;

	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING) {
		uint8_t data;
		uint8_t ctrl;
		int retval;

		retval = cortex_m_dcc_read(target, &data, &ctrl);
		if (retval != ERROR_OK)
			return retval;

		/* check if we have data */
		if (ctrl & (1 << 0)) {
			uint32_t request;

			/* we assume target is quick enough */
			request = data;
			for (int i = 1; i <= 3; i++) {
				retval = cortex_m_dcc_read(target, &data, &ctrl);
				if (retval != ERROR_OK)
					return retval;
				request |= ((uint32_t)data << (i * 8));
			}
			target_request(target, request);
		}
	}

	return ERROR_OK;
}

static int cortex_m_init_arch_info(struct target *target,
	struct cortex_m_common *cortex_m, struct adiv5_dap *dap)
{
	struct armv7m_common *armv7m = &cortex_m->armv7m;

	armv7m_init_arch_info(target, armv7m);

	/* default reset mode is to use srst if fitted
	 * if not it will use CORTEX_M3_RESET_VECTRESET */
	cortex_m->soft_reset_config = CORTEX_M_RESET_VECTRESET;

	armv7m->arm.dap = dap;

	/* register arch-specific functions */
	armv7m->examine_debug_reason = cortex_m_examine_debug_reason;

	armv7m->post_debug_entry = NULL;

	armv7m->pre_restore_context = NULL;

	armv7m->load_core_reg_u32 = cortex_m_load_core_reg_u32;
	armv7m->store_core_reg_u32 = cortex_m_store_core_reg_u32;

	target_register_timer_callback(cortex_m_handle_target_request, 1,
		TARGET_TIMER_TYPE_PERIODIC, target);

	return ERROR_OK;
}

static int cortex_m_target_create(struct target *target, Jim_Interp *interp)
{
	struct adiv5_private_config *pc;

	pc = (struct adiv5_private_config *)target->private_config;
	if (adiv5_verify_config(pc) != ERROR_OK)
		return ERROR_FAIL;

	struct cortex_m_common *cortex_m = calloc(1, sizeof(struct cortex_m_common));
	if (!cortex_m) {
		LOG_TARGET_ERROR(target, "No memory creating target");
		return ERROR_FAIL;
	}

	cortex_m->common_magic = CORTEX_M_COMMON_MAGIC;
	cortex_m->apsel = pc->ap_num;

	cortex_m_init_arch_info(target, cortex_m, pc->dap);

	return ERROR_OK;
}

static const struct command_registration cortex_m_command_handlers[] = {
	{
		.chain = armv7m_command_handlers,
	},
	{
		.chain = armv7m_trace_command_handlers,
	},
	/* START_DEPRECATED_TPIU */
	{
		.chain = arm_tpiu_deprecated_command_handlers,
	},
	/* END_DEPRECATED_TPIU */
	{
		.chain = rtt_target_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type numicro_dap_target = {
	.name = "numicro_dap",

	.poll = cortex_m_poll,
	.arch_state = armv7m_arch_state,

	.target_request_data = cortex_m_target_request_data,

	.halt = cortex_m_halt,
	.resume = cortex_m_resume,
	.step = cortex_m_step,

	.assert_reset = cortex_m_assert_reset,
	.deassert_reset = cortex_m_deassert_reset,
	.soft_reset_halt = cortex_m_soft_reset_halt,

	.get_gdb_arch = arm_get_gdb_arch,
	.get_gdb_reg_list = armv7m_get_gdb_reg_list,

	.read_memory = cortex_m_read_memory,
	.write_memory = cortex_m_write_memory,
	.checksum_memory = armv7m_checksum_memory,
	.blank_check_memory = armv7m_blank_check_memory,

	.run_algorithm = armv7m_run_algorithm,
	.start_algorithm = armv7m_start_algorithm,
	.wait_algorithm = armv7m_wait_algorithm,

	.add_breakpoint = cortex_m_add_breakpoint,
	.remove_breakpoint = cortex_m_remove_breakpoint,
	.add_watchpoint = cortex_m_add_watchpoint,
	.remove_watchpoint = cortex_m_remove_watchpoint,
	.hit_watchpoint = cortex_m_hit_watchpoint,

	.commands = cortex_m_command_handlers,
	.target_create = cortex_m_target_create,
	.target_jim_configure = adiv5_jim_configure,
	.init_target = cortex_m_init_target,
	.examine = cortex_m_examine,
	.deinit_target = cortex_m_deinit_target,

	.profiling = cortex_m_profiling,
};
