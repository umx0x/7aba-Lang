#ifndef FFI_H
#define FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FFILibrary FFILibrary;

typedef enum
{
    FFI_I64,
    FFI_F64
} FFIType;

typedef struct
{
    FFIType type;
    union
    {
        int64_t integer;
        double floating;
    } as;
} FFIValue;

typedef struct
{
    FFIType result;
    FFIType arguments[2];
    size_t argument_count;
} FFISignature;

FFILibrary *ffi_load_library(const char *path, char *error, size_t error_capacity);
bool ffi_unload_library(FFILibrary *library, char *error, size_t error_capacity);
bool ffi_extern_call(FFILibrary *library, const char *symbol,
                     const FFISignature *signature, const FFIValue *arguments,
                     FFIValue *result, char *error, size_t error_capacity);

#endif
