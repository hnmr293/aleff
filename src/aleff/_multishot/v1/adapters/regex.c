#include "regex.h"

#include <string.h>

/* The substitution loop follows CPython's Modules/_sre/sre.c.  CPython's
 * license terms are included in LICENSES/CPython.txt. */

typedef enum {
    REGEX_READY,
    REGEX_WAIT_REPLACEMENT,
} RegexPhase;

/* MatchObject is private to _sre, but this prefix is unchanged across the
 * supported CPython 3.12-3.14 releases.  Pattern.sub keeps pos at the initial
 * boundary while advancing a separate search cursor. */
typedef struct {
    PyObject_VAR_HEAD
    PyObject *string;
    PyObject *regs;
    PyObject *pattern;
    Py_ssize_t pos;
    Py_ssize_t endpos;
} AleffSreMatchPrefix;

typedef struct {
    PyObject *pattern;
    PyObject *replacement;
    PyObject *string;
    PyObject *pieces;
    PyObject *buffer_snapshot;
    Py_buffer view;
    Py_ssize_t count;
    Py_ssize_t substitutions;
    Py_ssize_t copied_until;
    Py_ssize_t search_start;
    Py_ssize_t endpos;
    Py_ssize_t match_start;
    Py_ssize_t match_end;
    RegexPhase phase;
    int subn;
    int is_bytes;
    int must_advance;
    int has_view;
} RegexState;

static const AleffAdapterVTable regex_vtable;
static PyObject *original_sub;
static PyObject *original_subn;
static PyObject *installed_pattern_type;
static PyMethodDef sub_method;
static PyMethodDef subn_method;
static int regex_installed;

static void regex_free_state(void *raw_state);

static void
regex_clear_state(RegexState *state)
{
    if (state->has_view) {
        PyBuffer_Release(&state->view);
        state->has_view = 0;
    }
    Py_CLEAR(state->buffer_snapshot);
    Py_CLEAR(state->pieces);
    Py_CLEAR(state->string);
    Py_CLEAR(state->replacement);
    Py_CLEAR(state->pattern);
}

static PyObject *
regex_slice(const RegexState *state, Py_ssize_t start, Py_ssize_t end)
{
    if (!state->is_bytes) {
        return PyUnicode_Substring(state->string, start, end);
    }
    if (start < 0 || end < start || end > state->view.len) {
        PyErr_SetString(PyExc_RuntimeError, "invalid regex byte slice");
        return NULL;
    }
    const char *buffer = state->view.buf;
    return PyBytes_FromStringAndSize(buffer + start, end - start);
}

static int
regex_match_span(PyObject *match, Py_ssize_t *start, Py_ssize_t *end)
{
    PyObject *span = PyObject_CallMethod(match, "span", NULL);
    if (span == NULL) {
        return -1;
    }
    if (!PyTuple_Check(span) || PyTuple_GET_SIZE(span) != 2) {
        Py_DECREF(span);
        PyErr_SetString(PyExc_RuntimeError, "regex match returned an invalid span");
        return -1;
    }
    *start = PyLong_AsSsize_t(PyTuple_GET_ITEM(span, 0));
    if (*start != -1 || !PyErr_Occurred()) {
        *end = PyLong_AsSsize_t(PyTuple_GET_ITEM(span, 1));
    }
    Py_DECREF(span);
    return PyErr_Occurred() ? -1 : 0;
}

static PyObject *
regex_next_match(RegexState *state)
{
    PyObject *iterator = PyObject_CallMethod(
        state->pattern,
        "finditer",
        "Onn",
        state->string,
        state->search_start,
        state->endpos
    );
    if (iterator == NULL) {
        return NULL;
    }
    PyObject *match = PyIter_Next(iterator);
    if (match != NULL && state->must_advance) {
        Py_ssize_t start;
        Py_ssize_t end;
        if (regex_match_span(match, &start, &end) < 0) {
            Py_DECREF(match);
            Py_DECREF(iterator);
            return NULL;
        }
        if (start == state->search_start && end == start) {
            Py_DECREF(match);
            match = PyIter_Next(iterator);
        }
    }
    Py_DECREF(iterator);
    if (match != NULL) {
        ((AleffSreMatchPrefix *)match)->pos = 0;
    }
    return match;
}

static int
regex_append_replacement(RegexState *state, PyObject *replacement)
{
    if (replacement != Py_None && PyList_Append(state->pieces, replacement) < 0) {
        return -1;
    }
    state->copied_until = state->match_end;
    state->substitutions++;
    state->must_advance = state->match_start == state->match_end;
    state->search_start = state->match_end;
    state->phase = REGEX_READY;
    return 0;
}

