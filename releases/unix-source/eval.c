#include "main.h"
#include "bytecode.h"
#include "diagnostic.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>

#define MAX_COLLECTION_ELEMENTS 1000000U

static void print_display_border(size_t width, const char *top_left, const char *top_right);
static void print_display_row(const char *text, size_t width);
static bool format_collection_value(const Value *value, char *buffer, size_t capacity);

bool runtime_consume_execution_step(RuntimeContext *runtime)
{
    if (!runtime_is_initialized(runtime))
        return false;
    if (runtime->execution_steps >= MAX_EXECUTION_STEPS)
    {
        char message[128];
        snprintf(message, sizeof(message), "execution step limit (%lu) reached",
                 (unsigned long)MAX_EXECUTION_STEPS);
        report_diagnostic(runtime, "error", "E3001", message, runtime->current_line, 1);
        return false;
    }
    runtime->execution_steps++;
    return true;
}

static void clear_variable_value(Variable *variable)
{
    if (variable->has_runtime_value)
        value_release(&variable->runtime_value);
    variable->has_runtime_value = 0;
    variable->runtime_value = value_null();
    variable->collection_kind = VARIABLE_COLLECTION_NONE;
}

static void trace_scope(void *root, GCMarkObject mark, void *mark_context)
{
    Scope *scope = root;
    if (!scope || !scope->vars)
        return;
    for (size_t i = 0; i < scope->count; i++)
        if (scope->vars[i].has_runtime_value)
            value_trace(&scope->vars[i].runtime_value, mark, mark_context);
}

static size_t class_total_field_count(const RuntimeContext *runtime, int class_index)
{
    if (!runtime || class_index < 0 || (size_t)class_index >= runtime->class_count)
        return 0;
    size_t parent_count = class_total_field_count(
        runtime, runtime->classes[class_index].parent_index);
    return parent_count + runtime->classes[class_index].field_count;
}

static int find_object_field_index(const RuntimeContext *runtime, int class_index,
                                   const char *field_name)
{
    if (!runtime || class_index < 0 ||
        (size_t)class_index >= runtime->class_count)
        return -1;
    const ClassDefinition *definition = &runtime->classes[class_index];
    for (size_t i = 0; i < definition->field_count; i++)
        if (strcmp(definition->fields[i].name, field_name) == 0)
            return (int)(class_total_field_count(runtime, definition->parent_index) + i);
    return find_object_field_index(runtime, definition->parent_index, field_name);
}

static void copy_class_fields(const RuntimeContext *runtime, int class_index,
                              ClassField *fields, size_t *count)
{
    if (!runtime || class_index < 0 ||
        (size_t)class_index >= runtime->class_count)
        return;
    copy_class_fields(runtime, runtime->classes[class_index].parent_index,
                      fields, count);
    for (size_t i = 0; i < runtime->classes[class_index].field_count; i++)
        fields[(*count)++] = runtime->classes[class_index].fields[i];
}

bool runtime_object_create(RuntimeContext *runtime, size_t class_id, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result ||
        class_id >= runtime->class_count ||
        class_total_field_count(runtime, (int)class_id) > MAX_CLASS_FIELDS)
        return false;
    size_t field_count = class_total_field_count(runtime, (int)class_id);
    Value object = value_null();
    if (!value_object_new(runtime, class_id, field_count, &object) ||
        !value_root_add(runtime, &object))
    {
        value_release(&object);
        return false;
    }

    ClassField definitions[MAX_CLASS_FIELDS];
    size_t definition_count = 0;
    copy_class_fields(runtime, (int)class_id, definitions, &definition_count);
    bool success = definition_count == field_count;
    for (size_t i = 0; success && i < field_count; i++)
    {
        Value field_value = definitions[i].is_string
                                ? value_null()
                                : value_int(definitions[i].integer_value);
        if (definitions[i].is_string &&
            !value_string(runtime, definitions[i].string_value,
                          strlen(definitions[i].string_value), &field_value))
            success = false;
        if (success)
            success = value_object_set(&object, i, &field_value);
        value_release(&field_value);
    }
    if (success)
        success = value_copy(result, &object);
    if (!value_root_remove(runtime, &object))
        success = false;
    value_release(&object);
    return success;
}

bool runtime_object_field_get(RuntimeContext *runtime, const Value *object,
                              const char *field_name,
                              Value *result)
{
    if (!runtime || !object || object->type != VALUE_OBJECT ||
        !field_name || !result)
        return false;
    size_t class_id = value_object_class_id(object);
    if (class_id >= runtime->class_count)
        return false;
    int field_index = find_object_field_index(runtime, (int)class_id, field_name);
    return field_index >= 0 &&
           (size_t)field_index < value_object_field_count(object) &&
           value_object_get(object, (size_t)field_index, result);
}

bool runtime_object_field_set(RuntimeContext *runtime, Value *object,
                              const char *field_name,
                              const Value *field_value)
{
    if (!runtime || !object || object->type != VALUE_OBJECT ||
        !field_name || !field_value)
        return false;
    size_t class_id = value_object_class_id(object);
    if (class_id >= runtime->class_count)
        return false;
    int field_index = find_object_field_index(runtime, (int)class_id, field_name);
    return field_index >= 0 &&
           (size_t)field_index < value_object_field_count(object) &&
           value_object_set(object, (size_t)field_index, field_value);
}

static bool split_object_path(const char *path, char receiver_name[64],
                              char field_name[64])
{
    if (!path)
        return false;
    const char *dot = strchr(path, '.');
    if (!dot || dot == path || !dot[1] || strchr(dot + 1, '.') ||
        (size_t)(dot - path) >= 64 || strlen(dot + 1) >= 64)
        return false;
    size_t receiver_length = (size_t)(dot - path);
    memcpy(receiver_name, path, receiver_length);
    receiver_name[receiver_length] = '\0';
    strcpy(field_name, dot + 1);
    return true;
}

static bool runtime_object_path_get(RuntimeContext *runtime, const char *path,
                                    Value *result)
{
    char receiver_name[64], field_name[64];
    if (!split_object_path(path, receiver_name, field_name))
        return false;
    Variable *receiver = get_var_obj(runtime, receiver_name);
    return receiver && receiver->has_runtime_value &&
           runtime_object_field_get(runtime, &receiver->runtime_value,
                                    field_name, result);
}

