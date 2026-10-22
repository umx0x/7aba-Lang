#include "../ast.h"
#include "../typecheck.h"

#include <assert.h>
#include <limits.h>
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

static TypecheckResult check(const char *source)
{
    AstNode *node = parse(source);
    TypecheckResult result = typecheck_expression(node);
    ast_free(node);
    return result;
}

static void expect_type(const char *source, TypecheckType type)
{
    TypecheckResult result = check(source);
    assert(result.status == TYPECHECK_OK);
    assert(result.type == type);
}

static void expect_unknown(const char *source)
{
    TypecheckResult result = check(source);
    assert(result.status == TYPECHECK_UNKNOWN);
    assert(result.type == TYPECHECK_UNKNOWN_TYPE);
    assert(result.message[0] != '\0');
}

static void expect_error(const char *source)
{
    TypecheckResult result = check(source);
    if (result.status != TYPECHECK_ERROR)
        fprintf(stderr, "expected error for '%s', got status %d (%s)\n",
                source, (int)result.status, result.message);
    assert(result.status == TYPECHECK_ERROR);
    assert(result.type == TYPECHECK_UNKNOWN_TYPE);
    assert(result.message[0] != '\0');
    assert(result.span.line != 0);
}

static void test_literals_and_operators(void)
{
    expect_type("12", TYPECHECK_INT);
    expect_type("true", TYPECHECK_BOOL);
    expect_type("\"text\"", TYPECHECK_STRING);
    expect_type("null", TYPECHECK_NULL);
    expect_type("-3 * 4 + 5", TYPECHECK_INT);
    expect_type("-2 * -3", TYPECHECK_INT);
    expect_type("\"a\" + \"b\"", TYPECHECK_STRING);
    expect_type("\"a\" + 1", TYPECHECK_STRING);
    expect_type("1 + \"a\"", TYPECHECK_STRING);
    expect_type("3 < 4", TYPECHECK_BOOL);
    expect_type("\"a\" <= \"b\"", TYPECHECK_BOOL);
    expect_type("null == 1", TYPECHECK_BOOL);
    expect_type("\"x\" in \"text\"", TYPECHECK_BOOL);
    expect_type("1 in set{1, 2}", TYPECHECK_BOOL);
    expect_type("\"key\" in {\"key\": 1}", TYPECHECK_BOOL);
}

static void test_collections_and_builtins(void)
{
    expect_type("{}", TYPECHECK_MAP);
    expect_type("{\"a\": 1, \"b\": true}", TYPECHECK_MAP);
    expect_type("set{}", TYPECHECK_SET);
    expect_type("set{1, \"mixed\"}", TYPECHECK_SET);
    expect_type("abs(-1)", TYPECHECK_INT);
    expect_type("min(1, 2)", TYPECHECK_INT);
    expect_type("max(1, 2)", TYPECHECK_INT);
    expect_type("len(\"text\")", TYPECHECK_INT);
    expect_type("len(set{1})", TYPECHECK_INT);
    expect_type("lower(\"TEXT\")", TYPECHECK_STRING);
    expect_type("upper(\"text\")", TYPECHECK_STRING);
    expect_type("substr(\"text\", 1, 2)", TYPECHECK_STRING);
    expect_type("trim(\" text \")", TYPECHECK_STRING);
    expect_type("starts_with(\"hello\", \"he\")", TYPECHECK_BOOL);
    expect_type("ends_with(\"hello\", \"lo\")", TYPECHECK_BOOL);
    expect_type("contains(\"hello\", \"ell\")", TYPECHECK_BOOL);
    expect_type("index_of(\"hello\", \"ell\")", TYPECHECK_INT);
    expect_type("count(\"banana\", \"an\")", TYPECHECK_INT);
    expect_type("replace(\"hello\", \"ell\", \"ipp\")", TYPECHECK_STRING);
    expect_type("repeat(\"ha\", 3)", TYPECHECK_STRING);
    expect_type("sqrt(9)", TYPECHECK_FLOAT);
    expect_type("sin(1)", TYPECHECK_FLOAT);
    expect_type("floor(1)", TYPECHECK_FLOAT);
    expect_type("pi()", TYPECHECK_FLOAT);
    expect_type("sqrt(9) + 1", TYPECHECK_FLOAT);
    expect_type("-cos(1)", TYPECHECK_FLOAT);
    expect_unknown("min(sqrt(9), 4)");
    expect_type("random_int(1, 10)", TYPECHECK_INT);
    expect_type("random_seed(123)", TYPECHECK_NULL);
}

static void test_unknowns_and_invalid_types(void)
{
    expect_unknown("name");
    expect_unknown("name + 1");
    expect_unknown("len(name)");
    expect_unknown("name.field");
    expect_unknown("name[0]");
    expect_error("true + 1");
    expect_error("\"x\" - \"y\"");
    expect_error("1 < \"x\"");
    expect_error("set{} in set{1}");
    expect_type("\"x\" in set{}", TYPECHECK_BOOL);
    expect_error("len(true)");
    expect_error("abs(\"x\")");
    expect_error("substr(\"x\", true, 2)");
    expect_error("trim(42)");
    expect_error("starts_with(\"x\", 1)");
    expect_error("contains(\"x\", 1)");
    expect_error("index_of(1, \"x\")");
    expect_error("count(\"x\", true)");
    expect_error("replace(\"x\", 1, \"y\")");
    expect_error("repeat(\"x\", true)");
    expect_error("sqrt(\"x\")");
    expect_error("random_int(1, true)");
    expect_error("random_seed(\"seed\")");
}

static void test_overflow_and_division_checks(void)
{
    char expression[96];
    (void)snprintf(expression, sizeof(expression), "%d + 1", INT_MAX);
    expect_error(expression);
    (void)snprintf(expression, sizeof(expression), "%d * 2", INT_MAX);
    expect_error(expression);
    (void)snprintf(expression, sizeof(expression), "-%d * -2", INT_MAX);
    expect_error(expression);
    expect_error("1 / 0");

    {
        AstNode *node = parse("0 - 1");
        TypecheckResult result;
        node->as.binary.left->as.integer = INT_MIN;
        result = typecheck_expression(node);
        assert(result.status == TYPECHECK_ERROR);
        ast_free(node);
    }
    {
        AstNode *node = parse("-1");
        TypecheckResult result;
        node->as.unary.operand->as.integer = INT_MIN;
        result = typecheck_expression(node);
        assert(result.status == TYPECHECK_ERROR);
        ast_free(node);
    }
    {
        AstNode *node = parse("abs(1)");
        TypecheckResult result;
        node->as.call.arguments[0]->as.integer = INT_MIN;
        result = typecheck_expression(node);
        assert(result.status == TYPECHECK_ERROR);
        ast_free(node);
    }
}

static void test_arity_defenses_and_null_node(void)
{
    AstNode *node = parse("abs(1)");
    node->as.call.argument_count = 2;
    {
        TypecheckResult result = typecheck_expression(node);
        assert(result.status == TYPECHECK_ERROR);
        assert(strstr(result.message, "arity") != NULL);
    }
    node->as.call.argument_count = 1;
    ast_free(node);

    {
        TypecheckResult result = typecheck_expression(NULL);
        assert(result.status == TYPECHECK_ERROR);
    }
}

int main(void)
{
    test_literals_and_operators();
    test_collections_and_builtins();
    test_unknowns_and_invalid_types();
    test_overflow_and_division_checks();
    test_arity_defenses_and_null_node();
    puts("Expression type-analysis tests passed");
    return 0;
}