static PyObject *
regex_finish(RegexState *state)
{
    if (state->copied_until < state->endpos) {
        PyObject *tail = regex_slice(
            state,
            state->copied_until,
            state->endpos
        );
        if (tail == NULL) {
            return NULL;
        }
        int status = PyList_Append(state->pieces, tail);
        Py_DECREF(tail);
        if (status < 0) {
            return NULL;
        }
    }

    PyObject *joiner = state->is_bytes
        ? PyBytes_FromStringAndSize("", 0)
        : PyUnicode_New(0, 0);
    if (joiner == NULL) {
        return NULL;
    }
    PyObject *result;
    if (PyList_GET_SIZE(state->pieces) == 0) {
        result = joiner;
    }
    else {
        result = PyObject_CallMethod(joiner, "join", "O", state->pieces);
        Py_DECREF(joiner);
        if (result == NULL) {
            return NULL;
        }
    }
    if (!state->subn) {
        return result;
    }
    PyObject *substitutions = PyLong_FromSsize_t(state->substitutions);
    if (substitutions == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    PyObject *tuple = PyTuple_New(2);
    if (tuple == NULL) {
        Py_DECREF(result);
        Py_DECREF(substitutions);
        return NULL;
    }
    PyTuple_SET_ITEM(tuple, 0, result);
    PyTuple_SET_ITEM(tuple, 1, substitutions);
    return tuple;
}

static PyObject *
regex_continue(RegexState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->phase != REGEX_WAIT_REPLACEMENT) {
            PyErr_SetString(PyExc_RuntimeError, "invalid regex resume phase");
            return NULL;
        }
        if (regex_append_replacement(state, resumed_value) < 0) {
            return NULL;
        }
    }

    while (state->count == 0 || state->substitutions < state->count) {
        PyObject *match = regex_next_match(state);
        if (match == NULL) {
            if (PyErr_Occurred()) {
                return NULL;
            }
            break;
        }
        if (regex_match_span(
                match,
                &state->match_start,
                &state->match_end
            ) < 0) {
            Py_DECREF(match);
            return NULL;
        }
        if (state->copied_until < state->match_start) {
            PyObject *prefix = regex_slice(
                state,
                state->copied_until,
                state->match_start
            );
            if (prefix == NULL) {
                Py_DECREF(match);
                return NULL;
            }
            int status = PyList_Append(state->pieces, prefix);
            Py_DECREF(prefix);
            if (status < 0) {
                Py_DECREF(match);
                return NULL;
            }
        }

        state->phase = REGEX_WAIT_REPLACEMENT;
        PyObject *item = PyObject_CallOneArg(state->replacement, match);
        Py_DECREF(match);
        if (item == NULL) {
            return NULL;
        }
        int status = regex_append_replacement(state, item);
        Py_DECREF(item);
        if (status < 0) {
            return NULL;
        }
    }
    return regex_finish(state);
}

static void *
regex_copy_state(const void *raw_state)
{
    const RegexState *source = raw_state;
    RegexState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->pattern = Py_NewRef(source->pattern);
    copy->replacement = Py_NewRef(source->replacement);
    copy->string = Py_NewRef(source->string);
    copy->pieces = PyList_GetSlice(
        source->pieces,
        0,
        PyList_GET_SIZE(source->pieces)
    );
    copy->buffer_snapshot = NULL;
    copy->has_view = 0;
    memset(&copy->view, 0, sizeof(copy->view));
    if (copy->pieces == NULL) {
        regex_free_state(copy);
        return NULL;
    }
    if (source->has_view &&
        PyObject_GetBuffer(copy->string, &copy->view, PyBUF_SIMPLE) < 0) {
        regex_free_state(copy);
        return NULL;
    }
    copy->has_view = source->has_view;
    if (source->buffer_snapshot != NULL) {
        copy->buffer_snapshot = Py_NewRef(source->buffer_snapshot);
    }
    else if (source->has_view && !source->view.readonly) {
        copy->buffer_snapshot = PyBytes_FromStringAndSize(
            source->view.buf,
            source->view.len
        );
    }
    if (source->has_view && !source->view.readonly &&
        copy->buffer_snapshot == NULL) {
        regex_free_state(copy);
        return NULL;
    }
    return copy;
}

static void
regex_free_state(void *raw_state)
{
    RegexState *state = raw_state;
    if (state == NULL) {
        return;
    }
    regex_clear_state(state);
    PyMem_Free(state);
}

