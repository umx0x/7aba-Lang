#include "ast.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char *source;
    const char *cursor;
    AstError *error;
    bool failed;
} Parser;

static void set_error(Parser *parser, const char *message)
{
    if (!parser->error || parser->failed)
        return;
    parser->failed = true;
    const char *at = parser->cursor;
    size_t line = 1;
    size_t column = 1;
    for (const char *p = parser->source; p < at; p++)
    {
        if (*p == '\n')
        {
            line++;
            column = 1;
        }
        else
            column++;
    }
    parser->error->span.line = line;
    parser->error->span.column = column;
    parser->error->span.end_line = line;
    parser->error->span.end_column = column + (*at ? 1 : 0);
    (void)snprintf(parser->error->message, sizeof(parser->error->message), "%s",
                   message);
}

static void set_oom(Parser *parser)
{
    set_error(parser, "Out of memory while parsing expression.");
}

static AstSpan span_for(const char *source, const char *start, const char *end)
{
    AstSpan span;
    span.line = 1;
    span.column = 1;
    for (const char *p = source; p < start; p++)
    {
        if (*p == '\n')
        {
            span.line++;
            span.column = 1;
        }
        else
            span.column++;
    }
    span.end_line = span.line;
    span.end_column = span.column;
    for (const char *p = start; p < end; p++)
    {
        if (*p == '\n')
        {
            span.end_line++;
            span.end_column = 1;
        }
        else
            span.end_column++;
    }
    return span;
}

static AstNode *new_node(Parser *parser, AstNodeKind kind,
                         const char *start, const char *end)
{
    AstNode *node = (AstNode *)calloc(1, sizeof(*node));
    if (!node)
    {
        set_oom(parser);
        return NULL;
    }
    node->kind = kind;
    node->span = span_for(parser->source, start, end);
    return node;
}

void ast_free(AstNode *node)
{
    if (!node)
        return;
    switch (node->kind)
    {
    case AST_STRING:
        free(node->as.string.bytes);
        break;
    case AST_NAME:
        free(node->as.name.name);
        break;
    case AST_UNARY:
        ast_free(node->as.unary.operand);
        break;
    case AST_GROUP:
        ast_free(node->as.grouped);
        break;
    case AST_BINARY:
        ast_free(node->as.binary.left);
        ast_free(node->as.binary.right);
        break;
    case AST_INDEX:
        ast_free(node->as.index.collection);
        ast_free(node->as.index.index);
        break;
    case AST_MEMBER:
        ast_free(node->as.member.object);
        free(node->as.member.name);
        break;
    case AST_CALL:
        free(node->as.call.name);
        for (size_t i = 0; i < node->as.call.argument_count; i++)
            ast_free(node->as.call.arguments[i]);
        free(node->as.call.arguments);
        break;
    case AST_MAP:
        for (size_t i = 0; i < node->as.map.count; i++)
        {
            ast_free(node->as.map.entries[i].key);
            ast_free(node->as.map.entries[i].value);
        }
        free(node->as.map.entries);
        break;
    case AST_SET:
        for (size_t i = 0; i < node->as.set.count; i++)
            ast_free(node->as.set.items[i]);
        free(node->as.set.items);
        break;
    case AST_INTEGER:
    case AST_BOOLEAN:
    case AST_NULL:
        break;
    }
    free(node);
}

static void skip_space(Parser *parser)
{
    while (isspace((unsigned char)*parser->cursor))
        parser->cursor++;
}

static char *copy_text(Parser *parser, const char *start, size_t length)
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

static bool reserve(void **items, size_t *capacity, size_t item_size)
{
    size_t next = *capacity ? *capacity * 2 : 4;
    if (next < *capacity || next > SIZE_MAX / item_size)
        return false;
    void *memory = realloc(*items, next * item_size);
    if (!memory)
        return false;
    *items = memory;
    *capacity = next;
    return true;
}

