#include "buffers.h"

typedef enum {
    BUFFER_STAGE_ACQUIRE,
    BUFFER_STAGE_CALL,
    BUFFER_STAGE_RELEASE,
} BufferStage;

typedef struct {
    PyObject *callable;
    PyObject *receiver;
    PyObject *args;
    PyObject *kwargs;
    PyObject *owners;
    PyObject *views;
    PyObject *result;
    PyObject *exception;
    newfunc constructor;
    AleffBufferArgument *arguments;
    Py_ssize_t argument_count;
    Py_ssize_t argument_index;
    Py_ssize_t pending_index;
    Py_ssize_t acquired_count;
    Py_ssize_t release_index;
    BufferStage stage;
} BufferState;

static const AleffAdapterVTable buffer_vtable;

PyObject *
adapter_wrap_python_callable(PyObject *original, PyObject *bridge)
{
    PyObject *globals = PyDict_New();
    if (globals == NULL) {
        return NULL;
    }
    PyObject *factory = PyRun_String(
        "lambda bridge: (lambda *args, **kwargs: bridge(*args, **kwargs))",
        Py_eval_input,
        globals,
        globals
    );
    Py_DECREF(globals);
    if (factory == NULL) {
        return NULL;
    }
    PyObject *wrapper = PyObject_CallOneArg(factory, bridge);
    Py_DECREF(factory);
    if (wrapper == NULL) {
        return NULL;
    }
    PyObject *functools = PyImport_ImportModule("functools");
    PyObject *update_wrapper = functools == NULL
        ? NULL : PyObject_GetAttrString(functools, "update_wrapper");
    Py_XDECREF(functools);
    if (update_wrapper == NULL) {
        Py_DECREF(wrapper);
        return NULL;
    }
    PyObject *updated = PyObject_CallFunctionObjArgs(
        update_wrapper, wrapper, original, NULL
    );
    Py_DECREF(update_wrapper);
    Py_DECREF(wrapper);
    return updated;
}

static PyObject *
buffer_copy_args(PyObject *source)
{
    Py_ssize_t size = PyTuple_GET_SIZE(source);
    PyObject *copy = PyTuple_New(size);
    if (copy == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < size; index++) {
        PyTuple_SET_ITEM(copy, index, Py_NewRef(PyTuple_GET_ITEM(source, index)));
    }
    return copy;
}

static PyObject *
buffer_new_slots(Py_ssize_t count)
{
    PyObject *slots = PyList_New(count);
    if (slots == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < count; index++) {
        PyList_SET_ITEM(slots, index, Py_NewRef(Py_None));
    }
    return slots;
}

