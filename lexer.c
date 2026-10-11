#include "main.h"

// المتغيرات العامة للأسطر والملفات
char **all_lines = NULL;
size_t total_lines = 0;
size_t lines_capacity = 0;

// دالة إضافة وتخزين سطر جديد مع تخصيص ديناميكي آمن للذاكرة
void append_line(const char *line) {
    if (total_lines >= lines_capacity) {
        lines_capacity = lines_capacity == 0 ? 64 : lines_capacity * 2;
        char **new_lines = realloc(all_lines, lines_capacity * sizeof(char*));
        if (!new_lines) {
            fprintf(stderr, "[7ABA Fatal Error]: Out of memory in lexer while allocating lines.\n");
            exit(EXIT_FAILURE);
        }
        all_lines = new_lines;
    }
    
    char clean_buffer[MAX_LINE_LENGTH];
    strncpy(clean_buffer, line, MAX_LINE_LENGTH - 1);
    clean_buffer[MAX_LINE_LENGTH - 1] = '\0';
    
    all_lines[total_lines] = strdup(clean_buffer);
    if (!all_lines[total_lines]) {
        fprintf(stderr, "[7ABA Fatal Error]: Memory allocation failed for line string.\n");
        exit(EXIT_FAILURE);
    }
    total_lines++;
}

// محلل الرموز المتقدم (Tokenizer) - يفكك السطر إلى مصفوفة Tokens احترافية تنافس اللغات الكبرى
Token* tokenize_line(const char *line, size_t *out_count) {
    size_t capacity = 16;
    size_t count = 0;
    Token *tokens = malloc(capacity * sizeof(Token));
    if (!tokens) {
        fprintf(stderr, "[7ABA Fatal Error]: Out of memory during tokenization.\n");
        exit(EXIT_FAILURE);
    }

    const char *p = line;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        // تخطي الفراغات والمسافات البيضاء
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
            break; // نهاية السطر أو بداية تعليق
        }

        // معالجة النصوص المحاطة بعلامات التنصيص ("...")
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p != '\0' && *p != '"') {
                p++;
            }
            size_t len = p - start;
            char *val = malloc(len + 1);
            if (!val) {
                fprintf(stderr, "[7ABA Fatal Error]: Out of memory for string token.\n");
                exit(EXIT_FAILURE);
            }
            strncpy(val, start, len);
            val[len] = '\0';

            if (count >= capacity) {
                capacity *= 2;
                Token *new_tokens = realloc(tokens, capacity * sizeof(Token));
                if (!new_tokens) {
                    fprintf(stderr, "[7ABA Fatal Error]: Out of memory reallocating tokens.\n");
                    exit(EXIT_FAILURE);
                }
                tokens = new_tokens;
            }
            tokens[count].type = TOKEN_STRING;
            tokens[count].lexeme = val;
            count++;
            if (*p == '"') p++;
            continue;
        }

        // معالجة العوامل والرموز الخاصة (=, +, -, *, /, <, >, [, ], {, }, (, ))
        if (*p == '=' || *p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '<' || *p == '>' || *p == '[' || *p == ']' || *p == '{' || *p == '}' || *p == '(' || *p == ')') {
            char op_str[3] = {*p, '\0', '\0'};
            p++;
            // دعم العوامل المركبة مثل ==, >=, <=, !=
            if ((*op_str == '=' && *p == '=') || (*op_str == '>' && *p == '=') || (*op_str == '<' && *p == '=') || (*op_str == '!' && *p == '=')) {
                op_str[1] = *p;
                p++;
            }

            if (count >= capacity) {
                capacity *= 2;
                Token *new_tokens = realloc(tokens, capacity * sizeof(Token));
                if (!new_tokens) {
                    fprintf(stderr, "[7ABA Fatal Error]: Out of memory reallocating tokens.\n");
                    exit(EXIT_FAILURE);
                }
                tokens = new_tokens;
            }
            tokens[count].type = TOKEN_OPERATOR;
            tokens[count].lexeme = strdup(op_str);
            count++;
            continue;
        }

        // معالجة الكلمات المفتاحية والمعرفات (Keywords & Identifiers)
        if (isalpha(*p) || *p == '_') {
            const char *start = p;
            while (isalnum(*p) || *p == '_') {
                p++;
            }
            size_t len = p - start;
            char *val = malloc(len + 1);
            if (!val) {
                fprintf(stderr, "[7ABA Fatal Error]: Out of memory for identifier token.\n");
                exit(EXIT_FAILURE);
            }
            strncpy(val, start, len);
            val[len] = '\0';

            if (count >= capacity) {
                capacity *= 2;
                Token *new_tokens = realloc(tokens, capacity * sizeof(Token));
                if (!new_tokens) {
                    fprintf(stderr, "[7ABA Fatal Error]: Out of memory reallocating tokens.\n");
                    exit(EXIT_FAILURE);
                }
                tokens = new_tokens;
            }
            
            // تحديد ما إذا كانت الكلمة تابعة للكلمات المفتاحية المدعومة في لغة 7ABA
            if (strcmp(val, "display") == 0 || strcmp(val, "input") == 0 || strcmp(val, "while") == 0 || 
                strcmp(val, "repeat") == 0 || strcmp(val, "when") == 0 || strcmp(val, "otherwise") == 0 || 
                strcmp(val, "list") == 0 || strcmp(val, "pile") == 0 || strcmp(val, "push") == 0 || 
                strcmp(val, "pop") == 0 || strcmp(val, "len") == 0 || strcmp(val, "save") == 0 || 
                strcmp(val, "load") == 0 || strcmp(val, "def") == 0 || strcmp(val, "call") == 0) {
                tokens[count].type = TOKEN_KEYWORD;
            } else {
                tokens[count].type = TOKEN_IDENTIFIER;
            }
            tokens[count].lexeme = val;
            count++;
            continue;
        }

        // معالجة الأرقام (Numbers) بدقة
        if (isdigit(*p) || (*p == '-' && isdigit(*(p + 1)))) {
            const char *start = p;
            if (*p == '-') p++;
            while (isdigit(*p)) p++;
            if (*p == '.' && isdigit(*(p + 1))) {
                p++;
                while (isdigit(*p)) p++;
            }
            size_t len = p - start;
            char *val = malloc(len + 1);
            if (!val) {
                fprintf(stderr, "[7ABA Fatal Error]: Out of memory for number token.\n");
                exit(EXIT_FAILURE);
            }
            strncpy(val, start, len);
            val[len] = '\0';

            if (count >= capacity) {
                capacity *= 2;
                Token *new_tokens = realloc(tokens, capacity * sizeof(Token));
                if (!new_tokens) {
                    fprintf(stderr, "[7ABA Fatal Error]: Out of memory reallocating tokens.\n");
                    exit(EXIT_FAILURE);
                }
                tokens = new_tokens;
            }
            tokens[count].type = TOKEN_NUMBER;
            tokens[count].lexeme = val;
            count++;
            continue;
        }

        p++; // تخطي الرموز غير المعرفة الحالية لضمان عدم توقف المفسر
    }

    *out_count = count;
    return tokens;
}

// دالة تحرير وتحرّر الذاكرة الخاصة بالرموز بالكامل لمنع أي تسريب (Memory Leaks)
void free_tokens(Token *tokens, size_t count) {
    if (!tokens) return;
    for (size_t i = 0; i < count; i++) {
        free(tokens[i].lexeme);
    }
    free(tokens);
}
