#define Py_BUILD_CORE_MODULE
#include "api.h"
#include "codecs.h"

#include <string.h>

/* The codec registry is not part of the public C API.  It is nevertheless
 * the state used by the CPython implementation that this adapter replaces.
 * Its spelling changed in 3.13, and these are the same internal headers used
 * by Python/codecs.c in the corresponding CPython releases. */
#include <internal/pycore_interp.h>
#undef Py_BUILD_CORE
#undef Py_BUILD_CORE_MODULE

typedef enum {
    CODEC_LOOKUP,
    CODEC_ENCODE,
    CODEC_DECODE,
    CODEC_REGISTER,
    CODEC_UNREGISTER,
    CODEC_REGISTER_ERROR,
    CODEC_LOOKUP_ERROR,
} CodecOperation;

typedef enum {
    CODEC_STAGE_NONE,
    CODEC_STAGE_SEARCH,
    CODEC_STAGE_COMPONENT,
    CODEC_STAGE_COMPONENT_ADAPTER,
} CodecStage;

typedef struct {
    PyObject *original;
    PyObject *arguments;
    PyObject *keywords;
    PyObject *object;
    PyObject *encoding;
    PyObject *errors;
    PyObject *encoding_key;
    CodecOperation operation;
    CodecStage stage;
    Py_ssize_t search_index;
    Py_ssize_t search_length;
} CodecCallState;

typedef struct {
    PyObject_HEAD
    PyObject *original;
} CodecSearchWrapper;

typedef struct {
    PyObject *wrapper;
    PyObject *original;
    PyObject *arguments;
    Py_ssize_t search_index;
    Py_ssize_t search_length;
} CodecSearchState;

typedef struct {
    PyObject *callable;
    PyObject *arguments;
    CodecOperation operation;
} CodecComponentState;

typedef struct {
    PyObject *module;
    const char *name;
    PyObject *original;
} CodecBackup;

static const AleffAdapterVTable codec_call_vtable;
static const AleffAdapterVTable codec_search_vtable;
static const AleffAdapterVTable codec_component_vtable;
static PyTypeObject CodecSearchWrapperType;

static PyObject *installed_codecs;
static PyObject *installed_private_codecs;
static PyObject *installed_search_backups;
static CodecBackup backups[16];
static PyMethodDef replacement_methods[16];
static Py_ssize_t backup_count;
static int codecs_installed;

static int codec_wrap_existing_search_functions(void);

static PyObject *
codec_search_path(void)
{
    PyInterpreterState *interp = PyInterpreterState_Get();
#if PY_VERSION_HEX >= 0x030d0000
    return interp->codecs.search_path;
#else
    return interp->codec_search_path;
#endif
}

static PyObject *
codec_search_cache(void)
{
    PyInterpreterState *interp = PyInterpreterState_Get();
#if PY_VERSION_HEX >= 0x030d0000
    return interp->codecs.search_cache;
#else
    return interp->codec_search_cache;
#endif
}

static void
codec_search_path_lock(void)
{
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030d0000
    PyMutex_Lock(&PyInterpreterState_Get()->codecs.search_path_mutex);
#endif
}

static void
codec_search_path_unlock(void)
{
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030d0000
    PyMutex_Unlock(&PyInterpreterState_Get()->codecs.search_path_mutex);
#endif
}

static PyObject *
codec_search_item(PyObject *path, Py_ssize_t index)
{
#if PY_VERSION_HEX >= 0x030d0000
    return PyList_GetItemRef(path, index);
#else
    PyObject *item = PyList_GetItem(path, index);
    return Py_XNewRef(item);
#endif
}

static void
codec_call_clear_state(CodecCallState *state)
{
    Py_XDECREF(state->original);
    Py_XDECREF(state->arguments);
    Py_XDECREF(state->keywords);
    Py_XDECREF(state->object);
    Py_XDECREF(state->encoding);
    Py_XDECREF(state->errors);
    Py_XDECREF(state->encoding_key);
    state->original = NULL;
    state->arguments = NULL;
    state->keywords = NULL;
    state->object = NULL;
    state->encoding = NULL;
    state->errors = NULL;
    state->encoding_key = NULL;
}

static void *
codec_call_copy_state(const void *raw_state)
{
    const CodecCallState *source = raw_state;
    CodecCallState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->original = Py_NewRef(source->original);
    copy->arguments = Py_NewRef(source->arguments);
    copy->keywords = Py_XNewRef(source->keywords);
    copy->object = Py_XNewRef(source->object);
    copy->encoding = Py_XNewRef(source->encoding);
    copy->errors = Py_XNewRef(source->errors);
    copy->encoding_key = Py_XNewRef(source->encoding_key);
    copy->operation = source->operation;
    copy->stage = source->stage;
    copy->search_index = source->search_index;
    copy->search_length = source->search_length;
    return copy;
}