// تهيئة النطاق (Scope) مع حماية أمنية للذاكرة
bool init_scope(Scope *s, RuntimeContext *runtime)
{
    if (!s || !runtime_is_initialized(runtime))
        return false;
    s->count = 0;
    s->capacity = 8;
    s->runtime = runtime;
    s->vars = malloc(s->capacity * sizeof(Variable));
    if (!s->vars)
    {
        report_diagnostic(runtime, "error", "E3004", "unable to allocate runtime scope",
                          runtime->current_line, 1);
        s->runtime = NULL;
        s->capacity = 0;
        return false;
    }
    if (!gc_root_add(runtime, s, trace_scope))
    {
        free(s->vars);
        s->vars = NULL;
        s->count = 0;
        s->capacity = 0;
        s->runtime = NULL;
        report_diagnostic(runtime, "error", "E3005", "unable to register runtime scope with garbage collector",
                          runtime->current_line, 1);
        return false;
    }
    return true;
}

// تحرير النطاق والذاكرة المرتبطة به بالكامل (Zero-Leak RAII Engine)
void free_scope(Scope *s)
{
    if (!s || !s->vars)
        return;
    if (!gc_root_remove(s->runtime, s))
    {
        report_diagnostic(s->runtime, "error", "E3002", "runtime scope was not registered as a GC root",
                          s->runtime->current_line, 1);
    }
    for (size_t i = 0; i < (size_t)s->count; i++)
        clear_variable_value(&s->vars[i]);
    free(s->vars);
    s->vars = NULL;
    s->count = 0;
    s->capacity = 0;
    s->runtime = NULL;
}

static bool declared_type_accepts(VariableDeclaredType type, const Value *value)
{
    switch (type)
    {
    case VARIABLE_TYPE_UNDECLARED:
        return true;
    case VARIABLE_TYPE_INT:
        return value->type == VALUE_INT;
    case VARIABLE_TYPE_FLOAT:
        return value->type == VALUE_FLOAT;
    case VARIABLE_TYPE_BOOL:
        return value->type == VALUE_BOOL;
    case VARIABLE_TYPE_STRING:
        return value->type == VALUE_STRING;
    case VARIABLE_TYPE_NULL:
        return value->type == VALUE_NULL;
    }
    return false;
}

static bool store_variable_value(Variable *variable, const Value *value,
                                 bool replace, VariableDeclaredType declaration)
{
    if (!variable || !value ||
        (declaration != VARIABLE_TYPE_UNDECLARED &&
         variable->declared_type != VARIABLE_TYPE_UNDECLARED) ||
        !declared_type_accepts(declaration == VARIABLE_TYPE_UNDECLARED
                                   ? variable->declared_type
                                   : declaration,
                               value))
        return false;

    Value copy = value_null();
    if (!value_copy(&copy, value))
        return false;

    size_t string_length = 0;
    const char *string_bytes = NULL;
    if (copy.type == VALUE_STRING)
    {
        string_bytes = value_string_bytes(&copy, &string_length);
        if (!string_bytes)
        {
            value_release(&copy);
            return false;
        }
    }

    if (variable->has_runtime_value)
        value_release(&variable->runtime_value);
    variable->runtime_value = copy;
    variable->has_runtime_value = 1;
    if (replace)
    {
        variable->collection_kind = VARIABLE_COLLECTION_NONE;
        variable->is_string = 0;
    }
    if (declaration != VARIABLE_TYPE_UNDECLARED)
        variable->declared_type = declaration;

    if (copy.type == VALUE_STRING)
    {
        size_t copy_length = string_length < sizeof(variable->sval) - 1
                                 ? string_length
                                 : sizeof(variable->sval) - 1;
        memcpy(variable->sval, string_bytes, copy_length);
        variable->sval[copy_length] = '\0';
        variable->is_string = 1;
        variable->ival = 0;
    }
    else if (copy.type == VALUE_INT && copy.as.integer >= INT_MIN &&
             copy.as.integer <= INT_MAX)
    {
        variable->ival = (int)copy.as.integer;
        variable->is_string = 0;
    }
    else if (copy.type == VALUE_BOOL)
    {
        variable->ival = copy.as.boolean ? 1 : 0;
        variable->is_string = 0;
    }
    else if (copy.type == VALUE_NULL)
    {
        variable->ival = 0;
        variable->is_string = 0;
    }
    return true;
}

bool variable_set_runtime_value(Variable *variable, const Value *value)
{
    return store_variable_value(variable, value, false,
                                VARIABLE_TYPE_UNDECLARED);
}

bool variable_replace_runtime_value(Variable *variable, const Value *value)
{
    return store_variable_value(variable, value, true,
                                VARIABLE_TYPE_UNDECLARED);
}

bool variable_declare_and_set_runtime_value(Variable *variable,
                                            VariableDeclaredType type,
                                            const Value *value)
{
    if (type == VARIABLE_TYPE_UNDECLARED)
        return false;
    return store_variable_value(variable, value, true, type);
}

// البحث عن متغير داخل نطاق معين
Variable *find_var_in_scope(Scope *s, const char *name)
{
    if (!s || !s->vars)
        return NULL;
    for (size_t i = 0; i < (size_t)s->count; i++)
    {
        if (strcmp(s->vars[i].name, name) == 0)
            return &s->vars[i];
    }
    return NULL;
}

// الحصول على كائن المتغير عبر مكدس النطاقات (Scope Chain)
Variable *get_var_obj(RuntimeContext *runtime, const char *name)
{
    if (!runtime || !name)
        return NULL;
    for (int i = runtime->stack_top; i >= 0; i--)
    {
        Variable *v = find_var_in_scope(&runtime->call_stack[i], name);
        if (v)
            return v;
    }
    return NULL;
}

