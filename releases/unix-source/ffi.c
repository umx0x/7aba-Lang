#include "ffi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct FFILibrary
{
#ifdef _WIN32
    HMODULE handle;
#else
    void *handle;
#endif
};

typedef int64_t (*FnI64_0)(void);
typedef int64_t (*FnI64_I64)(int64_t);
typedef int64_t (*FnI64_I64I64)(int64_t, int64_t);
typedef double (*FnF64_0)(void);
typedef double (*FnF64_F64)(double);
typedef double (*FnF64_F64F64)(double, double);

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity > 0)
        snprintf(error, capacity, "%s", message);
}

FFILibrary *ffi_load_library(const char *path, char *error, size_t error_capacity)
{
    if (!path || !*path)
    {
        set_error(error, error_capacity, "Library path is empty.");
        return NULL;
    }
    FFILibrary *library = malloc(sizeof(*library));
    if (!library)
    {
        set_error(error, error_capacity, "Unable to allocate library handle.");
        return NULL;
    }
#ifdef _WIN32
    library->handle = LoadLibraryA(path);
    if (!library->handle)
    {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "LoadLibrary failed with error %lu.",
                     (unsigned long)GetLastError());
        free(library);
        return NULL;
    }
#else
    library->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library->handle)
    {
        const char *detail = dlerror();
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "dlopen failed: %s", detail ? detail : "unknown error");
        free(library);
        return NULL;
    }
#endif
    return library;
}

bool ffi_unload_library(FFILibrary *library, char *error, size_t error_capacity)
{
    if (!library)
        return true;
#ifdef _WIN32
    bool success = FreeLibrary(library->handle) != 0;
    if (!success && error && error_capacity > 0)
        snprintf(error, error_capacity, "FreeLibrary failed with error %lu.",
                 (unsigned long)GetLastError());
#else
    bool success = dlclose(library->handle) == 0;
    if (!success)
    {
        const char *detail = dlerror();
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "dlclose failed: %s", detail ? detail : "unknown error");
    }
#endif
    free(library);
    return success;
}

static void *lookup_symbol(FFILibrary *library, const char *name,
                           char *error, size_t error_capacity)
{
#ifdef _WIN32
    FARPROC symbol = GetProcAddress(library->handle, name);
    if (!symbol && error && error_capacity > 0)
        snprintf(error, error_capacity, "GetProcAddress failed with error %lu.",
                 (unsigned long)GetLastError());
    return (void *)symbol;
#else
    dlerror();
    void *symbol = dlsym(library->handle, name);
    const char *detail = dlerror();
    if (detail)
    {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "dlsym failed: %s", detail);
        return NULL;
    }
    return symbol;
#endif
}

bool ffi_extern_call(FFILibrary *library, const char *symbol_name,
                     const FFISignature *signature, const FFIValue *arguments,
                     FFIValue *result, char *error, size_t error_capacity)
{
    if (!library || !symbol_name || !*symbol_name || !signature || !result ||
        signature->argument_count > 2 ||
        (signature->argument_count > 0 && !arguments))
    {
        set_error(error, error_capacity, "Invalid FFI call description.");
        return false;
    }
    for (size_t i = 0; i < signature->argument_count; i++)
        if (arguments[i].type != signature->arguments[i])
        {
            set_error(error, error_capacity, "FFI argument type does not match declared signature.");
            return false;
        }

    void *address = lookup_symbol(library, symbol_name, error, error_capacity);
    if (!address)
        return false;

    if (signature->result == FFI_I64)
    {
        FnI64_0 fn0 = NULL;
        FnI64_I64 fn1 = NULL;
        FnI64_I64I64 fn2 = NULL;
        if (signature->argument_count == 0)
        {
            if (sizeof(fn0) != sizeof(address))
            {
                set_error(error, error_capacity, "Platform function and object pointers have incompatible sizes.");
                return false;
            }
            memcpy(&fn0, &address, sizeof(fn0));
        }
        else if (signature->argument_count == 1 && signature->arguments[0] == FFI_I64)
        {
            if (sizeof(fn1) != sizeof(address))
            {
                set_error(error, error_capacity, "Platform function and object pointers have incompatible sizes.");
                return false;
            }
            memcpy(&fn1, &address, sizeof(fn1));
        }
        else if (signature->argument_count == 2 &&
                 signature->arguments[0] == FFI_I64 &&
                 signature->arguments[1] == FFI_I64)
        {
            if (sizeof(fn2) != sizeof(address))
            {
                set_error(error, error_capacity, "Platform function and object pointers have incompatible sizes.");
                return false;
            }
            memcpy(&fn2, &address, sizeof(fn2));
        }
        else
        {
            set_error(error, error_capacity, "Unsupported FFI signature for integer result.");
            return false;
        }
        result->type = FFI_I64;
        result->as.integer = signature->argument_count == 0 ? fn0()
                             : signature->argument_count == 1 ? fn1(arguments[0].as.integer)
                                                              : fn2(arguments[0].as.integer, arguments[1].as.integer);
        return true;
    }
    if (signature->result == FFI_F64)
    {
        FnF64_0 fn0 = NULL;
        FnF64_F64 fn1 = NULL;
        FnF64_F64F64 fn2 = NULL;
        if (signature->argument_count == 0)
        {
            if (sizeof(fn0) != sizeof(address))
            {
                set_error(error, error_capacity, "Platform function and object pointers have incompatible sizes.");
                return false;
            }
            memcpy(&fn0, &address, sizeof(fn0));
        }
        else if (signature->argument_count == 1 && signature->arguments[0] == FFI_F64)
        {
            if (sizeof(fn1) != sizeof(address))
            {
                set_error(error, error_capacity, "Platform function and object pointers have incompatible sizes.");
                return false;
            }
            memcpy(&fn1, &address, sizeof(fn1));
        }
        else if (signature->argument_count == 2 &&
                 signature->arguments[0] == FFI_F64 &&
                 signature->arguments[1] == FFI_F64)
        {
            if (sizeof(fn2) != sizeof(address))
            {
                set_error(error, error_capacity, "Platform function and object pointers have incompatible sizes.");
                return false;
            }
            memcpy(&fn2, &address, sizeof(fn2));
        }
        else
        {
            set_error(error, error_capacity, "Unsupported FFI signature for float result.");
            return false;
        }
        result->type = FFI_F64;
        result->as.floating = signature->argument_count == 0 ? fn0()
                               : signature->argument_count == 1 ? fn1(arguments[0].as.floating)
                                                                : fn2(arguments[0].as.floating, arguments[1].as.floating);
        return true;
    }
    set_error(error, error_capacity, "Unsupported FFI result type.");
    return false;
}
