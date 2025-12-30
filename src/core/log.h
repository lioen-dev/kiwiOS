#ifndef CORE_LOG_H
#define CORE_LOG_H

void log_info(const char *component, const char *fmt, ...);
void log_ok(const char *component, const char *fmt, ...);
void log_error(const char *component, const char *fmt, ...);

#endif // CORE_LOG_H