// الحصول على المتغير في النطاق الحالي أو إنشائه تلقائياً مع توسيع آمن للذاكرة
Variable *get_or_create_var(RuntimeContext *runtime, const char *name)
{
    if (!runtime || runtime->stack_top < 0 ||
        runtime->stack_top >= MAX_STACK_DEPTH)
        return NULL;
    Scope *s = &runtime->call_stack[runtime->stack_top];
    Variable *v = find_var_in_scope(s, name);
    if (v)
        return v;

    if ((size_t)s->count >= s->capacity)
    {
        size_t new_cap = s->capacity * 2;
        Variable *new_vars = realloc(s->vars, new_cap * sizeof(Variable));
        if (!new_vars)
        {
            fprintf(stderr, "[7ABA Enterprise Security Error]: Out of memory expanding scope variables.\n");
            exit(EXIT_FAILURE);
        }
        s->capacity = new_cap;
        s->vars = new_vars;
    }
    Variable *new_v = &s->vars[s->count++];
    memset(new_v, 0, sizeof(Variable));
    new_v->runtime_value = value_null();
    strncpy(new_v->name, name, 63);
    new_v->name[63] = '\0';
    return new_v;
}

static void print_table_border(size_t row_width, size_t cell_width, size_t column_count,
                               const char *left, const char *junction, const char *right)
{
    fputs(left, stdout);
    for (size_t i = 0; i < row_width + 2; i++)
        fputs("─", stdout);
    for (size_t column = 0; column < column_count; column++)
    {
        fputs(junction, stdout);
        for (size_t i = 0; i < cell_width + 2; i++)
            fputs("─", stdout);
    }
    printf("%s\n", right);
}

static void print_display_border(size_t width, const char *top_left, const char *top_right);
static void print_display_row(const char *text, size_t width);

static bool format_collection_value(const Value *item, char *buffer, size_t capacity)
{
    int written;
    switch (item->type)
    {
    case VALUE_INT:
        written = snprintf(buffer, capacity, "%" PRId64, item->as.integer);
        break;
    case VALUE_FLOAT:
        written = snprintf(buffer, capacity, "%.17g", item->as.floating);
        break;
    case VALUE_BOOL:
        written = snprintf(buffer, capacity, "%s", item->as.boolean ? "true" : "false");
        break;
    case VALUE_NULL:
        written = snprintf(buffer, capacity, "null");
        break;
    case VALUE_STRING:
    {
        size_t length = 0;
        const char *bytes = value_string_bytes(item, &length);
        if (!bytes || length >= capacity)
            return false;
        memcpy(buffer, bytes, length);
        buffer[length] = '\0';
        return true;
    }
    default:
        written = snprintf(buffer, capacity, "[%s]", value_type_name(item->type));
        break;
    }
    return written >= 0 && (size_t)written < capacity;
}

static size_t managed_array_capacity(const Value *array)
{
    return value_array_capacity(array);
}

static void print_indexed_table(const char *name, const Value *array)
{
    size_t length = value_array_length(array);
    size_t index_width = 5;
    size_t value_width = 5;
    char index[32];
    char value[256];
    if (length == 0)
        value_width = strlen("<empty>");
    for (size_t item_index = 0; item_index < length; item_index++)
    {
        Value item = value_null();
        if (!value_array_get(array, item_index, &item) ||
            !format_collection_value(&item, value, sizeof(value)))
            strcpy(value, "[value]");
        snprintf(index, sizeof(index), "[%lu]", (unsigned long)item_index);
        if (strlen(index) > index_width)
            index_width = strlen(index);
        if (strlen(value) > value_width)
            value_width = strlen(value);
    }
    printf("TABLE %s (length=%lu)\n", name, (unsigned long)length);
    print_table_border(index_width, value_width, 1, "┌", "┬", "┐");
    printf("│ %-*s │ %-*s │\n", (int)index_width, "INDEX", (int)value_width, "VALUE");
    print_table_border(index_width, value_width, 1, "├", "┼", "┤");
    if (length == 0)
        printf("│ %-*s │ %-*s │\n", (int)index_width, "-", (int)value_width, "<empty>");
    else
        for (size_t item_index = 0; item_index < length; item_index++)
        {
            Value item = value_null();
            if (!value_array_get(array, item_index, &item) ||
                !format_collection_value(&item, value, sizeof(value)))
                strcpy(value, "[value]");
            snprintf(index, sizeof(index), "[%lu]", (unsigned long)item_index);
            printf("│ %-*s │ %-*s │\n", (int)index_width, index,
                   (int)value_width, value);
        }
    print_table_border(index_width, value_width, 1, "└", "┴", "┘");
}

static void print_display_border(size_t width, const char *top_left, const char *top_right)
{
    fputs(top_left, stdout);
    for (size_t i = 0; i < width + 2; i++)
        fputs("─", stdout);
    printf("%s\n", top_right);
}

static void print_display_row(const char *text, size_t width)
{
    printf("│ %-*s │\n", (int)width, text);
}

static void print_pile(const char *name, const Value *pile)
{
    size_t length = value_array_length(pile);
    size_t width = 0;
    char row[320];
    char value[256];
    for (size_t item_index = 0; item_index < length; item_index++)
    {
        Value item = value_null();
        if (!value_array_get(pile, item_index, &item) ||
            !format_collection_value(&item, value, sizeof(value)))
            strcpy(value, "[value]");
        int row_length = snprintf(row, sizeof(row), "[%lu] value=%s%s",
                                  (unsigned long)item_index, value,
                                  item_index + 1 == length ? " <- SP (top)" : "");
        if (row_length > 0 && (size_t)row_length > width)
            width = (size_t)row_length;
    }
    if (length == 0)
        width = strlen("<empty pile> (SP: none)");

    printf("PILE %s (size=%lu, capacity=%lu)\n", name,
           (unsigned long)length, (unsigned long)managed_array_capacity(pile));
    print_display_border(width, "┌", "┐");
    if (length == 0)
        print_display_row("<empty pile> (SP: none)", width);
    else
        for (size_t item_index = 0; item_index < length; item_index++)
        {
            Value item = value_null();
            if (!value_array_get(pile, item_index, &item) ||
                !format_collection_value(&item, value, sizeof(value)))
                strcpy(value, "[value]");
            snprintf(row, sizeof(row), "[%lu] value=%s%s",
                     (unsigned long)item_index, value,
                     item_index + 1 == length ? " <- SP (top)" : "");
            print_display_row(row, width);
        }
    print_display_border(width, "└", "┘");
}

