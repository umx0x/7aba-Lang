#include "main.h"
#include "bytecode.h"
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#endif

static void print_usage(FILE *stream)
{
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  7aba run <script.7ba>\n");
    fprintf(stream, "  7aba <script.7ba>\n");
    fprintf(stream, "  7aba --help\n");
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    if (!SetConsoleOutputCP(CP_UTF8))
    {
        fprintf(stderr, "[7ABA Warning]: unable to set Windows console output code page to UTF-8 (error %lu).\n",
                (unsigned long)GetLastError());
    }
#endif

    const char *script_path = NULL;
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "run") == 0)
    {
        script_path = argv[2];
    }
    else if (argc == 2)
    {
        script_path = argv[1];
    }
    else
    {
        print_usage(stderr);
        return 2;
    }

    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    if (!runtime_init(&runtime))
    {
        fprintf(stderr, "[7ABA Error]: unable to initialize runtime context.\n");
        return 1;
    }
    strncpy(runtime.source_filename, script_path,
            sizeof(runtime.source_filename) - 1);
    runtime.source_filename[sizeof(runtime.source_filename) - 1] = '\0';

    // فتح ملف السكريبت
    FILE *f = fopen(script_path, "r");
    if (!f)
    {
        fprintf(stderr, "[7ABA Error]: Could not open script file '%s'\n", script_path);
        runtime_destroy(&runtime);
        return 1;
    }

    // قراءة أسطر الملف وتخزينها عبر الـ Lexer
    char buffer[MAX_LINE_LENGTH];
    int input_error = 0;
    while (fgets(buffer, sizeof(buffer), f))
    {
        size_t length = strlen(buffer);
        if (length == sizeof(buffer) - 1 && buffer[length - 1] != '\n')
        {
            fprintf(stderr, "[7ABA Error]: source line %zu exceeds the %d-byte line limit.\n",
                    runtime.source_line_count + 1, MAX_LINE_LENGTH - 1);
            input_error = 1;
            break;
        }
        if (!append_line(&runtime, buffer))
        {
            fprintf(stderr, "[7ABA Error]: unable to retain source line %lu.\n",
                    (unsigned long)(runtime.source_line_count + 1));
            input_error = 1;
            break;
        }
    }
    if (ferror(f))
    {
        fprintf(stderr, "[7ABA Error]: failed while reading script '%s'.\n", script_path);
        input_error = 1;
    }
    fclose(f);

    if (input_error || runtime.source_line_count > (size_t)INT_MAX)
    {
        if (runtime.source_line_count > (size_t)INT_MAX)
            fprintf(stderr, "[7ABA Error]: script exceeds the supported line count.\n");
        runtime_destroy(&runtime);
        return 1;
    }

    // تهيئة النطاق الأساسي (Global Scope)
    if (!init_scope(&runtime.call_stack[0], &runtime))
    {
        runtime_destroy(&runtime);
        return 1;
    }

    // استرجاع حالة المتغيرات السابقة إذا وجدت
    load_state_from_persistence(&runtime);
    runtime_reset_execution_steps(&runtime);

    int execution_status = 0;
    if (runtime.source_line_count > 0)
    {
        BytecodeProgram program = BYTECODE_PROGRAM_INIT;
        if (bytecode_compile_program(&runtime, 0,
                                     (int)runtime.source_line_count - 1, &program))
        {
            if (!bytecode_execute(&runtime, &program, NULL))
                execution_status = 1;
            bytecode_free(&program);
        }
        else
        {
            execution_status = 1;
        }
    }

    // تنظيف وتحرير الذاكرة بالكامل لمنع تسريب الذاكرة (Memory Leaks)
    runtime_destroy(&runtime);

    return execution_status;
}