static void
codec_call_free_state(void *raw_state)
{
    CodecCallState *state = raw_state;
    if (state == NULL) {
        return;
    }
    codec_call_clear_state(state);
    PyMem_Free(state);
}

static void *
codec_search_copy_state(const void *raw_state)
{
    const CodecSearchState *source = raw_state;
    CodecSearchState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->wrapper = Py_NewRef(source->wrapper);
    copy->original = Py_NewRef(source->original);
    copy->arguments = Py_NewRef(source->arguments);
    copy->search_index = source->search_index;
    copy->search_length = source->search_length;
    return copy;
}

static void
codec_search_clear_state(CodecSearchState *state)
{
    Py_XDECREF(state->wrapper);
    Py_XDECREF(state->original);
    Py_XDECREF(state->arguments);
}

static void
codec_search_free_state(void *raw_state)
{
    CodecSearchState *state = raw_state;
    if (state == NULL) {
        return;
    }
    codec_search_clear_state(state);
    PyMem_Free(state);
}

static void *
codec_component_copy_state(const void *raw_state)
{
    const CodecComponentState *source = raw_state;
    CodecComponentState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->callable = Py_NewRef(source->callable);
    copy->arguments = Py_NewRef(source->arguments);
    copy->operation = source->operation;
    return copy;
}

static void
codec_component_free_state(void *raw_state)
{
    CodecComponentState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->callable);
    Py_XDECREF(state->arguments);
    PyMem_Free(state);
}

static PyObject *
codec_search_continue(CodecSearchState *state, PyObject *result);

static PyObject *
codec_search_call_next(
    CodecSearchState *source,
    PyObject *function,
    Py_ssize_t index
)
{
    CodecSearchState *working = codec_search_copy_state(source);
    if (working == NULL) {
        return NULL;
    }
    working->search_index = index;
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_search_vtable, working) < 0) {
        codec_search_free_state(working);
        return NULL;
    }
    PyObject *result = PyObject_CallOneArg(
        function, PyTuple_GET_ITEM(source->arguments, 0)
    );
    adapter_leave(&frame);
    codec_search_free_state(working);
    return result;
}

static PyObject *
codec_search_continue(CodecSearchState *state, PyObject *result)
{
    if (result != Py_None) {
        if (!PyTuple_Check(result) || PyTuple_GET_SIZE(result) != 4) {
            PyErr_SetString(
                PyExc_TypeError,
                "codec search functions must return 4-tuples"
            );
            return NULL;
        }
        return Py_NewRef(result);
    }

    PyObject *path = codec_search_path();
    if (path == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "codec registry is not initialized");
        return NULL;
    }
    for (Py_ssize_t index = state->search_index + 1;
         index < state->search_length;
         index++) {
        PyObject *function = codec_search_item(path, index);
        if (function == NULL) {
            return NULL;
        }
        PyObject *next = codec_search_call_next(state, function, index);
        Py_DECREF(function);
        if (next == NULL) {
            return NULL;
        }
        if (next == Py_None) {
            Py_DECREF(next);
            continue;
        }
        if (!PyTuple_Check(next) || PyTuple_GET_SIZE(next) != 4) {
            Py_DECREF(next);
            PyErr_SetString(
                PyExc_TypeError,
                "codec search functions must return 4-tuples"
            );
            return NULL;
        }
        return next;
    }

    const char *name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(state->arguments, 0));
    if (name == NULL) {
        return NULL;
    }
    PyErr_Format(PyExc_LookupError, "unknown encoding: %s", name);
    return NULL;
}

static PyObject *
codec_search_resume(const void *raw_state, PyObject *value)
{
    CodecSearchState *state = codec_search_copy_state(raw_state);
    if (state == NULL || value == NULL) {
        codec_search_free_state(state);
        return NULL;
    }
    PyObject *result = codec_search_continue(state, value);
    codec_search_free_state(state);
    return result;
}

static PyObject *
codec_component_resume(const void *raw_state, PyObject *value)
{
    const CodecComponentState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(value) || PyTuple_GET_SIZE(value) != 2) {
        if (state->operation == CODEC_ENCODE) {
            PyErr_SetString(
                PyExc_TypeError,
                "encoder must return a tuple (object, integer)"
            );
        }
        else {
            PyErr_SetString(
                PyExc_TypeError,
                "decoder must return a tuple (object,integer)"
            );
        }
        return NULL;
    }
    return Py_NewRef(PyTuple_GET_ITEM(value, 0));
}