static AleffBufferArgument *
buffer_copy_arguments(
    const AleffBufferArgument *source,
    Py_ssize_t count
)
{
    if (count == 0) {
        return NULL;
    }
    AleffBufferArgument *copy = PyMem_Calloc(count, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (Py_ssize_t index = 0; index < count; index++) {
        copy[index] = source[index];
    }
    return copy;
}

static void
buffer_clear_state(BufferState *state)
{
    Py_XDECREF(state->callable);
    Py_XDECREF(state->receiver);
    Py_XDECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_XDECREF(state->owners);
    Py_XDECREF(state->views);
    Py_XDECREF(state->result);
    Py_XDECREF(state->exception);
    PyMem_Free(state->arguments);
}

static void *
buffer_copy_state(const void *raw_state)
{
    const BufferState *source = raw_state;
    BufferState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->arguments = NULL;
    copy->callable = Py_XNewRef(source->callable);
    copy->receiver = Py_XNewRef(source->receiver);
    copy->args = buffer_copy_args(source->args);
    copy->kwargs = source->kwargs == NULL ? NULL : PyDict_Copy(source->kwargs);
    copy->owners = PyList_GetSlice(source->owners, 0, PyList_GET_SIZE(source->owners));
    copy->views = PyList_GetSlice(source->views, 0, PyList_GET_SIZE(source->views));
    copy->result = Py_XNewRef(source->result);
    copy->exception = Py_XNewRef(source->exception);
    copy->arguments = buffer_copy_arguments(
        source->arguments, source->argument_count
    );
    if (copy->args == NULL ||
        (source->kwargs != NULL && copy->kwargs == NULL) ||
        copy->owners == NULL || copy->views == NULL ||
        (source->argument_count != 0 && copy->arguments == NULL)) {
        buffer_clear_state(copy);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
buffer_free_state(void *raw_state)
{
    BufferState *state = raw_state;
    if (state == NULL) {
        return;
    }
    PyObject *raised = PyErr_GetRaisedException();
    buffer_clear_state(state);
    PyMem_Free(state);
    PyErr_SetRaisedException(raised);
}

static PyObject *
call_raw_buffer(PyObject *object, int flags)
{
    PyObject *descriptor = lookup_raw_special(object, "__buffer__");
    if (descriptor == NULL) {
        return NULL;
    }
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, object, (PyObject *)Py_TYPE(object));
    Py_DECREF(descriptor);
    if (callable == NULL) {
        return NULL;
    }
    PyObject *flag_object = PyLong_FromLong(flags);
    PyObject *result = flag_object == NULL
        ? NULL : PyObject_CallOneArg(callable, flag_object);
    Py_XDECREF(flag_object);
    Py_DECREF(callable);
    return result;
}

static PyObject *
call_raw_release(PyObject *object, PyObject *view)
{
    PyObject *descriptor = lookup_raw_special(object, "__release_buffer__");
    if (descriptor == NULL) {
        return NULL;
    }
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, object, (PyObject *)Py_TYPE(object));
    Py_DECREF(descriptor);
    if (callable == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_CallOneArg(callable, view);
    Py_DECREF(callable);
    return result;
}

static int
has_python_buffer(PyObject *object)
{
    PyObject *descriptor = lookup_raw_special(object, "__buffer__");
    if (descriptor == NULL) {
        PyErr_Clear();
        return 0;
    }
    int result = !Py_IS_TYPE(descriptor, &PyWrapperDescr_Type);
    Py_DECREF(descriptor);
    return result;
}

static PyObject *
buffer_argument(BufferState *state, const AleffBufferArgument *argument, int *duplicate)
{
    PyObject *positional = argument->position < PyTuple_GET_SIZE(state->args)
        ? PyTuple_GET_ITEM(state->args, argument->position) : NULL;
    PyObject *keyword = state->kwargs == NULL || argument->keyword == NULL
        ? NULL : PyDict_GetItemString(state->kwargs, argument->keyword);
    *duplicate = positional != NULL && keyword != NULL;
    return positional != NULL ? positional : keyword;
}

static int
replace_buffer_argument(
    BufferState *state,
    const AleffBufferArgument *argument,
    PyObject *value
)
{
    if (argument->position < PyTuple_GET_SIZE(state->args)) {
        PyObject *previous = PyTuple_GET_ITEM(state->args, argument->position);
        PyTuple_SET_ITEM(state->args, argument->position, value);
        Py_DECREF(previous);
        return 0;
    }
    if (state->kwargs == NULL || argument->keyword == NULL) {
        Py_DECREF(value);
        PyErr_SetString(PyExc_RuntimeError, "missing buffer argument location");
        return -1;
    }
    int status = PyDict_SetItemString(state->kwargs, argument->keyword, value);
    Py_DECREF(value);
    return status;
}

static PyObject *
call_original(BufferState *state)
{
    if (state->constructor != NULL) {
        return state->constructor(
            (PyTypeObject *)state->receiver,
            state->args,
            state->kwargs
        );
    }
    if (state->receiver == NULL) {
        return PyObject_Call(state->callable, state->args, state->kwargs);
    }
    descrgetfunc get = Py_TYPE(state->callable)->tp_descr_get;
    PyObject *bound = get == NULL
        ? NULL
        : get(state->callable, state->receiver, (PyObject *)Py_TYPE(state->receiver));
    if (bound == NULL) {
        if (get == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "buffer adapter method is not a descriptor");
        }
        return NULL;
    }
    PyObject *result = PyObject_Call(bound, state->args, state->kwargs);
    Py_DECREF(bound);
    return result;
}

static PyObject *buffer_continue(BufferState *, PyObject *, int);

static PyObject *
buffer_resume(const void *raw_state, PyObject *value)
{
    BufferState *state = buffer_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &buffer_vtable, state) < 0) {
        buffer_free_state(state);
        return NULL;
    }
    PyObject *result = buffer_continue(state, value, 1);
    adapter_leave(&frame);
    buffer_free_state(state);
    return result;
}

