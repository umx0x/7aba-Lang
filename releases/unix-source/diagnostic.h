#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#include <stddef.h>

typedef struct RuntimeContext RuntimeContext;

void report_diagnostic(RuntimeContext *runtime, const char *severity,
                       const char *code, const char *message,
                       int line, size_t column);
void report_diagnostic_span(RuntimeContext *runtime, const char *severity, const char *code,
                            const char *message, int line, size_t column,
                            size_t span_length);

#endif
