#include "../main.h"
#include "../gc.h"
#include "../runtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                      \
    do                                                        \
    {                                                         \
        if (!(condition))                                     \
        {                                                     \
            fprintf(stderr, "check failed at line %d: %s\n", \
                    __LINE__, #condition);                    \
            return 1;                                         \
        }                                                     \
    } while (0)

int main(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.stack_top = 0;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));

    Value string = value_null();
    CHECK(value_string(&runtime, "held by scope", 13, &string));
    Variable *variable = get_or_create_var(&runtime, "retained");
    CHECK(variable_set_runtime_value(variable, &string));
    value_release(&string);

    size_t before = gc_get_stats(&runtime).object_count;
    CHECK(before == 1);
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == 1);

    Value replacement = value_int(7);
    CHECK(variable_set_runtime_value(variable, &replacement));
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == 0);

    free_scope(&runtime.call_stack[0]);
    runtime_destroy(&runtime);
    puts("scope GC root tests passed");
    return 0;
}