static const AleffAdapterVTable buffer_vtable = {
    .copy_state = buffer_copy_state,
    .free_state = buffer_free_state,
    .resume = buffer_resume,
};

static PyObject *
buffer_finish(BufferState *state)
{
    if (state->exception != NULL) {
        PyErr_SetRaisedException(Py_NewRef(state->exception));
        return NULL;
    }
    return Py_XNewRef(state->result);
}

static int
buffer_accept_view(BufferState *state, PyObject *value)
{
    if (!PyMemoryView_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "__buffer__ returned non-memoryview object");
        return -1;
    }
    const AleffBufferArgument *argument = &state->arguments[state->pending_index];
    int duplicate = 0;
    PyObject *owner = buffer_argument(state, argument, &duplicate);
    if (owner == NULL || duplicate) {
        PyErr_SetString(PyExc_RuntimeError, "invalid pending buffer argument");
        return -1;
    }
    Py_ssize_t slot = state->acquired_count++;
    PyObject *previous_owner = PyList_GET_ITEM(state->owners, slot);
    PyObject *previous_view = PyList_GET_ITEM(state->views, slot);
    PyList_SET_ITEM(state->owners, slot, Py_NewRef(owner));
    PyList_SET_ITEM(state->views, slot, Py_NewRef(value));
    Py_DECREF(previous_owner);
    Py_DECREF(previous_view);
    PyObject *replacement = Py_NewRef(value);
    if (argument->make_bytearray) {
        Py_buffer buffer;
        if (PyObject_GetBuffer(value, &buffer, PyBUF_SIMPLE) < 0) {
            Py_DECREF(replacement);
            return -1;
        }
        Py_DECREF(replacement);
        replacement = PyByteArray_FromStringAndSize(buffer.buf, buffer.len);
        PyBuffer_Release(&buffer);
        if (replacement == NULL) {
            return -1;
        }
    }
    return replace_buffer_argument(state, argument, replacement);
}

static void
release_memoryview(PyObject *view)
{
    PyObject *released = PyObject_CallMethod(view, "release", NULL);
    if (released == NULL) {
        PyErr_Clear();
    }
    else {
        Py_DECREF(released);
    }
}

static void
buffer_release_after_error(BufferState *state)
{
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "buffer acquisition failed without an exception");
    }
    Py_XSETREF(state->exception, PyErr_GetRaisedException());
    state->release_index = state->acquired_count - 1;
    state->stage = BUFFER_STAGE_RELEASE;
}