bool runtime_display_object(RuntimeContext *runtime, const char *name,
                            const Value *object)
{
    if (!runtime || !name || !object || object->type != VALUE_OBJECT)
        return false;
    size_t class_id = value_object_class_id(object);
    size_t field_count = value_object_field_count(object);
    if (class_id >= runtime->class_count || field_count > MAX_CLASS_FIELDS)
        return false;

    ClassField definitions[MAX_CLASS_FIELDS];
    size_t definition_count = 0;
    copy_class_fields(runtime, (int)class_id, definitions, &definition_count);
    if (definition_count != field_count)
        return false;

    char rows[MAX_CLASS_FIELDS][384];
    size_t width = strlen("<no fields>");
    for (size_t i = 0; i < field_count; i++)
    {
        Value field_value = value_null();
        char formatted[256];
        if (!value_object_get(object, i, &field_value) ||
            !format_collection_value(&field_value, formatted, sizeof(formatted)))
        {
            value_release(&field_value);
            return false;
        }
        int length = field_value.type == VALUE_STRING
                         ? snprintf(rows[i], sizeof(rows[i]), "%s = \"%s\"",
                                    definitions[i].name, formatted)
                         : snprintf(rows[i], sizeof(rows[i]), "%s = %s",
                                    definitions[i].name, formatted);
        value_release(&field_value);
        if (length < 0 || (size_t)length >= sizeof(rows[i]))
            return false;
        if ((size_t)length > width)
            width = (size_t)length;
    }

    printf("OBJECT %s (class=%s, address=%p)\n", name,
           runtime->classes[class_id].name, (const void *)object->as.object);
    print_display_border(width, "┌", "┐");
    if (field_count == 0)
        print_display_row("<no fields>", width);
    else
        for (size_t i = 0; i < field_count; i++)
            print_display_row(rows[i], width);
    print_display_border(width, "└", "┘");
    return true;
}

bool runtime_collection_save(RuntimeContext *runtime, const char *name,
                             const char *filename)
{
    if (!runtime_is_initialized(runtime) || !name || !filename || !*filename)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "save requires a collection name and file path",
                          runtime->current_line, 1);
        return false;
    }
    Variable *source = get_var_obj(runtime, name);
    if (!source || !source->has_runtime_value ||
        source->runtime_value.type != VALUE_ARRAY)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "save requires an initialized list or pile",
                          runtime->current_line, 1);
        return false;
    }

    size_t length = value_array_length(&source->runtime_value);
    for (size_t index = 0; index < length; index++)
    {
        Value item = value_null();
        if (!value_array_get(&source->runtime_value, index, &item) ||
            item.type != VALUE_INT)
        {
            report_diagnostic(runtime, "runtime error", "E2001",
                              "save currently supports integer list and pile elements only",
                              runtime->current_line, 1);
            return false;
        }
    }

    FILE *file = fopen(filename, "w");
    if (!file)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "unable to open collection file for writing",
                          runtime->current_line, 1);
        return false;
    }
    bool success = true;
    for (size_t index = 0; index < length; index++)
    {
        Value item = value_null();
        if (!value_array_get(&source->runtime_value, index, &item) ||
            fprintf(file, "%" PRId64 "\n", item.as.integer) < 0)
        {
            success = false;
            break;
        }
    }
    if (fclose(file) != 0)
        success = false;
    if (!success)
        report_diagnostic(runtime, "runtime error", "E2001",
                          "failed while writing collection file",
                          runtime->current_line, 1);
    return success;
}

bool runtime_collection_load(RuntimeContext *runtime, const char *name,
                             const char *filename)
{
    if (!runtime_is_initialized(runtime) || !name || !filename || !*filename)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "load requires a collection name and file path",
                          runtime->current_line, 1);
        return false;
    }
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "unable to open collection file for reading",
                          runtime->current_line, 1);
        return false;
    }

    Value array = value_null();
    if (!value_array(runtime, &array))
    {
        fclose(file);
        report_diagnostic(runtime, "runtime error", "E2001",
                          "unable to allocate or root loaded collection",
                          runtime->current_line, 1);
        return false;
    }
    if (!value_root_add(runtime, &array))
    {
        value_release(&array);
        fclose(file);
        report_diagnostic(runtime, "runtime error", "E2001",
                          "unable to allocate or root loaded collection",
                          runtime->current_line, 1);
        return false;
    }

    bool success = true;
    char line[128];
    while (fgets(line, sizeof(line), file))
    {
        size_t line_length = strlen(line);
        if (line_length == sizeof(line) - 1 &&
            line[line_length - 1] != '\n' && !feof(file))
        {
            success = false;
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        char *start = line;
        while (isspace((unsigned char)*start))
            start++;
        if (*start == '\0')
            continue;
        errno = 0;
        char *end = NULL;
        intmax_t parsed = strtoimax(start, &end, 10);
        while (end && isspace((unsigned char)*end))
            end++;
        if (errno == ERANGE || end == start || !end || *end != '\0' ||
            parsed < INT64_MIN || parsed > INT64_MAX ||
            value_array_length(&array) >= MAX_COLLECTION_ELEMENTS ||
            !value_array_push(&array, &(Value){.type = VALUE_INT,
                                               .as.integer = (int64_t)parsed}))
        {
            success = false;
            break;
        }
    }
    if (ferror(file))
        success = false;
    if (fclose(file) != 0)
        success = false;

    if (success)
    {
        Variable *target = get_or_create_var(runtime, name);
        success = variable_replace_runtime_value(target, &array);
        if (success)
        {
            target->collection_kind = VARIABLE_COLLECTION_LIST;
            save_state_to_persistence(runtime);
        }
    }
    if (!value_root_remove(runtime, &array))
        success = false;
    value_release(&array);
    if (!success)
        report_diagnostic(runtime, "runtime error", "E2001",
                          "collection file is invalid, too large, or could not be loaded",
                          runtime->current_line, 1);
    return success;
}

