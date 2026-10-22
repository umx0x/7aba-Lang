#include "../program_ast.h"
#include "../resolver.h"

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

static bool resolve_fails(const char *source, const char *message_fragment)
{
    ProgramAst *program = parse(source);
    ProgramResolution resolution;
    ResolverError error;
    bool success = resolver_resolve_program(program, &resolution, &error);
    if (!success)
    {
        assert(strstr(error.message, message_fragment) != NULL);
        assert(error.span.line >= 1);
        assert(error.span.column >= 1);
        resolver_resolution_free(&resolution);
    }
    program_ast_free(program);
    return !success;
}

static void test_forward_declarations_and_reference_table(void)
{
    ProgramAst *program = parse(
        "later(seed)\n"
        "seed = 3\n"
        "action later(value: Int) {\n"
        "    local = value + seed\n"
        "    when local > 0 {\n"
        "        display local\n"
        "    }\n"
        "    empty\n"
        "}\n");
    ProgramResolution resolution;
    ResolverError error;
    size_t symbol_index;
    assert(resolver_resolve_program(program, &resolution, &error));
    assert(resolution.scope_count == 2);
    assert(resolver_lookup(&resolution, 0, "seed", RESOLVER_VARIABLE,
                           &symbol_index));
    assert(strcmp(resolution.symbols[symbol_index].name, "seed") == 0);
    assert(resolver_lookup(&resolution, 1, "value", RESOLVER_VARIABLE,
                           &symbol_index));
    assert(strcmp(resolution.symbols[symbol_index].type_name, "Int") == 0);
    assert(resolver_lookup(&resolution, 1, "seed", RESOLVER_VARIABLE,
                           &symbol_index));
    assert(resolver_lookup(&resolution, 0, "local", RESOLVER_VARIABLE,
                           NULL) == false);
    assert(resolution.reference_count == 8);
    resolver_resolution_free(&resolution);
    resolver_resolution_free(&resolution);
    program_ast_free(program);
}

static void test_assignment_and_type_declaration_rules(void)
{
    ProgramAst *program = parse(
        "value = 1\n"
        "value = 2\n"
        "value: Int = 3\n"
        "display value\n");
    ProgramResolution resolution;
    ResolverError error;
    assert(resolver_resolve_program(program, &resolution, &error));
    assert(resolver_lookup(&resolution, 0, "value", RESOLVER_VARIABLE,
                           NULL));
    resolver_resolution_free(&resolution);
    program_ast_free(program);

    assert(resolve_fails("value: Int = 1\nvalue: Int = 2\n",
                         "Duplicate typed declaration"));
}

static void test_collection_name_resolution(void)
{
    ProgramAst *program = parse(
        "list values\n"
        "push values 5\n"
        "len values count\n"
        "display count\n");
    ProgramResolution resolution;
    ResolverError error;
    assert(resolver_resolve_program(program, &resolution, &error));
    assert(resolver_lookup(&resolution, 0, "values", RESOLVER_VARIABLE,
                           NULL));
    assert(resolver_lookup(&resolution, 0, "count", RESOLVER_VARIABLE,
                           NULL));
    resolver_resolution_free(&resolution);
    program_ast_free(program);
}

static void test_io_name_resolution(void)
{
    ProgramAst *program = parse(
        "list values\n"
        "input entered\n"
        "push values entered\n"
        "save values \"values.dat\"\n"
        "load restored \"values.dat\"\n"
        "file_read \"input.txt\" contents\n"
        "file_write \"output.txt\" contents append\n"
        "display restored\n");
    ProgramResolution resolution;
    ResolverError error;
    assert(resolver_resolve_program(program, &resolution, &error));
    assert(resolver_lookup(&resolution, 0, "entered", RESOLVER_VARIABLE,
                           NULL));
    assert(resolver_lookup(&resolution, 0, "restored", RESOLVER_VARIABLE,
                           NULL));
    assert(resolver_lookup(&resolution, 0, "contents", RESOLVER_VARIABLE,
                           NULL));
    resolver_resolution_free(&resolution);
    program_ast_free(program);
}

static void test_supplied_program_fixture(void)
{
    FILE *file = fopen("pro.7ba", "rb");
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
    ProgramResolution resolution;
    ResolverError error;
    assert(resolver_resolve_program(program, &resolution, &error));
    resolver_resolution_free(&resolution);
    program_ast_free(program);
    free(source);
}

static void test_lists_files_fixture(void)
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
    ProgramResolution resolution;
    ResolverError error;
    assert(resolver_resolve_program(program, &resolution, &error));
    resolver_resolution_free(&resolution);
    program_ast_free(program);
    free(source);
}

static void test_errors_and_scope_isolation(void)
{
    assert(resolve_fails("display missing\n", "Unresolved variable"));
    assert(resolve_fails("save missing \"data.txt\"\n",
                         "Unresolved variable"));
    assert(resolve_fails("file_write \"data.txt\" missing append\n",
                         "Unresolved variable"));
    assert(resolve_fails("action f() {\n}\nf(1)\n",
                         "incorrect arity"));
    assert(resolve_fails("action f() {\n    local = 1\n}\ndisplay local\n",
                         "Unresolved variable"));
    assert(resolve_fails("action outer() {\n}\nwhen true {\n"
                         "    action nested() {\n    }\n}\n",
                         "only allowed at program scope"));
    assert(resolve_fails("empty\n", "only valid inside an action"));
    assert(resolve_fails(
        "action takes_text(value: String) {\n}\n"
        "takes_text(42)\n",
        "parameter requires String"));
    assert(resolve_fails(
        "action returns_int() -> Int {\n"
        "    empty\n"
        "}\n",
        "returns Null"));
}

int main(void)
{
    test_forward_declarations_and_reference_table();
    test_assignment_and_type_declaration_rules();
    test_collection_name_resolution();
    test_io_name_resolution();
    test_supplied_program_fixture();
    test_lists_files_fixture();
    test_errors_and_scope_isolation();
    puts("Program name resolver tests passed");
    return 0;
}
