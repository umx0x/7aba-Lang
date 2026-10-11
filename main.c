#include "main.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: 7aba <script.7ba>\n");
        return 1;
    }

    // حفظ اسم الملف الحالي لتفعيل نظام الحفظ والاسترجاع الآلي (Persistence)
    strncpy(current_script_filename, argv[1], sizeof(current_script_filename) - 1);
    current_script_filename[sizeof(current_script_filename) - 1] = '\0';

    // فتح ملف السكريبت
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "[7ABA Error]: Could not open script file '%s'\n", argv[1]);
        return 1;
    }

    // قراءة أسطر الملف وتخزينها عبر الـ Lexer
    char buffer[MAX_LINE_LENGTH];
    while (fgets(buffer, sizeof(buffer), f)) {
        append_line(buffer);
    }
    fclose(f);

    // تهيئة النطاق الأساسي (Global Scope)
    init_scope(&call_stack[0]);
    stack_top = 0;

    // استرجاع حالة المتغيرات السابقة إذا وجدت
    load_state_from_persistence();

    // بدء تنفيذ الكود البرمجي بالكامل
    if (total_lines > 0) {
        execute_block(0, (int)total_lines - 1);
    }

    // تنظيف وتحرير الذاكرة بالكامل لمنع تسريب الذاكرة (Memory Leaks)
    free_scope(&call_stack[0]);

    if (all_lines) {
        for (size_t i = 0; i < total_lines; i++) {
            free(all_lines[i]);
        }
        free(all_lines);
    }

    if (funcs) {
        free(funcs);
    }

    return 0;
}