void save_state_to_persistence(RuntimeContext *runtime)
{
    if (!runtime || runtime->source_filename[0] == '\0')
        return;
    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s.state", runtime->source_filename);
    FILE *f = fopen(state_file, "w");
    if (!f)
        return;

    Scope *s = &runtime->call_stack[0];
    for (size_t i = 0; i < (size_t)s->count; i++)
    {
        Variable *v = &s->vars[i];
        if (v->has_runtime_value && v->runtime_value.type == VALUE_OBJECT)
            continue;
        if (v->has_runtime_value)
        {
            if (v->runtime_value.type == VALUE_INT &&
                v->runtime_value.as.integer >= INT_MIN &&
                v->runtime_value.as.integer <= INT_MAX)
            {
                fprintf(f, "VAR_INT %s %d\n", v->name,
                        (int)v->runtime_value.as.integer);
                continue;
            }
            if (v->runtime_value.type == VALUE_BOOL)
            {
                fprintf(f, "VAR_BOOL %s %d\n", v->name,
                        v->runtime_value.as.boolean ? 1 : 0);
                continue;
            }
            if (v->runtime_value.type == VALUE_STRING)
            {
                size_t length = 0;
                const char *bytes = value_string_bytes(&v->runtime_value, &length);
                if (bytes && length <= 255)
                {
                    fprintf(f, "VAR_STR %s ", v->name);
                    fwrite(bytes, 1, length, f);
                    fputc('\n', f);
                }
                continue;
            }
            if (v->runtime_value.type == VALUE_NULL)
            {
                fprintf(f, "VAR_NULL %s\n", v->name);
                continue;
            }
            if (v->runtime_value.type == VALUE_MATRIX)
            {
                size_t rows = value_matrix_rows(&v->runtime_value);
                size_t columns = value_matrix_columns(&v->runtime_value);
                bool valid = true;
                for (size_t row = 0; row < rows && valid; row++)
                    for (size_t column = 0; column < columns; column++)
                    {
                        Value item = value_null();
                        if (!value_matrix_get(&v->runtime_value, row, column, &item) ||
                            item.type != VALUE_INT)
                        {
                            valid = false;
                            break;
                        }
                    }
                if (!valid)
                    continue;
                fprintf(f, "VAR_MATRIX %s %lu %lu", v->name,
                        (unsigned long)rows, (unsigned long)columns);
                for (size_t row = 0; row < rows; row++)
                    for (size_t column = 0; column < columns; column++)
                    {
                        Value item = value_null();
                        (void)value_matrix_get(&v->runtime_value, row, column, &item);
                        fprintf(f, " %lld", (long long)item.as.integer);
                    }
                fputc('\n', f);
                continue;
            }
            if (v->runtime_value.type == VALUE_QUEUE)
            {
                size_t length = value_queue_length(&v->runtime_value);
                bool valid = true;
                for (size_t index = 0; index < length; index++)
                {
                    Value item = value_null();
                    if (!value_queue_get(&v->runtime_value, index, &item) ||
                        item.type != VALUE_INT)
                    {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                    continue;
                fprintf(f, "VAR_QUEUE %s %lu", v->name, (unsigned long)length);
                for (size_t index = 0; index < length; index++)
                {
                    Value item = value_null();
                    (void)value_queue_get(&v->runtime_value, index, &item);
                    fprintf(f, " %lld", (long long)item.as.integer);
                }
                fputc('\n', f);
                continue;
            }
            if (v->runtime_value.type == VALUE_ARRAY)
            {
                size_t length = value_array_length(&v->runtime_value);
                bool valid = true;
                for (size_t item_index = 0; item_index < length; item_index++)
                {
                    Value item = value_null();
                    if (!value_array_get(&v->runtime_value, item_index, &item) ||
                        item.type != VALUE_INT)
                    {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                    continue;
                fprintf(f, "%s %s %lu",
                        v->collection_kind == VARIABLE_COLLECTION_PILE
                            ? "VAR_PILE"
                            : "VAR_LIST",
                        v->name, (unsigned long)length);
                for (size_t item_index = 0; item_index < length; item_index++)
                {
                    Value item = value_null();
                    (void)value_array_get(&v->runtime_value, item_index, &item);
                    fprintf(f, " %" PRId64, item.as.integer);
                }
                fputc('\n', f);
                continue;
            }
            if (v->runtime_value.type == VALUE_ARRAY ||
                v->runtime_value.type == VALUE_MAP ||
                v->runtime_value.type == VALUE_SET ||
                v->runtime_value.type == VALUE_FLOAT ||
                v->runtime_value.type == VALUE_NATIVE ||
                v->runtime_value.type == VALUE_OBJECT)
                continue;
        }
        if (!v->has_runtime_value)
        {
            if (v->is_string)
            {
                fprintf(f, "VAR_STR %s %s\n", v->name, v->sval);
            }
            else
            {
                fprintf(f, "VAR_INT %s %d\n", v->name, v->ival);
            }
        }
    }
    fclose(f);
}

void load_state_from_persistence(RuntimeContext *runtime)
{
    if (!runtime || runtime->source_filename[0] == '\0')
        return;
    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s.state", runtime->source_filename);
    FILE *f = fopen(state_file, "r");
    if (!f)
        return;

    char type[32], name[64];
    while (fscanf(f, "%31s %63s", type, name) == 2)
    {
        if (strcmp(type, "VAR_INT") == 0)
        {
            int val;
            if (fscanf(f, "%d", &val) == 1)
            {
                Variable *v = get_or_create_var(runtime, name);
                Value integer_value = value_int(val);
                if (!variable_replace_runtime_value(v, &integer_value))
                    report_diagnostic(runtime, "error", "E3002",
                                      "unable to restore integer value",
                                      runtime->current_line, 1);
            }
        }
        else if (strcmp(type, "VAR_BOOL") == 0)
        {
            int value;
            if (fscanf(f, "%d", &value) == 1 && (value == 0 || value == 1))
            {
                Variable *v = get_or_create_var(runtime, name);
                Value boolean_value = value_bool(value != 0);
                if (!variable_replace_runtime_value(v, &boolean_value))
                    report_diagnostic(runtime, "error", "E3002", "unable to restore boolean value",
                                      runtime->current_line, 1);
            }
        }
        else if (strcmp(type, "VAR_NULL") == 0)
        {
            Variable *v = get_or_create_var(runtime, name);
            Value null_value = value_null();
            if (!variable_replace_runtime_value(v, &null_value))
                report_diagnostic(runtime, "error", "E3002", "unable to restore null value",
                                  runtime->current_line, 1);
        }
        else if (strcmp(type, "VAR_STR") == 0)
        {
            char sval[256];
            if (fgets(sval, sizeof(sval), f))
            {
                sval[strcspn(sval, "\r\n")] = 0;
                while (sval[0] == ' ')
                    memmove(sval, sval + 1, strlen(sval));
                Variable *v = get_or_create_var(runtime, name);
                Value string_value = value_null();
                bool restored = value_string(runtime, sval, strlen(sval),
                                             &string_value);
                if (restored)
                    restored = variable_replace_runtime_value(v, &string_value);
                value_release(&string_value);
                if (!restored)
                    report_diagnostic(runtime, "error", "E3002",
                                      "unable to restore string value",
                                      runtime->current_line, 1);
            }
        }
        else if (strcmp(type, "VAR_LIST") == 0 || strcmp(type, "VAR_PILE") == 0)
        {
            bool is_pile = strcmp(type, "VAR_PILE") == 0;
            unsigned long stored_count;
            if (fscanf(f, "%lu", &stored_count) != 1 ||
                stored_count > (unsigned long)MAX_COLLECTION_ELEMENTS)
                break;
            size_t count = (size_t)stored_count;
            Value array = value_null();
            if (!value_array(runtime, &array) || !value_root_add(runtime, &array))
                break;
            bool valid = true;
            for (size_t item = 0; item < count; item++)
            {
                int64_t value;
                if (fscanf(f, "%" SCNd64, &value) != 1 ||
                    !value_array_push(&array, &(Value){.type = VALUE_INT,
                                                       .as.integer = value}))
                {
                    valid = false;
                    break;
                }
            }
            if (valid)
            {
                Variable *v = get_or_create_var(runtime, name);
                valid = variable_replace_runtime_value(v, &array);
                if (valid)
                    v->collection_kind = is_pile
                                             ? VARIABLE_COLLECTION_PILE
                                             : VARIABLE_COLLECTION_LIST;
            }
            if (!value_root_remove(runtime, &array))
                valid = false;
            value_release(&array);
            if (!valid)
                break;
        }
        else if (strcmp(type, "VAR_MATRIX") == 0)
        {
            unsigned long stored_rows, stored_columns;
            if (fscanf(f, "%lu %lu", &stored_rows, &stored_columns) != 2 ||
                stored_rows == 0 || stored_columns == 0 ||
                stored_rows > MAX_COLLECTION_ELEMENTS / stored_columns)
                break;
            size_t rows = (size_t)stored_rows;
            size_t columns = (size_t)stored_columns;
            Value matrix = value_null();
            if (!value_matrix(runtime, rows, columns, &matrix) ||
                !value_root_add(runtime, &matrix))
                break;
            size_t count = rows * columns;
            bool valid = true;
            for (size_t item = 0; item < count; item++)
            {
                int value;
                if (fscanf(f, "%d", &value) != 1 ||
                    !value_matrix_set(&matrix, item / columns, item % columns,
                                      &(Value){.type = VALUE_INT,
                                               .as.integer = value}))
                {
                    valid = false;
                    break;
                }
            }
            if (valid)
            {
                Variable *v = get_or_create_var(runtime, name);
                if (!variable_replace_runtime_value(v, &matrix))
                    valid = false;
            }
            if (!value_root_remove(runtime, &matrix))
            {
                report_diagnostic(runtime, "error", "E3002", "unable to remove restored matrix root",
                                  runtime->current_line, 1);
                valid = false;
            }
            if (!valid)
            {
                report_diagnostic(runtime, "error", "E3002", "invalid or incomplete persisted matrix",
                                  runtime->current_line, 1);
                break;
            }
        }
        else if (strcmp(type, "VAR_QUEUE") == 0)
        {
            unsigned long stored_count;
            if (fscanf(f, "%lu", &stored_count) != 1 ||
                stored_count > (unsigned long)MAX_COLLECTION_ELEMENTS)
                break;
            size_t count = (size_t)stored_count;
            Value queue = value_null();
            if (!value_queue(runtime, &queue) || !value_root_add(runtime, &queue))
                break;
            bool valid = true;
            for (size_t item = 0; item < count; item++)
            {
                int value;
                Value queue_item = value_null();
                if (fscanf(f, "%d", &value) != 1)
                {
                    valid = false;
                    break;
                }
                queue_item = value_int(value);
                if (!value_queue_enqueue(&queue, &queue_item))
                {
                    valid = false;
                    break;
                }
            }
            if (valid)
            {
                Variable *v = get_or_create_var(runtime, name);
                if (!variable_replace_runtime_value(v, &queue))
                    valid = false;
            }
            if (!value_root_remove(runtime, &queue))
            {
                report_diagnostic(runtime, "error", "E3002", "unable to remove restored queue root",
                                  runtime->current_line, 1);
                valid = false;
            }
            if (!valid)
            {
                report_diagnostic(runtime, "error", "E3002", "invalid or incomplete persisted queue",
                                  runtime->current_line, 1);
                break;
            }
        }
    }
    fclose(f);
}

bool runtime_file_read(RuntimeContext *runtime, const char *filename,
                       const char *target_name)
{
    if (!runtime_is_initialized(runtime))
        return false;
    if (!filename || !*filename || !target_name || !*target_name)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file_read requires a file path and target variable",
                          runtime->current_line, 1);
        return false;
    }
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "cannot open file for reading",
                          runtime->current_line, 1);
        return false;
    }

    char content[256];
    size_t length = fread(content, 1, sizeof(content), file);
    int extra = length == sizeof(content) ? fgetc(file) : EOF;
    bool read_error = ferror(file) != 0;
    fclose(file);
    if (read_error || length > 255 || extra != EOF || memchr(content, '\0', length))
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file is unreadable, binary, or exceeds the 255-byte string limit",
                          runtime->current_line, 1);
        return false;
    }

    content[length] = '\0';
    Value text = value_null();
    if (!value_string(runtime, content, length, &text))
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "unable to allocate file contents",
                          runtime->current_line, 1);
        return false;
    }
    Variable *target = get_or_create_var(runtime, target_name);
    bool stored = variable_replace_runtime_value(target, &text);
    value_release(&text);
    if (!stored)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "unable to store file contents",
                          runtime->current_line, 1);
        return false;
    }
    save_state_to_persistence(runtime);
    return true;
}

