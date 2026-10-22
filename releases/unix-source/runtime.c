#include "main.h"

#include <string.h>
#include <time.h>

bool runtime_init(RuntimeContext *runtime)
{
    if (!runtime)
        return false;
    if (runtime->initialized || runtime->heap.state)
        return false;
    if (!gc_context_init(runtime))
        return false;
    runtime->initialized = true;
    runtime->executed_instructions = 0;
    runtime->execution_steps = 0;
    runtime_random_seed(runtime,
                        (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)runtime);
    return true;
}

void runtime_destroy(RuntimeContext *runtime)
{
    if (!runtime)
        return;
    for (int i = runtime->stack_top; i >= 0; i--)
        free_scope(&runtime->call_stack[i]);
    runtime->stack_top = 0;
    clear_functions(runtime);
    clear_classes(runtime);
    runtime_clear_source(runtime);
    if (runtime->initialized)
        gc_context_destroy(runtime);
    runtime->heap.state = NULL;
    runtime->initialized = false;
    runtime->executed_instructions = 0;
    runtime->execution_steps = 0;
    runtime->random_state = 0;
    runtime->current_line = 0;
    runtime->diagnostic_count = 0;
    memset(&runtime->last_diagnostic, 0, sizeof(runtime->last_diagnostic));
}

bool runtime_is_initialized(const RuntimeContext *runtime)
{
    return runtime && runtime->initialized && runtime->heap.state;
}

void runtime_record_instruction(RuntimeContext *runtime)
{
    if (runtime_is_initialized(runtime) &&
        runtime->executed_instructions != UINT64_MAX)
        runtime->executed_instructions++;
}

uint64_t runtime_get_executed_instructions(const RuntimeContext *runtime)
{
    return runtime_is_initialized(runtime) ? runtime->executed_instructions : 0;
}

void runtime_reset_execution_steps(RuntimeContext *runtime)
{
    if (runtime_is_initialized(runtime))
        runtime->execution_steps = 0;
}

void runtime_random_seed(RuntimeContext *runtime, uint64_t seed)
{
    if (!runtime_is_initialized(runtime))
        return;
    runtime->random_state = seed ? seed : UINT64_C(0x9e3779b97f4a7c15);
}

uint64_t runtime_random_next(RuntimeContext *runtime)
{
    uint64_t value;
    if (!runtime_is_initialized(runtime))
        return 0;
    value = runtime->random_state;
    if (value == 0)
        value = UINT64_C(0x9e3779b97f4a7c15);
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    runtime->random_state = value;
    return value * UINT64_C(0x2545f4914f6cdd1d);
}

void runtime_clear_source(RuntimeContext *runtime)
{
    if (!runtime)
        return;
    for (size_t i = 0; i < runtime->source_line_count; i++)
        free(runtime->source_lines[i]);
    free(runtime->source_lines);
    runtime->source_lines = NULL;
    runtime->source_line_count = 0;
    runtime->source_lines_capacity = 0;
    runtime->source_filename[0] = '\0';
}
