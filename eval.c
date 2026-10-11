#include "main.h"

// المتغيرات العامة للنطاقات والتنفيذ
Scope call_stack[MAX_STACK_DEPTH];
int stack_top = 0;
int current_line_number = 0;
char current_script_filename[256] = "";

// تهيئة النطاق (Scope) مع حماية أمنية للذاكرة
void init_scope(Scope *s) {
    s->count = 0;
    s->capacity = 8;
    s->vars = malloc(s->capacity * sizeof(Variable));
    if (!s->vars) {
        fprintf(stderr, "[7ABA Enterprise Security Error]: Out of memory allocating scope.\n");
        exit(EXIT_FAILURE);
    }
}

// تحرير النطاق والذاكرة المرتبطة به بالكامل (Zero-Leak RAII Engine)
void free_scope(Scope *s) {
    if (!s || !s->vars) return;
    for (size_t i = 0; i < (size_t)s->count; i++) {
        if (s->vars[i].is_list) {
            free(s->vars[i].list_val.items);
            s->vars[i].list_val.items = NULL;
        }
    }
    free(s->vars);
    s->vars = NULL;
    s->count = 0;
    s->capacity = 0;
}

// البحث عن متغير داخل نطاق معين
Variable* find_var_in_scope(Scope *s, const char *name) {
    if (!s || !s->vars) return NULL;
    for (size_t i = 0; i < (size_t)s->count; i++) {
        if (strcmp(s->vars[i].name, name) == 0) return &s->vars[i];
    }
    return NULL;
}

// الحصول على كائن المتغير عبر مكدس النطاقات (Scope Chain)
Variable* get_var_obj(const char *name) {
    for (int i = stack_top; i >= 0; i--) {
        Variable *v = find_var_in_scope(&call_stack[i], name);
        if (v) return v;
    }
    return NULL;
}

// الحصول على المتغير في النطاق الحالي أو إنشائه تلقائياً مع توسيع آمن للذاكرة
Variable* get_or_create_var(const char *name) {
    Scope *s = &call_stack[stack_top];
    Variable *v = find_var_in_scope(s, name);
    if (v) return v;

    if ((size_t)s->count >= s->capacity) {
        size_t new_cap = s->capacity * 2;
        Variable *new_vars = realloc(s->vars, new_cap * sizeof(Variable));
        if (!new_vars) {
            fprintf(stderr, "[7ABA Enterprise Security Error]: Out of memory expanding scope variables.\n");
            exit(EXIT_FAILURE);
        }
        s->capacity = new_cap;
        s->vars = new_vars;
    }
    Variable *new_v = &s->vars[s->count++];
    memset(new_v, 0, sizeof(Variable));
    strncpy(new_v->name, name, 63);
    new_v->name[63] = '\0';
    return new_v;
}

// --- نظام تحليل التعابير الحسابية المتقدم والدقيق (يدعم الفهارس الديناميكية المتداخلة) ---
int parse_expr_str(const char **expr_ptr);

