#include "api.h"
#include "json_decoder.h"

#include "internal.h"

#include <stddef.h>
#include <structmember.h>
#include <string.h>

/*
 * This is deliberately a scanner, rather than a wrapper around the Python
 * scanner.  _json's scanner keeps the parse loop in C stack frames.  A Python
 * callback can suspend those frames, so calling the scanner again on resume
 * would either replay the callback or restart the parse.  The scanner below
 * keeps the cursor and the parse loop in explicit, adapter-owned state.
 *
 * The implementation follows the public behaviour of Modules/_json.c in
 * CPython 3.12--3.14.  CPython's license notice for the corresponding
 * implementation is included in LICENSES/CPython.txt.
 */

#define JSON_SCANNER_NAME "_json.Scanner"

typedef enum {
    JSON_FRAME_ARRAY,
    JSON_FRAME_OBJECT,
} JsonFrameKind;

typedef enum {
    JSON_ARRAY_START,
    JSON_ARRAY_VALUE,
    JSON_ARRAY_AFTER_VALUE,
    JSON_OBJECT_START,
    JSON_OBJECT_KEY,
    JSON_OBJECT_COLON,
    JSON_OBJECT_VALUE,
    JSON_OBJECT_AFTER_VALUE,
    JSON_OBJECT_HOOK,
} JsonFramePhase;

typedef enum {
    JSON_WAIT_NONE,
    JSON_WAIT_NUMBER,
    JSON_WAIT_CONSTANT,
    JSON_WAIT_OBJECT_HOOK,
} JsonWaitPhase;

typedef struct {
    JsonFrameKind kind;
    JsonFramePhase phase;
    PyObject *container;
    PyObject *key;
    Py_ssize_t comma_index;
} JsonFrame;

typedef struct {
    PyObject_HEAD
    signed char strict;
    PyObject *object_hook;
    PyObject *object_pairs_hook;
    PyObject *parse_float;
    PyObject *parse_int;
    PyObject *parse_constant;
} JsonScannerObject;

typedef struct {
    JsonScannerObject *scanner;
    PyObject *input;
    PyObject *memo;
    JsonFrame *frames;
    Py_ssize_t depth;
    Py_ssize_t frame_capacity;
    Py_ssize_t index;
    PyObject *value;
    JsonWaitPhase wait;
} JsonDecoderState;

static const AleffAdapterVTable json_decoder_vtable;
static PyObject *installed_scanner_module;
static PyObject *original_make_scanner;
static PyMethodDef replacement_make_scanner_method;
static int json_decoder_installed;
static int json_scanner_type_ready;

static PyObject *json_scanner_call(
    JsonScannerObject *scanner,
    PyObject *args,
    PyObject *kwargs
);

static void
json_scanner_clear(JsonScannerObject *scanner)
{
    Py_CLEAR(scanner->object_hook);
    Py_CLEAR(scanner->object_pairs_hook);
    Py_CLEAR(scanner->parse_float);
    Py_CLEAR(scanner->parse_int);
    Py_CLEAR(scanner->parse_constant);
}

static int
json_scanner_traverse(JsonScannerObject *scanner, visitproc visit, void *arg)
{
    Py_VISIT(scanner->object_hook);
    Py_VISIT(scanner->object_pairs_hook);
    Py_VISIT(scanner->parse_float);
    Py_VISIT(scanner->parse_int);
    Py_VISIT(scanner->parse_constant);
    return 0;
}

static int
json_scanner_clear_gc(PyObject *object)
{
    json_scanner_clear((JsonScannerObject *)object);
    return 0;
}

static void
json_scanner_dealloc(JsonScannerObject *scanner)
{
    PyObject_GC_UnTrack((PyObject *)scanner);
    json_scanner_clear(scanner);
    Py_TYPE(scanner)->tp_free((PyObject *)scanner);
}

static PyMemberDef json_scanner_members[] = {
    {"strict", Py_T_BOOL, offsetof(JsonScannerObject, strict), Py_READONLY,
     "strict"},
    {"object_hook", T_OBJECT, offsetof(JsonScannerObject, object_hook),
     Py_READONLY, "object_hook"},
    {"object_pairs_hook", T_OBJECT,
     offsetof(JsonScannerObject, object_pairs_hook), Py_READONLY,
     "object_pairs_hook"},
    {"parse_float", T_OBJECT, offsetof(JsonScannerObject, parse_float),
     Py_READONLY, "parse_float"},
    {"parse_int", T_OBJECT, offsetof(JsonScannerObject, parse_int),
     Py_READONLY, "parse_int"},
    {"parse_constant", T_OBJECT,
     offsetof(JsonScannerObject, parse_constant), Py_READONLY,
     "parse_constant"},
    {NULL}
};

static PyTypeObject json_scanner_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = JSON_SCANNER_NAME,
    .tp_basicsize = sizeof(JsonScannerObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor)json_scanner_dealloc,
    .tp_call = (ternaryfunc)json_scanner_call,
    .tp_traverse = (traverseproc)json_scanner_traverse,
    .tp_clear = json_scanner_clear_gc,
    .tp_members = json_scanner_members,
};

static PyObject *
json_get_context_attr(PyObject *context, const char *name)
{
    PyObject *value = PyObject_GetAttrString(context, name);
    if (value == NULL) {
        return NULL;
    }
    return value;
}

