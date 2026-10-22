#include "main.h"
#include "diagnostic.h"

static bool use_color(void)
{
    const char *term = getenv("TERM");
    return getenv("NO_COLOR") == NULL && term != NULL && strcmp(term, "dumb") != 0;
}

static size_t source_length(const char *source)
{
    size_t length = strcspn(source, "\r\n");
    return length;
}

static size_t display_column(const char *source, size_t column)
{
    size_t rendered = 0;
    for (size_t i = 0; i + 1 < column && source[i] != '\0' && source[i] != '\r' && source[i] != '\n'; i++)
        rendered = source[i] == '\t' ? (rendered / 4 + 1) * 4 : rendered + 1;
    return rendered;
}

void report_diagnostic(RuntimeContext *runtime, const char *severity,
                       const char *code, const char *message,
                       int line, size_t column)
{
    report_diagnostic_span(runtime, severity, code, message, line, column, 1);
}

void report_diagnostic_span(RuntimeContext *runtime, const char *severity, const char *code,
                            const char *message, int line, size_t column,
                            size_t span_length)
{
    if (!runtime)
        return;
    if (span_length == 0)
        span_length = 1;
    bool color = use_color();
    const char *red = color ? "\033[1;31m" : "";
    const char *blue = color ? "\033[1;34m" : "";
    const char *reset = color ? "\033[0m" : "";
    const char *filename = runtime->source_filename[0]
                               ? runtime->source_filename
                               : "<input>";

    runtime->diagnostic_count++;
    snprintf(runtime->last_diagnostic.severity,
             sizeof(runtime->last_diagnostic.severity), "%s", severity);
    snprintf(runtime->last_diagnostic.code,
             sizeof(runtime->last_diagnostic.code), "%s", code);
    snprintf(runtime->last_diagnostic.message,
             sizeof(runtime->last_diagnostic.message), "%s", message);
    runtime->last_diagnostic.line = line;
    runtime->last_diagnostic.column = column;
    runtime->last_diagnostic.span_length = span_length;

    fprintf(stderr, "%s:%d:%zu: %s%s%s[%s]: %s\n",
            filename, line, column, red, severity, reset, code, message);

    if (line <= 0 || (size_t)line > runtime->source_line_count ||
        !runtime->source_lines || !runtime->source_lines[line - 1])
        return;

    const char *source = runtime->source_lines[line - 1];
    size_t length = source_length(source);
    size_t source_column = column > 0 ? column - 1 : 0;
    if (source_column > length)
        source_column = length;

    fprintf(stderr, " %5d | %.*s\n       | ", line, (int)length, source);
    size_t caret_column = display_column(source, source_column + 1);
    for (size_t i = 0; i < caret_column; i++)
        fputc(' ', stderr);
    fprintf(stderr, "%s^%s", blue, reset);
    size_t end_column = column > SIZE_MAX - span_length
                            ? SIZE_MAX
                            : column + span_length;
    size_t underline_end = display_column(source, end_column);
    if (underline_end > length)
        underline_end = length;
    for (size_t i = caret_column + 1; i < underline_end; i++)
        fputc('~', stderr);
    fputc('\n', stderr);
    fprintf(stderr, "       = help: check the expression or statement near this location\n");
}