int parse_factor(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;

    if (**p == '(') {
        (*p)++;
        int val = parse_expr_str(p);
        while (**p == ' ' || **p == '\t') (*p)++;
        if (**p == ')') (*p)++;
        return val;
    }

    if (isdigit(**p) || (**p == '-' && isdigit(*((*p)+1)))) {
        return (int)strtol(*p, (char **)p, 10);
    }

    char token[128];
    int idx = 0;
    while (**p && (isalnum(**p) || **p == '_' || **p == '[' || **p == ']')) {
        token[idx++] = **p;
        (*p)++;
    }
    token[idx] = '\0';

    if (strlen(token) == 0) return 0;

    // فحص ما إذا كان التعبير عبارة عن وصول لمصفوفة بفهرس ثابت أو متغير ديناميكي (مثل list[idx])
    char var_name[64];
    char index_expr[64];
    if (sscanf(token, "%[^[][%[^]]]", var_name, index_expr) == 2) {
        int resolved_index = 0;
        int is_num_idx = 1;
        for (int i = 0; index_expr[i] != '\0'; i++) {
            if (!isdigit(index_expr[i]) && !(i == 0 && index_expr[0] == '-')) { is_num_idx = 0; break; }
        }
        if (is_num_idx) {
            resolved_index = atoi(index_expr);
        } else {
            resolved_index = get_val(index_expr);
        }

        Variable *v = get_var_obj(var_name);
        if (v && v->is_list && resolved_index >= 0 && (size_t)resolved_index < v->list_val.size) {
            return v->list_val.items[resolved_index];
        }
        return 0;
    }

    // فحص إن كان رقماً عادياً
    int is_numeric = 1;
    for (int i = 0; token[i] != '\0'; i++) {
        if (!isdigit(token[i]) && !(i == 0 && token[0] == '-')) {
            is_numeric = 0;
            break;
        }
    }
    if (is_numeric) {
        return atoi(token);
    }

    // جلب قيمة المتغير العادي
    Variable *v = get_var_obj(token);
    return v ? v->ival : 0;
}

int parse_term(const char **p) {
    int val = parse_factor(p);
    while (1) {
        while (**p == ' ' || **p == '\t') (*p)++;
        if (**p == '*') {
            (*p)++;
            val *= parse_factor(p);
        } else if (**p == '/') {
            (*p)++;
            int divisor = parse_factor(p);
            if (divisor != 0) val /= divisor;
            else {
                printf("[7ABA Guard Error at line %d]: Division by zero detected and prevented!\n", current_line_number);
            }
        } else {
            break;
        }
    }
    return val;
}

int parse_expr_str(const char **p) {
    int val = parse_term(p);
    while (1) {
        while (**p == ' ' || **p == '\t') (*p)++;
        if (**p == '+') {
            (*p)++;
            val += parse_term(p);
        } else if (**p == '-') {
            (*p)++;
            val -= parse_term(p);
        } else {
            break;
        }
    }
    return val;
}

int get_val(const char *str) {
    const char *p = str;
    return parse_expr_str(&p);
}

// --- نظام القوائم والجداول الأصلية (Native DataFrames) ---
void init_list(ListData *l) {
    l->size = 0;
    l->capacity = 4;
    l->items = malloc(l->capacity * sizeof(int));
    if (!l->items) {
        fprintf(stderr, "[7ABA Enterprise Security Error]: Out of memory allocating list items.\n");
        exit(EXIT_FAILURE);
    }
}

void create_list(const char *name) {
    Variable *v = get_or_create_var(name);
    if (!v->is_list) {
        init_list(&v->list_val);
    }
    v->is_list = 1;
    v->is_string = 0;
    v->list_val.size = 0;
}

void push_to_list(const char *name, int val) {
    Variable *v = get_var_obj(name);
    if (v && v->is_list) {
        if (v->list_val.size >= (size_t)v->list_val.capacity) {
            size_t new_cap = v->list_val.capacity * 2;
            int *new_items = realloc(v->list_val.items, new_cap * sizeof(int));
            if (!new_items) {
                fprintf(stderr, "[7ABA Enterprise Security Error]: Out of memory expanding list.\n");
                exit(EXIT_FAILURE);
            }
            v->list_val.capacity = (int)new_cap;
            v->list_val.items = new_items;
        }
        v->list_val.items[v->list_val.size++] = val;
    }
}

void pop_from_list(const char *name) {
    Variable *v = get_var_obj(name);
    if (v && v->is_list && v->list_val.size > 0) {
        v->list_val.size--;
    }
}

void get_list_len(const char *list_name, const char *var_name) {
    Variable *v = get_var_obj(list_name);
    Variable *target_var = get_or_create_var(var_name);
    target_var->ival = (v && v->is_list) ? (int)v->list_val.size : 0;
    target_var->is_string = 0;
    target_var->is_list = 0;
}

