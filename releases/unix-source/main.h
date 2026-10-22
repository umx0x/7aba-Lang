#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "value.h"
#include "runtime.h"

// أنواع الرموز لمحلل الرموز المتقدم (Tokenizer)
typedef enum
{
    TOKEN_KEYWORD = 1,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_OPERATOR,
    TOKEN_DELIMITER,
    TOKEN_UNKNOWN
} TokenKind;

typedef struct
{
    TokenKind type;
    char *lexeme;
} Token;

// --- وظائف الذاكرة والنطاقات ---
bool init_scope(Scope *s, RuntimeContext *runtime);
void free_scope(Scope *s);
Variable *find_var_in_scope(Scope *s, const char *name);
Variable *get_var_obj(RuntimeContext *runtime, const char *name);
Variable *get_or_create_var(RuntimeContext *runtime, const char *name);
bool variable_set_runtime_value(Variable *variable, const Value *value);
bool variable_replace_runtime_value(Variable *variable, const Value *value);
bool variable_declare_and_set_runtime_value(Variable *variable,
                                            VariableDeclaredType type,
                                            const Value *value);
bool runtime_display_special(RuntimeContext *runtime, const char *token);

// --- وظائف الـ Lexer وقراءة الملفات ---
bool append_line(RuntimeContext *runtime, const char *line);
Token *tokenize_line(const char *line, size_t *out_count);
void free_tokens(Token *tokens, size_t count);

// --- وظائف الـ Parser والتحليل الدلالي ---
bool add_function(RuntimeContext *runtime, const Function *function);
int find_function_index(const RuntimeContext *runtime, const char *name);
void clear_functions(RuntimeContext *runtime);
bool parse_action_header(const char *line, Function *function);
bool parse_class_header(const char *line, ClassDefinition *class_definition);
void clear_classes(RuntimeContext *runtime);
int find_class_index(const RuntimeContext *runtime, const char *name);
int find_class_method(const RuntimeContext *runtime, int class_index, const char *name);
bool is_inside_function(const RuntimeContext *runtime, int line_idx);
bool match_command(const char *line, const char *cmd);
char *skip_cmd(char *p, const char *cmd);

bool runtime_object_create(RuntimeContext *runtime, size_t class_id, Value *result);
bool runtime_object_field_get(RuntimeContext *runtime, const Value *object,
                              const char *field_name, Value *result);
bool runtime_object_field_set(RuntimeContext *runtime, Value *object,
                              const char *field_name,
                              const Value *field_value);
bool runtime_display_object(RuntimeContext *runtime, const char *name,
                            const Value *object);

// --- وظائف الحفظ والاسترجاع (Persistence) ---
void save_state_to_persistence(RuntimeContext *runtime);
void load_state_from_persistence(RuntimeContext *runtime);
bool runtime_collection_save(RuntimeContext *runtime, const char *name,
                             const char *filename);
bool runtime_collection_load(RuntimeContext *runtime, const char *name,
                             const char *filename);
bool runtime_file_display(RuntimeContext *runtime, const char *filename);
bool runtime_file_read(RuntimeContext *runtime, const char *filename,
                       const char *target_name);
bool runtime_file_write(RuntimeContext *runtime, const char *filename,
                        const char *source_name, bool append);

#endif