static PyObject *
json_scanner_new(PyObject *context)
{
    if (!json_scanner_type_ready && PyType_Ready(&json_scanner_type) < 0) {
        return NULL;
    }
    json_scanner_type_ready = 1;

    JsonScannerObject *scanner = (JsonScannerObject *)json_scanner_type.tp_alloc(
        &json_scanner_type,
        0
    );
    if (scanner == NULL) {
        return NULL;
    }
    scanner->strict = 0;
    scanner->object_hook = NULL;
    scanner->object_pairs_hook = NULL;
    scanner->parse_float = NULL;
    scanner->parse_int = NULL;
    scanner->parse_constant = NULL;

    PyObject *strict = json_get_context_attr(context, "strict");
    if (strict == NULL) {
        goto error;
    }
    scanner->strict = (signed char)PyObject_IsTrue(strict);
    Py_DECREF(strict);
    if (scanner->strict < 0) {
        goto error;
    }

#define GET_SCANNER_ATTR(field, name) \
    do { \
        scanner->field = json_get_context_attr(context, name); \
        if (scanner->field == NULL) { \
            goto error; \
        } \
    } while (0)
    GET_SCANNER_ATTR(object_hook, "object_hook");
    GET_SCANNER_ATTR(object_pairs_hook, "object_pairs_hook");
    GET_SCANNER_ATTR(parse_float, "parse_float");
    GET_SCANNER_ATTR(parse_int, "parse_int");
    GET_SCANNER_ATTR(parse_constant, "parse_constant");
#undef GET_SCANNER_ATTR

    return (PyObject *)scanner;

error:
    Py_DECREF((PyObject *)scanner);
    return NULL;
}

static void
json_frame_clear(JsonFrame *frame)
{
    Py_XDECREF(frame->container);
    Py_XDECREF(frame->key);
    frame->container = NULL;
    frame->key = NULL;
}

static void
json_state_clear(JsonDecoderState *state)
{
    if (state == NULL) {
        return;
    }
    for (Py_ssize_t i = 0; i < state->depth; i++) {
        json_frame_clear(&state->frames[i]);
    }
    PyMem_Free(state->frames);
    Py_XDECREF(state->value);
    Py_XDECREF(state->memo);
    Py_XDECREF(state->input);
    Py_XDECREF(state->scanner);
    PyMem_Free(state);
}

static PyObject *
json_clone_value(PyObject *value, PyObject *seen);

static PyObject *
json_clone_key(PyObject *key, PyObject *seen)
{
    return json_clone_value(key, seen);
}

static PyObject *
json_clone_list(PyObject *source, PyObject *seen)
{
    PyObject *identity = PyLong_FromVoidPtr(source);
    if (identity == NULL) {
        return NULL;
    }
    PyObject *existing = PyDict_GetItemWithError(seen, identity);
    if (existing != NULL) {
        Py_INCREF(existing);
        Py_DECREF(identity);
        return existing;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(identity);
        return NULL;
    }

    PyObject *copy = PyList_New(0);
    if (copy == NULL || PyDict_SetItem(seen, identity, copy) < 0) {
        Py_XDECREF(copy);
        Py_DECREF(identity);
        return NULL;
    }
    Py_DECREF(identity);
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(source); i++) {
        PyObject *item = json_clone_value(PyList_GET_ITEM(source, i), seen);
        if (item == NULL || PyList_Append(copy, item) < 0) {
            Py_XDECREF(item);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(item);
    }
    return copy;
}

static PyObject *
json_clone_tuple(PyObject *source, PyObject *seen)
{
    PyObject *identity = PyLong_FromVoidPtr(source);
    if (identity == NULL) {
        return NULL;
    }
    PyObject *existing = PyDict_GetItemWithError(seen, identity);
    if (existing != NULL) {
        Py_INCREF(existing);
        Py_DECREF(identity);
        return existing;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(identity);
        return NULL;
    }

    Py_ssize_t size = PyTuple_GET_SIZE(source);
    PyObject *copy = PyTuple_New(size);
    if (copy == NULL || PyDict_SetItem(seen, identity, copy) < 0) {
        Py_XDECREF(copy);
        Py_DECREF(identity);
        return NULL;
    }
    Py_DECREF(identity);
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = json_clone_value(PyTuple_GET_ITEM(source, i), seen);
        if (item == NULL) {
            Py_DECREF(copy);
            return NULL;
        }
        PyTuple_SET_ITEM(copy, i, item);
    }
    return copy;
}

