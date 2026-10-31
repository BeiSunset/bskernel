#ifndef BS_MODULE_H
#define BS_MODULE_H

/*
 * Central module identity and stage definitions.
 *
 * Keep this file free of Linux headers so it can be included from any
 * translation unit without pulling in unnecessary dependencies.
 */

#define BS_MODULE_VERSION "1.0.6"

enum bs_stage_mode {
	BS_STAGE_FULL = 0,	/* syscall hook + reboot kprobe */
	BS_STAGE_COMPAT = 1,	/* symbol compatibility layer only */
	BS_STAGE_MEM = 2,	/* + process memory */
	BS_STAGE_FILE = 3,	/* + file I/O */
	BS_STAGE_SYSCALL = 4,	/* syscall hook only */
	BS_STAGE_REBOOT = 5,	/* reboot kprobe only */
	BS_STAGE_NO_SYSCALL = 6,/* full, but skip the syscall hook */
};

#endif /* BS_MODULE_H */