static int
regex_prepare_resume(void *raw_state)
{
    RegexState *state = raw_state;
    if (state->buffer_snapshot == NULL) {
        return 0;
    }
    if (state->view.readonly ||
        PyBytes_GET_SIZE(state->buffer_snapshot) != state->view.len) {
        PyErr_SetString(PyExc_RuntimeError, "regex input buffer changed size");
        return -1;
    }
    memcpy(
        state->view.buf,
        PyBytes_AS_STRING(state->buffer_snapshot),
        (size_t)state->view.len
    );
    return 0;
}

static PyObject *
regex_resume(const void *raw_state, PyObject *value)
{
    RegexState *state = regex_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    Py_CLEAR(state->buffer_snapshot);
    if (value == NULL) {
        regex_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &regex_vtable, state) < 0) {
        regex_free_state(state);
        return NULL;
    }
    PyObject *result = regex_continue(state, value, 1);
    adapter_leave(&frame);
    regex_free_state(state);
    return result;
}

static const AleffAdapterVTable regex_vtable = {
    .copy_state = regex_copy_state,
    .free_state = regex_free_state,
    .resume = regex_resume,
    .prepare_resume = regex_prepare_resume,
};

static PyObject *
call_original_method(
    PyObject *descriptor,
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
)
{
    Py_ssize_t size = PyTuple_GET_SIZE(args);
    PyObject *full_args = PyTuple_New(size + 1);
    if (full_args == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(full_args, 0, Py_NewRef(self));
    for (Py_ssize_t index = 0; index < size; index++) {
        PyTuple_SET_ITEM(
            full_args,
            index + 1,
            Py_NewRef(PyTuple_GET_ITEM(args, index))
        );
    }
    PyObject *result = PyObject_Call(descriptor, full_args, kwargs);
    Py_DECREF(full_args);
    return result;
}

static PyObject *
regex_replacement_argument(PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) > 0) {
        return PyTuple_GET_ITEM(args, 0);
    }
    return kwargs == NULL
        ? NULL
        : PyDict_GetItemString(kwargs, "repl");
}

static int
regex_state_init(
    RegexState *state,
    PyObject *pattern,
    PyObject *replacement,
    PyObject *string,
    Py_ssize_t count,
    int subn
)
{
    memset(state, 0, sizeof(*state));
    state->pattern = Py_NewRef(pattern);
    state->replacement = Py_NewRef(replacement);
    state->string = Py_NewRef(string);
    state->pieces = PyList_New(0);
    state->count = count;
    state->subn = subn;
    state->phase = REGEX_READY;
    if (state->pieces == NULL) {
        return -1;
    }

    PyObject *pattern_text = PyObject_GetAttrString(pattern, "pattern");
    if (pattern_text == NULL) {
        return -1;
    }
    state->is_bytes = PyBytes_Check(pattern_text);
    Py_DECREF(pattern_text);
    if (!state->is_bytes) {
        if (!PyUnicode_Check(string)) {
            return 1;
        }
        state->endpos = PyUnicode_GET_LENGTH(string);
        return 0;
    }
    if (!PyObject_CheckBuffer(string)) {
        return 1;
    }
    if (PyObject_GetBuffer(string, &state->view, PyBUF_SIMPLE) < 0) {
        return -1;
    }
    state->has_view = 1;
    state->endpos = state->view.len;
    return 0;
}

static PyObject *
regex_call(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs,
    int subn
)
{
    PyObject *descriptor = subn ? original_subn : original_sub;
    PyObject *candidate = regex_replacement_argument(args, kwargs);
    if (candidate == NULL || !PyCallable_Check(candidate)) {
        return call_original_method(descriptor, self, args, kwargs);
    }

    PyObject *replacement;
    PyObject *string;
    Py_ssize_t count = 0;
    static char *keywords[] = {"repl", "string", "count", NULL};
    const char *function_name = subn ? "subn" : "sub";
    char format[16];
    PyOS_snprintf(format, sizeof(format), "OO|n:%s", function_name);
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            format,
            keywords,
            &replacement,
            &string,
            &count
        )) {
        return NULL;
    }

    RegexState state;
    int init_status = regex_state_init(
        &state,
        self,
        replacement,
        string,
        count,
        subn
    );
    if (init_status != 0) {
        regex_clear_state(&state);
        if (init_status > 0) {
            return call_original_method(descriptor, self, args, kwargs);
        }
        return NULL;
    }

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &regex_vtable, &state) < 0) {
        regex_clear_state(&state);
        return NULL;
    }
    PyObject *result = regex_continue(&state, NULL, 0);
    adapter_leave(&frame);
    regex_clear_state(&state);
    return result;
}

