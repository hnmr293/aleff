#define PY_SSIZE_T_CLEAN

#include "api.h"
#include "zoneinfo.h"
#include "internal.h"

#include <stdint.h>
#include <string.h>

typedef enum {
    ZONE_READ_MAGIC,
    ZONE_READ_VERSION,
    ZONE_READ_RESERVED,
    ZONE_READ_COUNTS,
    ZONE_READ_TIMES,
    ZONE_READ_INDICES,
    ZONE_READ_TYPES,
    ZONE_READ_ABBREVIATIONS,
    ZONE_READ_LEAPS,
    ZONE_READ_STANDARD,
    ZONE_READ_UTC,
    ZONE_FINISHED,
} ZonePhase;

typedef struct {
    PyObject *cls;
    PyObject *file;
    PyObject *args;
    PyObject *kwargs;
    PyObject *data;
    PyObject *original;
    ZonePhase phase;
    Py_ssize_t header_offset;
    Py_ssize_t pending_size;
    Py_ssize_t counts[6];
    int version;
    int block;
} ZoneState;

typedef struct {
    PyObject_HEAD
    PyObject *data;
    Py_ssize_t position;
} ZoneBufferObject;

static PyTypeObject *zoneinfo_type;
static PyObject *zoneinfo_original_from_file;
static PyMethodDef zoneinfo_method;
static int zoneinfo_installed;

static const AleffAdapterVTable zone_vtable;

static void
zone_state_clear(ZoneState *state)
{
    Py_XDECREF(state->cls);
    Py_XDECREF(state->file);
    Py_XDECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_XDECREF(state->data);
    Py_XDECREF(state->original);
}

