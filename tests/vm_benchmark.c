#include "../bytecode.h"
#include "../main.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static bool parse_iterations(int argc, char **argv, uint64_t *iterations)
{
    if (argc == 1)
    {
        *iterations = 1000000;
        return true;
    }
    if (argc != 2 || !argv[1] || !*argv[1] || argv[1][0] == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(argv[1], &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || parsed == 0)
        return false;
    *iterations = (uint64_t)parsed;
    return true;
}

int main(int argc, char **argv)
{
    uint64_t iterations;
    if (!parse_iterations(argc, argv, &iterations))
    {
        fprintf(stderr, "usage: vm_benchmark [positive-iterations]\n");
        return 2;
    }

    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    if (!runtime_init(&runtime))
    {
        fprintf(stderr, "unable to initialize benchmark runtime\n");
        return 1;
    }
    runtime.stack_top = 0;
    if (!init_scope(&runtime.call_stack[0], &runtime))
    {
        runtime_destroy(&runtime);
        return 1;
    }

    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    if (!bytecode_compile_expression(&runtime,
                                     "((17 * 23 + 5) / 3) == 132", &program))
    {
        free_scope(&runtime.call_stack[0]);
        runtime_destroy(&runtime);
        return 1;
    }

    clock_t start = clock();
    if (start == (clock_t)-1)
    {
        fprintf(stderr, "clock() is unavailable on this platform\n");
        bytecode_free(&program);
        free_scope(&runtime.call_stack[0]);
        runtime_destroy(&runtime);
        return 1;
    }
    for (uint64_t i = 0; i < iterations; i++)
    {
        runtime_reset_execution_steps(&runtime);
        Value result = value_null();
        if (!bytecode_execute_value(&runtime, &program, &result) ||
            result.type != VALUE_BOOL || !result.as.boolean)
        {
            fprintf(stderr, "benchmark expression returned an invalid result\n");
            value_release(&result);
            bytecode_free(&program);
            free_scope(&runtime.call_stack[0]);
            runtime_destroy(&runtime);
            return 1;
        }
        value_release(&result);
    }
    clock_t finish = clock();
    if (finish == (clock_t)-1 || finish < start)
    {
        fprintf(stderr, "clock() returned an invalid elapsed time\n");
        bytecode_free(&program);
        free_scope(&runtime.call_stack[0]);
        runtime_destroy(&runtime);
        return 1;
    }

    double elapsed_seconds = (double)(finish - start) / (double)CLOCKS_PER_SEC;
    uint64_t instruction_count = runtime_get_executed_instructions(&runtime);
    printf("iterations: %" PRIu64 "\n", iterations);
    printf("VM instructions: %" PRIu64 "\n", instruction_count);
    printf("CPU seconds: %.6f\n", elapsed_seconds);
    if (elapsed_seconds > 0.0)
    {
        printf("iterations/second: %.0f\n", (double)iterations / elapsed_seconds);
        printf("instructions/second: %.0f\n",
               (double)instruction_count / elapsed_seconds);
    }
    else
    {
        puts("throughput: below clock() resolution");
    }

    bytecode_free(&program);
    free_scope(&runtime.call_stack[0]);
    runtime_destroy(&runtime);
    return 0;
}