static PyObject *
buffer_continue(BufferState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->stage == BUFFER_STAGE_ACQUIRE) {
            if (resumed_value == NULL || buffer_accept_view(state, resumed_value) < 0) {
                buffer_release_after_error(state);
            }
            else {
                state->argument_index = state->pending_index + 1;
            }
        }
        else if (state->stage == BUFFER_STAGE_RELEASE) {
            if (resumed_value == NULL && PyErr_Occurred()) {
                PyErr_WriteUnraisable(
                    PyList_GET_ITEM(state->owners, state->release_index)
                );
            }
            release_memoryview(
                PyList_GET_ITEM(state->views, state->release_index)
            );
            state->release_index--;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid resumed buffer adapter stage");
            return NULL;
        }
    }

    while (state->stage == BUFFER_STAGE_ACQUIRE) {
        if (state->argument_index >= state->argument_count) {
            state->stage = BUFFER_STAGE_CALL;
            break;
        }
        Py_ssize_t index = state->argument_index++;
        const AleffBufferArgument *argument = &state->arguments[index];
        int duplicate = 0;
        PyObject *object = buffer_argument(state, argument, &duplicate);
        if (object == NULL || duplicate || !has_python_buffer(object)) {
            continue;
        }
        state->pending_index = index;
        PyObject *view = call_raw_buffer(object, argument->flags);
        if (view == NULL) {
            buffer_release_after_error(state);
            break;
        }
        int status = buffer_accept_view(state, view);
        Py_DECREF(view);
        if (status < 0) {
            buffer_release_after_error(state);
            break;
        }
    }

    if (state->stage == BUFFER_STAGE_CALL) {
        PyObject *result = call_original(state);
        if (result == NULL) {
            state->exception = PyErr_GetRaisedException();
        }
        else {
            state->result = result;
        }
        state->release_index = state->acquired_count - 1;
        state->stage = BUFFER_STAGE_RELEASE;
    }

    while (state->release_index >= 0) {
        PyObject *owner = PyList_GET_ITEM(state->owners, state->release_index);
        PyObject *view = PyList_GET_ITEM(state->views, state->release_index);
        PyObject *descriptor = lookup_raw_special(owner, "__release_buffer__");
        if (descriptor == NULL) {
            PyErr_Clear();
            release_memoryview(view);
            state->release_index--;
            continue;
        }
        Py_DECREF(descriptor);
        PyObject *released = call_raw_release(owner, view);
        if (released == NULL && PyErr_Occurred()) {
            PyErr_WriteUnraisable(owner);
        }
        Py_XDECREF(released);
        release_memoryview(view);
        state->release_index--;
    }
    return buffer_finish(state);
}

PyObject *
adapter_buffer_call(
    PyObject *callable,
    PyObject *receiver,
    PyObject *args,
    PyObject *kwargs,
    const AleffBufferArgument *arguments,
    Py_ssize_t argument_count
)
{
    int needs_adapter = 0;
    int defer_argument_validation = 0;
    for (Py_ssize_t index = 0; index < argument_count; index++) {
        const AleffBufferArgument *argument = &arguments[index];
        PyObject *positional = argument->position < PyTuple_GET_SIZE(args)
            ? PyTuple_GET_ITEM(args, argument->position) : NULL;
        PyObject *keyword = kwargs == NULL || argument->keyword == NULL
            ? NULL : PyDict_GetItemString(kwargs, argument->keyword);
        PyObject *exclusive = kwargs == NULL ||
            argument->exclusive_keyword == NULL
            ? NULL
            : PyDict_GetItemString(kwargs, argument->exclusive_keyword);
        if ((positional != NULL && keyword != NULL) ||
            ((positional != NULL || keyword != NULL) && exclusive != NULL)) {
            defer_argument_validation = 1;
            break;
        }
        PyObject *object = positional != NULL ? positional : keyword;
        if (object != NULL && has_python_buffer(object)) {
            needs_adapter = 1;
            break;
        }
    }
    if (defer_argument_validation || !needs_adapter) {
        if (receiver == NULL) {
            return PyObject_Call(callable, args, kwargs);
        }
        descrgetfunc get = Py_TYPE(callable)->tp_descr_get;
        PyObject *bound = get == NULL
            ? NULL : get(callable, receiver, (PyObject *)Py_TYPE(receiver));
        if (bound == NULL) {
            if (get == NULL) {
                PyErr_SetString(PyExc_RuntimeError, "buffer adapter method is not a descriptor");
            }
            return NULL;
        }
        PyObject *result = PyObject_Call(bound, args, kwargs);
        Py_DECREF(bound);
        return result;
    }

    BufferState state = {
        .callable = Py_NewRef(callable),
        .receiver = Py_XNewRef(receiver),
        .args = buffer_copy_args(args),
        .kwargs = kwargs == NULL ? NULL : PyDict_Copy(kwargs),
        .owners = buffer_new_slots(argument_count),
        .views = buffer_new_slots(argument_count),
        .result = NULL,
        .exception = NULL,
        .constructor = NULL,
        .arguments = buffer_copy_arguments(arguments, argument_count),
        .argument_count = argument_count,
        .argument_index = 0,
        .pending_index = -1,
        .acquired_count = 0,
        .release_index = -1,
        .stage = BUFFER_STAGE_ACQUIRE,
    };
    if (state.args == NULL || (kwargs != NULL && state.kwargs == NULL) ||
        state.owners == NULL || state.views == NULL ||
        (argument_count != 0 && state.arguments == NULL)) {
        buffer_clear_state(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &buffer_vtable, &state) < 0) {
        buffer_clear_state(&state);
        return NULL;
    }
    PyObject *result = buffer_continue(&state, NULL, 0);
    adapter_leave(&frame);
    buffer_clear_state(&state);
    return result;
}

