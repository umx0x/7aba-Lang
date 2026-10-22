#ifndef RESOLVER_H
#define RESOLVER_H

#include "program_ast.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    RESOLVER_VARIABLE,
    RESOLVER_ACTION
} ResolverSymbolKind;

typedef struct
{
    char *name;
    /* Action return annotation; variable annotation for variable symbols. */
    char *type_name;
    ResolverSymbolKind kind;
    size_t scope_id;
    size_t parameter_count;
    /* NULL entries represent unannotated parameters. */
    char **parameter_types;
    AstSpan declaration_span;
} ResolverSymbol;

typedef struct
{
    size_t id;
    size_t parent_id;
    bool has_parent;
} ResolverScope;

typedef struct
{
    char *name;
    AstSpan span;
    size_t symbol_index;
} ResolverReference;

typedef struct
{
    ResolverSymbol *symbols;
    size_t symbol_count;
    ResolverScope *scopes;
    size_t scope_count;
    ResolverReference *references;
    size_t reference_count;
} ProgramResolution;

typedef struct
{
    AstSpan span;
    char message[200];
} ResolverError;

/* Resolves declarations and checks statically known action argument tags.
 * `empty` is treated as a Null return and checked against annotated returns.
 * Unknown argument types remain dynamic. Top-level control-flow blocks share
 * the top-level scope; each action has one child scope. */
bool resolver_resolve_program(const ProgramAst *program,
                              ProgramResolution *resolution,
                              ResolverError *error);
void resolver_resolution_free(ProgramResolution *resolution);

/* Returns the nearest visible symbol index, or false when it is not present. */
bool resolver_lookup(const ProgramResolution *resolution, size_t scope_id,
                     const char *name, ResolverSymbolKind kind,
                     size_t *symbol_index);

#endif