static size_t escaped_byte_length(unsigned char byte)
{
    if (byte == '\\')
        return 2;
    if (byte >= 32 && byte <= 126)
        return 1;
    return 4;
}

static void print_escaped_byte(unsigned char byte)
{
    if (byte == '\\')
        printf("\\\\");
    else if (byte >= 32 && byte <= 126)
        putchar(byte);
    else
        printf("\\x%02X", byte);
}

bool runtime_file_display(RuntimeContext *runtime, const char *filename)
{
    if (!runtime_is_initialized(runtime))
        return false;
    if (!filename || !*filename)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file_display requires a file path",
                          runtime->current_line, 1);
        return false;
    }
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "cannot open file for display",
                          runtime->current_line, 1);
        return false;
    }

    unsigned char content[256];
    size_t length = fread(content, 1, sizeof(content), file);
    int extra = length == sizeof(content) ? fgetc(file) : EOF;
    bool read_error = ferror(file) != 0;
    fclose(file);
    if (read_error || length > 255 || extra != EOF || memchr(content, '\0', length))
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file is unreadable, binary, or exceeds the 255-byte display limit",
                          runtime->current_line, 1);
        return false;
    }

    size_t line_count = 0;
    size_t width = strlen("<empty file>");
    for (size_t start = 0; start < length;)
    {
        size_t end = start;
        while (end < length && content[end] != '\n')
            end++;
        size_t visible_end = end;
        if (visible_end > start && content[visible_end - 1] == '\r')
            visible_end--;
        char prefix[32];
        int prefix_length = snprintf(prefix, sizeof(prefix), "[%lu] ", (unsigned long)line_count);
        size_t row_width = (size_t)prefix_length;
        for (size_t i = start; i < visible_end; i++)
            row_width += escaped_byte_length(content[i]);
        if (row_width > width)
            width = row_width;
        line_count++;
        start = end < length ? end + 1 : end;
    }

    printf("FILE \"%s\" (bytes=%lu, lines=%lu)\n", filename,
           (unsigned long)length, (unsigned long)line_count);
    print_display_border(width, "┌", "┐");
    if (line_count == 0)
        print_display_row("<empty file>", width);
    else
    {
        size_t line = 0;
        for (size_t start = 0; start < length; line++)
        {
            size_t end = start;
            while (end < length && content[end] != '\n')
                end++;
            size_t visible_end = end;
            if (visible_end > start && content[visible_end - 1] == '\r')
                visible_end--;
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "[%lu] ", (unsigned long)line);
            printf("│ %s", prefix);
            for (size_t i = start; i < visible_end; i++)
                print_escaped_byte(content[i]);
            size_t row_width = strlen(prefix);
            for (size_t i = start; i < visible_end; i++)
                row_width += escaped_byte_length(content[i]);
            for (size_t i = row_width; i < width; i++)
                putchar(' ');
            puts(" │");
            start = end < length ? end + 1 : end;
        }
    }
    print_display_border(width, "└", "┘");
    return true;
}

