#include "../value.h"
#include "../runtime.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    Value member;
} TestObject;

static void trace_test_object(void *object, GCMarkObject mark, void *mark_context)
{
    TestObject *instance = object;
    if (instance->member.type == VALUE_STRING)
        mark(mark_context, instance->member.as.string);
    else if (instance->member.type == VALUE_ARRAY)
        mark(mark_context, instance->member.as.array);
    else if (instance->member.type == VALUE_OBJECT)
        mark(mark_context, instance->member.as.object);
}

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

static bool binary(RuntimeContext *runtime, ValueBinaryOp operation,
                   Value left, Value right, Value *result)
{
    char error[128] = "";
    bool success = value_binary(runtime, operation, &left, &right, result,
                                error, sizeof(error));
    if (!success)
        fprintf(stderr, "unexpected value operation error: %s\n", error);
    return success;
}

int main(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    CHECK(!runtime_init(&runtime));
    RuntimeContext second_runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&second_runtime));
    Value second_context_value = value_null();
    CHECK(value_string(&second_runtime, "separate heap", 13, &second_context_value));
    CHECK(value_root_add(&second_runtime, &second_context_value));
    CHECK(gc_get_stats(&runtime).object_count == 0);
    CHECK(gc_get_stats(&second_runtime).object_count == 1);
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == 0);
    CHECK(gc_get_stats(&second_runtime).object_count == 1);

    Value result = value_null();
    CHECK(binary(&runtime, VALUE_OP_ADD, value_int(12), value_int(30), &result));
    CHECK(result.type == VALUE_INT && result.as.integer == 42);
    value_release(&result);

    char error[128] = "";
    CHECK(!value_binary(&runtime, VALUE_OP_MULTIPLY, &(Value){.type = VALUE_INT, .as.integer = INT64_MAX},
                        &(Value){.type = VALUE_INT, .as.integer = 2}, &result,
                        error, sizeof(error)));
    CHECK(strstr(error, "overflow") != NULL);
    CHECK(!value_binary(&runtime, VALUE_OP_DIVIDE, &(Value){.type = VALUE_INT, .as.integer = 1},
                        &(Value){.type = VALUE_INT, .as.integer = 0}, &result,
                        error, sizeof(error)));
    CHECK(strstr(error, "zero") != NULL);

    CHECK(binary(&runtime, VALUE_OP_ADD, value_int(2), value_float(0.5), &result));
    CHECK(result.type == VALUE_FLOAT && result.as.floating == 2.5);
    value_release(&result);

    Value text = value_null();
    CHECK(value_string(&runtime, "answer=", 7, &text));
    CHECK(binary(&runtime, VALUE_OP_ADD, text, value_int(42), &result));
    size_t length = 0;
    const char *bytes = value_string_bytes(&result, &length);
    CHECK(bytes && length == 9 && memcmp(bytes, "answer=42", 9) == 0);
    value_release(&result);
    value_release(&text);

    Value array = value_null();
    CHECK(value_array(&runtime, &array));
    CHECK(value_root_add(&runtime, &array));
    Value item = value_int(8);
    CHECK(value_array_push(&array, &item));
    Value nested = value_null();
    CHECK(value_array(&runtime, &nested));
    CHECK(value_array_push(&nested, &array));
    CHECK(value_array_push(&array, &nested));
    value_release(&nested);
    Value alias = value_null();
    CHECK(value_copy(&alias, &array));
    CHECK(value_array_push(&alias, &item));
    Value loaded = value_null();
    CHECK(value_array_get(&array, 0, &loaded));
    CHECK(loaded.type == VALUE_INT && loaded.as.integer == 8);
    CHECK(value_array_length(&array) == 3);
    CHECK(strcmp(value_type_name(VALUE_ARRAY), "array") == 0);
    value_release(&loaded);
    value_release(&alias);

    Value pop_array = value_null();
    CHECK(value_array(&runtime, &pop_array));
    CHECK(value_root_add(&runtime, &pop_array));
    CHECK(!value_array_pop(&pop_array, &loaded));
    CHECK(value_array_push(&pop_array, &item));
    CHECK(value_array_pop(&pop_array, &loaded));
    CHECK(loaded.type == VALUE_INT && loaded.as.integer == 8);
    CHECK(value_array_length(&pop_array) == 0);
    value_release(&loaded);

    Value alias_array = value_null();
    CHECK(value_array(&runtime, &alias_array));
    CHECK(value_root_add(&runtime, &alias_array));
    CHECK(value_array_capacity(&alias_array) == 4);
    for (int i = 0; i < 8; i++)
    {
        Value element = value_int(i);
        CHECK(value_array_push(&alias_array, &element));
    }
    CHECK(value_array_push(&alias_array, &alias_array));
    CHECK(value_array_length(&alias_array) == 9);
    CHECK(value_array_capacity(&alias_array) == 16);
    CHECK(value_array_get(&alias_array, 8, &loaded));
    CHECK(loaded.type == VALUE_ARRAY && loaded.as.array == alias_array.as.array);
    value_release(&loaded);

    Value matrix = value_null();
    CHECK(!value_matrix(&runtime, 0, 1, &matrix));
    CHECK(!value_matrix(&runtime, 1, 0, &matrix));
    CHECK(!value_matrix(&runtime, SIZE_MAX, 2, &matrix));
    CHECK(value_matrix(&runtime, 2, 3, &matrix));
    CHECK(value_root_add(&runtime, &matrix));
    CHECK(value_matrix_rows(&matrix) == 2);
    CHECK(value_matrix_columns(&matrix) == 3);
    CHECK(!value_matrix_get(&matrix, 2, 0, &loaded));
    CHECK(!value_matrix_set(&matrix, 0, 3, &item));
    CHECK(value_matrix_get(&matrix, 1, 2, &loaded));
    CHECK(loaded.type == VALUE_INT && loaded.as.integer == 0);
    value_release(&loaded);
    Value matrix_text = value_null();
    CHECK(value_string(&runtime, "matrix-root", 11, &matrix_text));
    CHECK(value_matrix_set(&matrix, 1, 2, &matrix_text));
    value_release(&matrix_text);
    gc_collect(&runtime);
    CHECK(value_matrix_get(&matrix, 1, 2, &loaded));
    bytes = value_string_bytes(&loaded, &length);
    CHECK(bytes && length == 11 && memcmp(bytes, "matrix-root", 11) == 0);
    value_release(&loaded);

    Value queue = value_null();
    CHECK(value_queue(&runtime, &queue));
    CHECK(value_root_add(&runtime, &queue));
    CHECK(!value_queue_peek(&queue, &loaded));
    CHECK(!value_queue_dequeue(&queue, &loaded));
    for (int i = 0; i < 6; i++)
    {
        Value queued = value_int(i);
        CHECK(value_queue_enqueue(&queue, &queued));
    }
    for (int i = 0; i < 5; i++)
    {
        CHECK(value_queue_dequeue(&queue, &loaded));
        CHECK(loaded.type == VALUE_INT && loaded.as.integer == i);
    }
    for (int i = 6; i < 15; i++)
    {
        Value queued = value_int(i);
        CHECK(value_queue_enqueue(&queue, &queued));
    }
    CHECK(value_queue_capacity(&queue) == 16);
    CHECK(value_queue_length(&queue) == 10);
    CHECK(value_queue_peek(&queue, &loaded));
    CHECK(loaded.type == VALUE_INT && loaded.as.integer == 5);
    value_release(&loaded);
    for (int i = 5; i < 15; i++)
    {
        CHECK(value_queue_dequeue(&queue, &loaded));
        CHECK(loaded.type == VALUE_INT && loaded.as.integer == i);
    }
    CHECK(value_queue_length(&queue) == 0);
    CHECK(!value_queue_get(&queue, 0, &loaded));
    Value queue_text = value_null();
    CHECK(value_string(&runtime, "queue-root", 10, &queue_text));
    CHECK(value_queue_enqueue(&queue, &queue_text));
    value_release(&queue_text);
    gc_collect(&runtime);
    CHECK(value_queue_peek(&queue, &loaded));
    bytes = value_string_bytes(&loaded, &length);
    CHECK(bytes && length == 10 && memcmp(bytes, "queue-root", 10) == 0);
    value_release(&loaded);

    Value map = value_null();
    CHECK(value_map(&runtime, &map));
    CHECK(value_root_add(&runtime, &map));
    for (int i = 0; i < 96; i++)
    {
        char key_bytes[32];
        int key_length = snprintf(key_bytes, sizeof(key_bytes), "key-%d", i);
        Value key = value_null();
        CHECK(key_length > 0 && (size_t)key_length < sizeof(key_bytes));
        CHECK(value_string(&runtime, key_bytes, (size_t)key_length, &key));
        Value mapped_value = value_int((int64_t)i * 3);
        CHECK(value_map_set(&map, &key, &mapped_value));
        value_release(&key);
    }
    CHECK(value_map_length(&map) == 96);
    Value lookup_key = value_null();
    CHECK(value_string(&runtime, "key-42", 6, &lookup_key));
    CHECK(value_map_get(&map, &lookup_key, &loaded));
    CHECK(loaded.type == VALUE_INT && loaded.as.integer == 126);
    value_release(&loaded);
    bool present = false;
    CHECK(value_map_contains(&map, &lookup_key, &present) && present);
    bool removed = false;
    CHECK(value_map_remove(&map, &lookup_key, &removed) && removed);
    CHECK(!value_map_get(&map, &lookup_key, &loaded));
    CHECK(value_map_length(&map) == 95);
    CHECK(value_map_set(&map, &lookup_key, &(Value){.type = VALUE_INT, .as.integer = 900}));
    CHECK(value_map_get(&map, &lookup_key, &loaded) && loaded.as.integer == 900);
    value_release(&loaded);
    value_release(&lookup_key);

    Value one_float = value_float(1.0);
    Value one_integer = value_int(1);
    CHECK(value_map_set(&map, &one_integer, &(Value){.type = VALUE_INT, .as.integer = 11}));
    CHECK(value_map_set(&map, &one_float, &(Value){.type = VALUE_INT, .as.integer = 12}));
    CHECK(value_map_length(&map) == 97);
    CHECK(value_map_get(&map, &one_integer, &loaded) && loaded.as.integer == 12);
    value_release(&loaded);
    Value invalid_key = value_null();
    CHECK(value_array(&runtime, &invalid_key));
    CHECK(!value_map_set(&map, &invalid_key, &one_integer));
    value_release(&invalid_key);
    CHECK(!value_map_set(&map, &(Value){.type = VALUE_FLOAT, .as.floating = NAN},
                         &one_integer));
    CHECK(value_map_set(&map, &(Value){.type = VALUE_NULL},
                        &(Value){.type = VALUE_INT, .as.integer = 0}));
    CHECK(value_map_set(&map, &(Value){.type = VALUE_INT, .as.integer = 0},
                        &(Value){.type = VALUE_INT, .as.integer = 1}));
    CHECK(value_map_set(&map, &(Value){.type = VALUE_FLOAT, .as.floating = -0.0},
                        &(Value){.type = VALUE_INT, .as.integer = 2}));
    CHECK(value_map_get(&map, &(Value){.type = VALUE_INT, .as.integer = 0}, &loaded));
    CHECK(loaded.as.integer == 2);
    value_release(&loaded);
    CHECK(value_map_set(&map, &(Value){.type = VALUE_INT, .as.integer = INT64_MAX},
                        &(Value){.type = VALUE_INT, .as.integer = 3}));
    CHECK(value_map_set(&map, &(Value){.type = VALUE_FLOAT,
                                      .as.floating = (double)INT64_MAX},
                        &(Value){.type = VALUE_INT, .as.integer = 4}));
    CHECK(value_map_get(&map, &(Value){.type = VALUE_INT, .as.integer = INT64_MAX},
                        &loaded));
    CHECK(loaded.as.integer == 4);
    value_release(&loaded);


    Value set = value_null();
    CHECK(value_set(&runtime, &set));
    CHECK(value_root_add(&runtime, &set));
    bool added = false;
    CHECK(value_set_add(&set, &one_integer, &added) && added);
    CHECK(value_set_add(&set, &one_float, &added) && !added);
    CHECK(value_set_add(&set, &(Value){.type = VALUE_BOOL, .as.boolean = true}, &added) && added);
    for (int i = 0; i < 64; i++)
    {
        Value element = value_int((int64_t)i + 10);
        CHECK(value_set_add(&set, &element, &added));
    }
    Value set_text = value_null();
    CHECK(value_string(&runtime, "set-keeps-keys", 14, &set_text));
    CHECK(value_set_add(&set, &set_text, &added) && added);
    value_release(&set_text);
    CHECK(value_set_length(&set) == 67);
    CHECK(value_set_contains(&set, &one_float, &present) && present);
    CHECK(value_set_remove(&set, &one_integer, &removed) && removed);
    CHECK(value_set_length(&set) == 66);

    Value cycle_array = value_null();
    CHECK(value_array(&runtime, &cycle_array));
    CHECK(value_array_push(&cycle_array, &map));
    Value cycle_key = value_null();
    CHECK(value_string(&runtime, "cycle", 5, &cycle_key));
    CHECK(value_map_set(&map, &cycle_key, &cycle_array));
    value_release(&cycle_array);
    gc_collect(&runtime);
    CHECK(value_map_get(&map, &cycle_key, &loaded));
    CHECK(loaded.type == VALUE_ARRAY && value_array_length(&loaded) == 1);
    Value cycle_member = value_null();
    CHECK(value_array_get(&loaded, 0, &cycle_member));
    CHECK(cycle_member.type == VALUE_MAP && cycle_member.as.map == map.as.map);
    value_release(&cycle_member);
    value_release(&loaded);
    value_release(&cycle_key);
    CHECK(value_map_get(&map, &(Value){.type = VALUE_INT, .as.integer = 1}, &loaded));
    value_release(&loaded);
    CHECK(value_map_length(&map) == 101);
    CHECK(value_set_length(&set) == 66);
    Value set_lookup = value_null();
    CHECK(value_string(&runtime, "set-keeps-keys", 14, &set_lookup));
    CHECK(value_set_contains(&set, &set_lookup, &present) && present);
    value_release(&set_lookup);

    for (int i = 0; i < 80; i++)
    {
        char key_bytes[32];
        int key_length = snprintf(key_bytes, sizeof(key_bytes), "key-%d", i);
        Value key = value_null();
        CHECK(key_length > 0 && (size_t)key_length < sizeof(key_bytes));
        CHECK(value_string(&runtime, key_bytes, (size_t)key_length, &key));
        CHECK(value_map_remove(&map, &key, &removed) && removed);
        value_release(&key);
    }
    Value tombstone_key = value_null();
    CHECK(value_string(&runtime, "after-tombstones", 16, &tombstone_key));
    CHECK(value_map_set(&map, &tombstone_key, &one_integer));
    CHECK(value_map_get(&map, &tombstone_key, &loaded) && loaded.as.integer == 1);
    value_release(&loaded);
    value_release(&tombstone_key);

    for (int i = 10; i < 50; i++)
    {
        Value element = value_int(i);
        CHECK(value_set_remove(&set, &element, &removed) && removed);
    }
    Value set_reinsert = value_int(10000);
    CHECK(value_set_add(&set, &set_reinsert, &added) && added);
    CHECK(value_set_length(&set) == 27);
    CHECK(value_set_contains(&set, &set_reinsert, &present) && present);

    Value standalone_string = value_null();
    CHECK(value_string(&runtime, "rooted", 6, &standalone_string));
    CHECK(value_root_add(&runtime, &standalone_string));
    gc_collect(&runtime);
    size_t live_before_collection = gc_get_stats(&runtime).object_count;
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == live_before_collection);

    TestObject *object = gc_allocate(&runtime, sizeof(*object), trace_test_object, NULL);
    CHECK(object != NULL);
    CHECK(value_string(&runtime, "object-owned", 12, &object->member));
    Value object_root = value_object((ValueObject *)object);
    CHECK(value_root_add(&runtime, &object_root));
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == live_before_collection + 2);

    Value managed_object = value_null();
    CHECK(value_object_new(&runtime, 7, 1, &managed_object));
    CHECK(value_root_add(&runtime, &managed_object));
    CHECK(value_truthy(&managed_object));
    CHECK(value_object_class_id(&managed_object) == 7);
    CHECK(value_object_field_count(&managed_object) == 1);
    CHECK(value_object_set(&managed_object, 0, &managed_object));
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == live_before_collection + 3);

    CHECK(value_root_remove(&runtime, &array));
    CHECK(value_root_remove(&runtime, &alias_array));
    CHECK(value_root_remove(&runtime, &pop_array));
    CHECK(value_root_remove(&runtime, &matrix));
    CHECK(value_root_remove(&runtime, &queue));
    CHECK(value_root_remove(&runtime, &map));
    CHECK(value_root_remove(&runtime, &set));
    CHECK(value_root_remove(&runtime, &standalone_string));
    CHECK(value_root_remove(&runtime, &object_root));
    CHECK(value_root_remove(&runtime, &managed_object));
    CHECK(value_root_remove(&second_runtime, &second_context_value));
    value_release(&array);
    value_release(&alias_array);
    value_release(&pop_array);
    value_release(&matrix);
    value_release(&queue);
    value_release(&map);
    value_release(&set);
    value_release(&standalone_string);
    value_release(&object_root);
    value_release(&managed_object);
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == 0);
    gc_collect(&second_runtime);
    CHECK(gc_get_stats(&second_runtime).object_count == 0);
    value_release(&second_context_value);

    Value number_text = value_null();
    CHECK(value_string(&runtime, "123", 3, &number_text));
    CHECK(value_cast(&runtime, &number_text, VALUE_INT, &result, NULL, 0));
    CHECK(result.type == VALUE_INT && result.as.integer == 123);
    value_release(&result);
    value_release(&number_text);

    const char embedded_number[] = {'1', '\0', '2'};
    CHECK(value_string(&runtime, embedded_number, sizeof(embedded_number), &number_text));
    CHECK(!value_cast(&runtime, &number_text, VALUE_INT, &result, NULL, 0));
    value_release(&number_text);

    CHECK(!value_truthy(&(Value){.type = VALUE_NULL}));
    CHECK(value_truthy(&(Value){.type = VALUE_BOOL, .as.boolean = true}));
    CHECK(strcmp(value_type_name(VALUE_OBJECT), "object") == 0);
    runtime_destroy(&second_runtime);
    runtime_destroy(&runtime);
    puts("value tests passed");
    return 0;
}