static void *
zone_copy_state(const void *raw_state)
{
    const ZoneState *source = raw_state;
    ZoneState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->cls = Py_NewRef(source->cls);
    copy->file = Py_NewRef(source->file);
    copy->args = Py_NewRef(source->args);
    copy->kwargs = Py_XNewRef(source->kwargs);
    copy->data = PyByteArray_FromObject(source->data);
    copy->original = Py_NewRef(source->original);
    if (copy->data == NULL) {
        zone_state_clear(copy);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
zone_free_state(void *raw_state)
{
    ZoneState *state = raw_state;
    if (state == NULL) return;
    zone_state_clear(state);
    PyMem_Free(state);
}

static PyObject *zone_drive(ZoneState *state);

static PyObject *
zone_call_original(ZoneState *state, PyObject *file)
{
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    PyObject *args = PyTuple_New(count + 1);
    if (args == NULL) return NULL;
    PyTuple_SET_ITEM(args, 0, Py_NewRef(state->cls));
    PyTuple_SET_ITEM(args, 1, Py_NewRef(file));
    for (Py_ssize_t index = 1; index < count; index++) {
        PyTuple_SET_ITEM(
            args,
            index + 1,
            Py_NewRef(PyTuple_GET_ITEM(state->args, index))
        );
    }
    PyObject *result = PyObject_Call(state->original, args, state->kwargs);
    Py_DECREF(args);
    return result;
}

static PyObject *
zone_call_direct(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    PyObject *call_args = PyTuple_New(count + 1);
    if (call_args == NULL) return NULL;
    PyTuple_SET_ITEM(call_args, 0, Py_NewRef(cls));
    for (Py_ssize_t index = 0; index < count; index++) {
        PyTuple_SET_ITEM(
            call_args,
            index + 1,
            Py_NewRef(PyTuple_GET_ITEM(args, index))
        );
    }
    PyObject *result = PyObject_Call(
        zoneinfo_original_from_file,
        call_args,
        kwargs
    );
    Py_DECREF(call_args);
    return result;
}

static PyObject *
zone_buffer_read(PyObject *object, PyObject *args)
{
    ZoneBufferObject *buffer = (ZoneBufferObject *)object;
    Py_ssize_t requested;
    if (!PyArg_ParseTuple(args, "n:read", &requested)) return NULL;
    Py_ssize_t size = PyBytes_GET_SIZE(buffer->data);
    Py_ssize_t remaining = size - buffer->position;
    if (requested < 0 || requested > remaining) requested = remaining;
    PyObject *result = PyBytes_FromStringAndSize(
        PyBytes_AS_STRING(buffer->data) + buffer->position,
        requested
    );
    if (result != NULL) buffer->position += requested;
    return result;
}

static void
zone_buffer_dealloc(PyObject *object)
{
    ZoneBufferObject *buffer = (ZoneBufferObject *)object;
    Py_XDECREF(buffer->data);
    Py_TYPE(object)->tp_free(object);
}

static PyMethodDef zone_buffer_methods[] = {
    {"read", zone_buffer_read, METH_VARARGS, "Read bytes for a ZoneInfo parser."},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject zone_buffer_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._zoneinfo_buffer",
    .tp_basicsize = sizeof(ZoneBufferObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = zone_buffer_dealloc,
    .tp_methods = zone_buffer_methods,
};

static PyObject *
zone_make_buffer(ZoneState *state)
{
    ZoneBufferObject *buffer = PyObject_New(ZoneBufferObject, &zone_buffer_type);
    if (buffer == NULL) return NULL;
    Py_ssize_t size = PyByteArray_GET_SIZE(state->data);
    const char *data = size == 0 ? "" : PyByteArray_AS_STRING(state->data);
    buffer->data = PyBytes_FromStringAndSize(
        data,
        size
    );
    if (buffer->data == NULL) {
        Py_DECREF(buffer);
        return NULL;
    }
    buffer->position = 0;
    return (PyObject *)buffer;
}

static PyObject *
zone_finish(ZoneState *state)
{
    PyObject *buffer = zone_make_buffer(state);
    if (buffer == NULL) return NULL;
    PyObject *result = zone_call_original(state, buffer);
    Py_DECREF(buffer);
    return result;
}

static int
zone_append(ZoneState *state, PyObject *value)
{
    if (!PyBytes_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "ZoneInfo.from_file() read() returned non-bytes %.200s",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    Py_ssize_t old_size = PyByteArray_GET_SIZE(state->data);
    Py_ssize_t value_size = PyBytes_GET_SIZE(value);
    if (value_size > PY_SSIZE_T_MAX - old_size) {
        PyErr_NoMemory();
        return -1;
    }
    if (PyByteArray_Resize(state->data, old_size + value_size) < 0) {
        return -1;
    }
    if (value_size != 0) {
        memcpy(
            PyByteArray_AS_STRING(state->data) + old_size,
            PyBytes_AS_STRING(value),
            (size_t)value_size
        );
    }
    return 0;
}

static int
zone_scaled_count(Py_ssize_t count, Py_ssize_t factor, Py_ssize_t *result)
{
    if (count < 0 || count > PY_SSIZE_T_MAX / factor) return -1;
    *result = count * factor;
    return 0;
}

static int
zone_parse_counts(ZoneState *state)
{
    const unsigned char *data = (const unsigned char *)PyByteArray_AS_STRING(
        state->data
    );
    Py_ssize_t offset = state->header_offset + 20;
    for (int index = 0; index < 6; index++) {
        uint32_t value = ((uint32_t)data[offset] << 24) |
            ((uint32_t)data[offset + 1] << 16) |
            ((uint32_t)data[offset + 2] << 8) |
            (uint32_t)data[offset + 3];
        if (value > (uint32_t)PY_SSIZE_T_MAX) return -1;
        state->counts[index] = (Py_ssize_t)value;
        offset += 4;
    }
    return 0;
}

static int
zone_phase_size(ZoneState *state, Py_ssize_t *result)
{
    switch (state->phase) {
        case ZONE_READ_MAGIC: *result = 4; return 0;
        case ZONE_READ_VERSION: *result = 1; return 0;
        case ZONE_READ_RESERVED: *result = 15; return 0;
        case ZONE_READ_COUNTS: *result = 24; return 0;
        case ZONE_READ_TIMES:
            return zone_scaled_count(state->counts[3], 4, result);
        case ZONE_READ_INDICES: *result = state->counts[3]; return 0;
        case ZONE_READ_TYPES:
            return zone_scaled_count(state->counts[4], 6, result);
        case ZONE_READ_ABBREVIATIONS: *result = state->counts[5]; return 0;
        case ZONE_READ_LEAPS:
            return zone_scaled_count(state->counts[2], 8, result);
        case ZONE_READ_STANDARD: *result = state->counts[1]; return 0;
        case ZONE_READ_UTC: *result = state->counts[0]; return 0;
        case ZONE_FINISHED: *result = 0; return 0;
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid ZoneInfo parser phase");
    return -1;
}

static int
zone_advance(ZoneState *state)
{
    switch (state->phase) {
        case ZONE_READ_MAGIC:
            state->phase = ZONE_READ_VERSION;
            return 0;
        case ZONE_READ_VERSION:
            state->phase = ZONE_READ_RESERVED;
            return 0;
        case ZONE_READ_RESERVED:
            state->phase = ZONE_READ_COUNTS;
            return 0;
        case ZONE_READ_COUNTS:
            if (zone_parse_counts(state) < 0) {
                state->phase = ZONE_FINISHED;
                return 0;
            }
            state->phase = ZONE_READ_TIMES;
            return 0;
        case ZONE_READ_TIMES: state->phase = ZONE_READ_INDICES; return 0;
        case ZONE_READ_INDICES: state->phase = ZONE_READ_TYPES; return 0;
        case ZONE_READ_TYPES: state->phase = ZONE_READ_ABBREVIATIONS; return 0;
        case ZONE_READ_ABBREVIATIONS: state->phase = ZONE_READ_LEAPS; return 0;
        case ZONE_READ_LEAPS: state->phase = ZONE_READ_STANDARD; return 0;
        case ZONE_READ_STANDARD: state->phase = ZONE_READ_UTC; return 0;
        case ZONE_READ_UTC:
            if (state->block == 0 && state->version >= 2) {
                state->block = 1;
                state->header_offset = PyByteArray_GET_SIZE(state->data);
                state->phase = ZONE_READ_MAGIC;
            }
            else {
                state->phase = ZONE_FINISHED;
            }
            return 0;
        case ZONE_FINISHED:
            return 0;
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid ZoneInfo parser phase");
    return -1;
}

static int
zone_consume(ZoneState *state, PyObject *value)
{
    if (zone_append(state, value) < 0) return -1;
    if (PyBytes_GET_SIZE(value) != state->pending_size) {
        state->phase = ZONE_FINISHED;
        state->pending_size = 0;
        return 0;
    }
    state->pending_size = 0;
    if (state->phase == ZONE_READ_MAGIC) {
        if (memcmp(
                PyByteArray_AS_STRING(state->data) + state->header_offset,
                "TZif",
                4
            ) != 0) {
            state->phase = ZONE_FINISHED;
            return 0;
        }
    }
    if (state->phase == ZONE_READ_VERSION) {
        state->version = (unsigned char)PyByteArray_AS_STRING(state->data)[
            state->header_offset + 4
        ];
        if (state->version != 0 && state->version != 2 &&
            state->version != 3 && state->version != 4) {
            state->phase = ZONE_FINISHED;
            return 0;
        }
    }
    return zone_advance(state);
}

static PyObject *
zone_drive(ZoneState *state)
{
    for (;;) {
        if (state->phase == ZONE_FINISHED) return zone_finish(state);
        Py_ssize_t size;
        if (zone_phase_size(state, &size) < 0) {
            state->phase = ZONE_FINISHED;
            return zone_finish(state);
        }
        if (size == 0) {
            if (zone_advance(state) < 0) return NULL;
            continue;
        }
        state->pending_size = size;
        PyObject *value = PyObject_CallMethod(state->file, "read", "n", size);
        if (value == NULL) return NULL;
        int status = zone_consume(state, value);
        Py_DECREF(value);
        if (status < 0) return NULL;
        if (state->phase == ZONE_FINISHED) return zone_finish(state);
    }
}

static PyObject *
zone_resume(const void *raw_state, PyObject *value)
{
    const ZoneState *source = raw_state;
    if (value == NULL) return NULL;
    ZoneState *state = zone_copy_state(source);
    if (state == NULL) return NULL;
    if (state->pending_size == 0 || zone_consume(state, value) < 0) {
        zone_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &zone_vtable, state) < 0) {
        zone_free_state(state);
        return NULL;
    }
    PyObject *result = state->phase == ZONE_FINISHED
        ? zone_finish(state) : zone_drive(state);
    adapter_leave(&frame);
    zone_free_state(state);
    return result;
}

static const AleffAdapterVTable zone_vtable = {
    .copy_state = zone_copy_state,
    .free_state = zone_free_state,
    .resume = zone_resume,
    .prepare_resume = NULL,
};

static PyObject *
zone_from_file(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) == 0) {
        return zone_call_direct(cls, args, kwargs);
    }
    ZoneState state = {
        .cls = Py_NewRef(cls),
        .file = Py_NewRef(PyTuple_GET_ITEM(args, 0)),
        .args = Py_NewRef(args),
        .kwargs = Py_XNewRef(kwargs),
        .data = PyByteArray_FromStringAndSize(NULL, 0),
        .original = Py_NewRef(zoneinfo_original_from_file),
        .phase = ZONE_READ_MAGIC,
        .header_offset = 0,
        .pending_size = 0,
        .version = 0,
        .block = 0,
    };
    if (state.data == NULL) {
        zone_state_clear(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &zone_vtable, &state) < 0) {
        zone_state_clear(&state);
        return NULL;
    }
    PyObject *result = zone_drive(&state);
    adapter_leave(&frame);
    zone_state_clear(&state);
    return result;
}

int
adapter_zoneinfo_install(PyObject *module)
{
    if (zoneinfo_installed) return 0;
    PyObject *type_object = PyObject_GetAttrString(module, "ZoneInfo");
    if (type_object == NULL || !PyType_Check(type_object)) {
        Py_XDECREF(type_object);
        return -1;
    }
    zoneinfo_type = (PyTypeObject *)type_object;
    PyObject *dict = PyType_GetDict(zoneinfo_type);
    zoneinfo_original_from_file = dict == NULL
        ? NULL : Py_XNewRef(PyDict_GetItemString(dict, "from_file"));
    if (zoneinfo_original_from_file == NULL ||
        !Py_IS_TYPE(zoneinfo_original_from_file, &PyClassMethodDescr_Type) ||
        PyType_Ready(&zone_buffer_type) < 0) {
        Py_XDECREF(dict);
        Py_DECREF(type_object);
        adapter_zoneinfo_rollback();
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "cannot access ZoneInfo.from_file");
        }
        return -1;
    }
    zoneinfo_method = *((PyMethodDescrObject *)zoneinfo_original_from_file)->d_method;
    zoneinfo_method.ml_meth = _PyCFunction_CAST(zone_from_file);
    zoneinfo_method.ml_flags = METH_VARARGS | METH_KEYWORDS | METH_CLASS;
    PyObject *descriptor = PyDescr_NewClassMethod(zoneinfo_type, &zoneinfo_method);
    if (descriptor == NULL || aleff_adapter_register_callable(descriptor) < 0 ||
        PyDict_SetItemString(dict, "from_file", descriptor) < 0) {
        Py_XDECREF(descriptor);
        Py_DECREF(dict);
        Py_DECREF(type_object);
        adapter_zoneinfo_rollback();
        return -1;
    }
    Py_DECREF(descriptor);
    Py_DECREF(dict);
    PyType_Modified(zoneinfo_type);
    zoneinfo_installed = 1;
    Py_DECREF(type_object);
    return 0;
}

void
adapter_zoneinfo_rollback(void)
{
    if (zoneinfo_type != NULL && zoneinfo_original_from_file != NULL) {
        PyObject *dict = PyType_GetDict(zoneinfo_type);
        if (dict != NULL) {
            PyDict_SetItemString(dict, "from_file", zoneinfo_original_from_file);
            Py_DECREF(dict);
        }
        PyType_Modified(zoneinfo_type);
    }
    Py_CLEAR(zoneinfo_original_from_file);
    zoneinfo_type = NULL;
    zoneinfo_installed = 0;
}
