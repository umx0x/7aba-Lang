#include "program_ast.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char *start;
    const char *end;
    const char *visible_end;
    size_t number;
} SourceLine;

typedef struct
{
    const char *source;
    const char *cursor;
    size_t line_number;
    size_t action_depth;
    SourceLine line;
    bool has_line;
    ProgramAstError *error;
    bool failed;
    bool out_of_memory;
} Parser;

static void set_error_at(Parser *parser, const char *at, const char *message)
{
    if (parser->failed)
        return;
    parser->failed = true;
    size_t line = 1;
    size_t column = 1;
    for (const char *cursor = parser->source; cursor < at; cursor++)
    {
        if (*cursor == '\n')
        {
            line++;
            column = 1;
        }
        else
        {
            column++;
        }
    }
    if (parser->error)
    {
        parser->error->span.line = line;
        parser->error->span.column = column;
        parser->error->span.end_line = line;
        parser->error->span.end_column = column + (*at ? 1U : 0U);
        (void)snprintf(parser->error->message, sizeof(parser->error->message),
                       "%s", message);
    }
}

static void set_error(Parser *parser, const char *message)
{
    set_error_at(parser, parser->has_line ? parser->line.start : parser->cursor,
                 message);
}

static void set_oom(Parser *parser)
{
    parser->out_of_memory = true;
    set_error(parser, "Out of memory while parsing program.");
}

static char *copy_range(Parser *parser, const char *start, size_t length)
{
    if (length == SIZE_MAX)
    {
        set_oom(parser);
        return NULL;
    }
    char *copy = (char *)malloc(length + 1);
    if (!copy)
    {
        set_oom(parser);
        return NULL;
    }
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static bool identifier_start(char c)
{
    return isalpha((unsigned char)c) != 0 || c == '_';
}

static bool identifier_part(char c)
{
    return isalnum((unsigned char)c) != 0 || c == '_';
}

static const char *skip_space(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                            *cursor == '\r' || *cursor == '\n'))
        cursor++;
    return cursor;
}

static const char *trim_end(const char *start, const char *end)
{
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    return end;
}

static const char *line_visible_end(const char *start, const char *end)
{
    bool quoted = false;
    bool escaped = false;
    for (const char *cursor = start; cursor < end; cursor++)
    {
        if (quoted && escaped)
        {
            escaped = false;
            continue;
        }
        if (quoted && *cursor == '\\')
        {
            escaped = true;
            continue;
        }
        if (*cursor == '"')
            quoted = !quoted;
        else if (!quoted && *cursor == '#')
            return cursor;
    }
    return end;
}

static void advance_line(Parser *parser)
{
    if (*parser->cursor == '\0')
    {
        parser->has_line = false;
        return;
    }
    const char *start = parser->cursor;
    const char *end = start;
    while (*end && *end != '\n')
        end++;
    const char *content_end = end;
    if (content_end > start && content_end[-1] == '\r')
        content_end--;
    parser->line.start = start;
    parser->line.end = end;
    parser->line.visible_end = line_visible_end(start, content_end);
    parser->line.number = parser->line_number++;
    parser->has_line = true;
    parser->cursor = *end == '\n' ? end + 1 : end;
}

static const char *line_start_trimmed(const SourceLine *line)
{
    return skip_space(line->start, line->visible_end);
}

static bool line_is_ignorable(const SourceLine *line)
{
    return line_start_trimmed(line) == line->visible_end;
}

static bool char_is_word(const char *cursor, const char *word, size_t length,
                         const char *end)
{
    return (size_t)(end - cursor) >= length &&
           memcmp(cursor, word, length) == 0 &&
           (cursor + length == end || !identifier_part(cursor[length]));
}

static bool read_identifier(Parser *parser, const char **cursor,
                            const char *end, char **result)
{
    const char *start = *cursor;
    if (start >= end || !identifier_start(*start))
        return false;
    (*cursor)++;
    while (*cursor < end && identifier_part(**cursor))
        (*cursor)++;
    size_t length = (size_t)(*cursor - start);
    if (length > 63)
    {
        set_error_at(parser, start, "Identifier exceeds the 63-character limit.");
        return false;
    }
    *result = copy_range(parser, start, length);
    return *result != NULL;
}

static bool valid_type(const char *start, const char *end)
{
    size_t length = (size_t)(end - start);
    return (length == 3 && memcmp(start, "Int", 3) == 0) ||
           (length == 5 && memcmp(start, "Float", 5) == 0) ||
           (length == 4 && memcmp(start, "Bool", 4) == 0) ||
           (length == 6 && memcmp(start, "String", 6) == 0) ||
           (length == 4 && memcmp(start, "Null", 4) == 0);
}

static AstSpan span_between(Parser *parser, const char *start, const char *end)
{
    AstSpan span = {1, 1, 1, 1};
    for (const char *cursor = parser->source; cursor < start; cursor++)
    {
        if (*cursor == '\n')
        {
            span.line++;
            span.column = 1;
        }
        else
        {
            span.column++;
        }
    }
    span.end_line = span.line;
    span.end_column = span.column;
    for (const char *cursor = start; cursor < end; cursor++)
    {
        if (*cursor == '\n')
        {
            span.end_line++;
            span.end_column = 1;
        }
        else
        {
            span.end_column++;
        }
    }
    return span;
}

