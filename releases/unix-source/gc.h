#ifndef GC_H
#define GC_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*GCMarkObject)(void *context, void *object);
typedef void (*GCTraceObject)(void *object, GCMarkObject mark, void *mark_context);
typedef void (*GCDestroyObject)(void *object);
typedef void (*GCTraceRoot)(void *root, GCMarkObject mark, void *mark_context);

typedef struct
{
    void *state;
} GCHeap;

typedef struct RuntimeContext RuntimeContext;

typedef struct
{
    size_t object_count;
    size_t allocated_bytes;
    size_t collected_objects;
} GCStats;

bool gc_context_init(RuntimeContext *runtime);
void gc_context_destroy(RuntimeContext *runtime);
void *gc_allocate(RuntimeContext *runtime, size_t size,
                  GCTraceObject trace, GCDestroyObject destroy);
bool gc_root_add(RuntimeContext *runtime, void *address, GCTraceRoot trace);
bool gc_root_remove(RuntimeContext *runtime, void *address);
bool gc_safe_point(RuntimeContext *runtime);
void gc_collect(RuntimeContext *runtime);
GCStats gc_get_stats(const RuntimeContext *runtime);

#endif
