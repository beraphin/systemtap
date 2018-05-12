/* -*- linux-c -*-
 * Common functions for using dyninst-based uprobes
 * Copyright (C) 2012-2013 Red Hat Inc.
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */

#ifndef _UPROBES_DYNINST_C_
#define _UPROBES_DYNINST_C_

/* STAPDU: SystemTap Dyninst Uprobes */

// loc2c-generated code assumes pt_regs are available, so use this to make
// sure we always have *something* for it to dereference...
static struct pt_regs stapdu_dummy_uregs = {};

/* These functions implement the ABI in stapdyn.h
 *
 * NB: The translator will generate two arrays used here:
 *   struct stapdu_target stapdu_targets[];
 *   struct stapdu_probe stapdu_probes[];
 */


uint64_t stp_dyninst_target_count(void)
{
	return ARRAY_SIZE(stapdu_targets);
}

const char* stp_dyninst_target_path(uint64_t index)
{
	if (index >= stp_dyninst_target_count())
		return NULL;
	return stapdu_targets[index].path;
}


uint64_t stp_dyninst_probe_count(void)
{
	return ARRAY_SIZE(stapdu_probes);
}

uint64_t stp_dyninst_probe_target(uint64_t index)
{
	if (index >= stp_dyninst_probe_count())
		return (uint64_t)-1;
	return stapdu_probes[index].target;
}

uint64_t stp_dyninst_probe_offset(uint64_t index)
{
	if (index >= stp_dyninst_probe_count())
		return (uint64_t)-1;
	return stapdu_probes[index].offset;
}

uint64_t stp_dyninst_probe_semaphore(uint64_t index)
{
	if (index >= stp_dyninst_probe_count())
		return (uint64_t)-1;
	return stapdu_probes[index].semaphore;
}

uint64_t stp_dyninst_probe_flags(uint64_t index)
{
	if (index >= stp_dyninst_probe_count())
		return (uint64_t)-1;
	return stapdu_probes[index].flags;
}

#endif /* _UPROBES_DYNINST_C_ */

