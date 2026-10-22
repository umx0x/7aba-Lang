#include "../program_ast.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ProgramAst *parse(const char *source)
{
    ProgramAst *program = NULL;
    ProgramAstError error;
    if (!program_ast_parse(source, &program, &error))
    {
        fprintf(stderr, "parse failed: %s at %zu:%zu\n", error.message,
                error.span.line, error.span.column);
        assert(0);
    }
    return program;
}

static void expect_failure(const char *source)
{
    ProgramAst *program = (ProgramAst *)1;
    ProgramAstError error;
    assert(!program_ast_parse(source, &program, &error));
    assert(program == NULL);
    assert(error.message[0] != '\0');
    assert(error.span.line >= 1);
    assert(error.span.column >= 1);
    program_ast_free(program);
}

static void test_existing_lists_files_fixture(void)
{
    FILE *file = fopen("tests/lists_files.7ba", "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long file_length = ftell(file);
    assert(file_length >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    char *source = (char *)malloc((size_t)file_length + 1);
    assert(source != NULL);
    assert(fread(source, 1, (size_t)file_length, file) ==
           (size_t)file_length);
    assert(fclose(file) == 0);
    source[file_length] = '\0';
    ProgramAst *program = parse(source);
    assert(program->statement_count > 0);
    program_ast_free(program);
    free(source);
}

static void test_statements_and_spans(void)
{
    ProgramAst *program = parse(
        "value = 2 + 3\n"
        "count: Int = value\n"
        "ratio: Float = existing\n"
        "ready: Bool = true\n"
        "label: String = \"brace } # text\"\n"
        "empty_value: Null = null # }\n"
        "display \"value: {#}\" value\n"
        "update(value, 4)\n"
        "throw value\n"
        "empty\n");
    assert(program->statement_count == 10);
    assert(program->statements[0]->kind == PROGRAM_AST_ASSIGNMENT);
    assert(program->statements[0]->expression->kind == AST_BINARY);
    assert(program->statements[1]->kind == PROGRAM_AST_TYPED_DECLARATION);
    assert(strcmp(program->statements[1]->type_text, "Int") == 0);
    assert(program->statements[2]->kind == PROGRAM_AST_TYPED_DECLARATION);
    assert(strcmp(program->statements[2]->type_text, "Float") == 0);
    assert(program->statements[3]->kind == PROGRAM_AST_TYPED_DECLARATION);
    assert(program->statements[4]->kind == PROGRAM_AST_TYPED_DECLARATION);
    assert(program->statements[5]->kind == PROGRAM_AST_TYPED_DECLARATION);
    assert(program->statements[6]->kind == PROGRAM_AST_DISPLAY);
    assert(program->statements[6]->expression_count == 2);
    assert(program->statements[6]->expressions[0]->kind == AST_STRING);
    assert(program->statements[6]->expressions[1]->kind == AST_NAME);
    assert(program->statements[7]->kind == PROGRAM_AST_ACTION_CALL);
    assert(strcmp(program->statements[7]->name, "update") == 0);
    assert(program->statements[7]->expression_count == 2);
    assert(program->statements[8]->kind == PROGRAM_AST_THROW);
    assert(program->statements[9]->kind == PROGRAM_AST_RETURN);
    assert(program->statements[0]->span.line == 1);
    assert(program->statements[0]->span.column == 1);
    assert(program->statements[0]->span.end_line == 1);
    assert(program->statements[0]->expression->span.column == 9);
    assert(program->statements[0]->expression->span.end_column == 14);
    program_ast_free(program);
    program_ast_free(NULL);
}

static void test_collection_commands(void)
{
    ProgramAst *program = parse(
        "list values\n"
        "pile history\n"
        "push values 2 + 3\n"
        "len values count\n"
        "pop history\n");
    assert(program->statement_count == 5);
    assert(program->statements[0]->kind == PROGRAM_AST_LIST_CREATE);
    assert(strcmp(program->statements[0]->name, "values") == 0);
    assert(program->statements[1]->kind == PROGRAM_AST_PILE_CREATE);
    assert(program->statements[2]->kind == PROGRAM_AST_COLLECTION_PUSH);
    assert(program->statements[2]->expression->kind == AST_BINARY);
    assert(program->statements[3]->kind == PROGRAM_AST_COLLECTION_LEN);
    assert(strcmp(program->statements[3]->name, "values") == 0);
    assert(strcmp(program->statements[3]->target_name, "count") == 0);
    assert(program->statements[4]->kind == PROGRAM_AST_COLLECTION_POP);
    assert(program->statements[0]->span.line == 1);
    assert(program->statements[3]->span.end_column == 17);
    program_ast_free(program);
    expect_failure("push values\n");
    expect_failure("len values\n");
    expect_failure("list values unexpected\n");
}

static void test_input_and_file_commands(void)
{
    ProgramAst *program = parse(
        "input first second\n"
        "save values \"values.dat\"\n"
        "load restored \"values.dat\"\n"
        "file_display \"notes.txt\"\n"
        "file_read \"notes.txt\" content\n"
        "file_write \"notes.txt\" content overwrite\n"
        "file_write \"notes.txt\" content append # trailing comment\n");
    assert(program->statement_count == 7);
    assert(program->statements[0]->kind == PROGRAM_AST_INPUT);
    assert(program->statements[0]->name_count == 2);
    assert(strcmp(program->statements[0]->names[1], "second") == 0);
    assert(program->statements[1]->kind == PROGRAM_AST_COLLECTION_SAVE);
    assert(strcmp(program->statements[1]->path_text, "values.dat") == 0);
    assert(program->statements[2]->kind == PROGRAM_AST_COLLECTION_LOAD);
    assert(program->statements[3]->kind == PROGRAM_AST_FILE_DISPLAY);
    assert(program->statements[4]->kind == PROGRAM_AST_FILE_READ);
    assert(strcmp(program->statements[4]->target_name, "content") == 0);
    assert(program->statements[5]->kind == PROGRAM_AST_FILE_WRITE);
    assert(!program->statements[5]->append_mode);
    assert(program->statements[6]->append_mode);
    assert(program->statements[6]->span.line == 7);
    program_ast_free(program);

    expect_failure("input\n");
    expect_failure("save values\n");
    expect_failure("file_read \"x.txt\"\n");
    expect_failure("file_write \"x.txt\" data invalid\n");
}

static void test_nested_control_and_action_annotations(void)
{
    ProgramAst *program = parse(
        "action calculate(x: Int, y, title: String) -> Float {\n"
        "    when x > 0 { # open\n"
        "        while y < x {\n"
        "            repeat 2 {\n"
        "                y = y + 1\n"
        "            }\n"
        "        }\n"
        "    } otherwise {\n"
        "        throw 0\n"
        "    }\n"
        "    empty\n"
        "}\n"
        "when true {\n"
        "    display \"yes\"\n"
        "}\n"
        "otherwise {\n"
        "    display \"no\"\n"
        "}\n");
    assert(program->statement_count == 2);
    ProgramAstStatement *action = program->statements[0];
    assert(action->kind == PROGRAM_AST_ACTION);
    assert(strcmp(action->name, "calculate") == 0);
    assert(action->parameter_count == 3);
    assert(strcmp(action->parameters[0].name, "x") == 0);
    assert(strcmp(action->parameters[0].type_text, "Int") == 0);
    assert(action->parameters[1].type_text == NULL);
    assert(strcmp(action->parameters[2].type_text, "String") == 0);
    assert(strcmp(action->return_type, "Float") == 0);
    assert(action->child_count == 2);
    ProgramAstStatement *when = action->children[0];
    assert(when->kind == PROGRAM_AST_WHEN);
    assert(when->child_count == 1);
    assert(when->otherwise_child_count == 1);
    assert(when->children[0]->kind == PROGRAM_AST_WHILE);
    assert(when->children[0]->children[0]->kind == PROGRAM_AST_REPEAT);
    assert(when->children[0]->children[0]->children[0]->kind ==
           PROGRAM_AST_ASSIGNMENT);
    assert(when->otherwise_children[0]->kind == PROGRAM_AST_THROW);
    assert(action->children[1]->kind == PROGRAM_AST_RETURN);
    assert(action->span.end_line == 12);
    assert(program->statements[1]->kind == PROGRAM_AST_WHEN);
    assert(program->statements[1]->otherwise_child_count == 1);
    program_ast_free(program);

    program = parse("action noop() {\n}\n");
    assert(program->statement_count == 1);
    assert(program->statements[0]->kind == PROGRAM_AST_ACTION);
    assert(program->statements[0]->parameter_count == 0);
    assert(program->statements[0]->return_type == NULL);
    program_ast_free(program);

    program = parse(
        "when true {\n"
        "  display \"a { # }\"\n"
        "}\n"
        "otherwise {\n"
        "  display \"b\"\n"
        "}\n");
    assert(program->statement_count == 1);
    assert(program->statements[0]->otherwise_child_count == 1);
    program_ast_free(program);
}

static void test_malformed_syntax_and_cleanup(void)
{
    const char *bad[] = {
        "value =",
        "value: List = null",
        "value: Int",
        "display",
        "display value,",
        "throw # missing",
        "empty value",
        "unknown syntax",
        "value += 1",
        "action f(value: Unknown) {\n}\n",
        "action f(value,) {\n}\n",
        "action f(value) -> Any {\n}\n",
        "when true {\n value = 1\n",
        "}\n",
        "otherwise {\n}\n",
        "when true {\n}\n} otherwise {\n}\n",
        "value = \"unterminated\n",
        "value = 1\n}\n",
        "display foo(1)\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        expect_failure(bad[i]);
    expect_failure("action f() {\n value = 1\n unsupported\n}\n");
}

int main(void)
{
    test_statements_and_spans();
    test_collection_commands();
    test_input_and_file_commands();
    test_existing_lists_files_fixture();
    test_nested_control_and_action_annotations();
    test_malformed_syntax_and_cleanup();
    puts("Program AST parser tests passed");
    return 0;
}