static PyObject *
json_clone_dict(PyObject *source, PyObject *seen)
{
    PyObject *identity = PyLong_FromVoidPtr(source);
    if (identity == NULL) {
        return NULL;
    }
    PyObject *existing = PyDict_GetItemWithError(seen, identity);
    if (existing != NULL) {
        Py_INCREF(existing);
        Py_DECREF(identity);
        return existing;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(identity);
        return NULL;
    }

    PyObject *copy = PyDict_New();
    if (copy == NULL || PyDict_SetItem(seen, identity, copy) < 0) {
        Py_XDECREF(copy);
        Py_DECREF(identity);
        return NULL;
    }
    Py_DECREF(identity);
    Py_ssize_t position = 0;
    PyObject *key;
    PyObject *item;
    while (PyDict_Next(source, &position, &key, &item)) {
        PyObject *copy_key = json_clone_key(key, seen);
        PyObject *copy_item = copy_key == NULL
            ? NULL : json_clone_value(item, seen);
        if (copy_key == NULL || copy_item == NULL ||
            PyDict_SetItem(copy, copy_key, copy_item) < 0) {
            Py_XDECREF(copy_key);
            Py_XDECREF(copy_item);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(copy_key);
        Py_DECREF(copy_item);
    }
    return copy;
}

static PyObject *
json_clone_value(PyObject *value, PyObject *seen)
{
    if (PyList_Check(value)) {
        return json_clone_list(value, seen);
    }
    if (PyDict_Check(value)) {
        return json_clone_dict(value, seen);
    }
    if (PyTuple_Check(value)) {
        return json_clone_tuple(value, seen);
    }
    return Py_NewRef(value);
}

static JsonDecoderState *
json_state_new(JsonScannerObject *scanner, PyObject *input, Py_ssize_t index)
{
    JsonDecoderState *state = PyMem_Calloc(1, sizeof(*state));
    if (state == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    state->scanner = (JsonScannerObject *)Py_NewRef((PyObject *)scanner);
    state->input = Py_NewRef(input);
    state->memo = PyDict_New();
    state->index = index;
    if (state->memo == NULL) {
        json_state_clear(state);
        return NULL;
    }
    return state;
}

static void *
json_decoder_copy_state(const void *raw_state)
{
    const JsonDecoderState *source = raw_state;
    JsonDecoderState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->scanner = (JsonScannerObject *)Py_NewRef((PyObject *)source->scanner);
    copy->input = Py_NewRef(source->input);
    copy->memo = PyDict_Copy(source->memo);
    copy->depth = source->depth;
    copy->frame_capacity = source->depth;
    copy->index = source->index;
    copy->wait = source->wait;
    if (copy->memo == NULL) {
        json_state_clear(copy);
        return NULL;
    }

    PyObject *seen = PyDict_New();
    if (seen == NULL) {
        json_state_clear(copy);
        return NULL;
    }
    if (copy->depth != 0) {
        copy->frames = PyMem_Calloc(
            (size_t)copy->depth,
            sizeof(*copy->frames)
        );
        if (copy->frames == NULL) {
            Py_DECREF(seen);
            PyErr_NoMemory();
            json_state_clear(copy);
            return NULL;
        }
        for (Py_ssize_t i = 0; i < copy->depth; i++) {
            const JsonFrame *source_frame = &source->frames[i];
            JsonFrame *copy_frame = &copy->frames[i];
            copy_frame->kind = source_frame->kind;
            copy_frame->phase = source_frame->phase;
            copy_frame->comma_index = source_frame->comma_index;
            copy_frame->container = json_clone_value(source_frame->container, seen);
            copy_frame->key = source_frame->key == NULL
                ? NULL : json_clone_value(source_frame->key, seen);
            if (copy_frame->container == NULL ||
                (source_frame->key != NULL && copy_frame->key == NULL)) {
                Py_DECREF(seen);
                json_state_clear(copy);
                return NULL;
            }
        }
    }
    copy->value = source->value == NULL
        ? NULL : json_clone_value(source->value, seen);
    Py_DECREF(seen);
    if (source->value != NULL && copy->value == NULL) {
        json_state_clear(copy);
        return NULL;
    }
    return copy;
}

static void
json_decoder_free_state(void *raw_state)
{
    json_state_clear((JsonDecoderState *)raw_state);
}

static int
json_grow_frames(JsonDecoderState *state)
{
    if (state->frame_capacity > PY_SSIZE_T_MAX / 2 ||
        state->frame_capacity > (Py_ssize_t)(
            PY_SSIZE_T_MAX / (2 * sizeof(*state->frames))
        )) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t capacity = state->frame_capacity == 0
        ? 8 : state->frame_capacity * 2;
    JsonFrame *frames = PyMem_Realloc(
        state->frames,
        (size_t)capacity * sizeof(*frames)
    );
    if (frames == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    memset(
        frames + state->frame_capacity,
        0,
        (size_t)(capacity - state->frame_capacity) * sizeof(*frames)
    );
    state->frames = frames;
    state->frame_capacity = capacity;
    return 0;
}

static int
json_push_frame(
    JsonDecoderState *state,
    JsonFrameKind kind,
    PyObject *container,
    JsonFramePhase phase
)
{
    if (state->depth == state->frame_capacity && json_grow_frames(state) < 0) {
        return -1;
    }
    JsonFrame *frame = &state->frames[state->depth++];
    frame->kind = kind;
    frame->phase = phase;
    frame->container = Py_NewRef(container);
    frame->key = NULL;
    frame->comma_index = -1;
    return 0;
}

static void
json_pop_frame(JsonDecoderState *state)
{
    if (state->depth == 0) {
        return;
    }
    state->depth--;
    json_frame_clear(&state->frames[state->depth]);
}

static Py_UCS4
json_read_char(PyObject *input, Py_ssize_t index)
{
    return PyUnicode_READ(
        PyUnicode_KIND(input),
        PyUnicode_DATA(input),
        index
    );
}

static int
json_is_whitespace(Py_UCS4 character)
{
    return character == ' ' || character == '\t' ||
        character == '\n' || character == '\r';
}

static void
json_skip_whitespace(JsonDecoderState *state)
{
    Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
    while (state->index < length && json_is_whitespace(
            json_read_char(state->input, state->index))) {
        state->index++;
    }
}

static int
json_raise_error(JsonDecoderState *state, const char *message, Py_ssize_t index)
{
    PyObject *decoder = PyImport_ImportModule("json.decoder");
    if (decoder == NULL) {
        return -1;
    }
    PyObject *error_type = PyObject_GetAttrString(decoder, "JSONDecodeError");
    Py_DECREF(decoder);
    if (error_type == NULL) {
        return -1;
    }
    PyObject *error = PyObject_CallFunction(
        error_type,
        "sOn",
        message,
        state->input,
        index
    );
    if (error != NULL) {
        PyErr_SetObject(error_type, error);
        Py_DECREF(error);
    }
    Py_DECREF(error_type);
    return -1;
}

static int
json_raise_stop_iteration(Py_ssize_t index)
{
    PyObject *value = PyLong_FromSsize_t(index);
    if (value == NULL) {
        return -1;
    }
    PyErr_SetObject(PyExc_StopIteration, value);
    Py_DECREF(value);
    return -1;
}

static int
json_hex_digit(Py_UCS4 character)
{
    if (character >= '0' && character <= '9') {
        return (int)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (int)(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
        return (int)(character - 'A' + 10);
    }
    return -1;
}

static PyObject *
json_parse_string(JsonDecoderState *state, Py_ssize_t start, Py_ssize_t *end)
{
    PyObject *chunks = PyList_New(0);
    if (chunks == NULL) {
        return NULL;
    }
    Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
    Py_ssize_t cursor = start;
    Py_ssize_t chunk_start = start;
    while (cursor < length) {
        Py_UCS4 character = json_read_char(state->input, cursor);
        if (character == '"') {
            if (cursor != chunk_start) {
                PyObject *chunk = PyUnicode_Substring(
                    state->input,
                    chunk_start,
                    cursor
                );
                if (chunk == NULL || PyList_Append(chunks, chunk) < 0) {
                    Py_XDECREF(chunk);
                    Py_DECREF(chunks);
                    return NULL;
                }
                Py_DECREF(chunk);
            }
            PyObject *separator = PyUnicode_New(0, 0);
            if (separator == NULL) {
                Py_DECREF(chunks);
                return NULL;
            }
            PyObject *result = PyUnicode_Join(separator, chunks);
            Py_DECREF(separator);
            Py_DECREF(chunks);
            if (result != NULL) {
                *end = cursor + 1;
            }
            return result;
        }
        if (character == '\\') {
            if (cursor != chunk_start) {
                PyObject *chunk = PyUnicode_Substring(
                    state->input,
                    chunk_start,
                    cursor
                );
                if (chunk == NULL || PyList_Append(chunks, chunk) < 0) {
                    Py_XDECREF(chunk);
                    Py_DECREF(chunks);
                    return NULL;
                }
                Py_DECREF(chunk);
            }
            cursor++;
            if (cursor >= length) {
                Py_DECREF(chunks);
                json_raise_error(
                    state,
                    "Unterminated string starting at",
                    start - 1
                );
                return NULL;
            }
            Py_UCS4 escape = json_read_char(state->input, cursor);
            Py_UCS4 decoded;
            switch (escape) {
                case '"': decoded = '"'; cursor++; break;
                case '\\': decoded = '\\'; cursor++; break;
                case '/': decoded = '/'; cursor++; break;
                case 'b': decoded = '\b'; cursor++; break;
                case 'f': decoded = '\f'; cursor++; break;
                case 'n': decoded = '\n'; cursor++; break;
                case 'r': decoded = '\r'; cursor++; break;
                case 't': decoded = '\t'; cursor++; break;
                case 'u': {
                    if (cursor + 4 >= length) {
                        Py_DECREF(chunks);
                        json_raise_error(state, "Invalid \\uXXXX escape", cursor - 1);
                        return NULL;
                    }
                    decoded = 0;
                    for (int digit = 0; digit < 4; digit++) {
                        int value = json_hex_digit(
                            json_read_char(state->input, cursor + 1 + digit)
                        );
                        if (value < 0) {
                            Py_DECREF(chunks);
                            json_raise_error(
                                state,
                                "Invalid \\uXXXX escape",
                                cursor - 1
                            );
                            return NULL;
                        }
                        decoded = (decoded << 4) | (Py_UCS4)value;
                    }
                    cursor += 5;
                    if (decoded >= 0xd800 && decoded <= 0xdbff &&
                        cursor + 5 < length &&
                        json_read_char(state->input, cursor) == '\\' &&
                        json_read_char(state->input, cursor + 1) == 'u') {
                        Py_UCS4 low = 0;
                        int valid = 1;
                        for (int digit = 0; digit < 4; digit++) {
                            int value = json_hex_digit(
                                json_read_char(state->input, cursor + 2 + digit)
                            );
                            if (value < 0) {
                                valid = 0;
                                break;
                            }
                            low = (low << 4) | (Py_UCS4)value;
                        }
                        if (valid && low >= 0xdc00 && low <= 0xdfff) {
                            decoded = 0x10000 +
                                (((decoded - 0xd800) << 10) | (low - 0xdc00));
                            cursor += 6;
                        }
                    }
                    break;
                }
                default:
                    Py_DECREF(chunks);
                    json_raise_error(state, "Invalid \\escape", cursor - 1);
                    return NULL;
            }
            PyObject *character_object = PyUnicode_FromOrdinal(decoded);
            if (character_object == NULL ||
                PyList_Append(chunks, character_object) < 0) {
                Py_XDECREF(character_object);
                Py_DECREF(chunks);
                return NULL;
            }
            Py_DECREF(character_object);
            chunk_start = cursor;
            continue;
        }
        if (character <= 0x1f && state->scanner->strict) {
            Py_DECREF(chunks);
            json_raise_error(state, "Invalid control character at", cursor);
            return NULL;
        }
        cursor++;
    }
    Py_DECREF(chunks);
    json_raise_error(state, "Unterminated string starting at", start - 1);
    return NULL;
}

static int
json_set_value(JsonDecoderState *state, PyObject *value)
{
    Py_XSETREF(state->value, value);
    return 0;
}

static int
json_call_callback(
    JsonDecoderState *state,
    JsonWaitPhase wait,
    PyObject *callback,
    PyObject *argument
)
{
    state->wait = wait;
    PyObject *result = PyObject_CallOneArg(callback, argument);
    if (result == NULL) {
        return -1;
    }
    if (wait == JSON_WAIT_OBJECT_HOOK) {
        if (state->depth == 0 ||
            state->frames[state->depth - 1].phase != JSON_OBJECT_HOOK) {
            Py_DECREF(result);
            PyErr_SetString(PyExc_RuntimeError, "invalid JSON hook state");
            return -1;
        }
        json_pop_frame(state);
    }
    state->wait = JSON_WAIT_NONE;
    return json_set_value(state, result);
}

static int
json_parse_number(JsonDecoderState *state)
{
    Py_ssize_t start = state->index;
    Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
    Py_ssize_t cursor = start;
    int is_float = 0;
    if (cursor < length && json_read_char(state->input, cursor) == '-') {
        cursor++;
        if (cursor >= length) {
            return json_raise_stop_iteration(start);
        }
    }
    if (cursor < length && json_read_char(state->input, cursor) >= '1' &&
        json_read_char(state->input, cursor) <= '9') {
        cursor++;
        while (cursor < length && json_read_char(state->input, cursor) >= '0' &&
               json_read_char(state->input, cursor) <= '9') {
            cursor++;
        }
    }
    else if (cursor < length && json_read_char(state->input, cursor) == '0') {
        cursor++;
    }
    else {
        return json_raise_stop_iteration(start);
    }

    if (cursor + 1 < length && json_read_char(state->input, cursor) == '.' &&
        json_read_char(state->input, cursor + 1) >= '0' &&
        json_read_char(state->input, cursor + 1) <= '9') {
        is_float = 1;
        cursor += 2;
        while (cursor < length && json_read_char(state->input, cursor) >= '0' &&
               json_read_char(state->input, cursor) <= '9') {
            cursor++;
        }
    }
    if (cursor < length && (json_read_char(state->input, cursor) == 'e' ||
                            json_read_char(state->input, cursor) == 'E')) {
        Py_ssize_t exponent_start = cursor++;
        if (cursor < length && (json_read_char(state->input, cursor) == '-' ||
                                json_read_char(state->input, cursor) == '+')) {
            cursor++;
        }
        Py_ssize_t digits_start = cursor;
        while (cursor < length && json_read_char(state->input, cursor) >= '0' &&
               json_read_char(state->input, cursor) <= '9') {
            cursor++;
        }
        if (cursor == digits_start) {
            cursor = exponent_start;
        }
        else {
            is_float = 1;
        }
    }

    PyObject *number = PyUnicode_Substring(state->input, start, cursor);
    if (number == NULL) {
        return -1;
    }
    state->index = cursor;
    PyObject *callback = is_float
        ? state->scanner->parse_float
        : state->scanner->parse_int;
    int custom = is_float
        ? callback != (PyObject *)&PyFloat_Type
        : callback != (PyObject *)&PyLong_Type;
    if (custom) {
        int result = json_call_callback(state, JSON_WAIT_NUMBER, callback, number);
        Py_DECREF(number);
        return result;
    }
    PyObject *result;
    if (is_float) {
        result = PyFloat_FromString(number);
    }
    else {
        const char *text = PyUnicode_AsUTF8(number);
        result = text == NULL ? NULL : PyLong_FromString(text, NULL, 10);
    }
    Py_DECREF(number);
    return json_set_value(state, result);
}

static int
json_literal(JsonDecoderState *state, const char *literal, PyObject *value)
{
    Py_ssize_t size = (Py_ssize_t)strlen(literal);
    Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
    if (state->index > length - size) {
        return json_raise_stop_iteration(state->index);
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        if (json_read_char(state->input, state->index + i) !=
            (Py_UCS4)(unsigned char)literal[i]) {
            return json_raise_stop_iteration(state->index);
        }
    }
    state->index += size;
    return json_set_value(state, Py_NewRef(value));
}

static int
json_parse_constant(JsonDecoderState *state, const char *constant)
{
    PyObject *argument = PyUnicode_InternFromString(constant);
    if (argument == NULL) {
        return -1;
    }
    state->index += PyUnicode_GET_LENGTH(argument);
    int result = json_call_callback(
        state,
        JSON_WAIT_CONSTANT,
        state->scanner->parse_constant,
        argument
    );
    Py_DECREF(argument);
    return result;
}

static int json_start_value(JsonDecoderState *state);

static int
json_start_value(JsonDecoderState *state)
{
    Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
    if (state->index >= length) {
        return json_raise_stop_iteration(state->index);
    }
    Py_UCS4 character = json_read_char(state->input, state->index);
    if (character == '"') {
        Py_ssize_t end;
        PyObject *value = json_parse_string(state, state->index + 1, &end);
        if (value == NULL) {
            return -1;
        }
        state->index = end;
        return json_set_value(state, value);
    }
    if (character == '{') {
        state->index++;
        PyObject *container = state->scanner->object_pairs_hook != Py_None
            ? PyList_New(0) : PyDict_New();
        if (container == NULL) {
            return -1;
        }
        int result = json_push_frame(
            state,
            JSON_FRAME_OBJECT,
            container,
            JSON_OBJECT_START
        );
        Py_DECREF(container);
        return result;
    }
    if (character == '[') {
        state->index++;
        PyObject *container = PyList_New(0);
        if (container == NULL) {
            return -1;
        }
        int result = json_push_frame(
            state,
            JSON_FRAME_ARRAY,
            container,
            JSON_ARRAY_START
        );
        Py_DECREF(container);
        return result;
    }
    if (character == 'n' && state->index + 4 <= length &&
        json_read_char(state->input, state->index + 1) == 'u' &&
        json_read_char(state->input, state->index + 2) == 'l' &&
        json_read_char(state->input, state->index + 3) == 'l') {
        return json_literal(state, "null", Py_None);
    }
    if (character == 't' && state->index + 4 <= length &&
        json_read_char(state->input, state->index + 1) == 'r' &&
        json_read_char(state->input, state->index + 2) == 'u' &&
        json_read_char(state->input, state->index + 3) == 'e') {
        return json_literal(state, "true", Py_True);
    }
    if (character == 'f' && state->index + 5 <= length &&
        json_read_char(state->input, state->index + 1) == 'a' &&
        json_read_char(state->input, state->index + 2) == 'l' &&
        json_read_char(state->input, state->index + 3) == 's' &&
        json_read_char(state->input, state->index + 4) == 'e') {
        return json_literal(state, "false", Py_False);
    }
    if (character == 'N' && state->index + 3 <= length &&
        json_read_char(state->input, state->index + 1) == 'a' &&
        json_read_char(state->input, state->index + 2) == 'N') {
        return json_parse_constant(state, "NaN");
    }
    if (character == 'I' && state->index + 8 <= length &&
        json_read_char(state->input, state->index + 1) == 'n' &&
        json_read_char(state->input, state->index + 2) == 'f' &&
        json_read_char(state->input, state->index + 3) == 'i' &&
        json_read_char(state->input, state->index + 4) == 'n' &&
        json_read_char(state->input, state->index + 5) == 'i' &&
        json_read_char(state->input, state->index + 6) == 't' &&
        json_read_char(state->input, state->index + 7) == 'y') {
        return json_parse_constant(state, "Infinity");
    }
    if (character == '-' && state->index + 9 <= length &&
        json_read_char(state->input, state->index + 1) == 'I' &&
        json_read_char(state->input, state->index + 2) == 'n' &&
        json_read_char(state->input, state->index + 3) == 'f' &&
        json_read_char(state->input, state->index + 4) == 'i' &&
        json_read_char(state->input, state->index + 5) == 'n' &&
        json_read_char(state->input, state->index + 6) == 'i' &&
        json_read_char(state->input, state->index + 7) == 't' &&
        json_read_char(state->input, state->index + 8) == 'y') {
        return json_parse_constant(state, "-Infinity");
    }
    return json_parse_number(state);
}

static int
json_memoize_key(JsonDecoderState *state, PyObject **key)
{
    PyObject *existing = PyDict_GetItemWithError(state->memo, *key);
    if (existing != NULL) {
        Py_INCREF(existing);
        Py_XSETREF(*key, existing);
        return 0;
    }
    if (PyErr_Occurred() || PyDict_SetItem(state->memo, *key, *key) < 0) {
        return -1;
    }
    return 0;
}

static int
json_finish_object(JsonDecoderState *state)
{
    JsonFrame *frame = &state->frames[state->depth - 1];
    PyObject *hook = frame->kind == JSON_FRAME_OBJECT &&
        state->scanner->object_pairs_hook != Py_None
        ? state->scanner->object_pairs_hook
        : state->scanner->object_hook;
    if (hook == Py_None) {
        PyObject *result = Py_NewRef(frame->container);
        json_pop_frame(state);
        return json_set_value(state, result);
    }
    frame->phase = JSON_OBJECT_HOOK;
    return json_call_callback(
        state,
        JSON_WAIT_OBJECT_HOOK,
        hook,
        frame->container
    );
}

static int
json_finish_frame(JsonDecoderState *state)
{
    JsonFrame *frame = &state->frames[state->depth - 1];
    if (frame->kind == JSON_FRAME_OBJECT) {
        return json_finish_object(state);
    }
    PyObject *result = Py_NewRef(frame->container);
    json_pop_frame(state);
    return json_set_value(state, result);
}

static int
json_attach_value(JsonDecoderState *state)
{
    if (state->depth == 0 || state->value == NULL) {
        return 0;
    }
    JsonFrame *frame = &state->frames[state->depth - 1];
    if (frame->kind == JSON_FRAME_ARRAY && frame->phase == JSON_ARRAY_VALUE) {
        if (PyList_Append(frame->container, state->value) < 0) {
            return -1;
        }
        Py_CLEAR(state->value);
        frame->phase = JSON_ARRAY_AFTER_VALUE;
        return 0;
    }
    if (frame->kind == JSON_FRAME_OBJECT &&
        frame->phase == JSON_OBJECT_VALUE) {
        int result;
        if (state->scanner->object_pairs_hook != Py_None) {
            PyObject *pair = PyTuple_Pack(2, frame->key, state->value);
            result = pair == NULL ? -1 : PyList_Append(frame->container, pair);
            Py_XDECREF(pair);
        }
        else {
            result = PyDict_SetItem(frame->container, frame->key, state->value);
        }
        if (result < 0) {
            return -1;
        }
        Py_CLEAR(state->value);
        Py_CLEAR(frame->key);
        frame->phase = JSON_OBJECT_AFTER_VALUE;
        return 0;
    }
    return 0;
}

static PyObject *
json_parse_loop(JsonDecoderState *state, PyObject *resumed_value)
{
    if (resumed_value != NULL) {
        if (state->wait == JSON_WAIT_OBJECT_HOOK) {
            if (state->depth == 0 ||
                state->frames[state->depth - 1].phase != JSON_OBJECT_HOOK) {
                PyErr_SetString(PyExc_RuntimeError, "invalid JSON hook state");
                return NULL;
            }
            json_pop_frame(state);
        }
        else if (state->wait != JSON_WAIT_NUMBER &&
                 state->wait != JSON_WAIT_CONSTANT) {
            PyErr_SetString(PyExc_RuntimeError, "invalid JSON callback state");
            return NULL;
        }
        state->wait = JSON_WAIT_NONE;
        if (json_set_value(state, Py_NewRef(resumed_value)) < 0) {
            return NULL;
        }
    }

    for (;;) {
        if (state->value != NULL) {
            if (state->depth == 0) {
                PyObject *index = PyLong_FromSsize_t(state->index);
                if (index == NULL) {
                    return NULL;
                }
                PyObject *result = PyTuple_New(2);
                if (result == NULL) {
                    Py_DECREF(index);
                    return NULL;
                }
                PyTuple_SET_ITEM(result, 0, state->value);
                PyTuple_SET_ITEM(result, 1, index);
                state->value = NULL;
                return result;
            }
            if (json_attach_value(state) < 0) {
                return NULL;
            }
            continue;
        }
        if (state->depth == 0) {
            if (json_start_value(state) < 0) {
                return NULL;
            }
            continue;
        }

        JsonFrame *frame = &state->frames[state->depth - 1];
        if (frame->kind == JSON_FRAME_ARRAY) {
            if (frame->phase == JSON_ARRAY_START) {
                json_skip_whitespace(state);
                if (state->index < PyUnicode_GET_LENGTH(state->input) &&
                    json_read_char(state->input, state->index) == ']') {
                    state->index++;
                    if (json_finish_frame(state) < 0) {
                        return NULL;
                    }
                }
                else {
                    frame->phase = JSON_ARRAY_VALUE;
                    if (json_start_value(state) < 0) {
                        return NULL;
                    }
                }
                continue;
            }
            if (frame->phase == JSON_ARRAY_AFTER_VALUE) {
                json_skip_whitespace(state);
                Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
                if (state->index < length &&
                    json_read_char(state->input, state->index) == ']') {
                    state->index++;
                    if (json_finish_frame(state) < 0) {
                        return NULL;
                    }
                }
                else if (state->index < length &&
                         json_read_char(state->input, state->index) == ',') {
                    frame->comma_index = state->index++;
                    json_skip_whitespace(state);
                    if (state->index < length &&
                        json_read_char(state->input, state->index) == ']') {
                        json_raise_error(
                            state,
                            "Illegal trailing comma before end of array",
                            frame->comma_index
                        );
                        return NULL;
                    }
                    frame->phase = JSON_ARRAY_VALUE;
                    if (json_start_value(state) < 0) {
                        return NULL;
                    }
                }
                else {
                    json_raise_error(
                        state,
                        "Expecting ',' delimiter",
                        state->index
                    );
                    return NULL;
                }
                continue;
            }
            PyErr_SetString(PyExc_RuntimeError, "invalid JSON array state");
            return NULL;
        }

        if (frame->phase == JSON_OBJECT_START ||
            frame->phase == JSON_OBJECT_KEY) {
            json_skip_whitespace(state);
            Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
            if (state->index < length &&
                json_read_char(state->input, state->index) == '}') {
                state->index++;
                if (json_finish_frame(state) < 0) {
                    return NULL;
                }
                continue;
            }
            if (state->index >= length ||
                json_read_char(state->input, state->index) != '"') {
                json_raise_error(
                    state,
                    "Expecting property name enclosed in double quotes",
                    state->index
                );
                return NULL;
            }
            Py_ssize_t end;
            PyObject *key = json_parse_string(state, state->index + 1, &end);
            if (key == NULL) {
                return NULL;
            }
            state->index = end;
            if (json_memoize_key(state, &key) < 0) {
                Py_DECREF(key);
                return NULL;
            }
            frame->key = key;
            frame->phase = JSON_OBJECT_COLON;
            continue;
        }
        if (frame->phase == JSON_OBJECT_COLON) {
            json_skip_whitespace(state);
            Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
            if (state->index >= length ||
                json_read_char(state->input, state->index) != ':') {
                json_raise_error(
                    state,
                    "Expecting ':' delimiter",
                    state->index
                );
                return NULL;
            }
            state->index++;
            json_skip_whitespace(state);
            frame->phase = JSON_OBJECT_VALUE;
            if (json_start_value(state) < 0) {
                return NULL;
            }
            continue;
        }
        if (frame->phase == JSON_OBJECT_AFTER_VALUE) {
            json_skip_whitespace(state);
            Py_ssize_t length = PyUnicode_GET_LENGTH(state->input);
            if (state->index < length &&
                json_read_char(state->input, state->index) == '}') {
                state->index++;
                if (json_finish_frame(state) < 0) {
                    return NULL;
                }
            }
            else if (state->index < length &&
                     json_read_char(state->input, state->index) == ',') {
                frame->comma_index = state->index++;
                json_skip_whitespace(state);
                if (state->index < length &&
                    json_read_char(state->input, state->index) == '}') {
                    json_raise_error(
                        state,
                        "Illegal trailing comma before end of object",
                        frame->comma_index
                    );
                    return NULL;
                }
                frame->phase = JSON_OBJECT_KEY;
            }
            else {
                json_raise_error(
                    state,
                    "Expecting ',' delimiter",
                    state->index
                );
                return NULL;
            }
            continue;
        }
        if (frame->phase == JSON_OBJECT_HOOK) {
            PyErr_SetString(PyExc_RuntimeError, "invalid JSON hook state");
            return NULL;
        }
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON object state");
                    return NULL;
    }
}

static PyObject *
json_decoder_resume(const void *raw_state, PyObject *value)
{
    JsonDecoderState *state = json_decoder_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    if (value == NULL) {
        json_decoder_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &json_decoder_vtable, state) < 0) {
        json_decoder_free_state(state);
        return NULL;
    }
    PyObject *result = json_parse_loop(state, value);
    adapter_leave(&frame);
    json_decoder_free_state(state);
    return result;
}

static const AleffAdapterVTable json_decoder_vtable = {
    .copy_state = json_decoder_copy_state,
    .free_state = json_decoder_free_state,
    .resume = json_decoder_resume,
    .prepare_resume = NULL,
};

static PyObject *
json_scanner_call(
    JsonScannerObject *scanner,
    PyObject *args,
    PyObject *kwargs
)
{
    PyObject *input;
    Py_ssize_t index;
    static char *keywords[] = {"string", "idx", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "On:scan_once",
            keywords,
            &input,
            &index
        )) {
        return NULL;
    }
    if (!PyUnicode_Check(input)) {
        PyErr_Format(
            PyExc_TypeError,
            "first argument must be a string, not %.80s",
            Py_TYPE(input)->tp_name
        );
        return NULL;
    }
    if (index < 0) {
        PyErr_SetString(PyExc_ValueError, "idx cannot be negative");
        return NULL;
    }
    JsonDecoderState *state = json_state_new(scanner, input, index);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &json_decoder_vtable, state) < 0) {
        json_decoder_free_state(state);
        return NULL;
    }
    PyObject *result = json_parse_loop(state, NULL);
    adapter_leave(&frame);
    json_decoder_free_state(state);
    return result;
}