static void shift_expression_span(AstNode *node, size_t line, size_t column)
{
    if (!node)
        return;
    node->span.line += line - 1;
    node->span.end_line += line - 1;
    if (node->span.line == line)
        node->span.column += column - 1;
    if (node->span.end_line == line)
        node->span.end_column += column - 1;
    switch (node->kind)
    {
    case AST_GROUP:
        shift_expression_span(node->as.grouped, line, column);
        break;
    case AST_UNARY:
        shift_expression_span(node->as.unary.operand, line, column);
        break;
    case AST_BINARY:
        shift_expression_span(node->as.binary.left, line, column);
        shift_expression_span(node->as.binary.right, line, column);
        break;
    case AST_INDEX:
        shift_expression_span(node->as.index.collection, line, column);
        shift_expression_span(node->as.index.index, line, column);
        break;
    case AST_MEMBER:
        shift_expression_span(node->as.member.object, line, column);
        break;
    case AST_CALL:
        for (size_t i = 0; i < node->as.call.argument_count; i++)
            shift_expression_span(node->as.call.arguments[i], line, column);
        break;
    case AST_MAP:
        for (size_t i = 0; i < node->as.map.count; i++)
        {
            shift_expression_span(node->as.map.entries[i].key, line, column);
            shift_expression_span(node->as.map.entries[i].value, line, column);
        }
        break;
    case AST_SET:
        for (size_t i = 0; i < node->as.set.count; i++)
            shift_expression_span(node->as.set.items[i], line, column);
        break;
    case AST_INTEGER:
    case AST_STRING:
    case AST_BOOLEAN:
    case AST_NULL:
    case AST_NAME:
        break;
    }
}

static bool parse_expression_range(Parser *parser, const char *start,
                                  const char *end, AstNode **result)
{
    start = skip_space(start, end);
    end = trim_end(start, end);
    if (start == end)
    {
        set_error_at(parser, start, "Expected an expression.");
        return false;
    }
    char *text = copy_range(parser, start, (size_t)(end - start));
    if (!text)
        return false;
    AstError ast_error;
    bool success = ast_parse_expression_mode(text, false, result, &ast_error);
    if (!success)
    {
        if (strstr(ast_error.message, "Out of memory") != NULL)
            parser->out_of_memory = true;
        const char *error_at = start;
        size_t offset = ast_error.span.column > 0 ? ast_error.span.column - 1 : 0;
        if (offset <= (size_t)(end - start))
            error_at += offset;
        set_error_at(parser, error_at,
                     ast_error.message[0] ? ast_error.message
                                           : "Invalid expression.");
        if (parser->error)
        {
            parser->error->span.line = parser->line.number;
            parser->error->span.end_line = parser->line.number;
            parser->error->span.column =
                (size_t)(error_at - parser->line.start) + 1;
            parser->error->span.end_column =
                parser->error->span.column + (*error_at ? 1U : 0U);
        }
    }
    else
    {
        size_t column = (size_t)(start - parser->line.start) + 1;
        shift_expression_span(*result, parser->line.number, column);
    }
    free(text);
    return success;
}

static ProgramAstStatement *new_statement(Parser *parser, ProgramAstKind kind,
                                          const char *start, const char *end)
{
    ProgramAstStatement *statement =
        (ProgramAstStatement *)calloc(1, sizeof(*statement));
    if (!statement)
    {
        set_oom(parser);
        return NULL;
    }
    statement->kind = kind;
    statement->span = span_between(parser, start, end);
    return statement;
}

static void statement_free(ProgramAstStatement *statement)
{
    if (!statement)
        return;
    free(statement->name);
    free(statement->target_name);
    free(statement->path_text);
    free(statement->type_text);
    free(statement->return_type);
    ast_free(statement->expression);
    for (size_t i = 0; i < statement->expression_count; i++)
        ast_free(statement->expressions[i]);
    free(statement->expressions);
    for (size_t i = 0; i < statement->name_count; i++)
        free(statement->names[i]);
    free(statement->names);
    for (size_t i = 0; i < statement->parameter_count; i++)
    {
        free(statement->parameters[i].name);
        free(statement->parameters[i].type_text);
    }
    free(statement->parameters);
    for (size_t i = 0; i < statement->child_count; i++)
        statement_free(statement->children[i]);
    free(statement->children);
    for (size_t i = 0; i < statement->otherwise_child_count; i++)
        statement_free(statement->otherwise_children[i]);
    free(statement->otherwise_children);
    free(statement);
}

void program_ast_free(ProgramAst *program)
{
    if (!program)
        return;
    for (size_t i = 0; i < program->statement_count; i++)
        statement_free(program->statements[i]);
    free(program->statements);
    free(program);
}

static bool append_statement(Parser *parser, ProgramAstStatement ***items,
                             size_t *count, ProgramAstStatement *statement)
{
    if (*count == SIZE_MAX / sizeof(**items))
    {
        set_oom(parser);
        return false;
    }
    ProgramAstStatement **grown = (ProgramAstStatement **)realloc(
        *items, (*count + 1) * sizeof(**items));
    if (!grown)
    {
        set_oom(parser);
        return false;
    }
    grown[*count] = statement;
    *items = grown;
    (*count)++;
    return true;
}

static bool append_expression(Parser *parser, ProgramAstStatement *statement,
                              AstNode *expression)
{
    if (statement->expression_count == SIZE_MAX / sizeof(*statement->expressions))
    {
        set_oom(parser);
        return false;
    }
    AstNode **grown = (AstNode **)realloc(
        statement->expressions,
        (statement->expression_count + 1) * sizeof(*statement->expressions));
    if (!grown)
    {
        set_oom(parser);
        return false;
    }
    grown[statement->expression_count++] = expression;
    statement->expressions = grown;
    return true;
}

static bool append_parameter(Parser *parser, ProgramAstStatement *statement,
                             ProgramAstParameter parameter)
{
    if (statement->parameter_count == SIZE_MAX / sizeof(*statement->parameters))
    {
        set_oom(parser);
        return false;
    }
    ProgramAstParameter *grown = (ProgramAstParameter *)realloc(
        statement->parameters,
        (statement->parameter_count + 1) * sizeof(*statement->parameters));
    if (!grown)
    {
        set_oom(parser);
        return false;
    }
    grown[statement->parameter_count++] = parameter;
    statement->parameters = grown;
    return true;
}

static bool append_name(Parser *parser, ProgramAstStatement *statement,
                        char *name)
{
    if (statement->name_count == SIZE_MAX / sizeof(*statement->names))
    {
        free(name);
        set_oom(parser);
        return false;
    }
    char **grown = (char **)realloc(
        statement->names, (statement->name_count + 1) * sizeof(*grown));
    if (!grown)
    {
        free(name);
        set_oom(parser);
        return false;
    }
    statement->names = grown;
    statement->names[statement->name_count++] = name;
    return true;
}