static bool append_node(Parser *parser, AstNode ***items, size_t *count,
                        size_t *capacity, AstNode *node)
{
    if (*count == *capacity &&
        !reserve((void **)items, capacity, sizeof(**items)))
    {
        set_oom(parser);
        return false;
    }
    (*items)[(*count)++] = node;
    return true;
}

static bool append_map_entry(Parser *parser, AstNode *map,
                             size_t *capacity, AstNode *key, AstNode *value)
{
    if (map->as.map.count == *capacity &&
        !reserve((void **)&map->as.map.entries, capacity,
                 sizeof(*map->as.map.entries)))
    {
        set_oom(parser);
        return false;
    }
    AstMapEntry *entry = &map->as.map.entries[map->as.map.count++];
    entry->key = key;
    entry->value = value;
    return true;
}

static bool is_identifier_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_identifier_part(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static bool is_builtin(const char *name, size_t length)
{
    static const char *builtins[] = {
        "abs", "min", "max", "len", "lower", "upper", "substr",
        "trim", "starts_with", "ends_with", "contains", "index_of", "count",
        "replace", "repeat", "sqrt", "sin", "cos", "log", "exp", "floor",
        "ceil", "round", "pi", "random_int",
        "random_seed"};
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (strlen(builtins[i]) == length &&
            memcmp(name, builtins[i], length) == 0)
            return true;
    return false;
}

static size_t builtin_argument_count(const char *name, size_t length)
{
    if ((length == 3 && memcmp(name, "abs", 3) == 0) ||
        (length == 3 && memcmp(name, "len", 3) == 0) ||
        (length == 5 && memcmp(name, "lower", 5) == 0) ||
        (length == 5 && memcmp(name, "upper", 5) == 0) ||
        (length == 4 && memcmp(name, "trim", 4) == 0))
        return 1;
    if (length == 6 && memcmp(name, "substr", 6) == 0)
        return 3;
    if ((length == 11 && memcmp(name, "starts_with", 11) == 0) ||
        (length == 9 && memcmp(name, "ends_with", 9) == 0) ||
        (length == 8 && memcmp(name, "contains", 8) == 0) ||
        (length == 8 && memcmp(name, "index_of", 8) == 0) ||
        (length == 5 && memcmp(name, "count", 5) == 0))
        return 2;
    if ((length == 7 && memcmp(name, "replace", 7) == 0))
        return 3;
    if (length == 6 && memcmp(name, "repeat", 6) == 0)
        return 2;
    if ((length == 4 && memcmp(name, "sqrt", 4) == 0) ||
        (length == 3 && memcmp(name, "sin", 3) == 0) ||
        (length == 3 && memcmp(name, "cos", 3) == 0) ||
        (length == 3 && memcmp(name, "log", 3) == 0) ||
        (length == 3 && memcmp(name, "exp", 3) == 0) ||
        (length == 5 && memcmp(name, "floor", 5) == 0) ||
        (length == 4 && memcmp(name, "ceil", 4) == 0) ||
        (length == 5 && memcmp(name, "round", 5) == 0) ||
        (length == 11 && memcmp(name, "random_seed", 11) == 0))
        return 1;
    if (length == 2 && memcmp(name, "pi", 2) == 0)
        return 0;
    if (length == 10 && memcmp(name, "random_int", 10) == 0)
        return 2;
    return 2;
}

static AstNode *parse_expression(Parser *parser);
static AstNode *parse_sum(Parser *parser);

static AstNode *parse_collection(Parser *parser, bool force_set,
                                 const char *start)
{
    if (*parser->cursor != '{')
    {
        set_error(parser, "Expected '{' after set.");
        return NULL;
    }
    parser->cursor++;
    skip_space(parser);
    if (*parser->cursor == '}')
    {
        parser->cursor++;
        return new_node(parser, force_set ? AST_SET : AST_MAP,
                        start, parser->cursor);
    }

    AstNode *first = parse_expression(parser);
    if (!first)
        return NULL;
    skip_space(parser);
    bool is_map = !force_set && *parser->cursor == ':';
    AstNode *collection = new_node(parser, is_map ? AST_MAP : AST_SET,
                                   start, parser->cursor);
    if (!collection)
    {
        ast_free(first);
        return NULL;
    }

    size_t capacity = 0;
    if (is_map)
    {
        for (;;)
        {
            skip_space(parser);
            if (*parser->cursor != ':')
            {
                set_error(parser, "Expected ':' between map key and value.");
                ast_free(first);
                ast_free(collection);
                return NULL;
            }
            parser->cursor++;
            AstNode *value = parse_expression(parser);
            if (!value)
            {
                ast_free(first);
                ast_free(collection);
                return NULL;
            }
            if (!append_map_entry(parser, collection, &capacity, first, value))
            {
                ast_free(first);
                ast_free(value);
                ast_free(collection);
                return NULL;
            }
            first = NULL;
            skip_space(parser);
            if (*parser->cursor != ',')
                break;
            parser->cursor++;
            skip_space(parser);
            if (*parser->cursor == '}')
                break;
            first = parse_expression(parser);
            if (!first)
            {
                ast_free(collection);
                return NULL;
            }
        }
    }
    else
    {
        if (!append_node(parser, &collection->as.set.items,
                         &collection->as.set.count, &capacity, first))
        {
            ast_free(first);
            ast_free(collection);
            return NULL;
        }
        for (;;)
        {
            skip_space(parser);
            if (*parser->cursor != ',')
                break;
            parser->cursor++;
            skip_space(parser);
            if (*parser->cursor == '}')
                break;
            AstNode *item = parse_expression(parser);
            if (!item)
            {
                ast_free(collection);
                return NULL;
            }
            if (!append_node(parser, &collection->as.set.items,
                             &collection->as.set.count, &capacity, item))
            {
                ast_free(item);
                ast_free(collection);
                return NULL;
            }
        }
    }

    skip_space(parser);
    if (*parser->cursor != '}')
    {
        set_error(parser, "Expected '}' to close collection literal.");
        ast_free(collection);
        return NULL;
    }
    parser->cursor++;
    collection->span = span_for(parser->source, start, parser->cursor);
    return collection;
}

static AstNode *parse_string(Parser *parser, const char *start)
{
    parser->cursor++;
    size_t available = strlen(parser->cursor);
    char *bytes = (char *)malloc(available + 1);
    if (!bytes)
    {
        set_oom(parser);
        return NULL;
    }
    size_t length = 0;
    while (*parser->cursor && *parser->cursor != '"')
    {
        char ch = *parser->cursor++;
        if (ch == '\\' && *parser->cursor)
        {
            char escaped = *parser->cursor++;
            ch = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
        }
        bytes[length++] = ch;
    }
    if (*parser->cursor != '"')
    {
        free(bytes);
        set_error(parser, "Unterminated string literal.");
        return NULL;
    }
    parser->cursor++;
    bytes[length] = '\0';
    AstNode *node = new_node(parser, AST_STRING, start, parser->cursor);
    if (!node)
    {
        free(bytes);
        return NULL;
    }
    node->as.string.bytes = bytes;
    node->as.string.length = length;
    return node;
}

static AstNode *parse_call(Parser *parser, const char *name_start,
                           size_t name_length, const char *call_start)
{
    AstNode *call = new_node(parser, AST_CALL, call_start, call_start);
    if (!call)
        return NULL;
    call->as.call.name = copy_text(parser, name_start, name_length);
    if (!call->as.call.name)
    {
        ast_free(call);
        return NULL;
    }
    call->as.call.name_length = name_length;
    parser->cursor++;
    skip_space(parser);
    size_t capacity = 0;
    if (*parser->cursor != ')')
    {
        for (;;)
        {
            if (call->as.call.argument_count >= 3)
            {
                set_error(parser, "Built-in calls accept at most three arguments.");
                ast_free(call);
                return NULL;
            }
            AstNode *argument = parse_expression(parser);
            if (!argument)
            {
                ast_free(call);
                return NULL;
            }
            if (!append_node(parser, &call->as.call.arguments,
                             &call->as.call.argument_count, &capacity, argument))
            {
                ast_free(argument);
                ast_free(call);
                return NULL;
            }
            skip_space(parser);
            if (*parser->cursor != ',')
                break;
            parser->cursor++;
            skip_space(parser);
            if (*parser->cursor == ')')
            {
                set_error(parser, "Expected an argument after ','.");
                ast_free(call);
                return NULL;
            }
        }
    }
    if (*parser->cursor != ')')
    {
        set_error(parser, "Expected ')' after built-in arguments.");
        ast_free(call);
        return NULL;
    }
    parser->cursor++;
    if (call->as.call.argument_count !=
        builtin_argument_count(name_start, name_length))
    {
        set_error(parser, "Incorrect number of built-in arguments.");
        ast_free(call);
        return NULL;
    }
    call->span = span_for(parser->source, call_start, parser->cursor);
    return call;
}

static AstNode *parse_primary(Parser *parser)
{
    skip_space(parser);
    const char *start = parser->cursor;
    if (*start == '(')
    {
        parser->cursor++;
        AstNode *inner = parse_expression(parser);
        if (!inner)
            return NULL;
        skip_space(parser);
        if (*parser->cursor != ')')
        {
            set_error(parser, "Expected ')' to close expression.");
            ast_free(inner);
            return NULL;
        }
        parser->cursor++;
        AstNode *group = new_node(parser, AST_GROUP, start, parser->cursor);
        if (!group)
        {
            ast_free(inner);
            return NULL;
        }
        group->as.grouped = inner;
        return group;
    }
    if (*start == '{')
        return parse_collection(parser, false, start);
    if (*start == '"')
        return parse_string(parser, start);

    if (isdigit((unsigned char)*start))
    {
        errno = 0;
        char *end = NULL;
        long value = strtol(start, &end, 10);
        if (errno == ERANGE || value < INT_MIN || value > INT_MAX)
        {
            set_error(parser, "Integer literal is outside the supported range.");
            return NULL;
        }
        parser->cursor = end;
        AstNode *node = new_node(parser, AST_INTEGER, start, end);
        if (node)
            node->as.integer = (int)value;
        return node;
    }

    if (is_identifier_start(*start))
    {
        parser->cursor++;
        while (is_identifier_part(*parser->cursor))
            parser->cursor++;
        size_t length = (size_t)(parser->cursor - start);
        if (length >= 64)
        {
            set_error(parser, "Identifier exceeds the 63-character limit.");
            return NULL;
        }
        if (length == 3 && memcmp(start, "set", 3) == 0)
        {
            const char *after_name = parser->cursor;
            skip_space(parser);
            if (*parser->cursor == '{')
                return parse_collection(parser, true, start);
            parser->cursor = after_name;
        }
        if ((length == 4 && memcmp(start, "true", 4) == 0) ||
            (length == 5 && memcmp(start, "false", 5) == 0) ||
            (length == 4 && memcmp(start, "null", 4) == 0))
        {
            AstNodeKind kind = length == 4 && memcmp(start, "null", 4) == 0
                                   ? AST_NULL
                                   : AST_BOOLEAN;
            AstNode *node = new_node(parser, kind, start, parser->cursor);
            if (node && kind == AST_BOOLEAN)
                node->as.boolean = start[0] == 't';
            return node;
        }
        const char *after_name = parser->cursor;
        skip_space(parser);
        if (*parser->cursor == '(')
        {
            if (is_builtin(start, length))
                return parse_call(parser, start, length, start);
        }
        parser->cursor = after_name;
        AstNode *node = new_node(parser, AST_NAME, start, after_name);
        if (!node)
            return NULL;
        node->as.name.name = copy_text(parser, start, length);
        if (!node->as.name.name)
        {
            ast_free(node);
            return NULL;
        }
        node->as.name.length = length;
        return node;
    }
    set_error(parser, "Expected a literal, name, collection, or parenthesized expression.");
    return NULL;
}

static AstNode *parse_postfix(Parser *parser)
{
    skip_space(parser);
    bool parenthesized = *parser->cursor == '(';
    AstNode *node = parse_primary(parser);
    if (!node || node->kind != AST_NAME || parenthesized)
        return node;
    const char *after_name = parser->cursor;
    skip_space(parser);
    const char *start = parser->cursor;
    if (*start == '[')
    {
        parser->cursor++;
        AstNode *index = parse_expression(parser);
        if (!index)
        {
            ast_free(node);
            return NULL;
        }
        skip_space(parser);
        if (*parser->cursor != ']')
        {
            set_error(parser, "Expected ']' after collection index.");
            ast_free(node);
            ast_free(index);
            return NULL;
        }
        parser->cursor++;
        AstNode *result = new_node(parser, AST_INDEX, start, parser->cursor);
        if (!result)
        {
            ast_free(node);
            ast_free(index);
            return NULL;
        }
        result->as.index.collection = node;
        result->as.index.index = index;
        result->span.line = node->span.line;
        result->span.column = node->span.column;
        return result;
    }
    if (*start == '.')
    {
        parser->cursor++;
        skip_space(parser);
        const char *name_start = parser->cursor;
        if (!is_identifier_start(*name_start))
        {
            set_error(parser, "Expected a property name after '.'.");
            ast_free(node);
            return NULL;
        }
        parser->cursor++;
        while (is_identifier_part(*parser->cursor))
            parser->cursor++;
        size_t length = (size_t)(parser->cursor - name_start);
        if (length >= 64)
        {
            set_error(parser, "Property name exceeds the 63-character limit.");
            ast_free(node);
            return NULL;
        }
        AstNode *result = new_node(parser, AST_MEMBER, start, parser->cursor);
        if (!result)
        {
            ast_free(node);
            return NULL;
        }
        result->as.member.name = copy_text(parser, name_start, length);
        if (!result->as.member.name)
        {
            ast_free(result);
            ast_free(node);
            return NULL;
        }
        result->as.member.length = length;
        result->as.member.object = node;
        result->span.line = node->span.line;
        result->span.column = node->span.column;
        return result;
    }
    parser->cursor = after_name;
    return node;
}

static AstNode *parse_unary(Parser *parser)
{
    skip_space(parser);
    const char *start = parser->cursor;
    if (*start == '-')
    {
        parser->cursor++;
        AstNode *operand = parse_unary(parser);
        if (!operand)
            return NULL;
        AstNode *node = new_node(parser, AST_UNARY, start, parser->cursor);
        if (!node)
        {
            ast_free(operand);
            return NULL;
        }
        node->as.unary.operation = AST_OP_NEGATE;
        node->as.unary.operand = operand;
        return node;
    }
    return parse_postfix(parser);
}

static AstNode *make_binary(Parser *parser, AstNode *left,
                            AstNode *right, AstOperator operation,
                            const char *end)
{
    AstNode *node = new_node(parser, AST_BINARY, parser->source, end);
    if (!node)
    {
        ast_free(left);
        ast_free(right);
        return NULL;
    }
    node->as.binary.operation = operation;
    node->as.binary.left = left;
    node->as.binary.right = right;
    node->span.line = left->span.line;
    node->span.column = left->span.column;
    return node;
}

static AstNode *parse_term(Parser *parser)
{
    AstNode *left = parse_unary(parser);
    if (!left)
        return NULL;
    for (;;)
    {
        skip_space(parser);
        char op = *parser->cursor;
        if (op != '*' && op != '/')
            return left;
        parser->cursor++;
        AstNode *right = parse_unary(parser);
        if (!right)
        {
            ast_free(left);
            return NULL;
        }
        left = make_binary(parser, left, right,
                           op == '*' ? AST_OP_MULTIPLY : AST_OP_DIVIDE,
                           parser->cursor);
        if (!left)
            return NULL;
    }
}

static AstNode *parse_sum(Parser *parser)
{
    AstNode *left = parse_term(parser);
    if (!left)
        return NULL;
    for (;;)
    {
        skip_space(parser);
        char op = *parser->cursor;
        if (op != '+' && op != '-')
            return left;
        parser->cursor++;
        AstNode *right = parse_term(parser);
        if (!right)
        {
            ast_free(left);
            return NULL;
        }
        left = make_binary(parser, left, right,
                           op == '+' ? AST_OP_ADD : AST_OP_SUBTRACT,
                           parser->cursor);
        if (!left)
            return NULL;
    }
}

static bool comparison_operator(Parser *parser, AstOperator *operation,
                                 size_t *length)
{
    if (strncmp(parser->cursor, "==", 2) == 0)
        *operation = AST_OP_EQUAL;
    else if (strncmp(parser->cursor, "!=", 2) == 0)
        *operation = AST_OP_NOT_EQUAL;
    else if (strncmp(parser->cursor, "<=", 2) == 0)
        *operation = AST_OP_LESS_EQUAL;
    else if (strncmp(parser->cursor, ">=", 2) == 0)
        *operation = AST_OP_GREATER_EQUAL;
    else if (*parser->cursor == '<')
        *operation = AST_OP_LESS;
    else if (*parser->cursor == '>')
        *operation = AST_OP_GREATER;
    else
        return false;
    *length = (parser->cursor[1] == '=' &&
               (*parser->cursor == '=' || *parser->cursor == '!' ||
                *parser->cursor == '<' || *parser->cursor == '>'))
                  ? 2
                  : 1;
    return true;
}

static AstNode *parse_expression(Parser *parser)
{
    AstNode *left = parse_sum(parser);
    if (!left)
        return NULL;
    skip_space(parser);
    if (strncmp(parser->cursor, "in", 2) == 0 &&
        !is_identifier_part(parser->cursor[2]))
    {
        parser->cursor += 2;
        AstNode *right = parse_sum(parser);
        if (!right)
        {
            ast_free(left);
            return NULL;
        }
        left = make_binary(parser, left, right, AST_OP_IN, parser->cursor);
        if (!left)
            return NULL;
    }
    skip_space(parser);
    AstOperator operation;
    size_t length;
    if (comparison_operator(parser, &operation, &length))
    {
        parser->cursor += length;
        AstNode *right = parse_sum(parser);
        if (!right)
        {
            ast_free(left);
            return NULL;
        }
        left = make_binary(parser, left, right, operation, parser->cursor);
    }
    return left;
}

bool ast_parse_expression_mode(const char *source, bool allow_block_opener,
                              AstNode **result, AstError *error)
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
    Parser parser = {source, source, error, false};
    skip_space(&parser);
    AstNode *node = parse_expression(&parser);
    if (!node)
        return false;
    skip_space(&parser);
    if (allow_block_opener && *parser.cursor == '{')
    {
        parser.cursor++;
        skip_space(&parser);
    }
    if (*parser.cursor == '#')
        parser.cursor += strlen(parser.cursor);
    if (*parser.cursor != '\0')
    {
        set_error(&parser, "Unexpected characters after expression.");
        ast_free(node);
        return false;
    }
    *result = node;
    return true;
}

bool ast_parse_expression(const char *source, AstNode **result, AstError *error)
{
    return ast_parse_expression_mode(source, true, result, error);
}
