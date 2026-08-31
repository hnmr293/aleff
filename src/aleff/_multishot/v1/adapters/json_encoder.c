/*
 * This file is an adaptation of the encoder in CPython's Modules/_json.c.
 * CPython is distributed under the PSF license; the applicable terms are
 * included in this repository's LICENSES/CPython.txt and NOTICE.
 *
 * CPython's encoder stores traversal progress in C locals and C stack
 * frames.  Those frames cannot be restored by Aleff.  The implementation
 * below keeps the equivalent progress in an adapter-owned work stack.  A
 * callback suspension therefore resumes at the recorded phase and never
 * calls the callback which caused the suspension a second time.
 */

#include "json_encoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* This is the private prefix of CPython's _json.Encoder.  It is unchanged in
 * the CPython 3.12--3.14 sources.  Reading it is necessary for allow_nan,
 * which is intentionally not exposed as an Encoder member. */
typedef struct {
    PyObject_HEAD
    PyObject *markers;
    PyObject *defaultfn;
    PyObject *encoder;
    PyObject *indent;
    PyObject *key_separator;
    PyObject *item_separator;
    char sort_keys;
    char skipkeys;
    int allow_nan;
    PyCFunction fast_encode;
} JsonCEncoderObject;

typedef enum {
    JSON_FRAME_VALUE,
    JSON_FRAME_LIST,
    JSON_FRAME_DICT,
    JSON_FRAME_DEFAULT,
} JsonFrameKind;

typedef enum {
    JSON_FRAME_VALUE_READY,
    JSON_FRAME_LIST_READY,
    JSON_FRAME_LIST_VALUE,
    JSON_FRAME_DICT_ITEMS,
    JSON_FRAME_DICT_SORT,
    JSON_FRAME_DICT_READY,
    JSON_FRAME_DICT_KEY,
    JSON_FRAME_DICT_VALUE,
    JSON_FRAME_DEFAULT_WAIT,
    JSON_FRAME_DEFAULT_RESULT,
} JsonFramePhase;

typedef enum {
    JSON_WAIT_NONE,
    JSON_WAIT_DEFAULT,
    JSON_WAIT_ENCODER_VALUE,
    JSON_WAIT_ENCODER_KEY,
    JSON_WAIT_ITEMS,
    JSON_WAIT_SORT,
    JSON_WAIT_WRITE,
} JsonWaitPhase;

typedef struct JsonEncoderState {
    JsonFrameKind kind;
    JsonFramePhase phase;
    PyObject *object;
    PyObject *items;
    PyObject *marker_id;
    PyObject *pending_key;
    Py_ssize_t indent_level;
    Py_ssize_t index;
    Py_ssize_t sort_i;
    Py_ssize_t sort_j;
    int first;
    int recursive_entered;
} JsonFrame;

typedef struct {
    PyObject *defaultfn;
    PyObject *encoder;
    PyObject *indent;
    PyObject *key_separator;
    PyObject *item_separator;
    PyObject *markers;
    PyObject *chunks;
    PyObject *floatstr;
    JsonFrame *frames;
    Py_ssize_t frame_count;
    Py_ssize_t frame_capacity;
    Py_ssize_t waiting_frame;
    Py_ssize_t current_indent_level;
    JsonWaitPhase wait_phase;
    int sort_keys;
    int skipkeys;
    int allow_nan;
    int recursive_owned;
    int chunked_result;
    int write_chunks;
    int writer_checkpoint_valid;
    PyObject *owner;
    PyObject *writer;
    Py_ssize_t writer_position;
    Py_ssize_t next_chunk;
} JsonEncoderState;

static const AleffAdapterVTable json_encoder_vtable;
static PyTypeObject *installed_encoder_type;
static ternaryfunc original_encoder_call;
static PyObject *installed_encoder_module;
static PyObject *original_make_iterencode;
static PyObject *original_json_dump;
static PyObject *original_encode_basestring;
static PyObject *original_encode_basestring_ascii;
static PyObject *installed_json_module;
static int encoder_installed;

static void
json_frame_clear(JsonFrame *frame)
{
    Py_XDECREF(frame->object);
    Py_XDECREF(frame->items);
    Py_XDECREF(frame->marker_id);
    Py_XDECREF(frame->pending_key);
    frame->object = NULL;
    frame->items = NULL;
    frame->marker_id = NULL;
    frame->pending_key = NULL;
}

static void
json_state_clear(JsonEncoderState *state)
{
    while (state->frame_count > 0) {
        JsonFrame *frame = &state->frames[state->frame_count - 1];
        if (frame->recursive_entered && state->recursive_owned > 0) {
            Py_LeaveRecursiveCall();
            state->recursive_owned--;
        }
        json_frame_clear(frame);
        state->frame_count--;
    }
    PyMem_Free(state->frames);
    state->frames = NULL;
    state->frame_capacity = 0;
    Py_XDECREF(state->defaultfn);
    Py_XDECREF(state->encoder);
    Py_XDECREF(state->indent);
    Py_XDECREF(state->key_separator);
    Py_XDECREF(state->item_separator);
    Py_XDECREF(state->markers);
    Py_XDECREF(state->chunks);
    Py_XDECREF(state->floatstr);
    PyObject *owner = state->owner;
    state->owner = NULL;
    Py_XDECREF(owner);
    Py_XDECREF(state->writer);
    state->defaultfn = NULL;
    state->encoder = NULL;
    state->indent = NULL;
    state->key_separator = NULL;
    state->item_separator = NULL;
    state->markers = NULL;
    state->chunks = NULL;
    state->floatstr = NULL;
    state->next_chunk = 0;
    state->writer = NULL;
}

static void *
json_encoder_copy_state(const void *raw_state)
{
    const JsonEncoderState *source = raw_state;
    JsonEncoderState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->frames = NULL;
    copy->frame_count = 0;
    copy->frame_capacity = 0;
    copy->recursive_owned = 0;
    copy->floatstr = Py_XNewRef(source->floatstr);
    copy->owner = Py_XNewRef(source->owner);
    copy->writer = Py_XNewRef(source->writer);
    copy->defaultfn = Py_NewRef(source->defaultfn);
    copy->encoder = Py_NewRef(source->encoder);
    copy->indent = Py_XNewRef(source->indent);
    copy->key_separator = Py_NewRef(source->key_separator);
    copy->item_separator = Py_NewRef(source->item_separator);
    copy->markers = source->markers == NULL
        ? NULL : PyDict_Copy(source->markers);
    copy->chunks = source->chunks == NULL
        ? NULL : PyList_GetSlice(source->chunks, 0, PyList_GET_SIZE(source->chunks));
    if ((source->markers != NULL && copy->markers == NULL) ||
        (source->chunks != NULL && copy->chunks == NULL) ||
        (source->floatstr != NULL && copy->floatstr == NULL) ||
        (source->owner != NULL && copy->owner == NULL) ||
        (source->writer != NULL && copy->writer == NULL)) {
        json_state_clear(copy);
        PyMem_Free(copy);
        return NULL;
    }

    if (source->frame_capacity != 0) {
        copy->frames = PyMem_Calloc(
            (size_t)source->frame_capacity,
            sizeof(*copy->frames)
        );
        if (copy->frames == NULL) {
            PyErr_NoMemory();
            json_state_clear(copy);
            PyMem_Free(copy);
            return NULL;
        }
        copy->frame_capacity = source->frame_capacity;
    }
    for (Py_ssize_t index = 0; index < source->frame_count; index++) {
        const JsonFrame *from = &source->frames[index];
        JsonFrame *to = &copy->frames[index];
        *to = *from;
        to->object = Py_XNewRef(from->object);
        to->items = from->items == NULL
            ? NULL : PyList_GetSlice(from->items, 0, PyList_GET_SIZE(from->items));
        to->marker_id = Py_XNewRef(from->marker_id);
        to->pending_key = Py_XNewRef(from->pending_key);
        if ((from->items != NULL && to->items == NULL) ||
            (from->object != NULL && to->object == NULL) ||
            (from->marker_id != NULL && to->marker_id == NULL) ||
            (from->pending_key != NULL && to->pending_key == NULL)) {
            copy->frame_count = index + 1;
            json_state_clear(copy);
            PyMem_Free(copy);
            return NULL;
        }
        copy->frame_count++;
    }
    /* Recursive-call entries belong to the live C invocation.  The logical
     * entries are copied above and are re-entered only by json_encoder_resume. */
    copy->recursive_owned = 0;
    return copy;
}

static void
json_encoder_free_state(void *raw_state)
{
    JsonEncoderState *state = raw_state;
    if (state == NULL) {
        return;
    }
    json_state_clear(state);
    PyMem_Free(state);
}

typedef struct {
    PyObject **objects;
    Py_ssize_t count;
    Py_ssize_t capacity;
    /* Borrowed pointers used only by the current callback-free proof. */
    PyObject **seen;
    Py_ssize_t seen_capacity;
} JsonCGraph;

static size_t
json_c_graph_hash(PyObject *object)
{
    uintptr_t pointer = (uintptr_t)object;
    size_t hash = (size_t)(pointer >> 4);
    hash ^= hash >> (sizeof(hash) * 4);
    hash *= (size_t)0x9e3779b9U;
    hash ^= hash >> (sizeof(hash) * 3);
    return hash;
}