static PyObject *
json_make_scanner(
    PyObject *Py_UNUSED(self),
    PyObject *args,
    PyObject *kwargs
)
{
    PyObject *context;
    static char *keywords[] = {"context", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O:make_scanner",
            keywords,
            &context
        )) {
        return NULL;
    }
    return json_scanner_new(context);
}

static int
json_decoder_install_type(void)
{
    if (!json_scanner_type_ready) {
        if (PyType_Ready(&json_scanner_type) < 0) {
            return -1;
        }
        json_scanner_type_ready = 1;
    }
    replacement_make_scanner_method = (PyMethodDef){
        .ml_name = "make_scanner",
        .ml_meth = (PyCFunction)(void(*)(void))json_make_scanner,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "make_scanner(context) -> a continuation-aware JSON scanner",
    };
    return 0;
}

int
adapter_json_decoder_install(PyObject *Py_UNUSED(json_module))
{
    if (json_decoder_installed) {
        return 0;
    }
    if (json_decoder_install_type() < 0) {
        return -1;
    }
    if (aleff_adapter_register_callable((PyObject *)&json_scanner_type) < 0) {
        return -1;
    }
    PyObject *scanner_module = PyImport_ImportModule("json.scanner");
    if (scanner_module == NULL) {
        return -1;
    }
    PyObject *original = PyObject_GetAttrString(scanner_module, "make_scanner");
    if (original == NULL) {
        Py_DECREF(scanner_module);
        return -1;
    }
    PyObject *module_name = PyUnicode_FromString("json.scanner");
    PyObject *replacement = module_name == NULL
        ? NULL
        : PyCFunction_NewEx(
            &replacement_make_scanner_method,
            scanner_module,
            module_name
        );
    Py_XDECREF(module_name);
    if (replacement == NULL) {
        Py_DECREF(original);
        Py_DECREF(scanner_module);
        return -1;
    }
    if (aleff_adapter_register_callable(replacement) < 0 ||
        PyObject_SetAttrString(scanner_module, "make_scanner", replacement) < 0) {
        Py_DECREF(replacement);
        Py_DECREF(original);
        Py_DECREF(scanner_module);
        return -1;
    }
    Py_DECREF(replacement);
    installed_scanner_module = scanner_module;
    original_make_scanner = original;
    json_decoder_installed = 1;
    return 0;
}

void
adapter_json_decoder_rollback(void)
{
    if (installed_scanner_module == NULL) {
        return;
    }
    if (original_make_scanner != NULL && PyObject_SetAttrString(
            installed_scanner_module,
            "make_scanner",
            original_make_scanner
        ) < 0) {
        PyErr_Clear();
    }
    Py_CLEAR(original_make_scanner);
    Py_CLEAR(installed_scanner_module);
    json_decoder_installed = 0;
}