static bool parse_command_path(Parser *parser, const char **cursor,
                               const char *end, char **path)
{
    *cursor = skip_space(*cursor, end);
    bool quoted = *cursor < end && **cursor == '"';
    if (quoted)
        (*cursor)++;
    const char *start = *cursor;
    while (*cursor < end &&
           (quoted ? **cursor != '"' : **cursor != ' ' && **cursor != '\t'))
        (*cursor)++;
    size_t length = (size_t)(*cursor - start);
    if (length == 0 || length >= 260 || (quoted && *cursor >= end))
    {
        set_error_at(parser, start, "Expected a nonempty file path.");
        return false;
    }
    *path = copy_range(parser, start, length);
    if (!*path)
        return false;
    if (quoted)
        (*cursor)++;
    return true;
}

static bool parse_runtime_command(Parser *parser,
                                  ProgramAstStatement **result,
                                  const char *start, const char *end)
{
    ProgramAstKind kind;
    const char *keyword;
    size_t keyword_length;
    const char *cursor;
    bool is_input = char_is_word(start, "input", 5, end);
    bool is_save = char_is_word(start, "save", 4, end);
    bool is_load = char_is_word(start, "load", 4, end);
    bool is_file_display = char_is_word(start, "file_display", 12, end);
    bool is_file_read = char_is_word(start, "file_read", 9, end);
    bool is_file_write = char_is_word(start, "file_write", 10, end);
    if (!(is_input || is_save || is_load || is_file_display ||
          is_file_read || is_file_write))
        return false;

    kind = is_input            ? PROGRAM_AST_INPUT
           : is_save            ? PROGRAM_AST_COLLECTION_SAVE
           : is_load            ? PROGRAM_AST_COLLECTION_LOAD
           : is_file_display    ? PROGRAM_AST_FILE_DISPLAY
           : is_file_read       ? PROGRAM_AST_FILE_READ
                                : PROGRAM_AST_FILE_WRITE;
    keyword = is_input         ? "input"
              : is_save         ? "save"
              : is_load         ? "load"
              : is_file_display ? "file_display"
              : is_file_read    ? "file_read"
                                : "file_write";
    keyword_length = strlen(keyword);
    cursor = skip_space(start + keyword_length, end);
    ProgramAstStatement *statement = new_statement(parser, kind, start, end);
    if (!statement)
        return false;

    if (is_input)
    {
        while (cursor < end)
        {
            char *name = NULL;
            if (!read_identifier(parser, &cursor, end, &name) ||
                !append_name(parser, statement, name))
            {
                statement_free(statement);
                return false;
            }
            cursor = skip_space(cursor, end);
        }
        if (statement->name_count == 0)
            set_error_at(parser, start + keyword_length,
                         "`input` requires at least one variable name.");
    }
    else
    {
        if (is_save || is_load)
        {
            if (!read_identifier(parser, &cursor, end, &statement->name))
            {
                statement_free(statement);
                if (!parser->failed)
                    set_error_at(parser, cursor, "Expected a variable name.");
                return false;
            }
        }
        if (!parse_command_path(parser, &cursor, end, &statement->path_text))
        {
            statement_free(statement);
            return false;
        }
        cursor = skip_space(cursor, end);
        if (is_file_read)
        {
            if (!read_identifier(parser, &cursor, end, &statement->target_name))
            {
                statement_free(statement);
                if (!parser->failed)
                    set_error_at(parser, cursor,
                                 "Expected a destination variable name.");
                return false;
            }
            cursor = skip_space(cursor, end);
        }
        else if (is_file_write)
        {
            if (!read_identifier(parser, &cursor, end, &statement->name))
            {
                statement_free(statement);
                if (!parser->failed)
                    set_error_at(parser, cursor, "Expected a source variable name.");
                return false;
            }
            cursor = skip_space(cursor, end);
            if (cursor < end)
            {
                if (char_is_word(cursor, "append", 6, end))
                    statement->append_mode = true;
                else if (!char_is_word(cursor, "overwrite", 9, end))
                {
                    set_error_at(parser, cursor,
                                 "Expected `append` or `overwrite`.");
                    statement_free(statement);
                    return false;
                }
                cursor = skip_space(cursor +
                                        (statement->append_mode ? 6U : 9U),
                                    end);
            }
        }
        else if (is_save || is_load)
        {
            cursor = skip_space(cursor, end);
        }
    }
    if (cursor != end)
    {
        set_error_at(parser, cursor, "Unexpected syntax after command.");
        statement_free(statement);
        return false;
    }
    if (parser->failed)
    {
        statement_free(statement);
        return false;
    }
    *result = statement;
    return true;
}

static bool parse_sequence(Parser *parser, ProgramAstStatement ***items,
                           size_t *count, bool expect_close,
                           AstSpan *close_span, bool *inline_otherwise);
static ProgramAstStatement *parse_statement(Parser *parser, bool in_action);

static bool parse_close_suffix(Parser *parser, bool *is_otherwise,
                               const char **close_end)
{
    const char *start = line_start_trimmed(&parser->line);
    const char *end = parser->line.visible_end;
    if (start >= end || *start != '}')
        return false;
    *close_end = start + 1;
    const char *suffix = skip_space(start + 1, end);
    if (suffix == end)
    {
        *is_otherwise = false;
        return true;
    }
    if (!char_is_word(suffix, "otherwise", 9, end))
    {
        set_error_at(parser, suffix,
                     "Only `} otherwise {` may follow a block close on the same line.");
        return false;
    }
    suffix += 9;
    suffix = skip_space(suffix, end);
    if (suffix >= end || *suffix != '{' ||
        skip_space(suffix + 1, end) != end)
    {
        set_error_at(parser, suffix,
                     "Expected `otherwise {` after the closing brace.");
        return false;
    }
    *is_otherwise = true;
    return true;
}