static int
json_c_graph_resize_seen(JsonCGraph *graph, Py_ssize_t capacity)
{
    if (capacity <= 0 ||
        (size_t)capacity > (size_t)-1 / sizeof(*graph->seen)) {
        PyErr_NoMemory();
        return -1;
    }
    PyObject **seen = PyMem_Calloc(
        (size_t)capacity,
        sizeof(*seen)
    );
    if (seen == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t index = 0; index < graph->count; index++) {
        PyObject *object = graph->objects[index];
        size_t slot = json_c_graph_hash(object) & (size_t)(capacity - 1);
        while (seen[slot] != NULL) {
            slot = (slot + 1) & (size_t)(capacity - 1);
        }
        seen[slot] = object;
    }
    PyMem_Free(graph->seen);
    graph->seen = seen;
    graph->seen_capacity = capacity;
    return 0;
}

static int
json_c_graph_add(JsonCGraph *graph, PyObject *object)
{
    if (graph->seen_capacity == 0 &&
        json_c_graph_resize_seen(graph, 16) < 0) {
        return -1;
    }
    size_t slot = json_c_graph_hash(object) &
        (size_t)(graph->seen_capacity - 1);
    while (graph->seen[slot] != NULL) {
        if (graph->seen[slot] == object) {
            return 0;
        }
        slot = (slot + 1) & (size_t)(graph->seen_capacity - 1);
    }
    if (graph->count >= graph->seen_capacity / 2) {
        if (graph->seen_capacity > PY_SSIZE_T_MAX / 2 ||
            json_c_graph_resize_seen(
                graph,
                graph->seen_capacity * 2
            ) < 0) {
            if (!PyErr_Occurred()) {
                PyErr_NoMemory();
            }
            return -1;
        }
        slot = json_c_graph_hash(object) &
            (size_t)(graph->seen_capacity - 1);
        while (graph->seen[slot] != NULL) {
            slot = (slot + 1) & (size_t)(graph->seen_capacity - 1);
        }
    }
    graph->seen[slot] = object;

    if (graph->count == graph->capacity) {
        Py_ssize_t capacity;
        if (graph->capacity == 0) {
            capacity = 16;
        }
        else if (graph->capacity > PY_SSIZE_T_MAX / 2) {
            PyErr_NoMemory();
            return -1;
        }
        else {
            capacity = graph->capacity * 2;
        }
        if ((size_t)capacity > (size_t)-1 / sizeof(*graph->objects)) {
            PyErr_NoMemory();
            return -1;
        }
        PyObject **objects = PyMem_Realloc(
            graph->objects,
            (size_t)capacity * sizeof(*objects)
        );
        if (objects == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        graph->objects = objects;
        graph->capacity = capacity;
    }
    graph->objects[graph->count++] = object;
    return 1;
}

static void
json_c_graph_clear(JsonCGraph *graph)
{
    PyMem_Free(graph->objects);
    PyMem_Free(graph->seen);
    graph->objects = NULL;
    graph->seen = NULL;
}

static int
json_c_graph_key_supported(PyObject *object)
{
    return object == Py_None || object == Py_True || object == Py_False ||
        PyUnicode_CheckExact(object) || PyLong_CheckExact(object) ||
        PyFloat_CheckExact(object);
}

/* Prove that the original encoder cannot enter a Python callback.  The
 * traversal is iterative so the proof itself is safe for cyclic graphs. */
static int
json_c_graph_supported(PyObject *root)
{
    JsonCGraph graph = {0};
    int result = 1;
    if (json_c_graph_add(&graph, root) < 0) {
        json_c_graph_clear(&graph);
        return -1;
    }
    for (Py_ssize_t index = 0; index < graph.count; index++) {
        PyObject *object = graph.objects[index];
        if (object == Py_None || object == Py_True || object == Py_False ||
            PyUnicode_CheckExact(object) || PyLong_CheckExact(object) ||
            PyFloat_CheckExact(object)) {
            continue;
        }
        if (PyList_CheckExact(object)) {
            Py_ssize_t size = PyList_GET_SIZE(object);
            for (Py_ssize_t item = 0; item < size; item++) {
                if (json_c_graph_add(
                        &graph,
                        PyList_GET_ITEM(object, item)
                    ) < 0) {
                    result = -1;
                    goto done;
                }
            }
            continue;
        }
        if (PyTuple_CheckExact(object)) {
            Py_ssize_t size = PyTuple_GET_SIZE(object);
            for (Py_ssize_t item = 0; item < size; item++) {
                if (json_c_graph_add(
                        &graph,
                        PyTuple_GET_ITEM(object, item)
                    ) < 0) {
                    result = -1;
                    goto done;
                }
            }
            continue;
        }
        if (PyDict_CheckExact(object)) {
            Py_ssize_t position = 0;
            PyObject *key;
            PyObject *value;
            while (PyDict_Next(object, &position, &key, &value)) {
                if (!json_c_graph_key_supported(key) ||
                    json_c_graph_add(&graph, value) < 0) {
                    result = PyErr_Occurred() ? -1 : 0;
                    goto done;
                }
            }
            continue;
        }
        result = 0;
        goto done;
    }

done:
    json_c_graph_clear(&graph);
    return result;
}

static int
json_can_use_original_encoder(
    JsonCEncoderObject *encoder,
    PyObject *value,
    int chunked_result
)
{
    /* Marker bookkeeping stays inside the native encoder and does not call
     * Python.  Keep it on this path so its recursion boundary is preserved. */
    if (chunked_result || original_encoder_call == NULL ||
        (encoder->encoder != original_encode_basestring &&
         encoder->encoder != original_encode_basestring_ascii)) {
        return 0;
    }
    return json_c_graph_supported(value);
}

static int
json_append_text(JsonEncoderState *state, PyObject *text)
{
    if (PyList_Append(state->chunks, text) < 0) {
        return -1;
    }
    return 0;
}

static int
json_append_ascii(JsonEncoderState *state, const char *text)
{
    PyObject *value = PyUnicode_FromString(text);
    if (value == NULL) {
        return -1;
    }
    int result = json_append_text(state, value);
    Py_DECREF(value);
    return result;
}

static int
json_append_indent(JsonEncoderState *state, Py_ssize_t level)
{
    if (state->indent == Py_None) {
        return 0;
    }
    Py_ssize_t count = level < 0 ? 0 : level;
    PyObject *spaces = PySequence_Repeat(state->indent, count);
    if (spaces == NULL) {
        return -1;
    }
    PyObject *newline = PyUnicode_FromString("\n");
    if (newline != NULL) {
        PyUnicode_AppendAndDel(&newline, spaces);
        spaces = NULL;
    }
    Py_XDECREF(spaces);
    if (newline == NULL) {
        return -1;
    }
    int result = json_append_text(state, newline);
    Py_DECREF(newline);
    return result;
}

static int
json_enter_frame(JsonEncoderState *state, JsonFrame *frame)
{
    if (Py_EnterRecursiveCall(" while encoding a JSON object") < 0) {
        return -1;
    }
    frame->recursive_entered = 1;
    state->recursive_owned++;
    return 0;
}

static int
json_grow_frames(JsonEncoderState *state)
{
    if (state->frame_count < state->frame_capacity) {
        return 0;
    }
    Py_ssize_t capacity = state->frame_capacity == 0
        ? 16 : state->frame_capacity * 2;
    if (capacity < state->frame_capacity ||
        (size_t)capacity > (size_t)-1 / sizeof(*state->frames)) {
        PyErr_NoMemory();
        return -1;
    }
    JsonFrame *frames = PyMem_Realloc(
        state->frames,
        (size_t)capacity * sizeof(*frames)
    );
    if (frames == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    state->frames = frames;
    state->frame_capacity = capacity;
    return 0;
}

static int
json_push_value(JsonEncoderState *state, PyObject *object, Py_ssize_t level)
{
    if (state->frame_count >= Py_GetRecursionLimit()) {
        PyErr_SetString(
            PyExc_RecursionError,
            "maximum recursion depth exceeded while encoding a JSON object"
        );
        return -1;
    }
    if (json_grow_frames(state) < 0) {
        return -1;
    }
    JsonFrame *frame = &state->frames[state->frame_count++];
    *frame = (JsonFrame){
        .kind = JSON_FRAME_VALUE,
        .phase = JSON_FRAME_VALUE_READY,
        .object = Py_NewRef(object),
        .indent_level = level,
        .first = 1,
    };
    return 0;
}

static void
json_pop_frame(JsonEncoderState *state)
{
    JsonFrame *frame = &state->frames[state->frame_count - 1];
    if (frame->recursive_entered && state->recursive_owned > 0) {
        Py_LeaveRecursiveCall();
        state->recursive_owned--;
    }
    json_frame_clear(frame);
    state->frame_count--;
    if (state->frame_count == 0) {
        return;
    }
    JsonFrame *parent = &state->frames[state->frame_count - 1];
    if (parent->kind == JSON_FRAME_LIST &&
        parent->phase == JSON_FRAME_LIST_VALUE) {
        parent->phase = JSON_FRAME_LIST_READY;
    }
    else if (parent->kind == JSON_FRAME_DICT &&
             parent->phase == JSON_FRAME_DICT_VALUE) {
        Py_CLEAR(parent->pending_key);
        parent->phase = JSON_FRAME_DICT_READY;
    }
    else if (parent->kind == JSON_FRAME_DEFAULT &&
             parent->phase == JSON_FRAME_DEFAULT_RESULT) {
        /* The default frame is popped by the main loop so its marker is
         * removed after the converted value has completed. */
    }
}

static int
json_marker_enter(JsonEncoderState *state, JsonFrame *frame, PyObject *object)
{
    if (state->markers == NULL) {
        return 0;
    }
    PyObject *ident = PyLong_FromVoidPtr(object);
    if (ident == NULL) {
        return -1;
    }
    int present = PyDict_Contains(state->markers, ident);
    if (present < 0) {
        Py_DECREF(ident);
        return -1;
    }
    if (present) {
        Py_DECREF(ident);
        PyErr_SetString(PyExc_ValueError, "Circular reference detected");
        return -1;
    }
    if (PyDict_SetItem(state->markers, ident, object) < 0) {
        Py_DECREF(ident);
        return -1;
    }
    frame->marker_id = ident;
    return 0;
}

static int
json_marker_leave(JsonEncoderState *state, JsonFrame *frame)
{
    if (frame->marker_id == NULL) {
        return 0;
    }
    if (PyDict_DelItem(state->markers, frame->marker_id) < 0) {
        return -1;
    }
    Py_CLEAR(frame->marker_id);
    return 0;
}

static PyObject *
json_encode_float(JsonEncoderState *state, PyObject *object)
{
    if (state->floatstr != NULL) {
        return PyObject_CallOneArg(state->floatstr, object);
    }
    double value = PyFloat_AS_DOUBLE(object);
    if (!Py_IS_FINITE(value)) {
        if (!state->allow_nan) {
            PyErr_Format(
                PyExc_ValueError,
                "Out of range float values are not JSON compliant: %R",
                object
            );
            return NULL;
        }
        if (value > 0) {
            return PyUnicode_FromString("Infinity");
        }
        if (value < 0) {
            return PyUnicode_FromString("-Infinity");
        }
        return PyUnicode_FromString("NaN");
    }
    return PyFloat_Type.tp_repr(object);
}

static PyObject *
json_encode_string(JsonEncoderState *state, PyObject *object)
{
    PyObject *encoded = PyObject_CallOneArg(state->encoder, object);
    if (encoded != NULL && !PyUnicode_Check(encoded)) {
        PyErr_Format(
            PyExc_TypeError,
            "encoder() must return a string, not %.80s",
            Py_TYPE(encoded)->tp_name
        );
        Py_DECREF(encoded);
        return NULL;
    }
    return encoded;
}

static PyObject *
json_encode_key(JsonEncoderState *state, PyObject *key)
{
    if (PyUnicode_Check(key)) {
        return Py_NewRef(key);
    }
    if (PyFloat_Check(key)) {
        return json_encode_float(state, key);
    }
    if (key == Py_True || key == Py_False || key == Py_None) {
        if (key == Py_True) {
            return PyUnicode_FromString("true");
        }
        if (key == Py_False) {
            return PyUnicode_FromString("false");
        }
        return PyUnicode_FromString("null");
    }
    if (PyLong_Check(key)) {
        return PyLong_Type.tp_repr(key);
    }
    if (state->skipkeys) {
        return Py_NewRef(Py_None);
    }
    PyErr_Format(
        PyExc_TypeError,
        "keys must be str, int, float, bool or None, not %.100s",
        Py_TYPE(key)->tp_name
    );
    return NULL;
}

static int
json_start_list(JsonEncoderState *state, JsonFrame *frame)
{
    PyObject *fast = PySequence_Fast(
        frame->object,
        "_iterencode_list needs a sequence"
    );
    if (fast == NULL) {
        return -1;
    }
    PyObject *items = PySequence_List(fast);
    Py_DECREF(fast);
    if (items == NULL) {
        return -1;
    }
    frame->items = items;
    frame->index = 0;
    if (PyList_GET_SIZE(items) == 0) {
        if (json_append_ascii(state, "[]") < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (json_marker_enter(state, frame, frame->object) < 0 ||
        json_enter_frame(state, frame) < 0) {
        return -1;
    }
    frame->kind = JSON_FRAME_LIST;
    frame->phase = JSON_FRAME_LIST_READY;
    return 0;
}

static int
json_start_dict(JsonEncoderState *state, JsonFrame *frame)
{
    if (json_marker_enter(state, frame, frame->object) < 0 ||
        json_enter_frame(state, frame) < 0 ||
        json_append_ascii(state, "{") < 0) {
        return -1;
    }
    frame->items = PyMapping_Items(frame->object);
    if (frame->items == NULL) {
        state->wait_phase = JSON_WAIT_ITEMS;
        state->waiting_frame = state->frame_count - 1;
        frame->phase = JSON_FRAME_DICT_ITEMS;
        return -1;
    }
    if (!PyList_Check(frame->items)) {
        PyObject *list = PySequence_List(frame->items);
        if (list == NULL) {
            return -1;
        }
        Py_XSETREF(frame->items, list);
    }
    if (PyList_GET_SIZE(frame->items) == 0) {
        PyObject *open = PyList_GET_ITEM(
            state->chunks,
            PyList_GET_SIZE(state->chunks) - 1
        );
        PyObject *close = PyUnicode_FromString("}");
        PyObject *combined = close == NULL
            ? NULL : PyUnicode_Concat(open, close);
        Py_XDECREF(close);
        if (combined == NULL) {
            return -1;
        }
        if (PyList_SetItem(
                state->chunks,
                PyList_GET_SIZE(state->chunks) - 1,
                combined
            ) < 0) {
            return -1;
        }
        if (json_marker_leave(state, frame) < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (state->sort_keys) {
        frame->sort_i = 1;
        frame->sort_j = 1;
        frame->phase = JSON_FRAME_DICT_SORT;
    }
    else {
        frame->phase = JSON_FRAME_DICT_READY;
    }
    frame->index = 0;
    frame->first = 1;
    return 0;
}

static int
json_swap_items(PyObject *items, Py_ssize_t left, Py_ssize_t right)
{
    PyObject *left_value = PyList_GET_ITEM(items, left);
    PyObject *right_value = PyList_GET_ITEM(items, right);
    Py_INCREF(left_value);
    Py_INCREF(right_value);
    (void)PyList_SetItem(items, left, right_value);
    (void)PyList_SetItem(items, right, left_value);
    return 0;
}

static int
json_finish_container(JsonEncoderState *state, JsonFrame *frame, char close)
{
    if (state->indent != Py_None && !frame->first) {
        if (json_append_indent(state, frame->indent_level) < 0) {
            return -1;
        }
    }
    char text[2] = {close, '\0'};
    if (json_append_ascii(state, text) < 0 ||
        json_marker_leave(state, frame) < 0) {
        return -1;
    }
    json_pop_frame(state);
    return 0;
}

static int
json_accept_default(JsonEncoderState *state, PyObject *value)
{
    if (state->frame_count == 0) {
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON default resume phase");
        return -1;
    }
    JsonFrame *frame = &state->frames[state->frame_count - 1];
    if (frame->kind != JSON_FRAME_DEFAULT ||
        frame->phase != JSON_FRAME_DEFAULT_WAIT) {
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON default resume phase");
        return -1;
    }
    if (json_enter_frame(state, frame) < 0) {
        return -1;
    }
    frame->phase = JSON_FRAME_DEFAULT_RESULT;
    if (json_push_value(state, value, frame->indent_level) < 0) {
        return -1;
    }
    return 0;
}

static int
json_accept_encoded(JsonEncoderState *state, PyObject *value, int key)
{
    if (value == NULL || !PyUnicode_Check(value)) {
        if (value != NULL) {
            PyErr_Format(
                PyExc_TypeError,
                "encoder() must return a string, not %.80s",
                Py_TYPE(value)->tp_name
            );
        }
        return -1;
    }
    if (json_append_text(state, value) < 0) {
        return -1;
    }
    if (!key) {
        json_pop_frame(state);
        return 0;
    }
    if (state->frame_count == 0) {
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON key resume phase");
        return -1;
    }
    JsonFrame *frame = &state->frames[state->frame_count - 1];
    if (frame->kind != JSON_FRAME_DICT ||
        frame->phase != JSON_FRAME_DICT_KEY) {
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON key resume phase");
        return -1;
    }
    if (json_append_text(state, state->key_separator) < 0) {
        return -1;
    }
    frame->phase = JSON_FRAME_DICT_VALUE;
    PyObject *value_object = PyTuple_GET_ITEM(
        PyList_GET_ITEM(frame->items, frame->index - 1),
        1
    );
    return json_push_value(state, value_object, frame->indent_level + 1);
}

static int
json_accept_items(JsonEncoderState *state, PyObject *value)
{
    if (state->waiting_frame < 0 ||
        state->waiting_frame >= state->frame_count ||
        !PyList_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "items must return a list");
        return -1;
    }
    JsonFrame *frame = &state->frames[state->waiting_frame];
    Py_XSETREF(frame->items, Py_NewRef(value));
    frame->index = 0;
    if (state->sort_keys) {
        frame->sort_i = 1;
        frame->sort_j = 1;
        frame->phase = JSON_FRAME_DICT_SORT;
    }
    else {
        frame->phase = JSON_FRAME_DICT_READY;
    }
    return 0;
}

static int
json_accept_sort(JsonEncoderState *state, PyObject *value)
{
    if (state->frame_count == 0 ||
        state->waiting_frame != state->frame_count - 1) {
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON sort resume phase");
        return -1;
    }
    JsonFrame *frame = &state->frames[state->waiting_frame];
    int comparison = PyObject_IsTrue(value);
    if (comparison < 0) {
        return -1;
    }
    if (comparison) {
        if (json_swap_items(frame->items, frame->sort_j - 1, frame->sort_j) < 0) {
            return -1;
        }
        frame->sort_j--;
        if (frame->sort_j == 0) {
            frame->sort_i++;
            frame->sort_j = frame->sort_i;
        }
    }
    else {
        frame->sort_i++;
        frame->sort_j = frame->sort_i;
    }
    return 0;
}

static int
json_process_value(JsonEncoderState *state)
{
    JsonFrame *frame = &state->frames[state->frame_count - 1];
    PyObject *object = frame->object;
    if (object == Py_None) {
        if (json_append_ascii(state, "null") < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (object == Py_True) {
        if (json_append_ascii(state, "true") < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (object == Py_False) {
        if (json_append_ascii(state, "false") < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (PyUnicode_Check(object)) {
        state->wait_phase = JSON_WAIT_ENCODER_VALUE;
        state->waiting_frame = state->frame_count - 1;
        PyObject *encoded = json_encode_string(state, object);
        if (encoded == NULL) {
            return -1;
        }
        state->wait_phase = JSON_WAIT_NONE;
        int result = json_accept_encoded(state, encoded, 0);
        Py_DECREF(encoded);
        return result;
    }
    if (PyLong_Check(object)) {
        PyObject *encoded = PyLong_Type.tp_repr(object);
        if (encoded == NULL) {
            return -1;
        }
        int result = json_append_text(state, encoded);
        Py_DECREF(encoded);
        if (result < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (PyFloat_Check(object)) {
        PyObject *encoded = json_encode_float(state, object);
        if (encoded == NULL) {
            return -1;
        }
        int result = json_append_text(state, encoded);
        Py_DECREF(encoded);
        if (result < 0) {
            return -1;
        }
        json_pop_frame(state);
        return 0;
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        frame->kind = JSON_FRAME_LIST;
        return json_start_list(state, frame);
    }
    if (PyDict_Check(object)) {
        frame->kind = JSON_FRAME_DICT;
        return json_start_dict(state, frame);
    }

    frame->kind = JSON_FRAME_DEFAULT;
    frame->phase = JSON_FRAME_DEFAULT_WAIT;
    if (json_marker_enter(state, frame, object) < 0) {
        return -1;
    }
    state->wait_phase = JSON_WAIT_DEFAULT;
    state->waiting_frame = state->frame_count - 1;
    PyObject *converted = PyObject_CallOneArg(state->defaultfn, object);
    if (converted == NULL) {
        return -1;
    }
    state->wait_phase = JSON_WAIT_NONE;
    int result = json_accept_default(state, converted);
    Py_DECREF(converted);
    return result;
}

static int
json_make_list_prefix(
    JsonEncoderState *state,
    JsonFrame *frame,
    Py_ssize_t index,
    PyObject **prefix
)
{
    PyObject *result;
    if (index == 0) {
        result = PyUnicode_FromString("[");
    }
    else {
        result = Py_NewRef(state->item_separator);
    }
    if (result == NULL) {
        return -1;
    }
    if (state->indent != Py_None) {
        PyObject *indent = PySequence_Repeat(
            state->indent,
            frame->indent_level + 1
        );
        if (indent == NULL) {
            Py_DECREF(result);
            return -1;
        }
        PyObject *newline = PyUnicode_FromString("\n");
        if (newline == NULL) {
            Py_DECREF(indent);
            Py_DECREF(result);
            return -1;
        }
        PyUnicode_AppendAndDel(&newline, indent);
        PyUnicode_AppendAndDel(&result, newline);
    }
    *prefix = result;
    return 0;
}

static int
json_append_list_simple(
    JsonEncoderState *state,
    JsonFrame *frame,
    Py_ssize_t index,
    PyObject *object,
    int *is_simple
)
{
    *is_simple = 1;
    PyObject *encoded;
    if (PyUnicode_Check(object)) {
        encoded = json_encode_string(state, object);
    }
    else if (object == Py_None) {
        encoded = PyUnicode_FromString("null");
    }
    else if (object == Py_True) {
        encoded = PyUnicode_FromString("true");
    }
    else if (object == Py_False) {
        encoded = PyUnicode_FromString("false");
    }
    else if (PyLong_Check(object)) {
        encoded = PyLong_Type.tp_repr(object);
    }
    else if (PyFloat_Check(object)) {
        encoded = json_encode_float(state, object);
    }
    else {
        *is_simple = 0;
        return 0;
    }
    if (encoded == NULL) {
        return -1;
    }

    PyObject *prefix;
    if (json_make_list_prefix(state, frame, index, &prefix) < 0) {
        Py_DECREF(encoded);
        return -1;
    }
    PyObject *combined = PyUnicode_Concat(prefix, encoded);
    Py_DECREF(prefix);
    Py_DECREF(encoded);
    if (combined == NULL) {
        return -1;
    }
    int result = json_append_text(state, combined);
    Py_DECREF(combined);
    return result;
}

static int
json_process_list(JsonEncoderState *state)
{
    JsonFrame *frame = &state->frames[state->frame_count - 1];
    Py_ssize_t size = PyList_GET_SIZE(frame->items);
    if (frame->index >= size) {
        return json_finish_container(state, frame, ']');
    }
    Py_ssize_t index = frame->index++;
    PyObject *object = PyList_GET_ITEM(frame->items, index);
    int is_simple;
    if (json_append_list_simple(
            state,
            frame,
            index,
            object,
            &is_simple
        ) < 0) {
        return -1;
    }
    frame->first = 0;
    if (is_simple) {
        return 0;
    }

    PyObject *prefix;
    if (json_make_list_prefix(state, frame, index, &prefix) < 0) {
        return -1;
    }
    int result = json_append_text(state, prefix);
    Py_DECREF(prefix);
    if (result < 0) {
        return -1;
    }
    frame->phase = JSON_FRAME_LIST_VALUE;
    return json_push_value(
        state,
        object,
        frame->indent_level + (state->indent == Py_None ? 0 : 1)
    );
}

static int
json_process_dict(JsonEncoderState *state)
{
    JsonFrame *frame = &state->frames[state->frame_count - 1];
    if (frame->phase == JSON_FRAME_DICT_SORT) {
        Py_ssize_t size = PyList_GET_SIZE(frame->items);
        if (frame->sort_i >= size) {
            frame->phase = JSON_FRAME_DICT_READY;
        }
        else if (frame->sort_j > 0) {
            PyObject *left = PyTuple_GET_ITEM(
                PyList_GET_ITEM(frame->items, frame->sort_j),
                0
            );
            PyObject *right = PyTuple_GET_ITEM(
                PyList_GET_ITEM(frame->items, frame->sort_j - 1),
                0
            );
            state->wait_phase = JSON_WAIT_SORT;
            state->waiting_frame = state->frame_count - 1;
            int comparison = PyObject_RichCompareBool(left, right, Py_LT);
            if (comparison < 0) {
                return -1;
            }
            state->wait_phase = JSON_WAIT_NONE;
            if (comparison) {
                json_swap_items(frame->items, frame->sort_j - 1, frame->sort_j);
                frame->sort_j--;
                if (frame->sort_j == 0) {
                    frame->sort_i++;
                    frame->sort_j = frame->sort_i;
                }
            }
            else {
                frame->sort_i++;
                frame->sort_j = frame->sort_i;
            }
            return 0;
        }
    }
    if (frame->phase == JSON_FRAME_DICT_READY) {
        Py_ssize_t size = PyList_GET_SIZE(frame->items);
        while (frame->index < size) {
            Py_ssize_t index = frame->index++;
            PyObject *item = PyList_GET_ITEM(frame->items, index);
            if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
                PyErr_SetString(PyExc_ValueError, "items must return 2-tuples");
                return -1;
            }
            PyObject *key = PyTuple_GET_ITEM(item, 0);
            PyObject *keystr = json_encode_key(state, key);
            if (keystr == NULL) {
                return -1;
            }
            if (keystr == Py_None) {
                Py_DECREF(keystr);
                continue;
            }
            if (frame->first) {
                if (state->indent != Py_None &&
                    json_append_indent(state, frame->indent_level + 1) < 0) {
                    Py_DECREF(keystr);
                    return -1;
                }
            }
            else {
                if (json_append_text(state, state->item_separator) < 0 ||
                    (state->indent != Py_None &&
                     json_append_indent(state, frame->indent_level + 1) < 0)) {
                    Py_DECREF(keystr);
                    return -1;
                }
            }
            frame->first = 0;
            state->wait_phase = JSON_WAIT_ENCODER_KEY;
            state->waiting_frame = state->frame_count - 1;
            frame->phase = JSON_FRAME_DICT_KEY;
            Py_XSETREF(frame->pending_key, Py_NewRef(keystr));
            PyObject *encoded = json_encode_string(state, keystr);
            Py_DECREF(keystr);
            if (encoded == NULL) {
                return -1;
            }
            state->wait_phase = JSON_WAIT_NONE;
            int result = json_accept_encoded(state, encoded, 1);
            Py_DECREF(encoded);
            return result;
        }
        return json_finish_container(state, frame, '}');
    }
    return 0;
}

static PyObject *
json_encoder_next_chunk(JsonEncoderState *state)
{
    for (;;) {
        if (state->next_chunk < PyList_GET_SIZE(state->chunks)) {
            return Py_NewRef(PyList_GET_ITEM(
                state->chunks,
                state->next_chunk++
            ));
        }
        if (state->frame_count == 0) {
            PyErr_SetNone(PyExc_StopIteration);
            return NULL;
        }
        JsonFrame *frame = &state->frames[state->frame_count - 1];
        int status;
        if (frame->kind == JSON_FRAME_VALUE) {
            status = json_process_value(state);
        }
        else if (frame->kind == JSON_FRAME_LIST) {
            status = json_process_list(state);
        }
        else if (frame->kind == JSON_FRAME_DICT) {
            status = json_process_dict(state);
        }
        else if (frame->phase == JSON_FRAME_DEFAULT_RESULT) {
            if (json_marker_leave(state, frame) < 0) {
                return NULL;
            }
            json_pop_frame(state);
            continue;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid JSON default state");
            return NULL;
        }
        if (status < 0) {
            return NULL;
        }
    }
}

typedef struct {
    PyObject_HEAD
    JsonEncoderState *state;
} JsonChunkIterator;

static void
json_chunk_iterator_dealloc(JsonChunkIterator *iterator)
{
    JsonEncoderState *state = iterator->state;
    iterator->state = NULL;
    if (state != NULL) {
        state->owner = NULL;
        json_encoder_free_state(state);
    }
    Py_TYPE(iterator)->tp_free((PyObject *)iterator);
}

static PyObject *
json_chunk_iterator_iter(PyObject *object)
{
    return Py_NewRef(object);
}

static PyObject *
json_encoder_next_chunk(JsonEncoderState *state);
static void json_add_error_notes(const JsonEncoderState *state);

static PyObject *
json_chunk_iterator_next(JsonChunkIterator *iterator)
{
    if (iterator->state == NULL) {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &json_encoder_vtable, iterator->state) < 0) {
        return NULL;
    }
    PyObject *result = json_encoder_next_chunk(iterator->state);
    if (result == NULL && PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Clear();
        JsonEncoderState *state = iterator->state;
        iterator->state = NULL;
        state->owner = NULL;
        adapter_leave(&frame);
        json_encoder_free_state(state);
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    if (result == NULL) {
        json_add_error_notes(iterator->state);
    }
    adapter_leave(&frame);
    return result;
}

static PyTypeObject json_chunk_iterator_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "generator",
    .tp_basicsize = sizeof(JsonChunkIterator),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)json_chunk_iterator_dealloc,
    .tp_iter = json_chunk_iterator_iter,
    .tp_iternext = (iternextfunc)json_chunk_iterator_next,
};

static PyObject *
json_chunk_iterator_from_state(JsonEncoderState *state)
{
    JsonChunkIterator *iterator = PyObject_New(
        JsonChunkIterator,
        &json_chunk_iterator_type
    );
    if (iterator == NULL) {
        return NULL;
    }
    iterator->state = state;
    state->owner = Py_NewRef((PyObject *)iterator);
    return (PyObject *)iterator;
}

static PyObject *
json_encoder_continue(
    JsonEncoderState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            return NULL;
        }
        int accepted = 0;
        switch (state->wait_phase) {
            case JSON_WAIT_DEFAULT:
                accepted = json_accept_default(state, resumed_value);
                break;
            case JSON_WAIT_ENCODER_VALUE:
                accepted = json_accept_encoded(state, resumed_value, 0);
                break;
            case JSON_WAIT_ENCODER_KEY:
                accepted = json_accept_encoded(state, resumed_value, 1);
                break;
            case JSON_WAIT_ITEMS:
                accepted = json_accept_items(state, resumed_value);
                break;
            case JSON_WAIT_SORT:
                accepted = json_accept_sort(state, resumed_value);
                break;
            case JSON_WAIT_WRITE:
                PyErr_SetString(PyExc_RuntimeError, "invalid JSON write resume phase");
                accepted = -1;
                break;
            case JSON_WAIT_NONE:
                PyErr_SetString(PyExc_RuntimeError, "invalid JSON resume phase");
                return NULL;
        }
        state->wait_phase = JSON_WAIT_NONE;
        if (accepted < 0) {
            return NULL;
        }
    }

    for (;;) {
        if (state->frame_count == 0) {
            PyObject *separator = PyUnicode_FromString("");
            if (separator == NULL) {
                return NULL;
            }
            PyObject *result = PyUnicode_Join(separator, state->chunks);
            Py_DECREF(separator);
            if (result == NULL) {
                return NULL;
            }
            PyObject *tuple = PyTuple_Pack(1, result);
            Py_DECREF(result);
            return tuple;
        }
        JsonFrame *frame = &state->frames[state->frame_count - 1];
        int status;
        if (frame->kind == JSON_FRAME_VALUE) {
            status = json_process_value(state);
        }
        else if (frame->kind == JSON_FRAME_LIST) {
            status = json_process_list(state);
        }
        else if (frame->kind == JSON_FRAME_DICT) {
            status = json_process_dict(state);
        }
        else {
            if (frame->phase == JSON_FRAME_DEFAULT_RESULT) {
                if (json_marker_leave(state, frame) < 0) {
                    return NULL;
                }
                json_pop_frame(state);
                continue;
            }
            PyErr_SetString(PyExc_RuntimeError, "invalid JSON default state");
            return NULL;
        }
        if (status < 0) {
            return NULL;
        }
    }
}

#if PY_VERSION_HEX >= 0x030e0000
static void
json_add_note(PyObject *object, const char *format, PyObject *key, Py_ssize_t index)
{
    PyObject *exception = PyErr_GetRaisedException();
    if (exception == NULL) {
        return;
    }
    PyObject *note = key == NULL
        ? PyUnicode_FromFormat(format, Py_TYPE(object)->tp_name, index)
        : PyUnicode_FromFormat(format, Py_TYPE(object)->tp_name, key);
    if (note != NULL) {
        PyObject *result = PyObject_CallMethod(exception, "add_note", "O", note);
        if (result == NULL) {
            PyErr_Clear();
        }
        Py_XDECREF(result);
        Py_DECREF(note);
    }
    PyErr_SetRaisedException(exception);
}

static void
json_add_error_notes(const JsonEncoderState *state)
{
    for (Py_ssize_t index = state->frame_count - 2; index >= 0; index--) {
        const JsonFrame *frame = &state->frames[index];
        if (frame->kind == JSON_FRAME_DEFAULT &&
            frame->phase == JSON_FRAME_DEFAULT_RESULT) {
            json_add_note(
                frame->object,
                "when serializing %s object",
                NULL,
                0
            );
        }
        else if (frame->kind == JSON_FRAME_LIST &&
                 frame->phase == JSON_FRAME_LIST_VALUE) {
            json_add_note(
                frame->object,
                "when serializing %s item %zd",
                NULL,
                frame->index - 1
            );
        }
        else if (frame->kind == JSON_FRAME_DICT &&
                 frame->phase == JSON_FRAME_DICT_VALUE &&
                 frame->pending_key != NULL) {
            json_add_note(
                frame->object,
                "when serializing %s item %R",
                frame->pending_key,
                0
            );
        }
        else if (frame->kind == JSON_FRAME_DICT &&
                 frame->phase == JSON_FRAME_DICT_KEY) {
            break;
        }
    }
}
#else
static void
json_add_error_notes(const JsonEncoderState *state)
{
    (void)state;
}
#endif

static int
json_reenter_frames(JsonEncoderState *state)
{
    for (Py_ssize_t index = 0; index < state->frame_count; index++) {
        JsonFrame *frame = &state->frames[index];
        if (!frame->recursive_entered) {
            continue;
        }
        if (Py_EnterRecursiveCall(" while encoding a JSON object") < 0) {
            while (state->recursive_owned > 0) {
                Py_LeaveRecursiveCall();
                state->recursive_owned--;
            }
            return -1;
        }
        state->recursive_owned++;
    }
    return 0;
}

static int json_encoder_write_all(JsonEncoderState *state);
static void json_encoder_capture_writer_position(JsonEncoderState *state);
static int json_encoder_restore_writer(JsonEncoderState *state);

static PyObject *
json_encoder_resume(const void *raw_state, PyObject *value)
{
    JsonEncoderState *state = json_encoder_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    if (value != NULL && json_reenter_frames(state) < 0) {
        json_encoder_free_state(state);
        return NULL;
    }
    AleffAdapterFrame adapter_frame;
    if (adapter_enter(&adapter_frame, &json_encoder_vtable, state) < 0) {
        json_encoder_free_state(state);
        return NULL;
    }
    PyObject *result;
    int transferred = 0;
    if (state->write_chunks) {
        if (value == NULL) {
            json_add_error_notes(state);
            adapter_leave(&adapter_frame);
            json_encoder_free_state(state);
            return NULL;
        }
        int writer_restored = json_encoder_restore_writer(state);
        if (writer_restored < 0) {
            adapter_leave(&adapter_frame);
            json_encoder_free_state(state);
            return NULL;
        }
        if (writer_restored) {
            state->next_chunk = 0;
        }
        int accepted = 0;
        switch (state->wait_phase) {
            case JSON_WAIT_DEFAULT:
                accepted = json_accept_default(state, value);
                break;
            case JSON_WAIT_ENCODER_VALUE:
                accepted = json_accept_encoded(state, value, 0);
                break;
            case JSON_WAIT_ENCODER_KEY:
                accepted = json_accept_encoded(state, value, 1);
                break;
            case JSON_WAIT_ITEMS:
                accepted = json_accept_items(state, value);
                break;
            case JSON_WAIT_SORT:
                accepted = json_accept_sort(state, value);
                break;
            case JSON_WAIT_WRITE:
                accepted = 0;
                break;
            case JSON_WAIT_NONE:
                PyErr_SetString(PyExc_RuntimeError, "invalid JSON resume phase");
                accepted = -1;
                break;
        }
        state->wait_phase = JSON_WAIT_NONE;
        result = accepted < 0 || json_encoder_write_all(state) < 0
            ? NULL : Py_NewRef(Py_None);
    }
    else if (state->chunked_result) {
        if (value == NULL) {
            json_add_error_notes(state);
            adapter_leave(&adapter_frame);
            json_encoder_free_state(state);
            return NULL;
        }
        int accepted = 0;
        switch (state->wait_phase) {
            case JSON_WAIT_DEFAULT:
                accepted = json_accept_default(state, value);
                break;
            case JSON_WAIT_ENCODER_VALUE:
                accepted = json_accept_encoded(state, value, 0);
                break;
            case JSON_WAIT_ENCODER_KEY:
                accepted = json_accept_encoded(state, value, 1);
                break;
            case JSON_WAIT_ITEMS:
                accepted = json_accept_items(state, value);
                break;
            case JSON_WAIT_SORT:
                accepted = json_accept_sort(state, value);
                break;
            case JSON_WAIT_WRITE:
                PyErr_SetString(PyExc_RuntimeError, "invalid JSON write resume phase");
                accepted = -1;
                break;
            case JSON_WAIT_NONE:
                PyErr_SetString(PyExc_RuntimeError, "invalid JSON resume phase");
                accepted = -1;
                break;
        }
        state->wait_phase = JSON_WAIT_NONE;
        result = accepted < 0 ? NULL : json_encoder_next_chunk(state);
    }
    else {
        result = json_encoder_continue(state, value, 1);
    }
    if (result == NULL) {
        json_add_error_notes(state);
    }
    adapter_leave(&adapter_frame);
    if (result != NULL && state->chunked_result && state->owner != NULL) {
        JsonChunkIterator *owner = (JsonChunkIterator *)state->owner;
        PyObject *owner_reference = state->owner;
        JsonEncoderState *old_state = owner->state;
        owner->state = state;
        state->owner = NULL;
        if (old_state != NULL) {
            old_state->owner = NULL;
            json_encoder_free_state(old_state);
        }
        Py_DECREF(owner_reference);
        transferred = 1;
    }
    if (!transferred) {
        json_encoder_free_state(state);
    }
    return result;
}

static const AleffAdapterVTable json_encoder_vtable = {
    .copy_state = json_encoder_copy_state,
    .free_state = json_encoder_free_state,
    .resume = json_encoder_resume,
    .prepare_resume = NULL,
};

static int json_encoder_state_init(
    JsonEncoderState *state,
    JsonCEncoderObject *encoder,
    Py_ssize_t indent_level,
    PyObject *floatstr,
    PyObject *writer,
    int chunked_result,
    int write_chunks
);

static PyObject *
json_encoder_call_impl(
    PyObject *object,
    PyObject *args,
    PyObject *kwargs,
    PyObject *floatstr,
    int chunked_result,
    int graph_proven
)
{
    static char *keywords[] = {"obj", "_current_indent_level", NULL};
    PyObject *value;
    Py_ssize_t indent_level;
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "On:_iterencode",
            keywords,
            &value,
            &indent_level
        )) {
        return NULL;
    }

    JsonCEncoderObject *encoder = (JsonCEncoderObject *)object;
    if (!graph_proven) {
        int can_delegate = json_can_use_original_encoder(
            encoder,
            value,
            chunked_result
        );
        if (can_delegate < 0) {
            return NULL;
        }
        if (can_delegate) {
            return original_encoder_call(object, args, kwargs);
        }
    }

    JsonEncoderState state;
    if (json_encoder_state_init(
            &state,
            encoder,
            indent_level,
            floatstr,
            NULL,
            chunked_result,
            0
        ) < 0) {
        json_state_clear(&state);
        return NULL;
    }

    if (json_push_value(&state, value, indent_level) < 0) {
        json_state_clear(&state);
        return NULL;
    }
    if (chunked_result) {
        JsonEncoderState *heap_state = PyMem_Malloc(sizeof(*heap_state));
        if (heap_state == NULL) {
            json_state_clear(&state);
            PyErr_NoMemory();
            return NULL;
        }
        *heap_state = state;
        memset(&state, 0, sizeof(state));
        PyObject *iterator = json_chunk_iterator_from_state(heap_state);
        if (iterator == NULL) {
            json_encoder_free_state(heap_state);
            return NULL;
        }
        return iterator;
    }
    AleffAdapterFrame adapter_frame;
    if (adapter_enter(&adapter_frame, &json_encoder_vtable, &state) < 0) {
        json_state_clear(&state);
        return NULL;
    }
    PyObject *result = json_encoder_continue(&state, NULL, 0);
    if (result == NULL) {
        json_add_error_notes(&state);
    }
    adapter_leave(&adapter_frame);
    json_state_clear(&state);
    return result;
}

static PyObject *
json_encoder_call(PyObject *object, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"obj", "_current_indent_level", NULL};
    PyObject *value;
    Py_ssize_t indent_level;
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "On:_iterencode",
            keywords,
            &value,
            &indent_level
        )) {
        return NULL;
    }
    (void)indent_level;
    int can_delegate = json_can_use_original_encoder(
        (JsonCEncoderObject *)object,
        value,
        0
    );
    if (can_delegate < 0) {
        return NULL;
    }
    if (can_delegate) {
        return original_encoder_call(object, args, kwargs);
    }
    return json_encoder_call_impl(object, args, kwargs, NULL, 0, 1);
}

static PyObject *
json_iterencode_call(PyObject *context, PyObject *args, PyObject *kwargs)
{
    if (!PyTuple_Check(context) || PyTuple_GET_SIZE(context) != 2) {
        PyErr_SetString(PyExc_RuntimeError, "invalid JSON iterencode context");
        return NULL;
    }
    return json_encoder_call_impl(
        PyTuple_GET_ITEM(context, 0),
        args,
        kwargs,
        PyTuple_GET_ITEM(context, 1),
        1,
        0
    );
}

static PyMethodDef json_iterencode_call_method = {
    .ml_name = "_iterencode",
    .ml_meth = (PyCFunction)(void(*)(void))json_iterencode_call,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Encode JSON values into chunks.",
};

static int
json_encoder_state_init(
    JsonEncoderState *state,
    JsonCEncoderObject *encoder,
    Py_ssize_t indent_level,
    PyObject *floatstr,
    PyObject *writer,
    int chunked_result,
    int write_chunks
)
{
    *state = (JsonEncoderState){
        .defaultfn = Py_NewRef(encoder->defaultfn),
        .encoder = Py_NewRef(encoder->encoder),
        .indent = Py_NewRef(encoder->indent),
        .key_separator = Py_NewRef(encoder->key_separator),
        .item_separator = Py_NewRef(encoder->item_separator),
        .markers = encoder->markers == Py_None
            ? NULL : PyDict_Copy(encoder->markers),
        .chunks = PyList_New(0),
        .floatstr = Py_XNewRef(floatstr),
        .writer = Py_XNewRef(writer),
        .waiting_frame = -1,
        .current_indent_level = indent_level,
        .wait_phase = JSON_WAIT_NONE,
        .allow_nan = encoder->allow_nan,
        .sort_keys = encoder->sort_keys,
        .skipkeys = encoder->skipkeys,
        .chunked_result = chunked_result,
        .write_chunks = write_chunks,
    };
    if ((encoder->markers != Py_None && state->markers == NULL) ||
        state->chunks == NULL) {
        json_state_clear(state);
        return -1;
    }
    json_encoder_capture_writer_position(state);
    return 0;
}

static int
json_encoder_write_all(JsonEncoderState *state)
{
    for (;;) {
        PyObject *chunk = json_encoder_next_chunk(state);
        if (chunk == NULL) {
            if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
                PyErr_Clear();
                return 0;
            }
            return -1;
        }
        state->wait_phase = JSON_WAIT_WRITE;
        PyObject *written = PyObject_CallMethod(state->writer, "write", "O", chunk);
        Py_DECREF(chunk);
        if (written == NULL) {
            return -1;
        }
        Py_DECREF(written);
        state->wait_phase = JSON_WAIT_NONE;
    }
}

static void
json_encoder_capture_writer_position(JsonEncoderState *state)
{
    if (state->writer == NULL ||
        (strcmp(Py_TYPE(state->writer)->tp_name, "_io.StringIO") != 0 &&
         strcmp(Py_TYPE(state->writer)->tp_name, "StringIO") != 0)) {
        return;
    }
    PyObject *position = PyObject_CallMethod(state->writer, "tell", NULL);
    if (position == NULL) {
        PyErr_Clear();
        return;
    }
    Py_ssize_t value = PyLong_AsSsize_t(position);
    Py_DECREF(position);
    if (value == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return;
    }
    state->writer_position = value;
    state->writer_checkpoint_valid = 1;
}

static int
json_encoder_restore_writer(JsonEncoderState *state)
{
    if (!state->writer_checkpoint_valid) {
        return 0;
    }
    PyObject *position = PyLong_FromSsize_t(state->writer_position);
    if (position == NULL) {
        return -1;
    }
    PyObject *result = PyObject_CallMethod(state->writer, "seek", "O", position);
    Py_DECREF(position);
    if (result == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            state->writer_checkpoint_valid = 0;
            return 0;
        }
        return -1;
    }
    Py_DECREF(result);
    result = PyObject_CallMethod(state->writer, "truncate", NULL);
    if (result == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            state->writer_checkpoint_valid = 0;
            return 0;
        }
        return -1;
    }
    Py_DECREF(result);
    return 1;
}

