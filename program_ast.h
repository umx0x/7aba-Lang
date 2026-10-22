#ifndef PROGRAM_AST_H
#define PROGRAM_AST_H

#include "ast.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    PROGRAM_AST_ASSIGNMENT,
    PROGRAM_AST_TYPED_DECLARATION,
    PROGRAM_AST_LIST_CREATE,
    PROGRAM_AST_PILE_CREATE,
    PROGRAM_AST_COLLECTION_PUSH,
    PROGRAM_AST_COLLECTION_POP,
    PROGRAM_AST_COLLECTION_LEN,
    PROGRAM_AST_INPUT,
    PROGRAM_AST_COLLECTION_SAVE,
    PROGRAM_AST_COLLECTION_LOAD,
    PROGRAM_AST_FILE_DISPLAY,
    PROGRAM_AST_FILE_READ,
    PROGRAM_AST_FILE_WRITE,
    PROGRAM_AST_DISPLAY,
    PROGRAM_AST_ACTION_CALL,
    PROGRAM_AST_RETURN,
    PROGRAM_AST_THROW,
    PROGRAM_AST_WHEN,
    PROGRAM_AST_WHILE,
    PROGRAM_AST_REPEAT,
    PROGRAM_AST_ACTION
} ProgramAstKind;

typedef struct
{
    char *name;
    char *type_text;
    AstSpan span;
} ProgramAstParameter;

typedef struct ProgramAstStatement ProgramAstStatement;

struct ProgramAstStatement
{
    ProgramAstKind kind;
    AstSpan span;
    /* Assignment/declaration target, collection, action, or called action. */
    char *name;
    /* Destination variable for collection length operations. */
    char *target_name;
    /* Path for file and persistence statements. */
    char *path_text;
    /* Input destinations. */
    char **names;
    size_t name_count;
    /* File write append mode; false means overwrite. */
    bool append_mode;
    /* Declaration annotation. */
    char *type_text;
    /* Optional action return annotation. */
    char *return_type;
    /* Initializer, control condition/count, or thrown expression. */
    AstNode *expression;
    /* Display items or action-call arguments. */
    AstNode **expressions;
    size_t expression_count;
    ProgramAstParameter *parameters;
    size_t parameter_count;
    /* Block body and the optional `when` otherwise body. */
    ProgramAstStatement **children;
    size_t child_count;
    ProgramAstStatement **otherwise_children;
    size_t otherwise_child_count;
};

typedef struct
{
    ProgramAstStatement **statements;
    size_t statement_count;
} ProgramAst;

typedef struct
{
    AstSpan span;
    char message[200];
} ProgramAstError;

/* Parses the supported line-oriented program grammar. The returned tree and
 * all strings and expression nodes are owned by the caller. */
bool program_ast_parse(const char *source, ProgramAst **result,
                       ProgramAstError *error);
void program_ast_free(ProgramAst *program);

#endif