static const AleffAdapterVTable codec_search_vtable = {
    .copy_state = codec_search_copy_state,
    .free_state = codec_search_free_state,
    .resume = codec_search_resume,
    .prepare_resume = NULL,
};

static const AleffAdapterVTable codec_component_vtable = {
    .copy_state = codec_component_copy_state,
    .free_state = codec_component_free_state,
    .resume = codec_component_resume,
    .prepare_resume = NULL,
};

static PyObject *
codec_find_original(PyObject *module, const char *name)
{
    for (Py_ssize_t index = 0; index < backup_count; index++) {
        CodecBackup *backup = &backups[index];
        if (backup->module == module && strcmp(backup->name, name) == 0) {
            return Py_NewRef(backup->original);
        }
    }
    PyErr_Format(PyExc_RuntimeError, "missing codecs adapter function %s", name);
    return NULL;
}

static int
codec_normalize(PyObject *encoding, PyObject **key)
{
    const char *source = PyUnicode_AsUTF8(encoding);
    if (source == NULL) {
        return -1;
    }
    size_t length = strlen(source);
    char *normalized = PyMem_Malloc(length + 1);
    if (normalized == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    char *target = normalized;
    int punctuation = 0;
    for (const unsigned char *cursor = (const unsigned char *)source;
         *cursor != '\0'; cursor++) {
        unsigned char character = *cursor;
        int alphanumeric =
            (character >= '0' && character <= '9') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z');
        if (alphanumeric || character == '.') {
            if (punctuation && target != normalized) {
                *target++ = '_';
            }
            punctuation = 0;
            *target++ = character >= 'A' && character <= 'Z'
                ? (char)(character + ('a' - 'A')) : (char)character;
        }
        else {
            punctuation = 1;
        }
    }
    *target = '\0';
    *key = PyUnicode_FromString(normalized);
    PyMem_Free(normalized);
    if (*key == NULL) {
        return -1;
    }
    PyUnicode_InternInPlace(key);
    return 0;
}

static int
codec_prepare_call(CodecCallState *state)
{
    PyObject *object = NULL;
    PyObject *encoding = NULL;
    PyObject *errors = NULL;
    PyObject *key = NULL;
    PyObject *default_encoding;
    static char *keywords[] = {"obj", "encoding", "errors", NULL};

    if (state->operation == CODEC_LOOKUP) {
        if (!PyArg_ParseTuple(state->arguments, "U:lookup", &encoding)) {
            return -1;
        }
        state->encoding = Py_NewRef(encoding);
    }
    else if (state->operation == CODEC_ENCODE ||
             state->operation == CODEC_DECODE) {
        if (!PyArg_ParseTupleAndKeywords(
                state->arguments,
                state->keywords,
                "O|OO",
                keywords,
                &object,
                &encoding,
                &errors
            )) {
            return -1;
        }
        if (encoding == NULL || encoding == Py_None) {
            default_encoding = PyUnicode_FromString(PyUnicode_GetDefaultEncoding());
            if (default_encoding == NULL) {
                return -1;
            }
            encoding = default_encoding;
        }
        else {
            Py_INCREF(encoding);
            default_encoding = encoding;
        }
        if (!PyUnicode_Check(encoding)) {
            Py_DECREF(default_encoding);
            PyErr_SetString(PyExc_TypeError, "encoding must be a string");
            return -1;
        }
        state->object = Py_NewRef(object);
        state->encoding = default_encoding;
        if (errors != NULL && errors != Py_None) {
            state->errors = Py_NewRef(errors);
            if (!PyUnicode_Check(state->errors)) {
                codec_call_clear_state(state);
                PyErr_SetString(PyExc_TypeError, "errors must be a string");
                return -1;
            }
        }
    }
    else {
        return 0;
    }

    if (codec_normalize(state->encoding, &key) < 0) {
        return -1;
    }
    state->encoding_key = key;
    if (state->operation == CODEC_LOOKUP) {
        state->stage = CODEC_STAGE_SEARCH;
    }
    else {
        PyObject *cache = codec_search_cache();
        state->stage = cache != NULL &&
            PyDict_GetItemWithError(cache, state->encoding_key) != NULL
            ? CODEC_STAGE_COMPONENT : CODEC_STAGE_SEARCH;
        if (PyErr_Occurred()) {
            return -1;
        }
    }
    return 0;
}

static int
codec_validate_search_result(PyObject *result)
{
    if (result == Py_None) {
        return 0;
    }
    if (!PyTuple_Check(result) || PyTuple_GET_SIZE(result) != 4) {
        PyErr_SetString(
            PyExc_TypeError,
            "codec search functions must return 4-tuples"
        );
        return -1;
    }
    return 1;
}

static PyObject *
codec_cache_result(CodecCallState *state, PyObject *result)
{
    PyObject *cache = codec_search_cache();
    if (cache == NULL || state->encoding_key == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "codec registry is not initialized");
        return NULL;
    }
    if (PyDict_SetItem(cache, state->encoding_key, result) < 0) {
        return NULL;
    }
    return Py_NewRef(result);
}