PyObject *
adapter_buffer_new(
    newfunc constructor,
    PyTypeObject *type,
    PyObject *args,
    PyObject *kwargs,
    const AleffBufferArgument *arguments,
    Py_ssize_t argument_count
)
{
    int needs_adapter = 0;
    int defer_argument_validation = 0;
    for (Py_ssize_t index = 0; index < argument_count; index++) {
        const AleffBufferArgument *argument = &arguments[index];
        PyObject *positional = argument->position < PyTuple_GET_SIZE(args)
            ? PyTuple_GET_ITEM(args, argument->position) : NULL;
        PyObject *keyword = kwargs == NULL || argument->keyword == NULL
            ? NULL : PyDict_GetItemString(kwargs, argument->keyword);
        PyObject *exclusive = kwargs == NULL ||
            argument->exclusive_keyword == NULL
            ? NULL
            : PyDict_GetItemString(kwargs, argument->exclusive_keyword);
        if ((positional != NULL && keyword != NULL) ||
            ((positional != NULL || keyword != NULL) && exclusive != NULL)) {
            defer_argument_validation = 1;
            break;
        }
        PyObject *object = positional != NULL ? positional : keyword;
        if (object != NULL && has_python_buffer(object)) {
            needs_adapter = 1;
            break;
        }
    }
    if (defer_argument_validation || !needs_adapter) {
        return constructor(type, args, kwargs);
    }

    BufferState state = {
        .callable = NULL,
        .receiver = Py_NewRef((PyObject *)type),
        .args = buffer_copy_args(args),
        .kwargs = kwargs == NULL ? NULL : PyDict_Copy(kwargs),
        .owners = buffer_new_slots(argument_count),
        .views = buffer_new_slots(argument_count),
        .result = NULL,
        .exception = NULL,
        .constructor = constructor,
        .arguments = buffer_copy_arguments(arguments, argument_count),
        .argument_count = argument_count,
        .argument_index = 0,
        .pending_index = -1,
        .acquired_count = 0,
        .release_index = -1,
        .stage = BUFFER_STAGE_ACQUIRE,
    };
    if (state.args == NULL || (kwargs != NULL && state.kwargs == NULL) ||
        state.owners == NULL || state.views == NULL ||
        (argument_count != 0 && state.arguments == NULL)) {
        buffer_clear_state(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &buffer_vtable, &state) < 0) {
        buffer_clear_state(&state);
        return NULL;
    }
    PyObject *result = buffer_continue(&state, NULL, 0);
    adapter_leave(&frame);
    buffer_clear_state(&state);
    return result;
}