void print_professional_table(const char *name, ListData *list) {
    printf("\n+-----------------------------+\n");
    printf("| Table Name: %-15s |\n", name);
    printf("+-----------+-----------------+\n");
    printf("|   Index   |      Value      |\n");
    printf("+-----------+-----------------+\n");

    if (list->size == 0) {
        printf("|   Empty   |        -        |\n");
    } else {
        for (size_t i = 0; i < list->size; i++) {
            printf("|    %-3zu    |     %-7d     |\n", i, list->items[i]);
        }
    }
    printf("+-----------+-----------------+\n\n");
}

void save_list_to_file(const char *name, const char *filename) {
    Variable *v = get_var_obj(name);
    if (v && v->is_list) {
        FILE *f = fopen(filename, "w");
        if (f) {
            for (size_t i = 0; i < v->list_val.size; i++) {
                fprintf(f, "%d\n", v->list_val.items[i]);
            }
            fclose(f);
            printf("[7ABA Enterprise Info]: Successfully saved dataframe to %s\n", filename);
        } else {
            printf("[7ABA Guard Error at line %d]: Could not open file '%s' for writing.\n", current_line_number, filename);
        }
    }
}

void load_list_from_file(const char *name, const char *filename) {
    create_list(name);
    Variable *v = get_var_obj(name);
    if (v && v->is_list) {
        FILE *f = fopen(filename, "r");
        if (f) {
            v->list_val.size = 0;
            char buffer[64];
            while (fgets(buffer, sizeof(buffer), f)) {
                buffer[strcspn(buffer, "\r\n")] = 0;
                if (strlen(buffer) > 0) {
                    if (v->list_val.size >= (size_t)v->list_val.capacity) {
                        size_t new_cap = v->list_val.capacity * 2;
                        int *new_items = realloc(v->list_val.items, new_cap * sizeof(int));
                        if (!new_items) {
                            fprintf(stderr, "[7ABA Enterprise Security Error]: Out of memory loading list.\n");
                            exit(EXIT_FAILURE);
                        }
                        v->list_val.capacity = (int)new_cap;
                        v->list_val.items = new_items;
                    }
                    v->list_val.items[v->list_val.size++] = atoi(buffer);
                }
            }
            fclose(f);
            printf("[7ABA Enterprise Info]: Successfully loaded dataframe from %s\n", filename);
        } else {
            printf("[7ABA Guard Error at line %d]: Could not find or open file '%s' for reading.\n", current_line_number, filename);
        }
    }
}

void save_state_to_persistence() {
    if (strlen(current_script_filename) == 0) return;
    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s.state", current_script_filename);
    FILE *f = fopen(state_file, "w");
    if (!f) return;

    Scope *s = &call_stack[0];
    for (size_t i = 0; i < (size_t)s->count; i++) {
        Variable *v = &s->vars[i];
        if (!v->is_list) {
            if (v->is_string) {
                fprintf(f, "VAR_STR %s %s\n", v->name, v->sval);
            } else {
                fprintf(f, "VAR_INT %s %d\n", v->name, v->ival);
            }
        }
    }
    fclose(f);
}

void load_state_from_persistence() {
    if (strlen(current_script_filename) == 0) return;
    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s.state", current_script_filename);
    FILE *f = fopen(state_file, "r");
    if (!f) return;

    char type[32], name[64];
    while (fscanf(f, "%31s %63s", type, name) == 2) {
        if (strcmp(type, "VAR_INT") == 0) {
            int val;
            if (fscanf(f, "%d", &val) == 1) {
                Variable *v = get_or_create_var(name);
                v->ival = val;
                v->is_string = 0;
                v->is_list = 0;
            }
        } else if (strcmp(type, "VAR_STR") == 0) {
            char sval[256];
            if (fgets(sval, sizeof(sval), f)) {
                sval[strcspn(sval, "\r\n")] = 0;
                while (sval[0] == ' ') memmove(sval, sval + 1, strlen(sval));
                Variable *v = get_or_create_var(name);
                strncpy(v->sval, sval, 255);
                v->sval[255] = '\0';
                v->is_string = 1;
                v->is_list = 0;
            }
        }
    }
    fclose(f);
}