static PyObject *codec_continue_search(CodecCallState *state, PyObject *result);

static PyObject *
codec_component_result(CodecCallState *state, PyObject *result)
{
    if (!PyTuple_Check(result) || PyTuple_GET_SIZE(result) != 2) {
        if (state->operation == CODEC_ENCODE) {
            PyErr_SetString(
                PyExc_TypeError,
                "encoder must return a tuple (object, integer)"
            );
        }
        else {
            PyErr_SetString(
                PyExc_TypeError,
                "decoder must return a tuple (object,integer)"
            );
        }
        return NULL;
    }
    return Py_NewRef(PyTuple_GET_ITEM(result, 0));
}

static PyObject *
codec_component_arguments(const CodecCallState *state)
{
    Py_ssize_t count = state->errors == NULL ? 1 : 2;
    PyObject *arguments = PyTuple_New(count);
    if (arguments == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(arguments, 0, Py_NewRef(state->object));
    if (state->errors != NULL) {
        PyTuple_SET_ITEM(arguments, 1, Py_NewRef(state->errors));
    }
    return arguments;
}

static PyObject *
codec_call_component(CodecCallState *source, PyObject *codec_info)
{
    Py_ssize_t index = source->operation == CODEC_ENCODE ? 0 : 1;
    PyObject *callable = Py_NewRef(PyTuple_GET_ITEM(codec_info, index));
    PyObject *arguments = codec_component_arguments(source);
    if (arguments == NULL) {
        Py_DECREF(callable);
        return NULL;
    }

    CodecCallState *working = codec_call_copy_state(source);
    if (working == NULL) {
        Py_DECREF(callable);
        Py_DECREF(arguments);
        return NULL;
    }
    working->stage = CODEC_STAGE_COMPONENT_ADAPTER;

    CodecComponentState component = {
        .callable = callable,
        .arguments = arguments,
        .operation = source->operation,
    };
    AleffAdapterFrame outer_frame;
    AleffAdapterFrame component_frame;
    if (adapter_enter(&outer_frame, &codec_call_vtable, working) < 0) {
        codec_call_free_state(working);
        Py_DECREF(callable);
        Py_DECREF(arguments);
        return NULL;
    }
    if (adapter_enter(&component_frame, &codec_component_vtable, &component) < 0) {
        adapter_leave(&outer_frame);
        codec_call_free_state(working);
        Py_DECREF(callable);
        Py_DECREF(arguments);
        return NULL;
    }
    PyObject *result = PyObject_Call(callable, arguments, NULL);
    adapter_leave(&component_frame);
    adapter_leave(&outer_frame);
    Py_DECREF(callable);
    Py_DECREF(arguments);

    if (result == NULL) {
        codec_call_free_state(working);
        return NULL;
    }
    PyObject *final = codec_component_result(source, result);
    Py_DECREF(result);
    codec_call_free_state(working);
    return final;
}

static PyObject *
codec_call_search(CodecCallState *source, PyObject *function, Py_ssize_t index)
{
    CodecCallState *working = codec_call_copy_state(source);
    if (working == NULL) {
        return NULL;
    }
    working->stage = CODEC_STAGE_SEARCH;
    working->search_index = index;

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_call_vtable, working) < 0) {
        codec_call_free_state(working);
        return NULL;
    }
    PyObject *result = PyObject_CallOneArg(function, source->encoding_key);
    adapter_leave(&frame);
    codec_call_free_state(working);
    return result;
}

