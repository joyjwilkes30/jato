/*
 * Copyright (c) 2008  Pekka Enberg
 * 
 * This file is released under the GPL version 2 with the following
 * clarification and special exception:
 *
 *     Linking this library statically or dynamically with other modules is
 *     making a combined work based on this library. Thus, the terms and
 *     conditions of the GNU General Public License cover the whole
 *     combination.
 *
 *     As a special exception, the copyright holders of this library give you
 *     permission to link this library with independent modules to produce an
 *     executable, regardless of the license terms of these independent
 *     modules, and to copy and distribute the resulting executable under terms
 *     of your choice, provided that you also meet, for each linked independent
 *     module, the terms and conditions of the license of that module. An
 *     independent module is a module which is not derived from or based on
 *     this library. If you modify this library, you may extend this exception
 *     to your version of the library, but you are not obligated to do so. If
 *     you do not wish to do so, delete this exception statement from your
 *     version.
 *
 * Please refer to the file LICENSE for details.
 */

#include "jit/compilation-unit.h"
#include "jit/stack-slot.h"
#include "jit/compiler.h"

#include "arch/instruction.h"

#include "lib/bitset.h"
#include "lib/buffer.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>

struct live_interval_mapping {
	struct live_interval *from, *to;
};

static struct insn *last_insn(struct live_interval *interval)
{
	unsigned long end;
	struct insn *ret;

	end = range_len(&interval->range) - 1;
	ret = interval->insn_array[end];
	assert(ret != NULL);

	return ret;
}

static struct stack_slot *
spill_interval(struct live_interval *interval,
	       struct compilation_unit *cu,
	       struct insn *last)
{
	struct stack_slot *slot;
	struct var_info *reg;
	struct insn *spill;
	/*
	 * We've already done register allocation, so use fixed registers for
	 * spilling and reloading.
	 */
	reg = get_fixed_var(cu, interval->reg);

	slot = get_spill_slot_32(cu->stack_frame);
	if (!slot)
		return NULL;

	spill = spill_insn(reg, slot);
	if (!spill)
		return NULL;

	spill->bytecode_offset = last->bytecode_offset;

	list_add(&spill->insn_list_node, &last->insn_list_node);

	return slot;
}

static int
insert_spill_insn(struct live_interval *interval, struct compilation_unit *cu)
{
	interval->spill_slot = spill_interval(interval, cu, last_insn(interval));

	if (!interval->spill_slot)
		return -ENOMEM;

	return 0;
}

static struct insn *first_insn(struct live_interval *interval)
{
	struct insn *ret;

	ret = interval->insn_array[0];
	assert(ret != NULL);

	return ret;
}

static int insert_reload_insn(struct live_interval *interval,
			      struct compilation_unit *cu,
			      struct stack_slot *from,
			      struct insn *first)
{
	struct insn *reload;
	struct var_info *reg;

	reg = get_fixed_var(cu, interval->reg);
	reload = reload_insn(from, reg);
	if (!reload)
		return -ENOMEM;

	reload->bytecode_offset = first->bytecode_offset;

	list_add_tail(&reload->insn_list_node, &first->insn_list_node);

	return 0;
}

static int
insert_copy_slot_insn(struct live_interval *interval,
		      struct compilation_unit *cu,
		      struct stack_slot *from,
		      struct stack_slot *to,
		      struct insn *push_at_insn,
		      struct insn *pop_at_insn)
{
	struct insn *push, *pop;

	push = push_slot_insn(from);
	if (!push)
		return -ENOMEM;
	push->bytecode_offset = push_at_insn->bytecode_offset;

	pop = pop_slot_insn(to);
	if (!pop) {
		free_insn(push);
		return -ENOMEM;
	}
	pop->bytecode_offset = pop_at_insn->bytecode_offset;

	list_add(&push->insn_list_node, &push_at_insn->insn_list_node);
	list_add(&pop->insn_list_node, &push->insn_list_node);

	return 0;

}

static int __insert_spill_reload_insn(struct live_interval *interval, struct compilation_unit *cu)
{
	int err = 0;

	if (range_is_empty(&interval->range))
		goto out;

	if (interval->need_reload) {
		err = insert_reload_insn(interval, cu,
				interval->spill_parent->spill_slot,
				first_insn(interval));
		if (err)
			goto out;
	}

	if (interval->need_spill) {
		err = insert_spill_insn(interval, cu);
		if (err)
			goto out;
	}
out:
	return err;
}