static PyObject *
json_normalize_indent(PyObject *indent)
{
    if (indent == Py_None || PyUnicode_Check(indent)) {
        return Py_NewRef(indent);
    }
    PyObject *space = PyUnicode_FromString(" ");
    if (space == NULL) {
        return NULL;
    }
    Py_ssize_t count = PyNumber_AsSsize_t(indent, PyExc_OverflowError);
    if (count == -1 && PyErr_Occurred()) {
        Py_DECREF(space);
        return NULL;
    }
    PyObject *result = PySequence_Repeat(space, count);
    Py_DECREF(space);
    return result;
}

static PyObject *
json_make_iterencode(
    PyObject *Py_UNUSED(module),
    PyObject *args,
    PyObject *kwargs
)
{
    static char *keywords[] = {
        "markers", "_default", "_encoder", "_indent", "_floatstr",
        "_key_separator", "_item_separator", "_sort_keys", "_skipkeys",
        "_one_shot", NULL
    };
    PyObject *markers;
    PyObject *defaultfn;
    PyObject *encoder;
    PyObject *indent;
    PyObject *floatstr;
    PyObject *key_separator;
    PyObject *item_separator;
    PyObject *sort_keys;
    PyObject *skipkeys;
    PyObject *one_shot;
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OOOOOOOOOO:_make_iterencode",
            keywords,
            &markers,
            &defaultfn,
            &encoder,
            &indent,
            &floatstr,
            &key_separator,
            &item_separator,
            &sort_keys,
            &skipkeys,
            &one_shot
        )) {
        return NULL;
    }
    (void)one_shot;

    PyObject *normalized_indent = json_normalize_indent(indent);
    if (normalized_indent == NULL) {
        return NULL;
    }

    PyObject *allow_nan = Py_True;
    PyObject *encoder_object = PyObject_CallFunctionObjArgs(
        (PyObject *)installed_encoder_type,
        markers,
        defaultfn,
        encoder,
        normalized_indent,
        key_separator,
        item_separator,
        sort_keys,
        skipkeys,
        allow_nan,
        NULL
    );
    Py_DECREF(normalized_indent);
    if (encoder_object == NULL) {
        return NULL;
    }
    PyObject *context = PyTuple_Pack(2, encoder_object, floatstr);
    Py_DECREF(encoder_object);
    if (context == NULL) {
        return NULL;
    }
    PyObject *module_name = PyUnicode_FromString("json.encoder");
    if (module_name == NULL) {
        Py_DECREF(context);
        return NULL;
    }
    PyObject *result = PyCFunction_NewEx(
        &json_iterencode_call_method,
        context,
        module_name
    );
    Py_DECREF(module_name);
    Py_DECREF(context);
    return result;
}