static PyObject *
codec_continue_search(CodecCallState *state, PyObject *result)
{
    PyObject *path = codec_search_path();
    if (path == NULL || state->encoding_key == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "codec registry is not initialized");
        return NULL;
    }
    if (state->search_index < 0) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "codec search continuation has no callback state"
        );
        return NULL;
    }

    int valid = codec_validate_search_result(result);
    if (valid < 0) {
        return NULL;
    }
    if (valid > 0) {
        if (state->operation == CODEC_LOOKUP) {
            return codec_cache_result(state, result);
        }
        if (state->operation == CODEC_ENCODE ||
            state->operation == CODEC_DECODE) {
            if (PyDict_SetItem(codec_search_cache(), state->encoding_key, result) < 0) {
                return NULL;
            }
            return codec_call_component(state, result);
        }
        return Py_NewRef(result);
    }

    Py_ssize_t length = state->search_length;
    if (length < 0) {
        length = PyList_Size(path);
        if (length < 0) {
            return NULL;
        }
    }
    for (Py_ssize_t index = state->search_index + 1; index < length; index++) {
        PyObject *function = codec_search_item(path, index);
        if (function == NULL) {
            return NULL;
        }
        PyObject *next = codec_call_search(state, function, index);
        Py_DECREF(function);
        if (next == NULL) {
            return NULL;
        }
        valid = codec_validate_search_result(next);
        if (valid < 0) {
            Py_DECREF(next);
            return NULL;
        }
        if (valid > 0) {
            PyObject *cached = codec_cache_result(state, next);
            Py_DECREF(next);
            if (cached == NULL) {
                return NULL;
            }
            if (state->operation == CODEC_ENCODE ||
                state->operation == CODEC_DECODE) {
                PyObject *component = codec_call_component(state, cached);
                Py_DECREF(cached);
                return component;
            }
            return cached;
        }
        Py_DECREF(next);
    }

    const char *encoding = PyUnicode_AsUTF8(state->encoding);
    if (encoding == NULL) {
        return NULL;
    }
    PyErr_Format(PyExc_LookupError, "unknown encoding: %s", encoding);
    return NULL;
}

static void
codec_add_component_note(const CodecCallState *state)
{
    if (!PyErr_Occurred() || state->encoding == NULL) {
        return;
    }
    const char *encoding = PyUnicode_AsUTF8(state->encoding);
    if (encoding == NULL) {
        PyErr_Clear();
        return;
    }
    const char *kind = state->operation == CODEC_ENCODE ? "encoding" : "decoding";
    PyObject *exception = PyErr_GetRaisedException();
    if (exception == NULL) {
        return;
    }
    PyObject *note = PyUnicode_FromFormat(
        "%s with '%s' codec failed", kind, encoding
    );
    PyObject *added = note == NULL
        ? NULL : PyObject_CallMethod(exception, "add_note", "O", note);
    if (added == NULL) {
        PyErr_Clear();
    }
    else {
        Py_DECREF(added);
    }
    Py_XDECREF(note);
    PyErr_SetRaisedException(exception);
}

static PyObject *
codec_call_resume(const void *raw_state, PyObject *value)
{
    const CodecCallState *source = raw_state;
    if (value == NULL) {
        PyObject *cache = source->operation == CODEC_ENCODE ||
            source->operation == CODEC_DECODE
            ? codec_search_cache() : NULL;
        int component_failed = source->stage == CODEC_STAGE_COMPONENT ||
            source->stage == CODEC_STAGE_COMPONENT_ADAPTER ||
            (cache != NULL && source->encoding_key != NULL &&
             PyDict_GetItem(cache, source->encoding_key) != NULL);
        if (component_failed) {
            codec_add_component_note(source);
        }
        return NULL;
    }

    if (source->stage == CODEC_STAGE_COMPONENT_ADAPTER) {
        return Py_NewRef(value);
    }

    CodecCallState *state = codec_call_copy_state(source);
    if (state == NULL) {
        return NULL;
    }

    if (source->operation == CODEC_LOOKUP) {
        if (source->stage == CODEC_STAGE_SEARCH &&
            (!PyTuple_Check(value) || PyTuple_GET_SIZE(value) != 4)) {
            PyObject *continued = codec_continue_search(state, value);
            codec_call_free_state(state);
            return continued;
        }
        PyObject *continued = codec_cache_result(state, value);
        codec_call_free_state(state);
        return continued;
    }
    if (source->operation == CODEC_ENCODE ||
        source->operation == CODEC_DECODE) {
        PyObject *continued;
        PyObject *cache = codec_search_cache();
        if (source->stage == CODEC_STAGE_SEARCH &&
            PyTuple_Check(value) && PyTuple_GET_SIZE(value) == 4) {
            if (cache == NULL || state->encoding_key == NULL) {
                codec_call_free_state(state);
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "codec registry is not initialized"
                );
                return NULL;
            }
            if (PyDict_SetItem(
                    cache, state->encoding_key, value
                ) < 0) {
                codec_call_free_state(state);
                return NULL;
            }
            continued = codec_call_component(state, value);
        }
        else {
            continued = codec_component_result(state, value);
        }
        codec_call_free_state(state);
        return continued;
    }
    codec_call_free_state(state);
    return Py_NewRef(value);
}

static const AleffAdapterVTable codec_call_vtable = {
    .copy_state = codec_call_copy_state,
    .free_state = codec_call_free_state,
    .resume = codec_call_resume,
    .prepare_resume = NULL,
};

