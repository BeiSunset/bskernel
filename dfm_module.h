#ifndef DFM_MODULE_H
#define DFM_MODULE_H

/*
 * Central module identity and stage definitions.
 *
 * Keep this file free of Linux headers so it can be included from any
 * translation unit without pulling in unnecessary dependencies.
 */

#define DFM_MODULE_VERSION "1.0.6"

enum dfm_stage_mode {
	DFM_STAGE_FULL = 0,	/* syscall hook + reboot kprobe */
	DFM_STAGE_COMPAT = 1,	/* symbol compatibility layer only */
	DFM_STAGE_MEM = 2,	/* + process memory */
	DFM_STAGE_FILE = 3,	/* + file I/O */
	DFM_STAGE_SYSCALL = 4,	/* syscall hook only */
	DFM_STAGE_REBOOT = 5,	/* reboot kprobe only */
	DFM_STAGE_NO_SYSCALL = 6,/* full, but skip the syscall hook */
};

#endif /* DFM_MODULE_H */