// --- تنفيذ سطر برمجي مفرد ---
void execute_single_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (strlen(line) == 0 || line[0] == '#' || line[0] == '{' || line[0] == '}') return;

    if (match_command(line, "def")) return;

    if (match_command(line, "list") || match_command(line, "pile")) {
        char *p = skip_cmd(line, "list");
        char name[64];
        sscanf(p, "%63s", name);
        create_list(name);
        save_state_to_persistence();
        return;
    }

    if (match_command(line, "push")) {
        char *p = skip_cmd(line, "push");
        char name[64], val_str[32];
        if (sscanf(p, "%63s %31s", name, val_str) == 2) {
            int val = get_val(val_str);
            push_to_list(name, val);
            save_state_to_persistence();
        }
        return;
    }

    if (match_command(line, "pop")) {
        char *p = skip_cmd(line, "pop");
        char name[64];
        sscanf(p, "%63s", name);
        pop_from_list(name);
        save_state_to_persistence();
        return;
    }

    if (match_command(line, "len")) {
        char *p = skip_cmd(line, "len");
        char list_name[64], var_name[64];
        if (sscanf(p, "%63s %63s", list_name, var_name) == 2) {
            get_list_len(list_name, var_name);
            save_state_to_persistence();
        }
        return;
    }

    if (match_command(line, "save")) {
        char *p = skip_cmd(line, "save");
        char name[64], filename[128];
        if (sscanf(p, "%63s \"%[^\"]\"", name, filename) == 2 || sscanf(p, "%63s %63s", name, filename) == 2) {
            save_list_to_file(name, filename);
        }
        return;
    }

    if (match_command(line, "load")) {
        char *p = skip_cmd(line, "load");
        char name[64], filename[128];
        if (sscanf(p, "%63s \"%[^\"]\"", name, filename) == 2 || sscanf(p, "%63s %63s", name, filename) == 2) {
            load_list_from_file(name, filename);
            save_state_to_persistence();
        }
        return;
    }

    if (match_command(line, "input")) {
        char *p = skip_cmd(line, "input");
        char var_name[64];
        sscanf(p, "%63s", var_name);
        char input_val[256];
        printf("? ");
        if (fgets(input_val, sizeof(input_val), stdin)) {
            input_val[strcspn(input_val, "\r\n")] = 0;
            int is_num = 1;
            for (int i = 0; input_val[i] != '\0'; i++) {
                if (!isdigit(input_val[i]) && !(i == 0 && input_val[0] == '-')) { is_num = 0; break; }
            }
            Variable *v = get_or_create_var(var_name);
            if (is_num && strlen(input_val) > 0) {
                v->ival = atoi(input_val);
                v->is_string = 0;
                v->is_list = 0;
            } else {
                strncpy(v->sval, input_val, 255);
                v->sval[255] = '\0';
                v->is_string = 1;
                v->is_list = 0;
            }
            save_state_to_persistence();
        }
        return;
    }

    if (match_command(line, "display")) {
        char *p = skip_cmd(line, "display");
        while (*p == ' ' || *p == '\t') p++;

        while (*p && *p != '\r' && *p != '\n' && *p != '#') {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\r' || *p == '\n') break;

            if (*p == '"') {
                p++;
                char *end = strchr(p, '"');
                if (end) {
                    *end = '\0';
                    printf("%s", p);
                    p = end + 1;
                } else {
                    printf("%s", p);
                    break;
                }
            } else {
                char token[128];
                int idx = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '"' && *p != '\r' && *p != '\n') {
                    token[idx++] = *p++;
                }
                token[idx] = '\0';

                if (strlen(token) > 0) {
                    char var_name[64];
                    char index_expr[64];
                    if (sscanf(token, "%[^[][%[^]]]", var_name, index_expr) == 2) {
                        int resolved_index = 0;
                        int is_num_idx = 1;
                        for (int i = 0; index_expr[i] != '\0'; i++) {
                            if (!isdigit(index_expr[i]) && !(i == 0 && index_expr[0] == '-')) { is_num_idx = 0; break; }
                        }
                        if (is_num_idx) {
                            resolved_index = atoi(index_expr);
                        } else {
                            resolved_index = get_val(index_expr);
                        }

                        Variable *v = get_var_obj(var_name);
                        if (v && v->is_list && resolved_index >= 0 && (size_t)resolved_index < v->list_val.size) {
                            printf("%d", v->list_val.items[resolved_index]);
                        } else {
                            printf("0");
                        }
                    } else {
                        Variable *v = get_var_obj(token);
                        if (v) {
                            if (v->is_list) {
                                print_professional_table(token, &v->list_val);
                            } else if (v->is_string) {
                                printf("%s", v->sval);
                            } else {
                                printf("%d", v->ival);
                            }
                        } else {
                            printf("%d", get_val(token));
                        }
                    }
                }
            }
        }
        printf("\n");
        return;
    }

    char var_name[64];
    char *eq = strchr(line, '=');
    if (eq) {
        *eq = '\0';
        sscanf(line, "%63s", var_name);
        char *expr = eq + 1;
        while (*expr == ' ' || *expr == '\t') expr++;

        Variable *v = get_or_create_var(var_name);
        if (expr[0] == '"') {
            char *end = strrchr(expr, '"');
            if (end) *end = '\0';
            strncpy(v->sval, expr + 1, 255);
            v->sval[255] = '\0';
            v->is_string = 1;
            v->is_list = 0;
        } else {
            v->ival = get_val(expr);
            v->is_string = 0;
            v->is_list = 0;
        }
        save_state_to_persistence();
    }
}