static int
codec_search_wrapper_index(
    PyObject *wrapper,
    Py_ssize_t *search_index,
    Py_ssize_t *search_length
)
{
    PyObject *path = codec_search_path();
    if (path == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "codec registry is not initialized");
        return -1;
    }
    Py_ssize_t length = PyList_Size(path);
    if (length < 0) {
        return -1;
    }
    for (Py_ssize_t index = 0; index < length; index++) {
        PyObject *item = codec_search_item(path, index);
        if (item == NULL) {
            return -1;
        }
        int match = item == wrapper;
        Py_DECREF(item);
        if (match) {
            *search_index = index;
            *search_length = length;
            return 0;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "codec search wrapper is not registered");
    return -1;
}

static PyObject *
codec_search_wrapper_call(PyObject *self, PyObject *arguments, PyObject *keywords)
{
    CodecSearchWrapper *wrapper = (CodecSearchWrapper *)self;
    Py_ssize_t search_index;
    Py_ssize_t search_length;
    if (codec_search_wrapper_index(self, &search_index, &search_length) < 0) {
        return NULL;
    }
    CodecSearchState state = {
        .wrapper = Py_NewRef(self),
        .original = Py_NewRef(wrapper->original),
        .arguments = Py_NewRef(arguments),
        .search_index = search_index,
        .search_length = search_length,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_search_vtable, &state) < 0) {
        codec_search_clear_state(&state);
        return NULL;
    }
    PyObject *result = PyObject_Call(state.original, state.arguments, keywords);
    adapter_leave(&frame);
    codec_search_clear_state(&state);
    return result;
}

static void
codec_search_wrapper_dealloc(PyObject *object)
{
    CodecSearchWrapper *wrapper = (CodecSearchWrapper *)object;
    Py_XDECREF(wrapper->original);
    Py_TYPE(object)->tp_free(object);
}

static PyTypeObject CodecSearchWrapperType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_aleff.CodecSearchWrapper",
    .tp_basicsize = sizeof(CodecSearchWrapper),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_call = codec_search_wrapper_call,
    .tp_dealloc = codec_search_wrapper_dealloc,
};

static PyObject *
codec_make_search_wrapper(PyObject *original)
{
    if (PyObject_TypeCheck(original, &CodecSearchWrapperType)) {
        return Py_NewRef(original);
    }
    CodecSearchWrapper *wrapper = PyObject_New(
        CodecSearchWrapper, &CodecSearchWrapperType
    );
    if (wrapper == NULL) {
        return NULL;
    }
    wrapper->original = Py_NewRef(original);
    return (PyObject *)wrapper;
}

static PyObject *
codec_find_search_wrapper(PyObject *original)
{
    PyObject *path = codec_search_path();
    if (path == NULL) {
        return NULL;
    }
    Py_ssize_t length = PyList_Size(path);
    if (length < 0) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < length; index++) {
        PyObject *item = codec_search_item(path, index);
        if (item == NULL) {
            return NULL;
        }
        if (PyObject_TypeCheck(item, &CodecSearchWrapperType) &&
            ((CodecSearchWrapper *)item)->original == original) {
            return item;
        }
        Py_DECREF(item);
    }
    return NULL;
}

static PyObject *
codec_call_function(
    PyObject *module,
    PyObject *args,
    PyObject *keywords,
    const char *name,
    CodecOperation operation
)
{
    PyObject *original = codec_find_original(module, name);
    if (original == NULL) {
        return NULL;
    }
    if ((operation == CODEC_LOOKUP || operation == CODEC_ENCODE ||
         operation == CODEC_DECODE) &&
        codec_wrap_existing_search_functions() < 0) {
        Py_DECREF(original);
        return NULL;
    }
    CodecCallState state = {
        .original = original,
        .arguments = Py_NewRef(args),
        .keywords = Py_XNewRef(keywords),
        .operation = operation,
        .stage = CODEC_STAGE_NONE,
        .search_index = -1,
        .search_length = -1,
    };
    if ((operation == CODEC_LOOKUP || operation == CODEC_ENCODE ||
         operation == CODEC_DECODE) && codec_prepare_call(&state) < 0) {
        /* The original C function remains the authority for argument errors.
         * A suspended call necessarily passed this validation already. */
        PyErr_Clear();
        codec_call_clear_state(&state);
        state.original = Py_NewRef(original);
        state.arguments = Py_NewRef(args);
        state.keywords = Py_XNewRef(keywords);
        state.operation = operation;
        state.stage = CODEC_STAGE_NONE;
        state.search_index = -1;
        state.search_length = -1;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_call_vtable, &state) < 0) {
        codec_call_clear_state(&state);
        return NULL;
    }
    PyObject *result = PyObject_Call(state.original, state.arguments, state.keywords);
    adapter_leave(&frame);
    codec_call_clear_state(&state);
    return result;
}

