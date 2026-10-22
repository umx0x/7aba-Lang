#include "gc.h"
#include "runtime.h"

#include <stdint.h>
#include <stdlib.h>

#define GC_INITIAL_BYTE_THRESHOLD (64U * 1024U)
#define GC_INITIAL_OBJECT_THRESHOLD 1024U

typedef struct GCAllocation GCAllocation;
typedef struct GCRoot GCRoot;

typedef struct
{
    GCAllocation *allocations;
    GCRoot *roots;
    GCStats stats;
    size_t bytes_since_collection;
    size_t objects_since_collection;
    size_t byte_threshold;
    size_t object_threshold;
    GCAllocation **pending_marks;
    size_t pending_mark_count;
    size_t pending_mark_capacity;
    bool marking_failed;
    bool collection_requested;
} GCState;

struct GCAllocation
{
    GCAllocation *next;
    size_t size;
    GCTraceObject trace;
    GCDestroyObject destroy;
    unsigned char marked;
    long double alignment;
    unsigned char payload[];
};

struct GCRoot
{
    GCRoot *next;
    void *address;
    GCTraceRoot trace;
};

static GCState *get_state(const RuntimeContext *runtime)
{
    if (!runtime_is_initialized(runtime))
        return NULL;
    return runtime->heap.state;
}

static GCAllocation *find_allocation(GCState *state, void *object)
{
    for (GCAllocation *allocation = state->allocations; allocation; allocation = allocation->next)
        if (allocation->payload == object)
            return allocation;
    return NULL;
}

static void mark_object(void *context, void *object)
{
    GCState *state = context;
    GCAllocation *allocation = find_allocation(state, object);
    if (!allocation || allocation->marked)
        return;
    if (state->pending_mark_count == state->pending_mark_capacity)
    {
        size_t capacity = state->pending_mark_capacity == 0
                              ? 32
                              : state->pending_mark_capacity * 2;
        if (capacity < state->pending_mark_capacity ||
            capacity > SIZE_MAX / sizeof(*state->pending_marks))
        {
            state->marking_failed = true;
            return;
        }
        GCAllocation **marks = realloc(state->pending_marks,
                                       capacity * sizeof(*marks));
        if (!marks)
        {
            state->marking_failed = true;
            return;
        }
        state->pending_marks = marks;
        state->pending_mark_capacity = capacity;
    }
    allocation->marked = 1;
    state->pending_marks[state->pending_mark_count++] = allocation;
}

bool gc_context_init(RuntimeContext *runtime)
{
    if (!runtime || runtime->heap.state)
        return false;
    GCState *state = calloc(1, sizeof(*state));
    if (!state)
        return false;
    state->byte_threshold = GC_INITIAL_BYTE_THRESHOLD;
    state->object_threshold = GC_INITIAL_OBJECT_THRESHOLD;
    runtime->heap.state = state;
    return true;
}

void gc_context_destroy(RuntimeContext *runtime)
{
    if (!runtime || !runtime->heap.state)
        return;
    GCState *state = runtime->heap.state;
    while (state->allocations)
    {
        GCAllocation *allocation = state->allocations;
        state->allocations = allocation->next;
        if (allocation->destroy)
            allocation->destroy(allocation->payload);
        free(allocation);
    }
    while (state->roots)
    {
        GCRoot *root = state->roots;
        state->roots = root->next;
        free(root);
    }
    free(state->pending_marks);
    free(state);
    runtime->heap.state = NULL;
}