static bool is_header_open(const char *start, const char *end,
                           const char **open)
{
    end = trim_end(start, end);
    if (end <= start || end[-1] != '{')
        return false;
    *open = end - 1;
    return true;
}

static bool is_exact_otherwise_header(const char *start, const char *end)
{
    const char *open = NULL;
    if (!is_header_open(start, end, &open))
        return false;
    const char *cursor = skip_space(start, open);
    return char_is_word(cursor, "otherwise", 9, open) &&
           skip_space(cursor + 9, open) == open;
}

static bool parse_block_body(Parser *parser, ProgramAstStatement *statement,
                             bool in_action, bool permit_otherwise_suffix,
                             AstSpan *close_span, bool *inline_otherwise)
{
    advance_line(parser);
    if (in_action)
        parser->action_depth++;
    if (!parse_sequence(parser, &statement->children, &statement->child_count,
                        true, close_span, inline_otherwise))
    {
        if (in_action)
            parser->action_depth--;
        return false;
    }
    if (in_action)
        parser->action_depth--;
    if (*inline_otherwise && !permit_otherwise_suffix)
    {
        set_error_at(parser, parser->line.start,
                     "An `otherwise` block can only follow a `when` statement.");
        return false;
    }
    return true;
}

static bool parse_otherwise_body(Parser *parser,
                                 ProgramAstStatement *statement,
                                 bool header_already_consumed)
{
    if (!header_already_consumed)
        advance_line(parser);
    AstSpan close_span = {0, 0, 0, 0};
    bool unexpected_otherwise = false;
    if (!parse_sequence(parser, &statement->otherwise_children,
                        &statement->otherwise_child_count, true, &close_span,
                        &unexpected_otherwise))
        return false;
    if (unexpected_otherwise)
    {
        set_error_at(parser, parser->cursor,
                     "An `otherwise` block cannot have another `otherwise` block.");
        return false;
    }
    statement->span.end_line = close_span.end_line;
    statement->span.end_column = close_span.end_column;
    return true;
}

static bool parse_when_otherwise(Parser *parser,
                                 ProgramAstStatement *statement,
                                 bool inline_otherwise)
{
    if (inline_otherwise)
    {
        if (!parse_otherwise_body(parser, statement, true))
            return false;
    }
    else
    {
        while (parser->has_line && line_is_ignorable(&parser->line))
            advance_line(parser);
        if (parser->has_line)
        {
            const char *start = line_start_trimmed(&parser->line);
            if (is_exact_otherwise_header(start, parser->line.visible_end))
            {
                const char *open = NULL;
                (void)is_header_open(start, parser->line.visible_end, &open);
                if (!parse_otherwise_body(parser, statement, false))
                    return false;
                (void)open;
            }
        }
    }
    return true;
}