bool runtime_file_write(RuntimeContext *runtime, const char *filename,
                        const char *source_name, bool append)
{
    if (!runtime_is_initialized(runtime))
        return false;
    if (!filename || !*filename || !source_name || !*source_name)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file_write requires a file path and source variable",
                          runtime->current_line, 1);
        return false;
    }
    Variable *source = get_var_obj(runtime, source_name);
    if (!source)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file_write source variable does not exist",
                          runtime->current_line, 1);
        return false;
    }

    char scalar[128];
    const char *bytes = scalar;
    size_t length = 0;
    if (source->has_runtime_value)
    {
        const Value *value = &source->runtime_value;
        int written = -1;
        if (value->type == VALUE_STRING)
            bytes = value_string_bytes(value, &length);
        else if (value->type == VALUE_INT)
            written = snprintf(scalar, sizeof(scalar), "%" PRId64,
                               value->as.integer);
        else if (value->type == VALUE_FLOAT)
            written = snprintf(scalar, sizeof(scalar), "%.17g",
                               value->as.floating);
        else if (value->type == VALUE_BOOL)
            written = snprintf(scalar, sizeof(scalar), "%s",
                               value->as.boolean ? "true" : "false");
        else if (value->type == VALUE_NULL)
            written = snprintf(scalar, sizeof(scalar), "null");
        if (value->type != VALUE_STRING)
        {
            if (written < 0 || (size_t)written >= sizeof(scalar))
                bytes = NULL;
            else
                length = (size_t)written;
        }
    }
    else if (source->is_string)
    {
        bytes = source->sval;
        length = strlen(source->sval);
    }
    else
    {
        int written = snprintf(scalar, sizeof(scalar), "%d", source->ival);
        if (written < 0 || (size_t)written >= sizeof(scalar))
            bytes = NULL;
        else
            length = (size_t)written;
    }
    if (!bytes)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "file_write requires a scalar source value",
                          runtime->current_line, 1);
        return false;
    }

    FILE *file = fopen(filename, append ? "ab" : "wb");
    if (!file)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "cannot open file for writing",
                          runtime->current_line, 1);
        return false;
    }

    bool success = fwrite(bytes, 1, length, file) == length;
    if (fclose(file) != 0)
        success = false;
    if (!success)
    {
        report_diagnostic(runtime, "runtime error", "E2001",
                          "failed while writing file",
                          runtime->current_line, 1);
        return false;
    }
    return true;
}

// --- تنفيذ سطر برمجي مفرد ---
static void print_runtime_value(const Value *value)
{
    if (!value)
        return;
    switch (value->type)
    {
    case VALUE_NULL:
        fputs("null", stdout);
        break;
    case VALUE_INT:
        printf("%lld", (long long)value->as.integer);
        break;
    case VALUE_FLOAT:
        printf("%.17g", value->as.floating);
        break;
    case VALUE_BOOL:
        fputs(value->as.boolean ? "true" : "false", stdout);
        break;
    case VALUE_STRING:
    {
        size_t length = 0;
        const char *bytes = value_string_bytes(value, &length);
        if (bytes)
            fwrite(bytes, 1, length, stdout);
        break;
    }
    case VALUE_ARRAY:
        printf("[array length=%lu]", (unsigned long)value_array_length(value));
        break;
    case VALUE_MAP:
        printf("[map length=%lu]", (unsigned long)value_map_length(value));
        break;
    case VALUE_SET:
        printf("[set length=%lu]", (unsigned long)value_set_length(value));
        break;
    case VALUE_MATRIX:
        printf("[matrix rows=%lu columns=%lu]",
               (unsigned long)value_matrix_rows(value),
               (unsigned long)value_matrix_columns(value));
        break;
    case VALUE_QUEUE:
        printf("[queue length=%lu]", (unsigned long)value_queue_length(value));
        break;
    case VALUE_OBJECT:
        fputs("[object]", stdout);
        break;
    case VALUE_NATIVE:
        fputs("[native]", stdout);
        break;
    }
}

