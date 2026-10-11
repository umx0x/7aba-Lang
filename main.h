#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#define MAX_STACK_DEPTH 256
#define MAX_LINE_LENGTH 1024

// أنواع الرموز لمحلل الرموز المتقدم (Tokenizer)
typedef enum {
    TOKEN_KEYWORD = 1,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_OPERATOR,
    TOKEN_DELIMITER,
    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char *lexeme;
} Token;

// هيكل القوائم والجداول الديناميكية المتقدمة
typedef struct {
    int *items;
    size_t size;
    size_t capacity;
} ListData;

// هيكل المتغيرات الشامل (يدعم الأرقام، النصوص، والقوائم)
typedef struct {
    char name[64];
    char sval[256];
    int ival;
    int is_string;
    int is_list;
    ListData list_val;
} Variable;

// نطاق المتغيرات ونظام الـ Stack للدوال
typedef struct {
    Variable *vars;
    size_t count;
    size_t capacity;
} Scope;

// هيكل الدوال المخصصة
typedef struct {
    char name[64];
    int start_line;
    int end_line;
} Function;

// المتغيرات العامة للمفسر
extern Scope call_stack[MAX_STACK_DEPTH];
extern int stack_top;
extern Function *funcs;
extern size_t func_count;
extern size_t func_capacity;
extern char **all_lines;
extern size_t total_lines;
extern size_t lines_capacity;
extern int current_line_number;
extern char current_script_filename[256];

// --- وظائف الذاكرة والنطاقات ---
void init_scope(Scope *s);
void free_scope(Scope *s);
Variable* find_var_in_scope(Scope *s, const char *name);
Variable* get_var_obj(const char *name);
Variable* get_or_create_var(const char *name);

// --- وظائف الـ Lexer وقراءة الملفات ---
void append_line(const char *line);
Token* tokenize_line(const char *line, size_t *out_count);
void free_tokens(Token *tokens, size_t count);

// --- وظائف الـ Parser والتحليل الدلالي ---
void add_function(const char *name, int start, int end);
bool is_inside_function(int line_idx);
int levenshtein_distance(const char *s1, const char *s2);
void to_lowercase(char *dest, const char *src);
void autocorrect_command(char *cmd_buf, const char *line);
bool match_command(const char *line, const char *cmd);
char* skip_cmd(char *p, const char *cmd);
void parse_condition_tokens(const char *src, char *var_name, char *op, char *val_str);

// --- وظائف الحسابات والتعبيرات ---
int get_val(const char *str);

// --- وظائف القوائم والجداول الاحترافية ---
void init_list(ListData *l);
void create_list(const char *name);
void push_to_list(const char *name, int val);
void pop_from_list(const char *name);
void get_list_len(const char *list_name, const char *var_name);
void print_professional_table(const char *name, ListData *list);

// --- وظائف الحفظ والاسترجاع (Persistence) ---
void save_state_to_persistence();
void load_state_from_persistence();
void save_list_to_file(const char *name, const char *filename);
void load_list_from_file(const char *name, const char *filename);

// --- محرك التنفيذ (Evaluator) ---
void execute_single_line(char *line);
void execute_block(int start, int end);

#endif
