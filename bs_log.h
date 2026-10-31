#ifndef BS_LOG_H
#define BS_LOG_H

/*
 * Kernel logging policy.
 *
 * A release build carries no diagnostic strings at all: bs_info()/bs_warn()
 * compile to nothing unless BS_LOG_VERBOSE_BUILD is defined, so the loaded
 * module cannot be fingerprinted by scanning its .rodata for feature names
 * such as "injected events" or "hide armed". Errors are kept because they only
 * appear when a call actually failed.
 *
 * Build with -DBS_LOG_VERBOSE_BUILD (and load with bs_log_verbose=1) on a test
 * device when the detailed trace is needed.
 */
extern int bs_log_verbose;

#ifdef BS_LOG_VERBOSE_BUILD

#define bs_info(fmt, ...)						\
	do {								\
		if (bs_log_verbose)					\
			pr_info(fmt, ##__VA_ARGS__);			\
	} while (0)

#define bs_info_ratelimited(fmt, ...)					\
	do {								\
		if (bs_log_verbose)					\
			pr_info_ratelimited(fmt, ##__VA_ARGS__);	\
	} while (0)

#define bs_warn(fmt, ...)						\
	do {								\
		if (bs_log_verbose)					\
			pr_warn(fmt, ##__VA_ARGS__);			\
	} while (0)

#define bs_warn_ratelimited(fmt, ...)					\
	do {								\
		if (bs_log_verbose)					\
			pr_warn_ratelimited(fmt, ##__VA_ARGS__);	\
	} while (0)

#else /* !BS_LOG_VERBOSE_BUILD */

#define bs_info(fmt, ...)		do { } while (0)
#define bs_info_ratelimited(fmt, ...)	do { } while (0)
#define bs_warn(fmt, ...)		do { } while (0)
#define bs_warn_ratelimited(fmt, ...)	do { } while (0)

#endif /* BS_LOG_VERBOSE_BUILD */

#define bs_err(fmt, ...)	pr_err(fmt, ##__VA_ARGS__)

#endif /* BS_LOG_H */