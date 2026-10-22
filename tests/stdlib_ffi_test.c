#include "../ffi.h"
#include "../stdlib.h"
#include "../runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

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

static bool square_value(const Value *arguments, size_t count, Value *result,
                         char *error, size_t error_capacity)
{
    if (count != 1 || arguments[0].type != VALUE_INT)
    {
        if (error && error_capacity)
            snprintf(error, error_capacity, "square expects one integer");
        return false;
    }
    *result = value_int(arguments[0].as.integer * arguments[0].as.integer);
    return true;
}

static bool positive_value(const Value *arguments, size_t count, Value *result,
                           char *error, size_t error_capacity)
{
    if (count != 1 || arguments[0].type != VALUE_INT)
    {
        if (error && error_capacity)
            snprintf(error, error_capacity, "positive expects one integer");
        return false;
    }
    *result = value_bool(arguments[0].as.integer > 0);
    return true;
}

int main(int argc, char **argv)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    Value source = value_null();
    CHECK(value_array(&runtime, &source));
    CHECK(value_root_add(&runtime, &source));
    CHECK(value_array_push(&source, &(Value){.type = VALUE_INT, .as.integer = 3}));
    CHECK(value_array_push(&source, &(Value){.type = VALUE_INT, .as.integer = -2}));
    CHECK(value_array_push(&source, &(Value){.type = VALUE_INT, .as.integer = 1}));

    Value mapped = value_null();
    char error[256] = "";
    CHECK(stdlib_array_map(&runtime, &source, square_value, &mapped,
                           error, sizeof(error)));
    Value value = value_null();
    CHECK(value_array_get(&mapped, 0, &value) && value.as.integer == 9);
    value_release(&value);
    CHECK(value_array_get(&mapped, 1, &value) && value.as.integer == 4);
    value_release(&value);
    value_release(&mapped);

    Value filtered = value_null();
    CHECK(stdlib_array_filter(&runtime, &source, positive_value, &filtered,
                              error, sizeof(error)));
    CHECK(value_array_length(&filtered) == 2);
    value_release(&filtered);

    Value sorted = value_null();
    CHECK(stdlib_array_sort(&runtime, &source, STD_ARRAY_ASCENDING, &sorted,
                            error, sizeof(error)));
    CHECK(value_array_get(&sorted, 0, &value) && value.as.integer == -2);
    value_release(&value);
    CHECK(value_array_get(&sorted, 2, &value) && value.as.integer == 3);
    value_release(&value);
    value_release(&sorted);

    Value text = value_null();
    CHECK(value_string(&runtime, " \t7aba\n ", 8, &text));
    CHECK(value_root_add(&runtime, &text));
    Value trimmed = value_null();
    CHECK(stdlib_string_trim(&runtime, &text, &trimmed, error,
                             sizeof(error)));
    size_t text_length = 0;
    const char *text_bytes = value_string_bytes(&trimmed, &text_length);
    CHECK(text_bytes != NULL && text_length == 4);
    CHECK(memcmp(text_bytes, "7aba", 4) == 0);
    Value prefix = value_null();
    CHECK(value_string(&runtime, "7a", 2, &prefix));
    CHECK(value_root_add(&runtime, &prefix));
    bool matches = false;
    CHECK(stdlib_string_starts_with(&trimmed, &prefix, &matches, error,
                                    sizeof(error)));
    CHECK(matches);
    CHECK(stdlib_string_ends_with(&trimmed, &prefix, &matches, error,
                                  sizeof(error)));
    CHECK(!matches);
    Value wrong_type = value_int(1);
    error[0] = '\0';
    CHECK(!stdlib_string_starts_with(&trimmed, &wrong_type, &matches, error,
                                     sizeof(error)));
    CHECK(error[0] != '\0');
    Value nine = value_int(9);
    Value math_result = value_null();
    CHECK(stdlib_math_unary(STD_MATH_SQRT, &nine, &math_result, error,
                            sizeof(error)));
    CHECK(math_result.type == VALUE_FLOAT &&
          fabs(math_result.as.floating - 3.0) < 0.000001);
    Value negative = value_int(-1);
    error[0] = '\0';
    CHECK(!stdlib_math_unary(STD_MATH_SQRT, &negative, &math_result, error,
                             sizeof(error)));
    CHECK(error[0] != '\0');
    Value minimum = value_int(INT64_MIN);
    Value maximum = value_int(INT64_MIN + 10);
    Value random_value = value_null();
    runtime_random_seed(&runtime, 123);
    for (size_t i = 0; i < 64; i++)
    {
        CHECK(stdlib_random_int(&runtime, &minimum, &maximum, &random_value,
                                error, sizeof(error)));
        CHECK(random_value.type == VALUE_INT &&
              random_value.as.integer >= INT64_MIN &&
              random_value.as.integer <= INT64_MIN + 10);
    }
    minimum = value_int(INT64_MAX - 10);
    maximum = value_int(INT64_MAX);
    for (size_t i = 0; i < 64; i++)
    {
        CHECK(stdlib_random_int(&runtime, &minimum, &maximum, &random_value,
                                error, sizeof(error)));
        CHECK(random_value.type == VALUE_INT &&
              random_value.as.integer >= INT64_MAX - 10 &&
              random_value.as.integer <= INT64_MAX);
    }
    minimum = value_int(2);
    maximum = value_int(1);
    error[0] = '\0';
    CHECK(!stdlib_random_int(&runtime, &minimum, &maximum, &random_value,
                             error, sizeof(error)));
    CHECK(error[0] != '\0');
    value_release(&trimmed);
    CHECK(value_root_remove(&runtime, &prefix));
    value_release(&prefix);
    CHECK(value_root_remove(&runtime, &text));
    value_release(&text);

    const char *path = "tests/stdlib_binary_test.tmp";
    const unsigned char content[] = {0, 1, 2, 0, 254, 255};
    StdFile *file = stdlib_file_open(path, STD_FILE_WRITE_BINARY);
    CHECK(file != NULL);
    size_t count = 0;
    CHECK(stdlib_file_write(file, content, sizeof(content), &count));
    CHECK(count == sizeof(content));
    CHECK(stdlib_file_flush(file));
    CHECK(stdlib_file_close(file));
    file = stdlib_file_open(path, STD_FILE_READ_BINARY);
    CHECK(file != NULL);
    unsigned char *loaded = NULL;
    size_t loaded_length = 0;
    CHECK(stdlib_file_read_all(file, &loaded, &loaded_length));
    CHECK(loaded_length == sizeof(content));
    CHECK(memcmp(loaded, content, sizeof(content)) == 0);
    free(loaded);
    CHECK(stdlib_file_close(file));
    CHECK(remove(path) == 0);

    FFILibrary *library = ffi_load_library("definitely-not-a-7aba-library", error, sizeof(error));
    CHECK(library == NULL);
    FFIValue result;
    FFISignature signature = {.result = FFI_I64, .arguments = {FFI_I64}, .argument_count = 1};
    FFIValue bad_argument = {.type = FFI_F64, .as.floating = 1.0};
    CHECK(!ffi_extern_call(NULL, "missing", &signature, &bad_argument,
                           &result, error, sizeof(error)));
    if (argc > 1)
    {
        FFILibrary *fixture = ffi_load_library(argv[1], error, sizeof(error));
        CHECK(fixture != NULL);
        FFIValue integers[2] = {
            {.type = FFI_I64, .as.integer = 19},
            {.type = FFI_I64, .as.integer = 23}};
        signature.result = FFI_I64;
        signature.arguments[0] = FFI_I64;
        signature.arguments[1] = FFI_I64;
        signature.argument_count = 2;
        CHECK(ffi_extern_call(fixture, "sevenba_add", &signature, integers,
                              &result, error, sizeof(error)));
        CHECK(result.type == FFI_I64 && result.as.integer == 42);
        FFIValue floats[2] = {
            {.type = FFI_F64, .as.floating = 2.0},
            {.type = FFI_F64, .as.floating = 4.0}};
        signature.result = FFI_F64;
        signature.arguments[0] = FFI_F64;
        signature.arguments[1] = FFI_F64;
        CHECK(ffi_extern_call(fixture, "sevenba_midpoint", &signature, floats,
                              &result, error, sizeof(error)));
        CHECK(result.type == FFI_F64 && result.as.floating == 3.0);
        CHECK(ffi_unload_library(fixture, error, sizeof(error)));
    }

    CHECK(value_root_remove(&runtime, &source));
    value_release(&source);
    gc_collect(&runtime);
    runtime_destroy(&runtime);
    puts("stdlib and FFI tests passed");
    return 0;
}