static int
regex_unpack_fastcall(
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names,
    PyObject **args,
    PyObject **kwargs
)
{
    *args = PyTuple_New(positional_count);
    *kwargs = NULL;
    if (*args == NULL) {
        return -1;
    }
    for (Py_ssize_t index = 0; index < positional_count; index++) {
        PyTuple_SET_ITEM(*args, index, Py_NewRef(values[index]));
    }
    if (keyword_names == NULL) {
        return 0;
    }
    Py_ssize_t keyword_count = PyTuple_GET_SIZE(keyword_names);
    *kwargs = PyDict_New();
    if (*kwargs == NULL) {
        Py_CLEAR(*args);
        return -1;
    }
    for (Py_ssize_t index = 0; index < keyword_count; index++) {
        if (PyDict_SetItem(
                *kwargs,
                PyTuple_GET_ITEM(keyword_names, index),
                values[positional_count + index]
            ) < 0) {
            Py_CLEAR(*kwargs);
            Py_CLEAR(*args);
            return -1;
        }
    }
    return 0;
}

static PyObject *
pattern_sub_wrapper(
    PyObject *self,
    PyTypeObject *defining_class,
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    (void)defining_class;
    PyObject *args;
    PyObject *kwargs;
    if (regex_unpack_fastcall(
            values,
            positional_count,
            keyword_names,
            &args,
            &kwargs
        ) < 0) {
        return NULL;
    }
    PyObject *result = regex_call(self, args, kwargs, 0);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return result;
}

static PyObject *
pattern_subn_wrapper(
    PyObject *self,
    PyTypeObject *defining_class,
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    (void)defining_class;
    PyObject *args;
    PyObject *kwargs;
    if (regex_unpack_fastcall(
            values,
            positional_count,
            keyword_names,
            &args,
            &kwargs
        ) < 0) {
        return NULL;
    }
    PyObject *result = regex_call(self, args, kwargs, 1);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return result;
}

static int
install_method(
    PyTypeObject *type,
    const char *name,
    PyObject *original,
    PyMethodDef *method,
    PyCFunction function
)
{
    if (!Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        PyErr_Format(PyExc_RuntimeError, "re.Pattern.%s is not a C method", name);
        return -1;
    }
    *method = *((PyMethodDescrObject *)original)->d_method;
    method->ml_name = name;
    method->ml_meth = function;
    PyObject *descriptor = PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
        return -1;
    }
    PyObject *dict = PyType_GetDict(type);
    int status = dict == NULL
        ? -1
        : PyDict_SetItemString(dict, name, descriptor);
    Py_XDECREF(dict);
    Py_DECREF(descriptor);
    if (status == 0) {
        PyType_Modified(type);
    }
    return status;
}

int
adapter_regex_install(PyObject *re_module)
{
    if (regex_installed) {
        return 0;
    }
    PyObject *pattern_type = PyObject_GetAttrString(re_module, "Pattern");
    if (pattern_type == NULL || !PyType_Check(pattern_type)) {
        Py_XDECREF(pattern_type);
        PyErr_SetString(PyExc_RuntimeError, "cannot access re.Pattern type");
        return -1;
    }
    PyObject *dict = PyType_GetDict((PyTypeObject *)pattern_type);
    if (dict == NULL) {
        Py_DECREF(pattern_type);
        return -1;
    }
    original_sub = Py_XNewRef(PyDict_GetItemString(dict, "sub"));
    original_subn = Py_XNewRef(PyDict_GetItemString(dict, "subn"));
    Py_DECREF(dict);
    installed_pattern_type = pattern_type;
    if (original_sub == NULL || original_subn == NULL ||
        install_method(
            (PyTypeObject *)pattern_type,
            "sub",
            original_sub,
            &sub_method,
            _PyCFunction_CAST(pattern_sub_wrapper)
        ) < 0 ||
        install_method(
            (PyTypeObject *)pattern_type,
            "subn",
            original_subn,
            &subn_method,
            _PyCFunction_CAST(pattern_subn_wrapper)
        ) < 0) {
        adapter_regex_rollback();
        return -1;
    }
    regex_installed = 1;
    return 0;
}

void
adapter_regex_rollback(void)
{
    if (installed_pattern_type == NULL) {
        return;
    }
    PyObject *dict = PyType_GetDict((PyTypeObject *)installed_pattern_type);
    if (dict != NULL) {
        if (original_sub != NULL &&
            PyDict_SetItemString(dict, "sub", original_sub) < 0) {
            PyErr_Clear();
        }
        if (original_subn != NULL &&
            PyDict_SetItemString(dict, "subn", original_subn) < 0) {
            PyErr_Clear();
        }
        PyType_Modified((PyTypeObject *)installed_pattern_type);
        Py_DECREF(dict);
    }
    Py_CLEAR(original_sub);
    Py_CLEAR(original_subn);
    Py_CLEAR(installed_pattern_type);
    regex_installed = 0;
}