static PyObject *
codec_register(PyObject *self, PyObject *args, PyObject *keywords)
{
    if (keywords != NULL && PyDict_Size(keywords) != 0) {
        return codec_call_function(self, args, keywords, "register", CODEC_REGISTER);
    }
    if (PyTuple_GET_SIZE(args) != 1) {
        return codec_call_function(self, args, keywords, "register", CODEC_REGISTER);
    }
    PyObject *original = PyTuple_GET_ITEM(args, 0);
    if (!PyCallable_Check(original)) {
        return codec_call_function(self, args, keywords, "register", CODEC_REGISTER);
    }
    PyObject *wrapper = codec_make_search_wrapper(original);
    if (wrapper == NULL) {
        return NULL;
    }
    PyObject *call_args = PyTuple_Pack(1, wrapper);
    Py_DECREF(wrapper);
    if (call_args == NULL) {
        return NULL;
    }
    PyObject *result = codec_call_function(
        self, call_args, NULL, "register", CODEC_REGISTER
    );
    Py_DECREF(call_args);
    return result;
}

static PyObject *
codec_unregister(PyObject *self, PyObject *args, PyObject *keywords)
{
    if ((keywords != NULL && PyDict_Size(keywords) != 0) ||
        PyTuple_GET_SIZE(args) != 1) {
        return codec_call_function(
            self, args, keywords, "unregister", CODEC_UNREGISTER
        );
    }
    PyObject *original = PyTuple_GET_ITEM(args, 0);
    PyObject *wrapper = codec_find_search_wrapper(original);
    if (wrapper == NULL && PyErr_Occurred()) {
        return NULL;
    }
    if (wrapper == NULL) {
        return codec_call_function(
            self, args, keywords, "unregister", CODEC_UNREGISTER
        );
    }
    PyObject *call_args = PyTuple_Pack(1, wrapper);
    Py_DECREF(wrapper);
    if (call_args == NULL) {
        return NULL;
    }
    PyObject *result = codec_call_function(
        self, call_args, NULL, "unregister", CODEC_UNREGISTER
    );
    Py_DECREF(call_args);
    return result;
}

#define CODEC_WRAPPER(name, operation) \
    static PyObject *codec_##name(PyObject *self, PyObject *args, PyObject *kwargs) \
    { \
        return codec_call_function(self, args, kwargs, #name, operation); \
    }

CODEC_WRAPPER(lookup, CODEC_LOOKUP)
CODEC_WRAPPER(encode, CODEC_ENCODE)
CODEC_WRAPPER(decode, CODEC_DECODE)
CODEC_WRAPPER(register_error, CODEC_REGISTER_ERROR)
CODEC_WRAPPER(lookup_error, CODEC_LOOKUP_ERROR)

static PyCFunction codec_functions[] = {
    _PyCFunction_CAST(codec_lookup),
    _PyCFunction_CAST(codec_encode),
    _PyCFunction_CAST(codec_decode),
    _PyCFunction_CAST(codec_register),
    _PyCFunction_CAST(codec_unregister),
    _PyCFunction_CAST(codec_register_error),
    _PyCFunction_CAST(codec_lookup_error),
};

static const char *const codec_names[] = {
    "lookup", "encode", "decode", "register", "unregister",
    "register_error", "lookup_error",
};

static int
codec_replace_function(PyObject *module, const char *name, PyCFunction function)
{
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        return -1;
    }
    if (!PyCFunction_Check(original)) {
        Py_DECREF(original);
        PyErr_Format(PyExc_RuntimeError, "codecs.%s is not a C function", name);
        return -1;
    }
    if (backup_count >= (Py_ssize_t)(sizeof(backups) / sizeof(*backups))) {
        Py_DECREF(original);
        PyErr_SetString(PyExc_RuntimeError, "too many codecs adapter functions");
        return -1;
    }
    CodecBackup *backup = &backups[backup_count];
    backup->module = Py_NewRef(module);
    backup->name = name;
    backup->original = original;
    PyMethodDef *replacement = &replacement_methods[backup_count];
    *replacement = *((PyCFunctionObject *)original)->m_ml;
    replacement->ml_name = name;
    replacement->ml_meth = function;
    replacement->ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    PyObject *function_object = module_name == NULL ? NULL : PyCFunction_NewEx(
        replacement,
        PyCFunction_GET_SELF(original),
        module_name
    );
    Py_XDECREF(module_name);
    if (function_object == NULL ||
        aleff_adapter_register_callable(function_object) < 0 ||
        PyObject_SetAttrString(module, name, function_object) < 0) {
        Py_XDECREF(function_object);
        Py_CLEAR(backup->module);
        Py_CLEAR(backup->original);
        return -1;
    }
    Py_DECREF(function_object);
    backup_count++;
    return 0;
}

