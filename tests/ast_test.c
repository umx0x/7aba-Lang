#include "../ast.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static AstNode *parse(const char *source)
{
    AstNode *node = NULL;
    AstError error;
    if (!ast_parse_expression(source, &node, &error))
    {
        fprintf(stderr, "parse failed: %s at %zu:%zu\n", error.message,
                error.span.line, error.span.column);
        assert(0);
    }
    return node;
}

static void test_precedence_and_literals(void)
{
    AstNode *node = parse("1 + 2 * -3 == 4");
    assert(node->kind == AST_BINARY && node->as.binary.operation == AST_OP_EQUAL);
    assert(node->as.binary.left->kind == AST_BINARY);
    assert(node->as.binary.left->as.binary.operation == AST_OP_ADD);
    assert(node->as.binary.left->as.binary.right->as.binary.operation ==
           AST_OP_MULTIPLY);
    assert(node->as.binary.left->as.binary.right->as.binary.right->kind ==
           AST_UNARY);
    ast_free(node);

    node = parse("true in {\"a\\n\", false, null}");
    assert(node->as.binary.operation == AST_OP_IN);
    assert(node->as.binary.right->kind == AST_SET);
    assert(node->as.binary.right->as.set.count == 3);
    assert(node->as.binary.right->as.set.items[0]->as.string.length == 2);
    assert(node->as.binary.right->as.set.items[0]->as.string.bytes[1] == '\n');
    ast_free(node);
}

static void test_collections_calls_and_postfix(void)
{
    AstNode *node = parse("len(data.items)");
    assert(node->kind == AST_CALL);
    assert(node->as.call.argument_count == 1);
    assert(node->as.call.arguments[0]->kind == AST_MEMBER);
    assert(node->as.call.arguments[0]->as.member.object->kind == AST_NAME);
    ast_free(node);
    node = parse("data[0]");
    assert(node->kind == AST_INDEX);
    ast_free(node);

    node = parse("{\"x\": 1, \"y\": 2,}");
    assert(node->kind == AST_MAP && node->as.map.count == 2);
    ast_free(node);
    node = parse("{}");
    assert(node->kind == AST_MAP && node->as.map.count == 0);
    ast_free(node);
    node = parse("set{}");
    assert(node->kind == AST_SET && node->as.set.count == 0);
    ast_free(node);
    node = parse("set{1, 2,}");
    assert(node->kind == AST_SET && node->as.set.count == 2);
    ast_free(node);

    node = parse("value[0] { # block opener");
    assert(node->kind == AST_INDEX);
    ast_free(node);
}

static void test_spans_and_errors(void)
{
    AstNode *node = parse("\n  (12 + 3)");
    assert(node->kind == AST_GROUP);
    assert(node->span.line == 2 && node->span.column == 3);
    assert(node->span.end_line == 2 && node->span.end_column == 11);
    assert(node->as.grouped->span.column == 4);
    assert(node->as.grouped->span.end_column == 10);
    ast_free(node);

    const char *bad[] = {
        "1 +", "\"unfinished", "{1:}", "len()", "len(1,)", "name[1", "a.",
        "a[0][1]", "a.b.c", "\"text\"[0]", "(a)[0]", "1 == 2 == 3",
        "set{1: 2}", "unknown()"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        AstNode *failed_node = NULL;
        AstError error;
        bool parsed = ast_parse_expression(bad[i], &failed_node, &error);
        if (parsed)
            fprintf(stderr, "unexpectedly accepted malformed expression: %s\n", bad[i]);
        assert(!parsed);
        assert(failed_node == NULL);
        assert(error.message[0] != '\0');
        assert(error.span.line >= 1 && error.span.column >= 1);
    }
}

int main(void)
{
    test_precedence_and_literals();
    test_collections_calls_and_postfix();
    test_spans_and_errors();
    puts("AST expression parser tests passed");
    return 0;
}