static PyMethodDef json_make_iterencode_method = {
    .ml_name = "_make_iterencode",
    .ml_meth = (PyCFunction)(void(*)(void))json_make_iterencode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Create the continuation-aware JSON encoder.",
};

static PyObject *
json_dump_bridge(PyObject *Py_UNUSED(module), PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "obj", "fp", "skipkeys", "ensure_ascii", "check_circular",
        "allow_nan", "cls", "indent", "separators", "default",
        "sort_keys", NULL
    };
    PyObject *value;
    PyObject *writer;
    int skipkeys = 0;
    int ensure_ascii = 1;
    int check_circular = 1;
    int allow_nan = 1;
    PyObject *cls = Py_None;
    PyObject *indent = Py_None;
    PyObject *separators = Py_None;
    PyObject *defaultfn = Py_None;
    int sort_keys = 0;
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OO|ppppOOOOp:dump",
            keywords,
            &value,
            &writer,
            &skipkeys,
            &ensure_ascii,
            &check_circular,
            &allow_nan,
            &cls,
            &indent,
            &separators,
            &defaultfn,
            &sort_keys
        )) {
        return NULL;
    }

    PyObject *json_module = PyImport_ImportModule("json");
    if (json_module == NULL) {
        return NULL;
    }
    PyObject *encoder_class = PyObject_GetAttrString(json_module, "JSONEncoder");
    if (encoder_class == NULL) {
        Py_DECREF(json_module);
        return NULL;
    }
    if (cls != Py_None && cls != encoder_class) {
        Py_DECREF(encoder_class);
        Py_DECREF(json_module);
        return PyObject_Call(original_json_dump, args, kwargs);
    }

    PyObject *encoder_kwargs = PyDict_New();
    if (encoder_kwargs == NULL) {
        Py_DECREF(encoder_class);
        Py_DECREF(json_module);
        return NULL;
    }