// --- تنفيذ الكتل البرمجية والحلقات بكفاءة مؤسسية تامة ---
void execute_block(int start, int end) {
    for (int i = start; i <= end; i++) {
        if (is_inside_function(i)) continue;

        current_line_number = i + 1;
        char line[512];
        strcpy(line, all_lines[i]);
        line[strcspn(line, "\r\n")] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strlen(p) == 0 || p[0] == '#') continue;

        if (match_command(p, "repeat")) {
            char *rp = skip_cmd(p, "repeat");
            int times = get_val(rp);
            int block_start = i + 1;
            int brace_count = 1;
            int block_end = i;

            for (int j = i; j <= end; j++) {
                if (strchr(all_lines[j], '{')) {
                    block_start = j + 1;
                    break;
                }
            }

            for (int j = block_start; j <= end; j++) {
                if (strchr(all_lines[j], '{')) brace_count++;
                if (strchr(all_lines[j], '}')) brace_count--;
                if (brace_count == 0) {
                    block_end = j - 1;
                    i = j;
                    break;
                }
            }

            for (int t = 0; t < times; t++) {
                execute_block(block_start, block_end);
            }
            continue;
        }

        if (match_command(p, "while")) {
            char *wp = skip_cmd(p, "while");
            char left_var[64], op[4], right_expr[64];
            parse_condition_tokens(wp, left_var, op, right_expr);

            int block_start = i + 1;
            int brace_count = 1;
            int block_end = i;

            for (int j = i; j <= end; j++) {
                if (strchr(all_lines[j], '{')) {
                    block_start = j + 1;
                    break;
                }
            }

            for (int j = block_start; j <= end; j++) {
                if (strchr(all_lines[j], '{')) brace_count++;
                if (strchr(all_lines[j], '}')) brace_count--;
                if (brace_count == 0) {
                    block_end = j - 1;
                    i = j;
                    break;
                }
            }

            // محرك while مؤسسي يعيد تقييم كلا الطرفين في كل دورة تكرار بدقة مطلقة
            int safety_loop_guard = 0;
            while (1) {
                int left_val = get_val(left_var);
                int right_val = get_val(right_expr);
                int matched = 0;

                if (strcmp(op, ">") == 0) matched = (left_val > right_val);
                else if (strcmp(op, "<") == 0) matched = (left_val < right_val);
                else if (strcmp(op, ">=") == 0) matched = (left_val >= right_val);
                else if (strcmp(op, "<=") == 0) matched = (left_val <= right_val);
                else if (strcmp(op, "==") == 0) matched = (left_val == right_val);
                else if (strcmp(op, "!=") == 0) matched = (left_val != right_val);

                if (!matched) break;

                execute_block(block_start, block_end);

                safety_loop_guard++;
                if (safety_loop_guard > 500000) {
                    fprintf(stderr, "[7ABA Enterprise Safety Guard Error at line %d]: Potential infinite loop detected and terminated.\n", current_line_number);
                    break;
                }
            }
            continue;
        }

        if (match_command(p, "when")) {
            char *whp = skip_cmd(p, "when");
            char var_name[64], op[4], val_str[32];
            parse_condition_tokens(whp, var_name, op, val_str);

            int block_start = i + 1;
            int brace_count = 1;
            int block_end = i;
            int has_brace = 0;

            for (int j = i; j <= end; j++) {
                if (strchr(all_lines[j], '{')) {
                    block_start = j + 1;
                    has_brace = 1;
                    break;
                }
            }

            if (has_brace) {
                for (int j = block_start; j <= end; j++) {
                    if (strchr(all_lines[j], '{')) brace_count++;
                    if (strchr(all_lines[j], '}')) brace_count--;
                    if (brace_count == 0) {
                        block_end = j - 1;
                        i = j;
                        break;
                    }
                }
                int v = get_val(var_name);
                int comp = get_val(val_str);
                int matched = 0;
                if (strcmp(op, ">") == 0) matched = (v > comp);
                else if (strcmp(op, "<") == 0) matched = (v < comp);
                else if (strcmp(op, ">=") == 0) matched = (v >= comp);
                else if (strcmp(op, "<=") == 0) matched = (v <= comp);
                else if (strcmp(op, "==") == 0) matched = (v == comp);
                else if (strcmp(op, "!=") == 0) matched = (v != comp);

                int otherwise_start = -1;
                int otherwise_end = -1;
                int end_of_if_chain = i;

                for (int j = i + 1; j <= end; j++) {
                    char temp[512];
                    strcpy(temp, all_lines[j]);
                    char *tp = temp;
                    while (*tp == ' ' || *tp == '\t') tp++;
                    if (match_command(tp, "otherwise")) {
                        int o_brace = 1;
                        int o_start = j + 1;
                        for (int k = j; k <= end; k++) {
                            if (strchr(all_lines[k], '{')) {
                                o_start = k + 1;
                                break;
                            }
                        }
                        for (int k = o_start; k <= end; k++) {
                            if (strchr(all_lines[k], '{')) o_brace++;
                            if (strchr(all_lines[k], '}')) o_brace--;
                            if (o_brace == 0) {
                                otherwise_start = o_start;
                                otherwise_end = k - 1;
                                end_of_if_chain = k;
                                break;
                            }
                        }
                        break;
                    } else if (strlen(tp) > 0 && tp[0] != '#') {
                        end_of_if_chain = j - 1;
                        break;
                    }
                }

                if (matched) {
                    execute_block(block_start, block_end);
                } else if (otherwise_start != -1) {
                    execute_block(otherwise_start, otherwise_end);
                }

                i = end_of_if_chain;
                continue;
            }
        }

        execute_single_line(p);
    }
}
