#include "../main.h"
#include "../runtime.h"
#include "../bytecode.h"
#include "../diagnostic.h"

#include <stdio.h>

int main(void)
{
    RuntimeContext first = RUNTIME_CONTEXT_INIT;
    RuntimeContext second = RUNTIME_CONTEXT_INIT;
    if (!runtime_init(&first) || !runtime_init(&second))
    {
        fprintf(stderr, "unable to initialize independent runtimes\n");
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }

    if (!init_scope(&first.call_stack[0], &first) ||
        !init_scope(&second.call_stack[0], &second) ||
        !append_line(&first, "class Unique {\n") ||
        !append_line(&first, "    field marker = 1\n") ||
        !append_line(&first, "}\n") ||
        !append_line(&first, "action isolated() {\n") ||
        !append_line(&first, "    display 1\n") ||
        !append_line(&first, "}\n") ||
        !append_line(&first, "value = 17\n") ||
        !append_line(&second, "value = 29\n"))
    {
        fprintf(stderr, "unable to initialize isolated runtime state\n");
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }
    runtime_random_seed(&first, 42);
    runtime_random_seed(&second, 42);
    uint64_t first_random = runtime_random_next(&first);
    uint64_t second_random = runtime_random_next(&second);
    if (first_random != second_random)
    {
        fprintf(stderr, "equal per-context random seeds diverged\n");
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }
    (void)runtime_random_next(&first);
    if (runtime_random_next(&second) == first_random)
    {
        fprintf(stderr, "random generator state was not isolated per context\n");
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }

    BytecodeProgram first_program = BYTECODE_PROGRAM_INIT;
    BytecodeProgram second_program = BYTECODE_PROGRAM_INIT;
    if (!bytecode_compile_program(&first, 0,
                                  (int)first.source_line_count - 1, &first_program) ||
        !bytecode_compile_program(&second, 0,
                                  (int)second.source_line_count - 1, &second_program) ||
        bytecode_execute(&second, &first_program, NULL) ||
        !bytecode_execute(&first, &first_program, NULL) ||
        !bytecode_execute(&second, &second_program, NULL))
    {
        fprintf(stderr, "independent runtime programs did not compile and execute\n");
        bytecode_free(&first_program);
        bytecode_free(&second_program);
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }

    Variable *first_value = get_var_obj(&first, "value");
    Variable *second_value = get_var_obj(&second, "value");
    if (!first_value || !second_value ||
        !first_value->has_runtime_value || !second_value->has_runtime_value ||
        first_value->runtime_value.type != VALUE_INT ||
        second_value->runtime_value.type != VALUE_INT ||
        first_value->runtime_value.as.integer != 17 ||
        second_value->runtime_value.as.integer != 29 ||
        first.source_line_count != 7 || second.source_line_count != 1)
    {
        fprintf(stderr, "runtime variables or source buffers crossed contexts\n");
        bytecode_free(&first_program);
        bytecode_free(&second_program);
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }

    if (find_function_index(&first, "isolated") < 0 ||
        find_function_index(&second, "isolated") >= 0 ||
        find_class_index(&first, "Unique") < 0 ||
        find_class_index(&second, "Unique") >= 0)
    {
        fprintf(stderr, "function or class registries crossed runtime contexts\n");
        bytecode_free(&first_program);
        bytecode_free(&second_program);
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }

    report_diagnostic(&first, "error", "E_TEST", "first runtime diagnostic", 1, 1);
    if (first.diagnostic_count != 1 || second.diagnostic_count != 0 ||
        strcmp(first.last_diagnostic.code, "E_TEST") != 0)
    {
        fprintf(stderr, "diagnostics crossed runtime contexts\n");
        bytecode_free(&first_program);
        bytecode_free(&second_program);
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }
    bytecode_free(&first_program);
    bytecode_free(&second_program);
    runtime_reset_execution_steps(&first);
    runtime_reset_execution_steps(&second);

    for (size_t step = 0; step < MAX_EXECUTION_STEPS; step++)
    {
        if (!runtime_consume_execution_step(&first))
        {
            fprintf(stderr, "first runtime exhausted its budget early\n");
            runtime_destroy(&first);
            runtime_destroy(&second);
            return 1;
        }
    }

    if (runtime_consume_execution_step(&first) ||
        !runtime_consume_execution_step(&second))
    {
        fprintf(stderr, "execution budgets are not isolated per runtime\n");
        runtime_destroy(&first);
        runtime_destroy(&second);
        return 1;
    }

    runtime_destroy(&first);
    runtime_destroy(&second);
    puts("runtime state and execution budget isolation passed");
    return 0;
}