static void insert_mov_insns(struct compilation_unit *cu,
			     struct live_interval_mapping *mappings,
			     int nr_mapped,
			     struct basic_block *from_bb,
			     struct basic_block *to_bb)
{
	struct stack_slot *slots[nr_mapped];
	int i;

	/* Spill all intervals that have to be resolved */
	for (i = 0; i < nr_mapped; i++) {
		struct insn *spill_at_insn, *reload_at_insn;
		struct live_interval *from_it, *to_it;

		spill_at_insn	= bb_last_insn(from_bb);
		from_it		= mappings[i].from;
		to_it		= mappings[i].to;

		printf("%s: to: %p, from: %p, i: %d\n", __func__, to_it, from_it, i);

		if (from_it->need_spill)
			slots[i] = from_it->spill_slot;
		else
			slots[i] = spill_interval(from_it, cu, spill_at_insn);

		/* Reload those intervals into their new location */
		reload_at_insn = bb_first_insn(to_bb);

		if (to_it->need_reload) {
			insert_copy_slot_insn(mappings[i].to, cu, slots[i],
					to_it->spill_parent->spill_slot,
					spill_at_insn, reload_at_insn);
		} else {
			insert_reload_insn(from_it, cu, slots[i], reload_at_insn);
		}
	}
}

static void maybe_add_mapping(struct live_interval_mapping *mappings,
			      struct compilation_unit *cu,
			      struct basic_block *from,
			      struct basic_block *to,
			      unsigned long vreg,
			      int *nr_mapped)
{
	struct live_interval *parent_it;
	struct live_interval *from_it, *to_it;

	parent_it	= vreg_start_interval(cu, vreg);
	from_it		= interval_child_at(parent_it, from->end);
	to_it		= interval_child_at(parent_it, to->start);

	/*
	 * The intervals are the same on both sides of the basic block edge.
	 */
	if (from_it == to_it)
		return;

	/*
	 * We seem to have some vregs that are alive at the beginning of a
	 * basic block but have no interval covering them. In that case, no
	 * mov instruction is to be inserted.
	 */
	if (!from_it || !to_it)
		return;

	/*
	 * If any of the intervals have no register assigned at this point, it
	 * is because the register allocator found out the interval is useless.
	 * In that case, we need to find what the *real* destination interval
	 * is.
	 */
	while (to_it->reg == REG_UNASSIGNED) {
		to_it = to_it->next_child;
		assert(to_it);
	}

	if (from_it->reg == REG_UNASSIGNED)
		return;

	assert(to_it);
	assert(from_it);

	printf("%s: to: %p, from: %p, nr_mapped: %d\n", __func__, to_it, from_it, *nr_mapped);

	mappings[*nr_mapped].from	= from_it;
	mappings[*nr_mapped].to		= to_it;
	(*nr_mapped)++;
}

static void resolve_data_flow(struct compilation_unit *cu)
{
	struct basic_block *from;
	unsigned long vreg;

	/*
	 * This implements the data flow resolution algorithm described in
	 * Section 5.8 ("Resolving the Data Flow") of Wimmer 2004.
	 */
	for_each_basic_block(from, &cu->bb_list) {
		unsigned int i;

		for (i = 0; i < from->nr_successors; i++) {
			struct live_interval_mapping mappings[from->nr_predecessors];
			struct basic_block *to;
			int nr_mapped = 0;

			if (from->nr_predecessors == 0)
				continue;

			memset(mappings, 0, sizeof(mappings));

			to = from->successors[i];

			for (vreg = 0; vreg < cu->nr_vregs; vreg++) {
				if (test_bit(to->live_in_set->bits, vreg)) {
					maybe_add_mapping(mappings, cu, from, to, vreg, &nr_mapped);
				}
			}

			insert_mov_insns(cu, mappings, nr_mapped, from, to);
		}
	}
}

int insert_spill_reload_insns(struct compilation_unit *cu)
{
	struct var_info *var;
	int err = 0;

	for_each_variable(var, cu->var_infos) {
		struct live_interval *interval;

		for (interval = var->interval; interval != NULL; interval = interval->next_child) {
			err = __insert_spill_reload_insn(interval, cu);
			if (err)
				break;
		}
	}

	/*
	 * Make sure intervals spilled across basic block boundaries will be
	 * reloaded correctly.
	 */
	resolve_data_flow(cu);

	return err;
}
