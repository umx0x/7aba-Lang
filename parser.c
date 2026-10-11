#include "main.h"

// المتغيرات العامة للدوال
Function *funcs = NULL;
size_t func_count = 0;
size_t func_capacity = 0;

// دالة تسجيل وتتبع الدوال البرمجية في السكريبت
void add_function(const char *name, int start, int end) {
    if (func_count >= func_capacity) {
        func_capacity = func_capacity == 0 ? 8 : func_capacity * 2;
        Function *new_funcs = realloc(funcs, func_capacity * sizeof(Function));
        if (!new_funcs) {
            fprintf(stderr, "[7ABA Fatal Error]: Out of memory while allocating functions.\n");
            exit(EXIT_FAILURE);
        }
        funcs = new_funcs;
    }
    strncpy(funcs[func_count].name, name, 63);
    funcs[func_count].name[63] = '\0';
    funcs[func_count].start_line = start;
    funcs[func_count].end_line = end;
    func_count++;
}

// التحقق مما إذا كان السطر الحالي يقع داخل نطاق دالة معرفة مسبقاً
bool is_inside_function(int line_idx) {
    for (size_t i = 0; i < func_count; i++) {
        if (line_idx >= funcs[i].start_line && line_idx <= funcs[i].end_line) {
            return true;
        }
    }
    return false;
}

// خوارزمية Levenshtein Distance العالمية لقياس التشابه وحساب الأخطاء الإملائية
int levenshtein_distance(const char *s1, const char *s2) {
    int len1 = strlen(s1);
    int len2 = strlen(s2);
    int *matrix = malloc((len1 + 1) * (len2 + 1) * sizeof(int));
    if (!matrix) return 999;

#define MATRIX_AT(r, c) matrix[(r) * (len2 + 1) + (c)]

    for (int i = 0; i <= len1; i++) MATRIX_AT(i, 0) = i;
    for (int j = 0; j <= len2; j++) MATRIX_AT(0, j) = j;

    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            int cost = (tolower(s1[i - 1]) == tolower(s2[j - 1])) ? 0 : 1;
            int deletion = MATRIX_AT(i - 1, j) + 1;
            int insertion = MATRIX_AT(i, j - 1) + 1;
            int substitution = MATRIX_AT(i - 1, j - 1) + cost;

            int min = deletion;
            if (insertion < min) min = insertion;
            if (substitution < min) min = substitution;
            MATRIX_AT(i, j) = min;
        }
    }

    int result = MATRIX_AT(len1, len2);
    free(matrix);
#undef MATRIX_AT
    return result;
}

// تحويل النصوص إلى أحرف صغيرة للمقارنة المرنة
void to_lowercase(char *dest, const char *src) {
    int i = 0;
    while (src[i]) {
        dest[i] = tolower((unsigned char)src[i]);
        i++;
    }
    dest[i] = '\0';
}

// نظام التصحيح الذكي للأوامر (Smart Auto-Correction) ينافس أقوى المترجمات العالمية
void autocorrect_command(char *cmd_buf, const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    int i = 0;
    char raw_cmd[64];
    while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '=' && line[i] != '<' && line[i] != '>' && line[i] != '[') {
        raw_cmd[i] = line[i];
        i++;
    }
    raw_cmd[i] = '\0';

    if (strlen(raw_cmd) == 0) {
        cmd_buf[0] = '\0';
        return;
    }

    char lower_cmd[64];
    to_lowercase(lower_cmd, raw_cmd);

    // قائمة الكلمات المفتاحية الرسمية للغة 7ABA
    const char *known_cmds[] = {
        "display", "input", "when", "otherwise", "repeat", "while", 
        "list", "pile", "push", "pop", "len", "save", "load", "def", "call"
    };
    int num_cmds = 15;

    // مطابقة تامة أولاً
    for (int c = 0; c < num_cmds; c++) {
        if (strcmp(lower_cmd, known_cmds[c]) == 0) {
            strcpy(cmd_buf, known_cmds[c]);
            return;
        }
    }

    // تصحيح ذكي بالاعتماد على المسافة (إذا كان الخطأ الحرفي بسيطاً جداً)
    int best_distance = 999;
    int best_match_idx = -1;
    for (int c = 0; c < num_cmds; c++) {
        int dist = levenshtein_distance(lower_cmd, known_cmds[c]);
        if (dist < best_distance) {
            best_distance = dist;
            best_match_idx = c;
        }
    }

    // إذا كان الخطأ ضمن نطاق مقبول (حرف أو حرفين)، يتم التصحيح تلقائياً
    if (best_distance <= 2 && best_match_idx != -1) {
        strcpy(cmd_buf, known_cmds[best_match_idx]);
    } else {
        strcpy(cmd_buf, raw_cmd);
    }
}

// دالة التحقق من الأوامر
bool match_command(const char *line, const char *cmd) {
    char corrected[64];
    autocorrect_command(corrected, line);
    return (strcmp(corrected, cmd) == 0);
}

// تخطي اسم الأمر للوصول إلى المعاملات
char* skip_cmd(char *p, const char *cmd) {
    (void)cmd;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// تحليل الشروط البرمجية (مثل الحلقات والشروط) بدقة تامة
void parse_condition_tokens(const char *src, char *var_name, char *op, char *val_str) {
    while (*src == ' ' || *src == '\t') src++;
    int i = 0;
    while (*src && *src != ' ' && *src != '\t' && *src != '<' && *src != '>' && *src != '=') {
        var_name[i++] = *src++;
    }
    var_name[i] = '\0';

    while (*src == ' ' || *src == '\t') src++;
    int o = 0;
    while (*src == '<' || *src == '>' || *src == '=') {
        op[o++] = *src++;
    }
    op[o] = '\0';

    while (*src == ' ' || *src == '\t') src++;
    int v = 0;
    while (*src && *src != ' ' && *src != '\t' && *src != '\r' && *src != '\n' && *src != '{') {
        val_str[v++] = *src++;
    }
    val_str[v] = '\0';
}
