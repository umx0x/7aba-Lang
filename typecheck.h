#ifndef TYPECHECK_H
#define TYPECHECK_H

#include "ast.h"

#include <stddef.h>

typedef enum
{
    TYPECHECK_INT,
    TYPECHECK_FLOAT,
    TYPECHECK_BOOL,
    TYPECHECK_STRING,
    TYPECHECK_NULL,
    TYPECHECK_ARRAY,
    TYPECHECK_MAP,
    TYPECHECK_SET,
    TYPECHECK_UNKNOWN_TYPE
} TypecheckType;

typedef enum
{
    TYPECHECK_OK,
    TYPECHECK_UNKNOWN,
    TYPECHECK_ERROR
} TypecheckStatus;

typedef struct
{
    TypecheckStatus status;
    TypecheckType type;
    AstSpan span;
    char message[160];
} TypecheckResult;

/*
 * Analyze an expression AST without changing it. Unknown names and AST forms
 * whose types cannot be established produce TYPECHECK_UNKNOWN, never success.
 * Floating-point values are currently inferred from math builtin calls; the
 * current AstNode API has no floating-point literal node.
 *
 * Collection types are shallow: element/key/value types remain unknown.
 * The current AST has map and set literals, but no array literal node.
 * This is an expression-analysis foundation, not declaration annotation or
 * environment/type-scope support.
 */
TypecheckResult typecheck_expression(const AstNode *node);

const char *typecheck_type_name(TypecheckType type);

#endif
