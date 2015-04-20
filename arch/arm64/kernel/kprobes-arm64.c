/*
 * arch/arm64/kernel/kprobes-arm64.c
 *
 * Copyright (C) 2013 Linaro Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <asm/kprobes.h>
#include <asm/insn.h>

#include "kprobes-arm64.h"

static bool __kprobes aarch64_insn_is_steppable(u32 insn)
{
	if (aarch64_get_insn_class(insn) == AARCH64_INSN_CLS_BR_SYS) {
		if (aarch64_insn_is_branch(insn))
			return false;

		/* modification of daif creates issues */
		if (aarch64_insn_is_daif_access(insn))
			return false;

		if (aarch64_insn_is_exception(insn))
			return false;

		if (aarch64_insn_is_hint(insn))
			return aarch64_insn_is_nop(insn);

		return true;
	}

	if (aarch64_insn_uses_literal(insn))
		return false;

	if (aarch64_insn_is_exclusive(insn))
		return false;

	return true;
}

/* Return:
 *   INSN_REJECTED     If instruction is one not allowed to kprobe,
 *   INSN_GOOD         If instruction is supported and uses instruction slot,
 *   INSN_GOOD_NO_SLOT If instruction is supported but doesn't use its slot.
 */
enum kprobe_insn __kprobes
arm_kprobe_decode_insn(kprobe_opcode_t insn, struct arch_specific_insn *asi)
{
	/*
	 * Instructions reading or modifying the PC won't work from the XOL
	 * slot.
	 */
	if (aarch64_insn_is_steppable(insn))
		return INSN_GOOD;
	else
		return INSN_REJECTED;
}
