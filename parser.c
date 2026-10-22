#include "main.h"

bool add_function(RuntimeContext *runtime, const Function *function)
{
    if (!runtime || !function)
        return false;
    for (size_t i = 0; i < runtime->func_count; i++)
        if (strcmp(runtime->funcs[i].name, function->name) == 0 &&
            strcmp(runtime->funcs[i].owner_class, function->owner_class) == 0)
            return false;
    if (runtime->func_count >= runtime->func_capacity)
    {
        size_t capacity = runtime->func_capacity == 0
                              ? 8
                              : runtime->func_capacity * 2;
        Function *new_funcs = realloc(runtime->funcs,
                                      capacity * sizeof(Function));
        if (!new_funcs)
            return false;
        runtime->funcs = new_funcs;
        runtime->func_capacity = capacity;
    }
    runtime->funcs[runtime->func_count++] = *function;
    return true;
}

int find_function_index(const RuntimeContext *runtime, const char *name)
{
    if (!runtime || !name)
        return -1;
    for (size_t i = 0; i < runtime->func_count; i++)
    {
        if (runtime->funcs[i].owner_class[0] == '\0' &&
            strcmp(runtime->funcs[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

void clear_classes(RuntimeContext *runtime)
{
    if (!runtime)
        return;
    memset(runtime->classes, 0, sizeof(runtime->classes));
    runtime->class_count = 0;
}

int find_class_index(const RuntimeContext *runtime, const char *name)
{
    if (!runtime || !name)
        return -1;
    for (size_t i = 0; i < runtime->class_count; i++)
        if (strcmp(runtime->classes[i].name, name) == 0)
            return (int)i;
    return -1;
}

int find_class_method(const RuntimeContext *runtime, int class_index,
                      const char *name)
{
    if (!runtime || !name)
        return -1;
    while (class_index >= 0 && (size_t)class_index < runtime->class_count)
    {
        for (size_t i = 0; i < runtime->func_count; i++)
            if (strcmp(runtime->funcs[i].owner_class,
                       runtime->classes[class_index].name) == 0 &&
                strcmp(runtime->funcs[i].name, name) == 0)
                return (int)i;
        class_index = runtime->classes[class_index].parent_index;
    }
    return -1;
}

void clear_functions(RuntimeContext *runtime)
{
    if (!runtime)
        return;
    free(runtime->funcs);
    runtime->funcs = NULL;
    runtime->func_count = 0;
    runtime->func_capacity = 0;
}

static const char *skip_space(const char *source)
{
    while (*source == ' ' || *source == '\t')
        source++;
    return source;
}

static bool read_identifier(const char **source, char destination[64])
{
    const char *start = *source;
    if (!(isalpha((unsigned char)*start) || *start == '_'))
        return false;
    const char *cursor = start + 1;
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    size_t length = (size_t)(cursor - start);
    if (length >= 64)
        return false;
    memcpy(destination, start, length);
    destination[length] = '\0';
    *source = cursor;
    return true;
}

bool parse_class_header(const char *line, ClassDefinition *class_definition)
{
    if (!line || !class_definition)
        return false;
    memset(class_definition, 0, sizeof(*class_definition));
    class_definition->parent_index = -1;
    const char *cursor = skip_space(line);
    if (strncmp(cursor, "class", 5) != 0 ||
        (isalnum((unsigned char)cursor[5]) || cursor[5] == '_'))
        return false;
    cursor = skip_space(cursor + 5);
    if (!read_identifier(&cursor, class_definition->name))
        return false;
    cursor = skip_space(cursor);
    if (strncmp(cursor, "extends", 7) == 0 &&
        !(isalnum((unsigned char)cursor[7]) || cursor[7] == '_'))
    {
        cursor = skip_space(cursor + 7);
        if (!read_identifier(&cursor, class_definition->parent_name))
            return false;
        cursor = skip_space(cursor);
    }
    if (*cursor == '{')
        cursor = skip_space(cursor + 1);
    return *cursor == '\0' || *cursor == '#' || *cursor == '\r' || *cursor == '\n';
}

bool parse_action_header(const char *line, Function *function)
{
    if (!line || !function)
        return false;
    memset(function, 0, sizeof(*function));
    const char *cursor = skip_space(line);
    if (strncmp(cursor, "action", 6) != 0 ||
        (isalnum((unsigned char)cursor[6]) || cursor[6] == '_'))
        return false;
    cursor = skip_space(cursor + 6);
    if (!read_identifier(&cursor, function->name))
        return false;
    cursor = skip_space(cursor);
    if (*cursor++ != '(')
        return false;
    cursor = skip_space(cursor);
    if (*cursor == ')')
    {
        cursor++;
    }
    else
    {
        while (true)
        {
            if (function->parameter_count >= MAX_ACTION_PARAMETERS)
                return false;
            char parameter[64];
            if (!read_identifier(&cursor, parameter))
                return false;
            for (size_t i = 0; i < function->parameter_count; i++)
            {
                if (strcmp(function->parameters[i], parameter) == 0)
                    return false;
            }
            strcpy(function->parameters[function->parameter_count++], parameter);
            cursor = skip_space(cursor);
            if (*cursor == ')')
            {
                cursor++;
                break;
            }
            if (*cursor++ != ',')
                return false;
            cursor = skip_space(cursor);
        }
    }
    cursor = skip_space(cursor);
    return *cursor == '{' || *cursor == '\0' || *cursor == '#' || *cursor == '\r' || *cursor == '\n';
}

// التحقق مما إذا كان السطر الحالي يقع داخل نطاق دالة معرفة مسبقاً
bool is_inside_function(const RuntimeContext *runtime, int line_idx)
{
    if (!runtime)
        return false;
    for (size_t i = 0; i < runtime->func_count; i++)
    {
        if (line_idx == runtime->funcs[i].header_line ||
            (line_idx >= runtime->funcs[i].start_line &&
             line_idx <= runtime->funcs[i].end_line))
        {
            return true;
        }
    }
    return false;
}

bool match_command(const char *line, const char *cmd)
{
    if (!line || !cmd)
        return false;
    while (*line == ' ' || *line == '\t')
        line++;
    size_t length = strlen(cmd);
    if (strncmp(line, cmd, length) != 0)
        return false;
    char next = line[length];
    return next == '\0' || next == ' ' || next == '\t' || next == '(' || next == '{' ||
           next == '=' || next == '<' || next == '>' || next == '[' || next == '\r' || next == '\n';
}

// تخطي اسم الأمر للوصول إلى المعاملات
char *skip_cmd(char *p, const char *cmd)
{
    (void)cmd;
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}
