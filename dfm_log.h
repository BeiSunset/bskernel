#ifndef DFM_LOG_H
#define DFM_LOG_H

/*
 * Kernel logging policy.
 *
 * A release build carries no diagnostic strings at all: dfm_info()/dfm_warn()
 * compile to nothing unless DFM_LOG_VERBOSE_BUILD is defined, so the loaded
 * module cannot be fingerprinted by scanning its .rodata for feature names
 * such as "injected events" or "hide armed". Errors are kept because they only
 * appear when a call actually failed.
 *
 * Build with -DDFM_LOG_VERBOSE_BUILD on a test
 * device when the detailed trace is needed.
 */
#ifdef DFM_LOG_VERBOSE_BUILD

#define dfm_info(fmt, ...)		pr_info(fmt, ##__VA_ARGS__)
#define dfm_info_ratelimited(fmt, ...)	pr_info_ratelimited(fmt, ##__VA_ARGS__)
#define dfm_warn(fmt, ...)		pr_warn(fmt, ##__VA_ARGS__)
#define dfm_warn_ratelimited(fmt, ...)	pr_warn_ratelimited(fmt, ##__VA_ARGS__)

#else /* !DFM_LOG_VERBOSE_BUILD */

#define dfm_info(fmt, ...)		do { } while (0)
#define dfm_info_ratelimited(fmt, ...)	do { } while (0)
#define dfm_warn(fmt, ...)		do { } while (0)
#define dfm_warn_ratelimited(fmt, ...)	do { } while (0)

#endif /* DFM_LOG_VERBOSE_BUILD */

#define dfm_err(fmt, ...)	pr_err(fmt, ##__VA_ARGS__)

#endif /* DFM_LOG_H */