static int
codec_wrap_existing_search_functions(void)
{
    PyObject *path = codec_search_path();
    if (path == NULL) {
        return 0;
    }
    if (PyType_Ready(&CodecSearchWrapperType) < 0) {
        return -1;
    }
    if (installed_search_backups == NULL) {
        installed_search_backups = PyList_New(0);
        if (installed_search_backups == NULL) {
            return -1;
        }
    }
    Py_ssize_t length = PyList_Size(path);
    if (length < 0) {
        return -1;
    }
    for (Py_ssize_t index = 0; index < length; index++) {
        PyObject *original = codec_search_item(path, index);
        if (original == NULL) {
            return -1;
        }
        if (PyObject_TypeCheck(original, &CodecSearchWrapperType)) {
            Py_DECREF(original);
            continue;
        }
        PyObject *wrapper = codec_make_search_wrapper(original);
        if (wrapper == NULL) {
            Py_DECREF(original);
            return -1;
        }
        PyObject *backup = PyTuple_Pack(2, wrapper, original);
        if (backup == NULL) {
            Py_DECREF(wrapper);
            Py_DECREF(original);
            return -1;
        }
        if (PyList_Append(installed_search_backups, backup) < 0) {
            Py_DECREF(backup);
            Py_DECREF(wrapper);
            Py_DECREF(original);
            return -1;
        }
        Py_DECREF(backup);
        codec_search_path_lock();
        int set_result = PyList_SetItem(path, index, wrapper);
        codec_search_path_unlock();
        if (set_result < 0) {
            Py_DECREF(original);
            return -1;
        }
        Py_DECREF(original);
    }
    return 0;
}

static void
codec_restore_search_functions(void)
{
    PyObject *path = codec_search_path();
    if (path != NULL && installed_search_backups != NULL) {
        for (Py_ssize_t backup_index = 0;
             backup_index < PyList_GET_SIZE(installed_search_backups);
             backup_index++) {
            PyObject *backup = PyList_GET_ITEM(
                installed_search_backups, backup_index
            );
            PyObject *wrapper = PyTuple_GET_ITEM(backup, 0);
            PyObject *original = PyTuple_GET_ITEM(backup, 1);
            Py_ssize_t length = PyList_Size(path);
            if (length < 0) {
                PyErr_Clear();
                continue;
            }
            for (Py_ssize_t index = 0; index < length; index++) {
                PyObject *item = codec_search_item(path, index);
                if (item == NULL) {
                    PyErr_Clear();
                    break;
                }
                int match = item == wrapper;
                Py_DECREF(item);
                if (match) {
                    Py_INCREF(original);
                    codec_search_path_lock();
                    int set_result = PyList_SetItem(path, index, original);
                    codec_search_path_unlock();
                    if (set_result < 0) {
                        PyErr_Clear();
                    }
                    break;
                }
            }
        }
    }
    Py_CLEAR(installed_search_backups);
}

int
adapter_codecs_install(PyObject *codecs_module)
{
    if (codecs_installed) {
        return 0;
    }
    installed_codecs = Py_NewRef(codecs_module);
    installed_private_codecs = PyImport_ImportModule("_codecs");
    if (installed_private_codecs == NULL) {
        adapter_codecs_rollback();
        return -1;
    }
    for (int module_index = 0; module_index < 2; module_index++) {
        PyObject *module = module_index == 0
            ? installed_codecs : installed_private_codecs;
        for (int function_index = 0; function_index < 7; function_index++) {
            if (codec_replace_function(
                    module,
                    codec_names[function_index],
                    codec_functions[function_index]
                ) < 0) {
                adapter_codecs_rollback();
                return -1;
            }
        }
    }
    if (codec_wrap_existing_search_functions() < 0) {
        adapter_codecs_rollback();
        return -1;
    }
    codecs_installed = 1;
    return 0;
}

void
adapter_codecs_rollback(void)
{
    codec_restore_search_functions();
    for (Py_ssize_t index = backup_count - 1; index >= 0; index--) {
        CodecBackup *backup = &backups[index];
        if (backup->module != NULL && backup->original != NULL &&
            PyObject_SetAttrString(backup->module, backup->name, backup->original) < 0) {
            PyErr_Clear();
        }
        Py_CLEAR(backup->module);
        Py_CLEAR(backup->original);
    }
    backup_count = 0;
    Py_CLEAR(installed_private_codecs);
    Py_CLEAR(installed_codecs);
    codecs_installed = 0;
}
