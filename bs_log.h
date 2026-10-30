#ifndef BS_LOG_H
#define BS_LOG_H

/*
 * Kernel logging gate.
 *
 * The module stays quiet by default: anything that reveals activity (feature
 * init, hooks, injected events, ...) makes the driver easy to spot in
 * logcat/dmesg. Errors are still printed because they only show up when a
 * call actually failed.
 *
 * Load with bs_log_verbose=1 on a test device to get the detailed trace.
 */
extern int bs_log_verbose;

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

#define bs_err(fmt, ...)	pr_err(fmt, ##__VA_ARGS__)

#endif /* BS_LOG_H */