static bool find_assignment_operator(const char *start, const char *end,
                                     const char **operator_position)
{
    bool quoted = false;
    bool escaped = false;
    size_t parentheses = 0;
    size_t brackets = 0;
    size_t braces = 0;
    for (const char *cursor = start; cursor < end; cursor++)
    {
        if (quoted && escaped)
        {
            escaped = false;
            continue;
        }
        if (quoted && *cursor == '\\')
        {
            escaped = true;
            continue;
        }
        if (*cursor == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (*cursor == '(')
            parentheses++;
        else if (*cursor == ')' && parentheses > 0)
            parentheses--;
        else if (*cursor == '[')
            brackets++;
        else if (*cursor == ']' && brackets > 0)
            brackets--;
        else if (*cursor == '{')
            braces++;
        else if (*cursor == '}' && braces > 0)
            braces--;
        else if (*cursor == '=' && parentheses == 0 && brackets == 0 &&
                 braces == 0 &&
                 (cursor + 1 == end || cursor[1] != '=') &&
                 (cursor == start || (cursor[-1] != '=' && cursor[-1] != '!' &&
                                       cursor[-1] != '<' && cursor[-1] != '>')))
        {
            *operator_position = cursor;
            return true;
        }
    }
    return false;
}

static bool parse_lvalue(Parser *parser, const char *start, const char *end,
                         char **name, char **type_text)
{
    start = skip_space(start, end);
    end = trim_end(start, end);
    const char *cursor = start;
    if (!read_identifier(parser, &cursor, end, name))
    {
        if (!parser->failed)
            set_error_at(parser, start, "Expected a variable name.");
        return false;
    }
    cursor = skip_space(cursor, end);
    if (cursor < end)
    {
        if (*cursor != ':')
        {
            set_error_at(parser, cursor,
                         "Only simple variable assignment is supported.");
            return false;
        }
        cursor = skip_space(cursor + 1, end);
        const char *type_start = cursor;
        while (cursor < end && identifier_part(*cursor))
            cursor++;
        const char *type_end = cursor;
        cursor = skip_space(cursor, end);
        if (type_start == type_end || cursor != end)
        {
            set_error_at(parser, type_start,
                         "Expected a scalar type after `:`.");
            return false;
        }
        if (!valid_type(type_start, type_end))
        {
            set_error_at(parser, type_start,
                         "Unsupported type; expected Int, Float, Bool, String, or Null.");
            return false;
        }
        *type_text = copy_range(parser, type_start,
                                (size_t)(type_end - type_start));
        if (!*type_text)
            return false;
    }
    else
    {
        *type_text = NULL;
    }
    return true;
}

static bool parse_assignment(Parser *parser, ProgramAstStatement **result,
                             const char *start, const char *end,
                             const char *equal)
{
    char *name = NULL;
    char *type_text = NULL;
    if (!parse_lvalue(parser, start, equal, &name, &type_text))
    {
        free(name);
        free(type_text);
        return false;
    }

    const char *value_start = skip_space(equal + 1, end);
    if (value_start == end)
    {
        free(name);
        free(type_text);
        set_error_at(parser, value_start,
                     "Assignment requires an expression.");
        return false;
    }
    ProgramAstStatement *statement = new_statement(
        parser, type_text ? PROGRAM_AST_TYPED_DECLARATION
                          : PROGRAM_AST_ASSIGNMENT,
        start, end);
    if (!statement)
    {
        free(name);
        free(type_text);
        return false;
    }
    statement->name = name;
    statement->type_text = type_text;
    if (!parse_expression_range(parser, value_start, end,
                                &statement->expression))
    {
        statement_free(statement);
        return false;
    }
    *result = statement;
    return true;
}

static bool parse_collection_statement(Parser *parser,
                                       ProgramAstStatement **result,
                                       const char *start, const char *end)
{
    ProgramAstKind kind;
    const char *cursor;
    char *name = NULL;
    char *target_name = NULL;
    AstNode *expression = NULL;
    if (char_is_word(start, "list", 4, end))
    {
        kind = PROGRAM_AST_LIST_CREATE;
        cursor = skip_space(start + 4, end);
    }
    else if (char_is_word(start, "pile", 4, end))
    {
        kind = PROGRAM_AST_PILE_CREATE;
        cursor = skip_space(start + 4, end);
    }
    else if (char_is_word(start, "push", 4, end))
    {
        kind = PROGRAM_AST_COLLECTION_PUSH;
        cursor = skip_space(start + 4, end);
    }
    else if (char_is_word(start, "pop", 3, end))
    {
        kind = PROGRAM_AST_COLLECTION_POP;
        cursor = skip_space(start + 3, end);
    }
    else if (char_is_word(start, "len", 3, end))
    {
        kind = PROGRAM_AST_COLLECTION_LEN;
        cursor = skip_space(start + 3, end);
    }
    else
        return false;

    if (!read_identifier(parser, &cursor, end, &name))
    {
        if (!parser->failed)
            set_error_at(parser, cursor, "Expected a collection variable name.");
        return false;
    }
    cursor = skip_space(cursor, end);
    if (kind == PROGRAM_AST_COLLECTION_LEN)
    {
        if (!read_identifier(parser, &cursor, end, &target_name))
        {
            free(name);
            if (!parser->failed)
                set_error_at(parser, cursor, "Expected a length destination name.");
            return false;
        }
        cursor = skip_space(cursor, end);
    }
    else if (kind == PROGRAM_AST_COLLECTION_PUSH)
    {
        if (cursor == end)
        {
            free(name);
            set_error_at(parser, cursor, "`push` requires a value expression.");
            return false;
        }
        if (!parse_expression_range(parser, cursor, end, &expression))
        {
            free(name);
            return false;
        }
        cursor = end;
    }
    if (cursor != end)
    {
        free(name);
        free(target_name);
        ast_free(expression);
        set_error_at(parser, cursor,
                     "Unexpected syntax after collection command.");
        return false;
    }
    ProgramAstStatement *statement = new_statement(parser, kind, start, end);
    if (!statement)
    {
        free(name);
        free(target_name);
        ast_free(expression);
        return false;
    }
    statement->name = name;
    statement->target_name = target_name;
    statement->expression = expression;
    *result = statement;
    return true;
}

static bool is_operator_word(const char *cursor, const char *end)
{
    return char_is_word(cursor, "in", 2, end);
}

static bool starts_expression_operator(const char *cursor, const char *end)
{
    cursor = skip_space(cursor, end);
    if (cursor >= end)
        return false;
    return *cursor == '+' || *cursor == '-' || *cursor == '*' ||
           *cursor == '/' || *cursor == '<' || *cursor == '>' ||
           *cursor == '=' || *cursor == '!' || is_operator_word(cursor, end);
}

static bool parse_display(Parser *parser, ProgramAstStatement *statement,
                          const char *start, const char *end)
{
    const char *cursor = skip_space(start, end);
    while (cursor < end)
    {
        const char *remaining_end = trim_end(cursor, end);
        AstNode *expression = NULL;
        AstError whole_error;
        char *whole = copy_range(parser, cursor,
                                 (size_t)(remaining_end - cursor));
        if (!whole)
            return false;
        bool whole_success =
            ast_parse_expression_mode(whole, false, &expression, &whole_error);
        free(whole);
        if (whole_success)
        {
            shift_expression_span(expression, parser->line.number,
                                 (size_t)(cursor - parser->line.start) + 1);
            if (!append_expression(parser, statement, expression))
            {
                ast_free(expression);
                return false;
            }
            return true;
        }
        if (strstr(whole_error.message, "Out of memory") != NULL)
        {
            parser->out_of_memory = true;
            set_error(parser, "Out of memory while parsing display expression.");
            return false;
        }

        bool quoted = false;
        bool escaped = false;
        size_t parentheses = 0;
        size_t brackets = 0;
        size_t braces = 0;
        bool found = false;
        for (const char *scan = cursor; scan < remaining_end; scan++)
        {
            if (quoted && escaped)
            {
                escaped = false;
                continue;
            }
            if (quoted && *scan == '\\')
            {
                escaped = true;
                continue;
            }
            if (*scan == '"')
            {
                quoted = !quoted;
                continue;
            }
            if (quoted)
                continue;
            if (*scan == '(')
                parentheses++;
            else if (*scan == ')' && parentheses > 0)
                parentheses--;
            else if (*scan == '[')
                brackets++;
            else if (*scan == ']' && brackets > 0)
                brackets--;
            else if (*scan == '{')
                braces++;
            else if (*scan == '}' && braces > 0)
                braces--;
            if (parentheses != 0 || brackets != 0 || braces != 0)
                continue;
            if (*scan != ',' && *scan != ' ' && *scan != '\t')
                continue;

            const char *candidate_end = trim_end(cursor, scan);
            const char *next = skip_space(scan, remaining_end);
            if (scan < remaining_end && *scan == ',')
            {
                next = skip_space(scan + 1, remaining_end);
                if (next == remaining_end)
                    continue;
            }
            if (candidate_end == cursor || starts_expression_operator(next,
                                                                        remaining_end))
                continue;
            AstNode *candidate = NULL;
            if (parse_expression_range(parser, cursor, candidate_end,
                                       &candidate))
            {
                if (!append_expression(parser, statement, candidate))
                {
                    ast_free(candidate);
                    return false;
                }
                cursor = next;
                found = true;
                break;
            }
            if (parser->out_of_memory)
                return false;
            parser->failed = false;
            if (parser->error)
            {
                parser->error->message[0] = '\0';
                memset(&parser->error->span, 0, sizeof(parser->error->span));
            }
        }
        if (!found)
        {
            const char *where = cursor;
            set_error_at(parser, where,
                         whole_error.message[0]
                             ? whole_error.message
                             : "Invalid display expression list.");
            if (parser->error)
            {
                parser->error->span.line = parser->line.number;
                parser->error->span.end_line = parser->line.number;
                parser->error->span.column =
                    (size_t)(where - parser->line.start) + 1;
                parser->error->span.end_column = parser->error->span.column + 1;
            }
            return false;
        }
    }
    if (statement->expression_count == 0)
    {
        set_error_at(parser, start, "`display` requires at least one expression.");
        return false;
    }
    return true;
}

static bool parse_call_arguments(Parser *parser, ProgramAstStatement *statement,
                                 const char **cursor, const char *end)
{
    if (**cursor != '(')
        return false;
    (*cursor)++;
    *cursor = skip_space(*cursor, end);
    if (*cursor < end && **cursor == ')')
    {
        (*cursor)++;
        return true;
    }
    while (*cursor < end && !parser->failed)
    {
        const char *arg_start = *cursor;
        bool quoted = false;
        bool escaped = false;
        size_t parentheses = 0;
        size_t brackets = 0;
        size_t braces = 0;
        while (*cursor < end)
        {
            char c = **cursor;
            if (quoted && escaped)
            {
                escaped = false;
                (*cursor)++;
                continue;
            }
            if (quoted && c == '\\')
            {
                escaped = true;
                (*cursor)++;
                continue;
            }
            if (c == '"')
                quoted = !quoted;
            else if (!quoted && c == '(')
                parentheses++;
            else if (!quoted && c == ')')
            {
                if (parentheses == 0 && brackets == 0 && braces == 0)
                    break;
                if (parentheses > 0)
                    parentheses--;
            }
            else if (!quoted && c == '[')
                brackets++;
            else if (!quoted && c == ']' && brackets > 0)
                brackets--;
            else if (!quoted && c == '{')
                braces++;
            else if (!quoted && c == '}' && braces > 0)
                braces--;
            else if (!quoted && c == ',' && parentheses == 0 &&
                     brackets == 0 && braces == 0)
                break;
            (*cursor)++;
        }
        const char *arg_end = trim_end(arg_start, *cursor);
        AstNode *argument = NULL;
        if (arg_start == arg_end ||
            !parse_expression_range(parser, arg_start, arg_end, &argument))
        {
            ast_free(argument);
            return false;
        }
        if (!append_expression(parser, statement, argument))
        {
            ast_free(argument);
            return false;
        }
        *cursor = skip_space(*cursor, end);
        if (*cursor < end && **cursor == ')')
        {
            (*cursor)++;
            return true;
        }
        if (*cursor >= end || **cursor != ',')
        {
            set_error_at(parser, *cursor,
                         "Expected `,` or `)` after action argument.");
            return false;
        }
        (*cursor)++;
        *cursor = skip_space(*cursor, end);
        if (*cursor >= end || **cursor == ')')
        {
            set_error_at(parser, *cursor,
                         "Action argument lists cannot have a trailing comma.");
            return false;
        }
    }
    set_error_at(parser, *cursor, "Expected `)` after action arguments.");
    return false;
}

static bool parse_action_call(Parser *parser, ProgramAstStatement **result,
                              const char *start, const char *end)
{
    const char *cursor = start;
    char *name = NULL;
    if (!read_identifier(parser, &cursor, end, &name))
    {
        if (!parser->failed)
            set_error_at(parser, start, "Expected an action name.");
        return false;
    }
    cursor = skip_space(cursor, end);
    if (cursor >= end || *cursor != '(')
    {
        free(name);
        return false;
    }
    ProgramAstStatement *statement =
        new_statement(parser, PROGRAM_AST_ACTION_CALL, start, end);
    if (!statement)
    {
        free(name);
        return false;
    }
    statement->name = name;
    if (!parse_call_arguments(parser, statement, &cursor, end))
    {
        statement_free(statement);
        return false;
    }
    cursor = skip_space(cursor, end);
    if (cursor != end)
    {
        statement_free(statement);
        set_error_at(parser, cursor,
                     "Unexpected text after action invocation.");
        return false;
    }
    *result = statement;
    return true;
}

static bool parse_action_header(Parser *parser, ProgramAstStatement *statement,
                                const char *start, const char *end,
                                const char **body_open)
{
    const char *cursor = start;
    if (!char_is_word(cursor, "action", 6, end))
        return false;
    cursor = skip_space(cursor + 6, end);
    if (!read_identifier(parser, &cursor, end, &statement->name))
        return false;
    cursor = skip_space(cursor, end);
    if (cursor >= end || *cursor != '(')
    {
        set_error_at(parser, cursor,
                     "Action signature requires a parenthesized parameter list.");
        return false;
    }
    cursor++;
    cursor = skip_space(cursor, end);
    if (cursor < end && *cursor != ')')
    {
        for (;;)
        {
            ProgramAstParameter parameter;
            memset(&parameter, 0, sizeof(parameter));
            const char *parameter_start = cursor;
            if (!read_identifier(parser, &cursor, end, &parameter.name))
            {
                free(parameter.name);
                return false;
            }
            cursor = skip_space(cursor, end);
            if (cursor < end && *cursor == ':')
            {
                cursor = skip_space(cursor + 1, end);
                const char *type_start = cursor;
                while (cursor < end && identifier_part(*cursor))
                    cursor++;
                const char *type_end = cursor;
                if (type_start == type_end || !valid_type(type_start, type_end))
                {
                    free(parameter.name);
                    set_error_at(parser, type_start,
                                 "Unsupported parameter type; expected Int, Float, Bool, String, or Null.");
                    return false;
                }
                parameter.type_text = copy_range(
                    parser, type_start, (size_t)(type_end - type_start));
                if (!parameter.type_text)
                {
                    free(parameter.name);
                    return false;
                }
                cursor = skip_space(cursor, end);
            }
            parameter.span = span_between(parser, parameter_start, cursor);
            for (size_t i = 0; i < statement->parameter_count; i++)
            {
                if (strcmp(statement->parameters[i].name, parameter.name) == 0)
                {
                    free(parameter.name);
                    free(parameter.type_text);
                    set_error_at(parser, parameter_start,
                                 "Duplicate action parameter name.");
                    return false;
                }
            }
            if (!append_parameter(parser, statement, parameter))
            {
                free(parameter.name);
                free(parameter.type_text);
                return false;
            }
            if (cursor < end && *cursor == ')')
                break;
            if (cursor >= end || *cursor != ',')
            {
                set_error_at(parser, cursor,
                             "Expected `,` or `)` in action parameter list.");
                return false;
            }
            cursor = skip_space(cursor + 1, end);
            if (cursor >= end || *cursor == ')')
            {
                set_error_at(parser, cursor,
                             "Action parameter lists cannot have a trailing comma.");
                return false;
            }
        }
    }
    if (cursor >= end || *cursor != ')')
    {
        set_error_at(parser, cursor, "Expected `)` in action signature.");
        return false;
    }
    cursor = skip_space(cursor + 1, end);
    const char *open = NULL;
    if (!is_header_open(start, end, &open))
    {
        set_error_at(parser, end, "Action definition must end with `{`.");
        return false;
    }
    if (cursor < open && open - cursor >= 2 && cursor[0] == '-' &&
        cursor[1] == '>')
    {
        cursor = skip_space(cursor + 2, open);
        const char *type_start = cursor;
        while (cursor < open && identifier_part(*cursor))
            cursor++;
        const char *type_end = cursor;
        if (type_start == type_end || !valid_type(type_start, type_end))
        {
            set_error_at(parser, type_start,
                         "Unsupported return type; expected Int, Float, Bool, String, or Null.");
            return false;
        }
        statement->return_type =
            copy_range(parser, type_start, (size_t)(type_end - type_start));
        if (!statement->return_type)
            return false;
        cursor = skip_space(cursor, open);
    }
    if (cursor != open)
    {
        set_error_at(parser, cursor,
                     "Unexpected syntax in action signature.");
        return false;
    }
    *body_open = open;
    return true;
}

static bool parse_control_header(Parser *parser, const char *start,
                                 const char *end, const char *keyword,
                                 const char **expression_start,
                                 const char **expression_end,
                                 const char **body_open)
{
    size_t length = strlen(keyword);
    if (!char_is_word(start, keyword, length, end))
        return false;
    const char *open = NULL;
    if (!is_header_open(start, end, &open))
    {
        set_error_at(parser, end,
                     "Control-flow header must end with `{`.");
        return false;
    }
    const char *expr_start = skip_space(start + length, open);
    const char *expr_end = trim_end(expr_start, open);
    if (expr_start == expr_end)
    {
        set_error_at(parser, expr_start,
                     "Control-flow header requires an expression.");
        return false;
    }
    *expression_start = expr_start;
    *expression_end = expr_end;
    *body_open = open;
    return true;
}

static ProgramAstStatement *parse_statement(Parser *parser, bool in_action)
{
    const char *start = line_start_trimmed(&parser->line);
    const char *end = trim_end(start, parser->line.visible_end);
    const char *line_start = parser->line.start;
    ProgramAstStatement *statement = NULL;

    const char *cursor = NULL;
    if (char_is_word(start, "action", 6, end))
    {
        if (in_action)
        {
            set_error_at(parser, start,
                         "Nested action definitions are not supported.");
            return NULL;
        }
        statement = new_statement(parser, PROGRAM_AST_ACTION, start, end);
        if (!statement)
            return NULL;
        const char *open = NULL;
        if (!parse_action_header(parser, statement, start, end, &open))
        {
            if (!parser->failed)
                set_error_at(parser, start, "Invalid action definition.");
            statement_free(statement);
            return NULL;
        }
        (void)open;
        AstSpan close_span = {0, 0, 0, 0};
        bool inline_otherwise = false;
        if (!parse_block_body(parser, statement, true, false, &close_span,
                              &inline_otherwise))
        {
            statement_free(statement);
            return NULL;
        }
        statement->span.end_line = close_span.end_line;
        statement->span.end_column = close_span.end_column;
        return statement;
    }

    if (char_is_word(start, "when", 4, end) ||
        char_is_word(start, "while", 5, end) ||
        char_is_word(start, "repeat", 6, end))
    {
        const char *keyword = char_is_word(start, "when", 4, end)
                                  ? "when"
                              : char_is_word(start, "while", 5, end)
                                  ? "while"
                                  : "repeat";
        const char *expr_start = NULL;
        const char *expr_end = NULL;
        const char *open = NULL;
        if (!parse_control_header(parser, start, end, keyword, &expr_start,
                                  &expr_end, &open))
            return NULL;
        ProgramAstKind kind = strcmp(keyword, "when") == 0
                                  ? PROGRAM_AST_WHEN
                              : strcmp(keyword, "while") == 0
                                  ? PROGRAM_AST_WHILE
                                  : PROGRAM_AST_REPEAT;
        statement = new_statement(parser, kind, start, end);
        if (!statement)
            return NULL;
        if (!parse_expression_range(parser, expr_start, expr_end,
                                    &statement->expression))
        {
            statement_free(statement);
            return NULL;
        }
        AstSpan close_span = {0, 0, 0, 0};
        bool inline_otherwise = false;
        if (!parse_block_body(parser, statement, in_action,
                              kind == PROGRAM_AST_WHEN, &close_span,
                              &inline_otherwise))
        {
            statement_free(statement);
            return NULL;
        }
        statement->span.end_line = close_span.end_line;
        statement->span.end_column = close_span.end_column;
        if (kind == PROGRAM_AST_WHEN &&
            !parse_when_otherwise(parser, statement, inline_otherwise))
        {
            statement_free(statement);
            return NULL;
        }
        (void)open;
        return statement;
    }

    if (char_is_word(start, "otherwise", 9, end))
    {
        set_error_at(parser, start,
                     "`otherwise` must immediately follow a `when` block.");
        return NULL;
    }

    if (char_is_word(start, "display", 7, end))
    {
        statement = new_statement(parser, PROGRAM_AST_DISPLAY, start, end);
        if (!statement)
            return NULL;
        cursor = skip_space(start + 7, end);
        if (!parse_display(parser, statement, cursor, end))
        {
            statement_free(statement);
            return NULL;
        }
        return statement;
    }

    if (char_is_word(start, "empty", 5, end))
    {
        cursor = skip_space(start + 5, end);
        if (cursor != end)
        {
            set_error_at(parser, cursor,
                         "`empty` is a standalone return statement.");
            return NULL;
        }
        statement = new_statement(parser, PROGRAM_AST_RETURN, start, end);
        return statement;
    }

    if (char_is_word(start, "throw", 5, end))
    {
        cursor = skip_space(start + 5, end);
        if (cursor == end)
        {
            set_error_at(parser, cursor, "`throw` requires an expression.");
            return NULL;
        }
        statement = new_statement(parser, PROGRAM_AST_THROW, start, end);
        if (!statement)
            return NULL;
        if (!parse_expression_range(parser, cursor, end,
                                    &statement->expression))
        {
            statement_free(statement);
            return NULL;
        }
        return statement;
    }

    if (*start == '{')
    {
        set_error_at(parser, start,
                     "Standalone block openers are not supported.");
        return NULL;
    }

    if (char_is_word(start, "list", 4, end) ||
        char_is_word(start, "pile", 4, end) ||
        char_is_word(start, "push", 4, end) ||
        char_is_word(start, "pop", 3, end) ||
        char_is_word(start, "len", 3, end))
    {
        if (!parse_collection_statement(parser, &statement, start, end))
            return NULL;
        return statement;
    }

    if (char_is_word(start, "input", 5, end) ||
        char_is_word(start, "save", 4, end) ||
        char_is_word(start, "load", 4, end) ||
        char_is_word(start, "file_display", 12, end) ||
        char_is_word(start, "file_read", 9, end) ||
        char_is_word(start, "file_write", 10, end))
    {
        if (!parse_runtime_command(parser, &statement, start, end))
            return NULL;
        return statement;
    }

    const char *equal = NULL;
    if (find_assignment_operator(start, end, &equal))
    {
        if (!parse_assignment(parser, &statement, start, end, equal))
            return NULL;
        return statement;
    }

    const char *after_name = start;
    while (after_name < end && identifier_part(*after_name))
        after_name++;
    if (after_name > start && identifier_start(*start))
    {
        if (parse_action_call(parser, &statement, start, end))
            return statement;
        if (parser->failed)
            return NULL;
    }
    set_error_at(parser, start,
                 "Unsupported statement syntax; expected assignment, declaration, display, action call, empty, throw, or block header.");
    (void)line_start;
    return NULL;
}

static bool parse_sequence(Parser *parser, ProgramAstStatement ***items,
                           size_t *count, bool expect_close,
                           AstSpan *close_span, bool *inline_otherwise)
{
    while (parser->has_line && !parser->failed)
    {
        if (line_is_ignorable(&parser->line))
        {
            advance_line(parser);
            continue;
        }
        const char *start = line_start_trimmed(&parser->line);
        if (*start == '}')
        {
            if (!expect_close)
            {
                set_error_at(parser, start, "Unexpected closing brace.");
                return false;
            }
            bool is_otherwise = false;
            const char *brace_end = NULL;
            if (!parse_close_suffix(parser, &is_otherwise, &brace_end))
                return false;
            *close_span = span_between(parser, start, brace_end);
            *inline_otherwise = is_otherwise;
            advance_line(parser);
            return true;
        }
        if (char_is_word(start, "otherwise", 9, parser->line.visible_end))
        {
            set_error_at(parser, start,
                         "`otherwise` is only valid after a `when` block.");
            return false;
        }
        const char *statement_line_start = parser->line.start;
        ProgramAstStatement *statement =
            parse_statement(parser, parser->action_depth != 0);
        if (!statement)
            return false;
        if (parser->has_line && parser->line.start == statement_line_start)
            advance_line(parser);
        if (!append_statement(parser, items, count, statement))
        {
            statement_free(statement);
            return false;
        }
    }
    if (expect_close && !parser->failed)
        set_error_at(parser, parser->cursor, "Expected `}` to close block.");
    return !parser->failed;
}

bool program_ast_parse(const char *source, ProgramAst **result,
                       ProgramAstError *error)
{
    if (result)
        *result = NULL;
    if (error)
    {
        memset(error, 0, sizeof(*error));
        error->span.line = 1;
        error->span.column = 1;
        error->span.end_line = 1;
        error->span.end_column = 1;
    }
    if (!source || !result)
    {
        if (error)
            (void)snprintf(error->message, sizeof(error->message),
                           "Source and result output are required.");
        return false;
    }

    ProgramAst *program = (ProgramAst *)calloc(1, sizeof(*program));
    if (!program)
    {
        if (error)
            (void)snprintf(error->message, sizeof(error->message),
                           "Out of memory while parsing program.");
        return false;
    }
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.source = source;
    parser.cursor = source;
    parser.line_number = 1;
    parser.error = error;
    advance_line(&parser);
    AstSpan close_span = {0, 0, 0, 0};
    bool inline_otherwise = false;
    if (!parse_sequence(&parser, &program->statements,
                        &program->statement_count, false, &close_span,
                        &inline_otherwise))
    {
        program_ast_free(program);
        return false;
    }
    *result = program;
    return true;
}
