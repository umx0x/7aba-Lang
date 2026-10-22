#ifndef AST_H
#define AST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    size_t line;
    size_t column;
    size_t end_line;
    size_t end_column;
} AstSpan;

typedef enum
{
    AST_INTEGER,
    AST_STRING,
    AST_BOOLEAN,
    AST_NULL,
    AST_NAME,
    AST_GROUP,
    AST_UNARY,
    AST_BINARY,
    AST_INDEX,
    AST_MEMBER,
    AST_CALL,
    AST_MAP,
    AST_SET
} AstNodeKind;

typedef enum
{
    AST_OP_NEGATE,
    AST_OP_ADD,
    AST_OP_SUBTRACT,
    AST_OP_MULTIPLY,
    AST_OP_DIVIDE,
    AST_OP_EQUAL,
    AST_OP_NOT_EQUAL,
    AST_OP_LESS,
    AST_OP_LESS_EQUAL,
    AST_OP_GREATER,
    AST_OP_GREATER_EQUAL,
    AST_OP_IN
} AstOperator;

typedef struct AstNode AstNode;

typedef struct
{
    AstNode *key;
    AstNode *value;
} AstMapEntry;

struct AstNode
{
    AstNodeKind kind;
    AstSpan span;
    union
    {
        int integer;
        bool boolean;
        AstNode *grouped;
        struct
        {
            char *bytes;
            size_t length;
        } string;
        struct
        {
            char *name;
            size_t length;
        } name;
        struct
        {
            AstOperator operation;
            AstNode *operand;
        } unary;
        struct
        {
            AstOperator operation;
            AstNode *left;
            AstNode *right;
        } binary;
        struct
        {
            AstNode *collection;
            AstNode *index;
        } index;
        struct
        {
            AstNode *object;
            char *name;
            size_t length;
        } member;
        struct
        {
            char *name;
            size_t name_length;
            AstNode **arguments;
            size_t argument_count;
        } call;
        struct
        {
            AstMapEntry *entries;
            size_t count;
        } map;
        struct
        {
            AstNode **items;
            size_t count;
        } set;
    } as;
};

typedef struct
{
    AstSpan span;
    char message[160];
} AstError;

/* Parses one production expression. The optional block-opening '{', trailing
 * whitespace, and a trailing # comment are accepted. Returned nodes are owned
 * by the caller and freed with ast_free(). */
bool ast_parse_expression(const char *source, AstNode **result, AstError *error);
bool ast_parse_expression_mode(const char *source, bool allow_block_opener,
                              AstNode **result, AstError *error);
void ast_free(AstNode *node);

#endif
