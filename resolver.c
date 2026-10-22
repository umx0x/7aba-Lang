#include "resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const ProgramAst *program;
    ProgramResolution *resolution;
    ResolverError *error;
    const char *active_return_type;
    bool failed;
} Resolver;

static void fail(Resolver *resolver, AstSpan span, const char *message)
{
    if (resolver->failed)
        return;
    resolver->failed = true;
    if (!resolver->error)
        return;
    resolver->error->span = span;
    (void)snprintf(resolver->error->message,
                   sizeof(resolver->error->message), "%s", message);
}

static void fail_name(Resolver *resolver, AstSpan span, const char *prefix,
                      const char *name)
{
    char message[200];
    (void)snprintf(message, sizeof(message), "%s '%s'.", prefix, name);
    fail(resolver, span, message);
}

static char *copy_string(const char *text)
{
    size_t length;
    char *copy;
    if (!text)
        return NULL;
    length = strlen(text);
    if (length == (size_t)-1)
        return NULL;
    copy = (char *)malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

void resolver_resolution_free(ProgramResolution *resolution)
{
    if (!resolution)
        return;
    for (size_t i = 0; i < resolution->symbol_count; i++)
    {
        free(resolution->symbols[i].name);
        free(resolution->symbols[i].type_name);
        for (size_t j = 0; j < resolution->symbols[i].parameter_count; j++)
            free(resolution->symbols[i].parameter_types[j]);
        free(resolution->symbols[i].parameter_types);
    }
    for (size_t i = 0; i < resolution->reference_count; i++)
        free(resolution->references[i].name);
    free(resolution->symbols);
    free(resolution->scopes);
    free(resolution->references);
    memset(resolution, 0, sizeof(*resolution));
}

static bool append_scope(Resolver *resolver, size_t parent_id, bool has_parent,
                         size_t *scope_id)
{
    ProgramResolution *result = resolver->resolution;
    ResolverScope *grown;
    size_t count = result->scope_count;
    if (count == (size_t)-1 / sizeof(*grown))
        goto oom;
    grown = (ResolverScope *)realloc(result->scopes,
                                     (count + 1) * sizeof(*grown));
    if (!grown)
        goto oom;
    result->scopes = grown;
    grown[count].id = count;
    grown[count].parent_id = parent_id;
    grown[count].has_parent = has_parent;
    result->scope_count++;
    *scope_id = count;
    return true;

oom:
    fail(resolver, (AstSpan){1, 1, 1, 1},
         "Out of memory while creating a lexical scope.");
    return false;
}

static bool local_lookup(const ProgramResolution *resolution, size_t scope_id,
                         const char *name, ResolverSymbolKind kind,
                         size_t *symbol_index)
{
    for (size_t i = resolution->symbol_count; i > 0; i--)
    {
        const ResolverSymbol *symbol = &resolution->symbols[i - 1];
        if (symbol->scope_id == scope_id && symbol->kind == kind &&
            strcmp(symbol->name, name) == 0)
        {
            if (symbol_index)
                *symbol_index = i - 1;
            return true;
        }
    }
    return false;
}

bool resolver_lookup(const ProgramResolution *resolution, size_t scope_id,
                     const char *name, ResolverSymbolKind kind,
                     size_t *symbol_index)
{
    if (!resolution || !name || scope_id >= resolution->scope_count)
        return false;
    for (;;)
    {
        if (local_lookup(resolution, scope_id, name, kind, symbol_index))
            return true;
        if (!resolution->scopes[scope_id].has_parent)
            return false;
        scope_id = resolution->scopes[scope_id].parent_id;
        if (scope_id >= resolution->scope_count)
            return false;
    }
}

static bool add_symbol(Resolver *resolver, const char *name,
                       const char *type_name, ResolverSymbolKind kind,
                       size_t scope_id, size_t parameter_count,
                       const ProgramAstStatement *action,
                       AstSpan declaration_span, size_t *symbol_index)
{
    ProgramResolution *result = resolver->resolution;
    ResolverSymbol *grown;
    char *name_copy;
    char *type_copy = NULL;
    char **parameter_types = NULL;
    if (!name || scope_id >= result->scope_count)
        return false;
    if (local_lookup(result, scope_id, name, kind, NULL))
    {
        fail_name(resolver, declaration_span,
                  kind == RESOLVER_ACTION ? "Duplicate action" :
                                            "Duplicate variable",
                  name);
        return false;
    }
    if (result->symbol_count == (size_t)-1 / sizeof(*grown))
        goto oom;
    name_copy = copy_string(name);
    if (!name_copy)
        goto oom;
    if (type_name)
    {
        type_copy = copy_string(type_name);
        if (!type_copy)
        {
            free(name_copy);
            goto oom;
        }
    }
    if (kind == RESOLVER_ACTION && parameter_count > 0)
    {
        if (parameter_count > (size_t)-1 / sizeof(*parameter_types))
        {
            free(name_copy);
            free(type_copy);
            goto oom;
        }
        parameter_types = (char **)calloc(parameter_count,
                                          sizeof(*parameter_types));
        if (!parameter_types)
        {
            free(name_copy);
            free(type_copy);
            goto oom;
        }
        for (size_t i = 0; i < parameter_count; i++)
        {
            if (action->parameters[i].type_text)
            {
                parameter_types[i] =
                    copy_string(action->parameters[i].type_text);
                if (!parameter_types[i])
                {
                    for (size_t j = 0; j < i; j++)
                        free(parameter_types[j]);
                    free(parameter_types);
                    free(name_copy);
                    free(type_copy);
                    goto oom;
                }
            }
        }
    }
    grown = (ResolverSymbol *)realloc(
        result->symbols, (result->symbol_count + 1) * sizeof(*grown));
    if (!grown)
    {
        free(name_copy);
        free(type_copy);
        for (size_t i = 0; i < parameter_count; i++)
            free(parameter_types ? parameter_types[i] : NULL);
        free(parameter_types);
        goto oom;
    }
    result->symbols = grown;
    result->symbols[result->symbol_count].name = name_copy;
    result->symbols[result->symbol_count].type_name = type_copy;
    result->symbols[result->symbol_count].kind = kind;
    result->symbols[result->symbol_count].scope_id = scope_id;
    result->symbols[result->symbol_count].parameter_count = parameter_count;
    result->symbols[result->symbol_count].parameter_types = parameter_types;
    result->symbols[result->symbol_count].declaration_span = declaration_span;
    if (symbol_index)
        *symbol_index = result->symbol_count;
    result->symbol_count++;
    return true;

oom:
    fail(resolver, declaration_span,
         "Out of memory while adding a resolved symbol.");
    return false;
}

static bool add_reference(Resolver *resolver, const char *name, AstSpan span,
                          size_t symbol_index)
{
    ProgramResolution *result = resolver->resolution;
    ResolverReference *grown;
    char *copy;
    if (result->reference_count == (size_t)-1 / sizeof(*grown))
        goto oom;
    copy = copy_string(name);
    if (!copy)
        goto oom;
    grown = (ResolverReference *)realloc(
        result->references,
        (result->reference_count + 1) * sizeof(*grown));
    if (!grown)
    {
        free(copy);
        goto oom;
    }
    result->references = grown;
    result->references[result->reference_count].name = copy;
    result->references[result->reference_count].span = span;
    result->references[result->reference_count].symbol_index = symbol_index;
    result->reference_count++;
    return true;

oom:
    fail(resolver, span, "Out of memory while recording a name reference.");
    return false;
}

static bool resolve_expression(Resolver *resolver, const AstNode *node,
                               size_t scope_id);
static bool resolve_sequence(Resolver *resolver,
                             ProgramAstStatement *const *statements,
                             size_t count, size_t scope_id,
                             bool allow_actions);

static bool collect_variables(Resolver *resolver,
                              ProgramAstStatement *const *statements,
                              size_t count, size_t scope_id)
{
    for (size_t i = 0; i < count && !resolver->failed; i++)
    {
        ProgramAstStatement *statement = statements[i];
        if (statement->kind == PROGRAM_AST_ACTION)
            continue;
        if (statement->kind == PROGRAM_AST_ASSIGNMENT ||
            statement->kind == PROGRAM_AST_TYPED_DECLARATION)
        {
            size_t existing;
            if (local_lookup(resolver->resolution, scope_id, statement->name,
                             RESOLVER_VARIABLE, &existing))
            {
                ResolverSymbol *symbol =
                    &resolver->resolution->symbols[existing];
                if (statement->kind == PROGRAM_AST_TYPED_DECLARATION)
                {
                    if (symbol->type_name)
                    {
                        fail_name(resolver, statement->span,
                                  "Duplicate typed declaration for",
                                  statement->name);
                        return false;
                    }
                    symbol->type_name = copy_string(statement->type_text);
                    if (!symbol->type_name)
                    {
                        fail(resolver, statement->span,
                             "Out of memory while recording a variable type.");
                        return false;
                    }
                }
            }
            else if (!add_symbol(
                         resolver, statement->name,
                         statement->kind == PROGRAM_AST_TYPED_DECLARATION
                             ? statement->type_text
                             : NULL,
                         RESOLVER_VARIABLE, scope_id, 0, NULL, statement->span,
                         NULL))
            {
                return false;
            }
        }
        else if (statement->kind == PROGRAM_AST_LIST_CREATE ||
                 statement->kind == PROGRAM_AST_PILE_CREATE)
        {
            if (!local_lookup(resolver->resolution, scope_id, statement->name,
                              RESOLVER_VARIABLE, NULL) &&
                !add_symbol(resolver, statement->name, NULL,
                            RESOLVER_VARIABLE, scope_id, 0, NULL,
                            statement->span, NULL))
                return false;
        }
        else if (statement->kind == PROGRAM_AST_COLLECTION_LEN)
        {
            if (!local_lookup(resolver->resolution, scope_id,
                              statement->target_name, RESOLVER_VARIABLE,
                              NULL) &&
                !add_symbol(resolver, statement->target_name, NULL,
                            RESOLVER_VARIABLE, scope_id, 0, NULL,
                            statement->span, NULL))
                return false;
        }
        else if (statement->kind == PROGRAM_AST_INPUT)
        {
            for (size_t name_index = 0;
                 name_index < statement->name_count; name_index++)
                if (!local_lookup(resolver->resolution, scope_id,
                                  statement->names[name_index],
                                  RESOLVER_VARIABLE, NULL) &&
                    !add_symbol(resolver, statement->names[name_index], NULL,
                                RESOLVER_VARIABLE, scope_id, 0, NULL,
                                statement->span, NULL))
                    return false;
        }
        else if (statement->kind == PROGRAM_AST_COLLECTION_LOAD ||
                 statement->kind == PROGRAM_AST_FILE_READ)
        {
            const char *target =
                statement->kind == PROGRAM_AST_COLLECTION_LOAD
                    ? statement->name
                    : statement->target_name;
            if (!local_lookup(resolver->resolution, scope_id, target,
                              RESOLVER_VARIABLE, NULL) &&
                !add_symbol(resolver, target, NULL, RESOLVER_VARIABLE,
                            scope_id, 0, NULL, statement->span, NULL))
                return false;
        }
        if ((statement->kind == PROGRAM_AST_WHEN ||
             statement->kind == PROGRAM_AST_WHILE ||
             statement->kind == PROGRAM_AST_REPEAT) &&
            !collect_variables(resolver, statement->children,
                               statement->child_count, scope_id))
            return false;
        if (statement->kind == PROGRAM_AST_WHEN &&
            !collect_variables(resolver, statement->otherwise_children,
                               statement->otherwise_child_count, scope_id))
            return false;
    }
    return !resolver->failed;
}

static bool add_reference_to_symbol(Resolver *resolver, const char *name,
                                    AstSpan span, size_t scope_id,
                                    ResolverSymbolKind kind)
{
    size_t index;
    if (!resolver_lookup(resolver->resolution, scope_id, name, kind, &index))
    {
        fail_name(resolver, span,
                  kind == RESOLVER_ACTION ? "Unresolved action" :
                                            "Unresolved variable",
                  name);
        return false;
    }
    return add_reference(resolver, name, span, index);
}

static const char *infer_expression_type(Resolver *resolver,
                                         const AstNode *node,
                                         size_t scope_id)
{
    if (!node)
        return NULL;
    switch (node->kind)
    {
    case AST_INTEGER:
        return "Int";
    case AST_STRING:
        return "String";
    case AST_BOOLEAN:
        return "Bool";
    case AST_NULL:
        return "Null";
    case AST_NAME:
    {
        size_t index;
        if (!resolver_lookup(resolver->resolution, scope_id,
                             node->as.name.name, RESOLVER_VARIABLE, &index))
            return NULL;
        return resolver->resolution->symbols[index].type_name;
    }
    case AST_GROUP:
        return infer_expression_type(resolver, node->as.grouped, scope_id);
    case AST_UNARY:
        return infer_expression_type(resolver, node->as.unary.operand,
                                     scope_id);
    case AST_BINARY:
    {
        const char *left = infer_expression_type(
            resolver, node->as.binary.left, scope_id);
        const char *right = infer_expression_type(
            resolver, node->as.binary.right, scope_id);
        switch (node->as.binary.operation)
        {
        case AST_OP_EQUAL:
        case AST_OP_NOT_EQUAL:
        case AST_OP_LESS:
        case AST_OP_LESS_EQUAL:
        case AST_OP_GREATER:
        case AST_OP_GREATER_EQUAL:
        case AST_OP_IN:
            return "Bool";
        case AST_OP_ADD:
            if ((left && strcmp(left, "String") == 0) ||
                (right && strcmp(right, "String") == 0))
                return "String";
            if (left && right && strcmp(left, "Int") == 0 &&
                strcmp(right, "Int") == 0)
                return "Int";
            return NULL;
        case AST_OP_SUBTRACT:
        case AST_OP_MULTIPLY:
        case AST_OP_DIVIDE:
            if (left && right && strcmp(left, "Int") == 0 &&
                strcmp(right, "Int") == 0)
                return "Int";
            return NULL;
        case AST_OP_NEGATE:
            return NULL;
        }
        return NULL;
    }
    case AST_CALL:
        if (node->as.call.name_length == 3 &&
            memcmp(node->as.call.name, "len", 3) == 0)
            return "Int";
        if (node->as.call.name_length == 3 &&
            memcmp(node->as.call.name, "abs", 3) == 0 &&
            node->as.call.argument_count == 1)
        {
            const char *argument = infer_expression_type(
                resolver, node->as.call.arguments[0], scope_id);
            return argument &&
                           (strcmp(argument, "Int") == 0 ||
                            strcmp(argument, "Float") == 0)
                       ? argument
                       : NULL;
        }
        if (node->as.call.name_length == 3 &&
            (memcmp(node->as.call.name, "min", 3) == 0 ||
             memcmp(node->as.call.name, "max", 3) == 0) &&
            node->as.call.argument_count == 2)
        {
            const char *left = infer_expression_type(
                resolver, node->as.call.arguments[0], scope_id);
            const char *right = infer_expression_type(
                resolver, node->as.call.arguments[1], scope_id);
            if (left && right && strcmp(left, right) == 0 &&
                (strcmp(left, "Int") == 0 || strcmp(left, "Float") == 0))
                return left;
            return NULL;
        }
        if ((node->as.call.name_length == 5 &&
             (memcmp(node->as.call.name, "lower", 5) == 0 ||
              memcmp(node->as.call.name, "upper", 5) == 0)) ||
            (node->as.call.name_length == 6 &&
             memcmp(node->as.call.name, "substr", 6) == 0))
            return "String";
        if ((node->as.call.name_length == 4 &&
              memcmp(node->as.call.name, "sqrt", 4) == 0) ||
             (node->as.call.name_length == 3 &&
              (memcmp(node->as.call.name, "sin", 3) == 0 ||
               memcmp(node->as.call.name, "cos", 3) == 0 ||
               memcmp(node->as.call.name, "log", 3) == 0 ||
               memcmp(node->as.call.name, "exp", 3) == 0)) ||
             (node->as.call.name_length == 5 &&
              (memcmp(node->as.call.name, "floor", 5) == 0 ||
               memcmp(node->as.call.name, "round", 5) == 0)) ||
             (node->as.call.name_length == 4 &&
              memcmp(node->as.call.name, "ceil", 4) == 0) ||
             (node->as.call.name_length == 2 &&
              memcmp(node->as.call.name, "pi", 2) == 0))
             return "Float";
        if (node->as.call.name_length == 10 &&
             memcmp(node->as.call.name, "random_int", 10) == 0)
             return "Int";
        if (node->as.call.name_length == 11 &&
             memcmp(node->as.call.name, "random_seed", 11) == 0)
             return "Null";
        return NULL;
    case AST_INDEX:
    case AST_MEMBER:
    case AST_MAP:
    case AST_SET:
        return NULL;
    }
    return NULL;
}

static bool check_argument_type(Resolver *resolver, const AstNode *argument,
                               const char *expected, size_t scope_id)
{
    const char *actual;
    if (!expected)
        return true;
    actual = infer_expression_type(resolver, argument, scope_id);
    if (!actual || strcmp(actual, expected) == 0)
        return true;
    char message[200];
    (void)snprintf(message, sizeof(message),
                   "Action argument has type %s; parameter requires %s.",
                   actual, expected);
    fail(resolver, argument->span, message);
    return false;
}

static bool resolve_expression(Resolver *resolver, const AstNode *node,
                               size_t scope_id)
{
    if (!node || resolver->failed)
        return !resolver->failed;
    switch (node->kind)
    {
    case AST_NAME:
        return add_reference_to_symbol(resolver, node->as.name.name,
                                       node->span, scope_id,
                                       RESOLVER_VARIABLE);
    case AST_GROUP:
        return resolve_expression(resolver, node->as.grouped, scope_id);
    case AST_UNARY:
        return resolve_expression(resolver, node->as.unary.operand, scope_id);
    case AST_BINARY:
        return resolve_expression(resolver, node->as.binary.left, scope_id) &&
               resolve_expression(resolver, node->as.binary.right, scope_id);
    case AST_INDEX:
        return resolve_expression(resolver, node->as.index.collection, scope_id) &&
               resolve_expression(resolver, node->as.index.index, scope_id);
    case AST_MEMBER:
        return resolve_expression(resolver, node->as.member.object, scope_id);
    case AST_CALL:
        for (size_t i = 0; i < node->as.call.argument_count; i++)
            if (!resolve_expression(resolver, node->as.call.arguments[i],
                                    scope_id))
                return false;
        return true;
    case AST_MAP:
        for (size_t i = 0; i < node->as.map.count; i++)
            if (!resolve_expression(resolver, node->as.map.entries[i].key,
                                    scope_id) ||
                !resolve_expression(resolver, node->as.map.entries[i].value,
                                    scope_id))
                return false;
        return true;
    case AST_SET:
        for (size_t i = 0; i < node->as.set.count; i++)
            if (!resolve_expression(resolver, node->as.set.items[i], scope_id))
                return false;
        return true;
    case AST_INTEGER:
    case AST_STRING:
    case AST_BOOLEAN:
    case AST_NULL:
        return true;
    }
    fail(resolver, node->span, "Unsupported expression node in name resolver.");
    return false;
}

static bool predeclare_actions(Resolver *resolver,
                               ProgramAstStatement *const *statements,
                               size_t count, size_t scope_id, bool top_level)
{
    for (size_t i = 0; i < count && !resolver->failed; i++)
    {
        ProgramAstStatement *statement = statements[i];
        if (statement->kind == PROGRAM_AST_ACTION)
        {
            if (!top_level)
            {
                fail(resolver, statement->span,
                     "Action definitions are only allowed at program scope.");
                return false;
            }
            if (!add_symbol(resolver, statement->name, statement->return_type,
                            RESOLVER_ACTION, scope_id,
                            statement->parameter_count, statement,
                            statement->span, NULL))
                return false;
        }
        else if (statement->kind == PROGRAM_AST_WHEN ||
                 statement->kind == PROGRAM_AST_WHILE ||
                 statement->kind == PROGRAM_AST_REPEAT)
        {
            if (!predeclare_actions(resolver, statement->children,
                                    statement->child_count, scope_id, false) ||
                (statement->kind == PROGRAM_AST_WHEN &&
                 !predeclare_actions(resolver, statement->otherwise_children,
                                    statement->otherwise_child_count,
                                    scope_id, false)))
                return false;
        }
    }
    return !resolver->failed;
}

static bool resolve_statement(Resolver *resolver,
                              ProgramAstStatement *statement,
                              size_t scope_id, bool in_action)
{
    switch (statement->kind)
    {
    case PROGRAM_AST_ASSIGNMENT:
    case PROGRAM_AST_TYPED_DECLARATION:
        return resolve_expression(resolver, statement->expression, scope_id) &&
               add_reference_to_symbol(resolver, statement->name,
                                       statement->span, scope_id,
                                       RESOLVER_VARIABLE);
    case PROGRAM_AST_DISPLAY:
        for (size_t i = 0; i < statement->expression_count; i++)
            if (!resolve_expression(resolver, statement->expressions[i],
                                    scope_id))
                return false;
        return true;
    case PROGRAM_AST_LIST_CREATE:
    case PROGRAM_AST_PILE_CREATE:
        return true;
    case PROGRAM_AST_COLLECTION_PUSH:
        return add_reference_to_symbol(resolver, statement->name,
                                       statement->span, scope_id,
                                       RESOLVER_VARIABLE) &&
               resolve_expression(resolver, statement->expression, scope_id);
    case PROGRAM_AST_COLLECTION_POP:
        return add_reference_to_symbol(resolver, statement->name,
                                       statement->span, scope_id,
                                       RESOLVER_VARIABLE);
    case PROGRAM_AST_COLLECTION_LEN:
        return add_reference_to_symbol(resolver, statement->name,
                                       statement->span, scope_id,
                                       RESOLVER_VARIABLE) &&
               add_reference_to_symbol(resolver, statement->target_name,
                                       statement->span, scope_id,
                                       RESOLVER_VARIABLE);
    case PROGRAM_AST_INPUT:
    case PROGRAM_AST_COLLECTION_LOAD:
    case PROGRAM_AST_FILE_READ:
    case PROGRAM_AST_FILE_DISPLAY:
        return true;
    case PROGRAM_AST_COLLECTION_SAVE:
    case PROGRAM_AST_FILE_WRITE:
        return add_reference_to_symbol(resolver, statement->name,
                                       statement->span, scope_id,
                                       RESOLVER_VARIABLE);
    case PROGRAM_AST_ACTION_CALL:
    {
        size_t function_index;
        if (!resolver_lookup(resolver->resolution, 0, statement->name,
                             RESOLVER_ACTION, &function_index))
        {
            fail_name(resolver, statement->span, "Unresolved action",
                      statement->name);
            return false;
        }
        ResolverSymbol *function =
            &resolver->resolution->symbols[function_index];
        if (function->parameter_count != statement->expression_count)
        {
            fail_name(resolver, statement->span,
                      "Action called with incorrect arity:",
                      statement->name);
            return false;
        }
        if (!add_reference(resolver, statement->name, statement->span,
                           function_index))
            return false;
        for (size_t i = 0; i < statement->expression_count; i++)
        {
            if (!check_argument_type(
                    resolver, statement->expressions[i],
                    function->parameter_types[i], scope_id))
                return false;
            if (!resolve_expression(resolver, statement->expressions[i],
                                    scope_id))
                return false;
        }
        return true;
    }
    case PROGRAM_AST_RETURN:
        if (!in_action)
        {
            fail(resolver, statement->span,
                 "`empty` return is only valid inside an action.");
            return false;
        }
        if (resolver->active_return_type &&
            strcmp(resolver->active_return_type, "Null") != 0)
        {
            char message[200];
            (void)snprintf(
                message, sizeof(message),
                "`empty` returns Null, but the action declares return type %s.",
                resolver->active_return_type);
            fail(resolver, statement->span, message);
            return false;
        }
        return true;
    case PROGRAM_AST_THROW:
        return resolve_expression(resolver, statement->expression, scope_id);
    case PROGRAM_AST_WHEN:
    case PROGRAM_AST_WHILE:
    case PROGRAM_AST_REPEAT:
        return resolve_expression(resolver, statement->expression, scope_id) &&
               resolve_sequence(resolver, statement->children,
                                statement->child_count, scope_id, in_action) &&
               (statement->kind != PROGRAM_AST_WHEN ||
                resolve_sequence(resolver, statement->otherwise_children,
                                 statement->otherwise_child_count, scope_id,
                                 in_action));
    case PROGRAM_AST_ACTION:
    {
        size_t action_scope;
        if (!append_scope(resolver, 0, true, &action_scope))
            return false;
        for (size_t i = 0; i < statement->parameter_count; i++)
            if (!add_symbol(resolver, statement->parameters[i].name,
                            statement->parameters[i].type_text,
                            RESOLVER_VARIABLE, action_scope, 0,
                            NULL, statement->parameters[i].span, NULL))
                return false;
        if (!collect_variables(resolver, statement->children,
                               statement->child_count, action_scope))
            return false;
        const char *previous_return_type = resolver->active_return_type;
        resolver->active_return_type = statement->return_type;
        bool success = resolve_sequence(resolver, statement->children,
                                        statement->child_count, action_scope,
                                        true);
        resolver->active_return_type = previous_return_type;
        return success;
    }
    }
    fail(resolver, statement->span, "Unsupported statement in name resolver.");
    return false;
}

static bool resolve_sequence(Resolver *resolver,
                             ProgramAstStatement *const *statements,
                             size_t count, size_t scope_id,
                             bool allow_actions)
{
    for (size_t i = 0; i < count && !resolver->failed; i++)
    {
        if (!allow_actions && statements[i]->kind == PROGRAM_AST_ACTION)
        {
            fail(resolver, statements[i]->span,
                 "Action definitions are only allowed at program scope.");
            return false;
        }
        if (!resolve_statement(resolver, statements[i], scope_id,
                               allow_actions &&
                                   statements[i]->kind == PROGRAM_AST_ACTION
                                   ? false
                                   : (scope_id != 0)))
            return false;
    }
    return !resolver->failed;
}

bool resolver_resolve_program(const ProgramAst *program,
                              ProgramResolution *resolution,
                              ResolverError *error)
{
    Resolver resolver;
    size_t root_scope;
    if (resolution)
        memset(resolution, 0, sizeof(*resolution));
    if (error)
        memset(error, 0, sizeof(*error));
    if (!program || !resolution)
    {
        if (error)
            (void)snprintf(error->message, sizeof(error->message),
                           "Program and resolution output are required.");
        return false;
    }
    resolver.program = program;
    resolver.resolution = resolution;
    resolver.error = error;
    resolver.active_return_type = NULL;
    resolver.failed = false;
    if (!append_scope(&resolver, 0, false, &root_scope) ||
        !predeclare_actions(&resolver, program->statements,
                            program->statement_count, root_scope, true) ||
        !collect_variables(&resolver, program->statements,
                           program->statement_count, root_scope) ||
        !resolve_sequence(&resolver, program->statements,
                          program->statement_count, root_scope, true))
    {
        if (!resolver.failed && error)
            (void)snprintf(error->message, sizeof(error->message),
                           "Unable to resolve program names.");
        resolver_resolution_free(resolution);
        return false;
    }
    return true;
}