void *gc_allocate(RuntimeContext *runtime, size_t size,
                  GCTraceObject trace, GCDestroyObject destroy)
{
    GCState *state = get_state(runtime);
    if (!state || size > SIZE_MAX - sizeof(GCAllocation) ||
        state->stats.object_count == SIZE_MAX ||
        size > SIZE_MAX - state->stats.allocated_bytes)
        return NULL;
    GCAllocation *allocation = malloc(sizeof(*allocation) + size);
    if (!allocation)
        return NULL;
    allocation->next = state->allocations;
    allocation->size = size;
    allocation->trace = trace;
    allocation->destroy = destroy;
    allocation->marked = 0;
    allocation->alignment = 0.0L;
    state->allocations = allocation;
    state->stats.object_count++;
    state->stats.allocated_bytes += size;
    if (state->bytes_since_collection <= SIZE_MAX - size)
        state->bytes_since_collection += size;
    else
        state->bytes_since_collection = SIZE_MAX;
    if (state->objects_since_collection < SIZE_MAX)
        state->objects_since_collection++;
    if (state->bytes_since_collection >= state->byte_threshold ||
        state->objects_since_collection >= state->object_threshold)
        state->collection_requested = true;
    return allocation->payload;
}

bool gc_root_add(RuntimeContext *runtime, void *address, GCTraceRoot trace)
{
    GCState *state = get_state(runtime);
    if (!state || !address || !trace)
        return false;
    for (GCRoot *root = state->roots; root; root = root->next)
        if (root->address == address)
            return root->trace == trace;
    GCRoot *root = malloc(sizeof(*root));
    if (!root)
        return false;
    root->address = address;
    root->trace = trace;
    root->next = state->roots;
    state->roots = root;
    return true;
}

bool gc_root_remove(RuntimeContext *runtime, void *address)
{
    GCState *state = get_state(runtime);
    if (!state || !address)
        return false;
    GCRoot **link = &state->roots;
    while (*link)
    {
        if ((*link)->address == address)
        {
            GCRoot *removed = *link;
            *link = removed->next;
            free(removed);
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

static bool gc_collect_internal(RuntimeContext *runtime)
{
    GCState *state = get_state(runtime);
    if (!state)
        return false;
    state->pending_mark_count = 0;
    state->marking_failed = false;
    for (GCRoot *root = state->roots; root; root = root->next)
        root->trace(root->address, mark_object, state);

    while (state->pending_mark_count > 0 && !state->marking_failed)
    {
        GCAllocation *allocation = state->pending_marks[--state->pending_mark_count];
        if (allocation->trace)
            allocation->trace(allocation->payload, mark_object, state);
    }
    if (state->marking_failed)
    {
        for (GCAllocation *allocation = state->allocations; allocation;
             allocation = allocation->next)
            allocation->marked = 0;
        state->pending_mark_count = 0;
        return false;
    }

    GCAllocation **link = &state->allocations;
    while (*link)
    {
        GCAllocation *allocation = *link;
        if (allocation->marked)
        {
            allocation->marked = 0;
            link = &allocation->next;
            continue;
        }
        *link = allocation->next;
        if (allocation->destroy)
            allocation->destroy(allocation->payload);
        state->stats.object_count--;
        state->stats.allocated_bytes -= allocation->size;
        state->stats.collected_objects++;
        free(allocation);
    }
    state->bytes_since_collection = 0;
    state->objects_since_collection = 0;
    state->collection_requested = false;
    state->byte_threshold = state->stats.allocated_bytes > SIZE_MAX / 2
                                ? SIZE_MAX
                                : state->stats.allocated_bytes * 2;
    if (state->byte_threshold < GC_INITIAL_BYTE_THRESHOLD)
        state->byte_threshold = GC_INITIAL_BYTE_THRESHOLD;
    state->object_threshold = state->stats.object_count > SIZE_MAX / 2
                                  ? SIZE_MAX
                                  : state->stats.object_count * 2;
    if (state->object_threshold < GC_INITIAL_OBJECT_THRESHOLD)
        state->object_threshold = GC_INITIAL_OBJECT_THRESHOLD;
    return true;
}

bool gc_safe_point(RuntimeContext *runtime)
{
    GCState *state = get_state(runtime);
    if (!state)
        return false;
    if (!state->collection_requested)
        return true;
    return gc_collect_internal(runtime);
}

void gc_collect(RuntimeContext *runtime)
{
    (void)gc_collect_internal(runtime);
}

GCStats gc_get_stats(const RuntimeContext *runtime)
{
    GCState *state = get_state(runtime);
    GCStats stats = {0, 0, 0};
    return state ? state->stats : stats;
}
