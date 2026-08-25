/*
 * hearth_log.h - logging indirection. The platform implements
 * hearth_log_write(); core code only uses the macros.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEARTH_LOG_ERROR = 0,
    HEARTH_LOG_WARN  = 1,
    HEARTH_LOG_INFO  = 2,
} hearth_log_level_t;

void hearth_log_write(hearth_log_level_t level, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define HEARTH_LOGE(tag, ...) hearth_log_write(HEARTH_LOG_ERROR, tag, __VA_ARGS__)
#define HEARTH_LOGW(tag, ...) hearth_log_write(HEARTH_LOG_WARN,  tag, __VA_ARGS__)
#define HEARTH_LOGI(tag, ...) hearth_log_write(HEARTH_LOG_INFO,  tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