#define SET_DUMP_OPTION(name, object) \
    do { \
        if (PyDict_SetItemString(encoder_kwargs, name, object) < 0) { \
            Py_DECREF(encoder_kwargs); \
            Py_DECREF(encoder_class); \
            Py_DECREF(json_module); \
            return NULL; \
        } \
    } while (0)
    SET_DUMP_OPTION("skipkeys", skipkeys ? Py_True : Py_False);
    SET_DUMP_OPTION("ensure_ascii", ensure_ascii ? Py_True : Py_False);
    SET_DUMP_OPTION("check_circular", check_circular ? Py_True : Py_False);
    SET_DUMP_OPTION("allow_nan", allow_nan ? Py_True : Py_False);
    SET_DUMP_OPTION("indent", indent);
    SET_DUMP_OPTION("separators", separators);
    SET_DUMP_OPTION("default", defaultfn);
    SET_DUMP_OPTION("sort_keys", sort_keys ? Py_True : Py_False);
#undef SET_DUMP_OPTION

    PyObject *empty_args = PyTuple_New(0);
    PyObject *encoder_object = empty_args == NULL
        ? NULL
        : PyObject_Call(encoder_class, empty_args, encoder_kwargs);
    Py_XDECREF(empty_args);
    Py_DECREF(encoder_kwargs);
    if (encoder_object == NULL) {
        Py_DECREF(encoder_class);
        Py_DECREF(json_module);
        return NULL;
    }
    Py_DECREF(encoder_class);

    PyObject *markers = check_circular ? PyDict_New() : Py_NewRef(Py_None);
    PyObject *default_object = PyObject_GetAttrString(encoder_object, "default");
    PyObject *ensure_ascii_object = PyObject_GetAttrString(
        encoder_object,
        "ensure_ascii"
    );
    PyObject *encoder_module = ensure_ascii_object == NULL
        ? NULL : PyImport_ImportModule("json.encoder");
    int ensure_ascii_value = encoder_module == NULL
        ? -1 : PyObject_IsTrue(ensure_ascii_object);
    PyObject *string_encoder = ensure_ascii_value < 0 || encoder_module == NULL
        ? NULL
        : PyObject_GetAttrString(
            encoder_module,
            ensure_ascii_value ? "encode_basestring_ascii" : "encode_basestring"
        );
    PyObject *encoder_indent = PyObject_GetAttrString(encoder_object, "indent");
    PyObject *key_separator = PyObject_GetAttrString(encoder_object, "key_separator");
    PyObject *item_separator = PyObject_GetAttrString(encoder_object, "item_separator");
    PyObject *encoder_sort_keys = PyObject_GetAttrString(encoder_object, "sort_keys");
    PyObject *encoder_skipkeys = PyObject_GetAttrString(encoder_object, "skipkeys");
    PyObject *encoder_allow_nan = PyObject_GetAttrString(encoder_object, "allow_nan");
    if (encoder_indent != NULL) {
        PyObject *normalized_indent = json_normalize_indent(encoder_indent);
        if (normalized_indent == NULL) {
            Py_DECREF(encoder_object);
            Py_XDECREF(markers);
            Py_XDECREF(default_object);
            Py_XDECREF(ensure_ascii_object);
            Py_XDECREF(encoder_module);
            Py_XDECREF(encoder_indent);
            Py_XDECREF(string_encoder);
            Py_XDECREF(key_separator);
            Py_XDECREF(item_separator);
            Py_XDECREF(encoder_sort_keys);
            Py_XDECREF(encoder_skipkeys);
            Py_XDECREF(encoder_allow_nan);
            Py_DECREF(json_module);
            return NULL;
        }
        Py_DECREF(encoder_indent);
        encoder_indent = normalized_indent;
    }
    Py_DECREF(encoder_object);
    Py_XDECREF(ensure_ascii_object);
    Py_XDECREF(encoder_module);
    Py_DECREF(json_module);
    if (markers == NULL || default_object == NULL || string_encoder == NULL ||
        encoder_indent == NULL || key_separator == NULL || item_separator == NULL ||
        encoder_sort_keys == NULL || encoder_skipkeys == NULL ||
        encoder_allow_nan == NULL) {
        Py_XDECREF(markers);
        Py_XDECREF(default_object);
        Py_XDECREF(string_encoder);
        Py_XDECREF(encoder_indent);
        Py_XDECREF(key_separator);
        Py_XDECREF(item_separator);
        Py_XDECREF(encoder_sort_keys);
        Py_XDECREF(encoder_skipkeys);
        Py_XDECREF(encoder_allow_nan);
        return NULL;
    }

    PyObject *c_encoder = PyObject_CallFunctionObjArgs(
        (PyObject *)installed_encoder_type,
        markers,
        default_object,
        string_encoder,
        encoder_indent,
        key_separator,
        item_separator,
        encoder_sort_keys,
        encoder_skipkeys,
        encoder_allow_nan,
        NULL
    );
    Py_DECREF(markers);
    Py_DECREF(default_object);
    Py_DECREF(string_encoder);
    Py_DECREF(encoder_indent);
    Py_DECREF(key_separator);
    Py_DECREF(item_separator);
    Py_DECREF(encoder_sort_keys);
    Py_DECREF(encoder_skipkeys);
    Py_DECREF(encoder_allow_nan);
    if (c_encoder == NULL) {
        return NULL;
    }

    JsonEncoderState state;
    int initialized = json_encoder_state_init(
        &state,
        (JsonCEncoderObject *)c_encoder,
        0,
        NULL,
        writer,
        0,
        1
    );
    Py_DECREF(c_encoder);
    if (initialized < 0) {
        return NULL;
    }
    if (json_push_value(&state, value, 0) < 0) {
        json_state_clear(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &json_encoder_vtable, &state) < 0) {
        json_state_clear(&state);
        return NULL;
    }
    int result = json_encoder_write_all(&state);
    if (result < 0) {
        json_add_error_notes(&state);
    }
    adapter_leave(&frame);
    json_state_clear(&state);
    if (result < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyMethodDef json_dump_bridge_method = {
    .ml_name = "_aleff_json_dump",
    .ml_meth = (PyCFunction)(void(*)(void))json_dump_bridge,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Continuation-aware JSON dump implementation.",
};

int
adapter_json_encoder_install(PyObject *module)
{
    if (encoder_installed) {
        return 0;
    }
    PyObject *factory = PyObject_GetAttrString(module, "make_encoder");
    if (factory == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            return -1;
        }
        PyErr_Clear();
        PyObject *accelerator = PyImport_ImportModule("_json");
        if (accelerator == NULL) {
            return -1;
        }
        factory = PyObject_GetAttrString(accelerator, "make_encoder");
        Py_DECREF(accelerator);
    }
    if (factory == NULL) {
        return -1;
    }
    if (!PyType_Check(factory)) {
        Py_DECREF(factory);
        PyErr_SetString(PyExc_RuntimeError, "_json.make_encoder is not a type");
        return -1;
    }
    PyTypeObject *type = (PyTypeObject *)factory;
    if (type->tp_call == NULL) {
        Py_DECREF(factory);
        PyErr_SetString(PyExc_RuntimeError, "_json.Encoder has no call slot");
        return -1;
    }
    if (PyType_Ready(&json_chunk_iterator_type) < 0) {
        Py_DECREF(factory);
        return -1;
    }

    PyObject *encoder_module = PyImport_ImportModule("json.encoder");
    if (encoder_module == NULL) {
        Py_DECREF(factory);
        return -1;
    }
    PyObject *make_iterencode = PyObject_GetAttrString(
        encoder_module,
        "_make_iterencode"
    );
    if (make_iterencode == NULL) {
        Py_DECREF(encoder_module);
        Py_DECREF(factory);
        return -1;
    }
    PyObject *encode_basestring = PyObject_GetAttrString(
        encoder_module,
        "encode_basestring"
    );
    PyObject *encode_basestring_ascii = PyObject_GetAttrString(
        encoder_module,
        "encode_basestring_ascii"
    );
    if (encode_basestring == NULL || encode_basestring_ascii == NULL) {
        Py_XDECREF(encode_basestring);
        Py_XDECREF(encode_basestring_ascii);
        Py_DECREF(make_iterencode);
        Py_DECREF(encoder_module);
        Py_DECREF(factory);
        return -1;
    }
    original_encode_basestring = encode_basestring;
    original_encode_basestring_ascii = encode_basestring_ascii;
    PyObject *module_name = PyUnicode_FromString("json.encoder");
    PyObject *replacement = module_name == NULL
        ? NULL
        : PyCFunction_NewEx(
            &json_make_iterencode_method,
            NULL,
            module_name
        );
    Py_XDECREF(module_name);
    if (replacement == NULL || PyObject_SetAttrString(
            encoder_module,
            "_make_iterencode",
            replacement
        ) < 0) {
        Py_XDECREF(replacement);
        Py_CLEAR(original_encode_basestring);
        Py_CLEAR(original_encode_basestring_ascii);
        Py_DECREF(make_iterencode);
        Py_DECREF(encoder_module);
        Py_DECREF(factory);
        return -1;
    }
    Py_DECREF(replacement);
    original_make_iterencode = make_iterencode;
    installed_encoder_module = encoder_module;
    installed_encoder_type = type;
    original_encoder_call = type->tp_call;
    type->tp_call = json_encoder_call;
    PyType_Modified(type);

    original_json_dump = PyObject_GetAttrString(module, "dump");
    if (original_json_dump == NULL) {
        adapter_json_encoder_rollback();
        Py_DECREF(factory);
        return -1;
    }
    installed_json_module = Py_NewRef(module);
    PyObject *json_name = PyUnicode_FromString("json");
    PyObject *dump_bridge = json_name == NULL
        ? NULL
        : PyCFunction_NewEx(&json_dump_bridge_method, NULL, json_name);
    Py_XDECREF(json_name);
    if (dump_bridge == NULL || PyObject_SetAttrString(
            module,
            "_aleff_json_dump",
            dump_bridge
        ) < 0) {
        Py_XDECREF(dump_bridge);
        adapter_json_encoder_rollback();
        Py_DECREF(factory);
        return -1;
    }
    Py_DECREF(dump_bridge);
    const char *dump_wrapper_source =
        "def dump(obj, fp, *, skipkeys=False, ensure_ascii=True, "
        "check_circular=True, allow_nan=True, cls=None, indent=None, "
        "separators=None, default=None, sort_keys=False, **kw):\n"
        "    return _aleff_json_dump(obj, fp, skipkeys=skipkeys, "
        "ensure_ascii=ensure_ascii, check_circular=check_circular, "
        "allow_nan=allow_nan, cls=cls, indent=indent, separators=separators, "
        "default=default, sort_keys=sort_keys, **kw)\n";
    PyObject *globals = PyDict_New();
    PyObject *name = PyUnicode_FromString("json");
    PyObject *source = PyUnicode_FromString(dump_wrapper_source);
    PyObject *code = (globals == NULL || name == NULL || source == NULL)
        ? NULL
        : Py_CompileString(
            PyUnicode_AsUTF8(source),
            "<aleff json dump>",
            Py_file_input
        );
    if (globals == NULL || name == NULL || source == NULL || code == NULL ||
        PyDict_SetItemString(globals, "__name__", name) < 0 ||
        PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) < 0) {
        Py_XDECREF(code);
        Py_XDECREF(source);
        Py_XDECREF(name);
        Py_XDECREF(globals);
        adapter_json_encoder_rollback();
        Py_DECREF(factory);
        return -1;
    }
    Py_DECREF(source);
    Py_DECREF(name);
    PyObject *bridge = PyObject_GetAttrString(module, "_aleff_json_dump");
    if (bridge == NULL || PyDict_SetItemString(globals, "_aleff_json_dump", bridge) < 0) {
        Py_XDECREF(bridge);
        Py_DECREF(code);
        Py_DECREF(globals);
        adapter_json_encoder_rollback();
        Py_DECREF(factory);
        return -1;
    }
    Py_DECREF(bridge);
    PyObject *evaluated = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    Py_XDECREF(evaluated);
    PyObject *dump_wrapper = evaluated == NULL
        ? NULL : PyDict_GetItemString(globals, "dump");
    if (dump_wrapper == NULL || PyObject_SetAttrString(module, "dump", dump_wrapper) < 0) {
        Py_DECREF(globals);
        adapter_json_encoder_rollback();
        Py_DECREF(factory);
        return -1;
    }
    Py_DECREF(globals);
    encoder_installed = 1;
    Py_DECREF(factory);
    return 0;
}