bool runtime_display_special(RuntimeContext *runtime, const char *token)
{
    if (!token || !*token)
        return false;

    Variable *variable = get_var_obj(runtime, token);
    if (variable && variable->has_runtime_value &&
        variable->runtime_value.type == VALUE_MATRIX)
    {
        const Value *matrix = &variable->runtime_value;
        size_t rows = value_matrix_rows(matrix);
        size_t columns = value_matrix_columns(matrix);
        const char *corner_label = "[i]\\[j]";
        size_t row_width = strlen(corner_label);
        size_t cell_width = 3;
        for (size_t row = 0; row < rows; row++)
        {
            char label[32];
            snprintf(label, sizeof(label), "[%lu]", (unsigned long)row);
            if (strlen(label) > row_width)
                row_width = strlen(label);
        }
        for (size_t column = 0; column < columns; column++)
        {
            char label[32];
            snprintf(label, sizeof(label), "[%lu]", (unsigned long)column);
            if (strlen(label) > cell_width)
                cell_width = strlen(label);
            for (size_t row = 0; row < rows; row++)
            {
                Value item = value_null();
                if (!value_matrix_get(matrix, row, column, &item))
                    return false;
                char text[128];
                if (item.type == VALUE_INT)
                    snprintf(text, sizeof(text), "%" PRId64, item.as.integer);
                else
                    snprintf(text, sizeof(text), "%s", value_type_name(item.type));
                if (strlen(text) > cell_width)
                    cell_width = strlen(text);
            }
        }
        printf("MATRIX %s (%lu rows x %lu columns)\n", token,
               (unsigned long)rows, (unsigned long)columns);
        print_table_border(row_width, cell_width, columns, "┌", "┬", "┐");
        printf("│ %-*s │", (int)row_width, corner_label);
        for (size_t column = 0; column < columns; column++)
        {
            char label[32];
            snprintf(label, sizeof(label), "[%lu]", (unsigned long)column);
            printf(" %*s │", (int)cell_width, label);
        }
        printf("\n");
        print_table_border(row_width, cell_width, columns, "├", "┼", "┤");
        for (size_t row = 0; row < rows; row++)
        {
            char label[32];
            snprintf(label, sizeof(label), "[%lu]", (unsigned long)row);
            printf("│ %-*s │", (int)row_width, label);
            for (size_t column = 0; column < columns; column++)
            {
                Value item = value_null();
                if (!value_matrix_get(matrix, row, column, &item))
                    return false;
                if (item.type == VALUE_INT)
                    printf(" %*" PRId64 " │", (int)cell_width, item.as.integer);
                else
                    printf(" %*s │", (int)cell_width, value_type_name(item.type));
            }
            printf("\n");
        }
        print_table_border(row_width, cell_width, columns, "└", "┴", "┘");
        return true;
    }
    if (variable && variable->has_runtime_value &&
        variable->runtime_value.type == VALUE_QUEUE)
    {
        const Value *queue = &variable->runtime_value;
        size_t length = value_queue_length(queue);
        size_t capacity = value_queue_capacity(queue);
        size_t width = strlen("FULL: NO");
        for (size_t index = 0; index < length; index++)
        {
            Value item = value_null();
            char text[128];
            if (!value_queue_get(queue, index, &item))
                return false;
            if (item.type == VALUE_INT)
                snprintf(text, sizeof(text), "%" PRId64, item.as.integer);
            else
                snprintf(text, sizeof(text), "%s", value_type_name(item.type));
            const char *edge = index == 0 && index + 1 == length ? "FRONT/BACK"
                               : index == 0                      ? "FRONT"
                               : index + 1 == length              ? "BACK"
                                                                  : "      ";
            int text_length = snprintf(NULL, 0, "%s -> [%lu] value=%s slot=%lu",
                                       edge, (unsigned long)index, text,
                                       (unsigned long)index);
            if (text_length > 0 && (size_t)text_length > width)
                width = (size_t)text_length;
        }
        if (length == 0 && strlen("<empty queue>") > width)
            width = strlen("<empty queue>");
        printf("QUEUE %s (FIFO, size=%lu, capacity=%lu)\n", token,
               (unsigned long)length, (unsigned long)capacity);
        print_display_border(width, "┌", "┐");
        if (length == 0)
            print_display_row("<empty queue>", width);
        for (size_t index = 0; index < length; index++)
        {
            Value item = value_null();
            char text[128];
            if (!value_queue_get(queue, index, &item))
                return false;
            if (item.type == VALUE_INT)
                snprintf(text, sizeof(text), "%" PRId64, item.as.integer);
            else
                snprintf(text, sizeof(text), "%s", value_type_name(item.type));
            const char *edge = index == 0 && index + 1 == length ? "FRONT/BACK"
                               : index == 0                      ? "FRONT"
                               : index + 1 == length              ? "BACK"
                                                                  : "      ";
            char row[192];
            snprintf(row, sizeof(row), "%s -> [%lu] value=%s slot=%lu", edge,
                     (unsigned long)index, text, (unsigned long)index);
            print_display_row(row, width);
        }
        print_display_row(length >= 1000000U ? "FULL: YES" : "FULL: NO", width);
        print_display_border(width, "└", "┘");
        return true;
    }
    if (variable && variable->has_runtime_value &&
        variable->runtime_value.type == VALUE_OBJECT)
        return runtime_display_object(runtime, token, &variable->runtime_value);

    Value field_value = value_null();
    if (runtime_object_path_get(runtime, token, &field_value))
    {
        print_runtime_value(&field_value);
        value_release(&field_value);
        return true;
    }
    if (variable && variable->has_runtime_value &&
        variable->runtime_value.type != VALUE_ARRAY)
        return false;
    if (variable && variable->collection_kind == VARIABLE_COLLECTION_PILE)
        print_pile(token, &variable->runtime_value);
    else if (variable && variable->has_runtime_value &&
             variable->runtime_value.type == VALUE_ARRAY)
        print_indexed_table(token, &variable->runtime_value);
    else
        return false;
    return true;
}