void
adapter_json_encoder_rollback(void)
{
    if (installed_json_module != NULL && original_json_dump != NULL) {
        if (PyObject_SetAttrString(
                installed_json_module,
                "dump",
                original_json_dump
            ) < 0) {
            PyErr_Clear();
        }
        if (PyObject_DelAttrString(
                installed_json_module,
                "_aleff_json_dump"
            ) < 0) {
            PyErr_Clear();
        }
    }
    Py_XDECREF(original_json_dump);
    Py_XDECREF(installed_json_module);
    original_json_dump = NULL;
    installed_json_module = NULL;
    if (installed_encoder_module != NULL && original_make_iterencode != NULL) {
        if (PyObject_SetAttrString(
                installed_encoder_module,
                "_make_iterencode",
                original_make_iterencode
            ) < 0) {
            PyErr_Clear();
        }
    }
    Py_XDECREF(original_make_iterencode);
    Py_XDECREF(installed_encoder_module);
    original_make_iterencode = NULL;
    installed_encoder_module = NULL;
    Py_XDECREF(original_encode_basestring);
    Py_XDECREF(original_encode_basestring_ascii);
    original_encode_basestring = NULL;
    original_encode_basestring_ascii = NULL;
    if (installed_encoder_type != NULL && original_encoder_call != NULL) {
        installed_encoder_type->tp_call = original_encoder_call;
        PyType_Modified(installed_encoder_type);
    }
    installed_encoder_type = NULL;
    original_encoder_call = NULL;
    encoder_installed = 0;
}
