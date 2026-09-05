#define PY_SSIZE_T_CLEAN

#include "api.h"
#include "datetime.h"
#include "internal.h"

#include <datetime.h>

#include <stdint.h>

typedef enum {
    DATETIME_NOW,
    DATETIME_FROMTIMESTAMP,
    DATETIME_ASTIMEZONE,
    DATETIME_TIMESTAMP,
    DATETIME_TIME_UTCOFFSET,
    DATETIME_TIME_DST,
    DATETIME_TIME_TZNAME,
    DATETIME_TIME_ISOFORMAT,
    DATETIME_TIME_COMPARE,
    DATETIME_DATETIME_UTCOFFSET,
    DATETIME_DATETIME_DST,
    DATETIME_DATETIME_TZNAME,
    DATETIME_DATETIME_STRFTIME,
    DATETIME_DATETIME_ISOFORMAT,
    DATETIME_DATETIME_COMPARE,
    DATETIME_DATETIME_SUBTRACT,
} DateOperation;

typedef enum {
    DATE_WAIT_CALLBACK,
    DATE_WAIT_RIGHT_OFFSET,
    DATE_WAIT_SECOND_LEFT_OFFSET,
    DATE_WAIT_SECOND_RIGHT_OFFSET,
    DATE_WAIT_FOLD_LEFT_OFFSET,
    DATE_WAIT_FOLD_RIGHT_OFFSET,
} DatePhase;

typedef enum {
    DATE_STAGE_NONE,
    DATE_STAGE_DST,
    DATE_STAGE_OFFSET,
    DATE_STAGE_NAME,
    DATE_STAGE_LEFT_OFFSET,
    DATE_STAGE_RIGHT_OFFSET,
    DATE_STAGE_ASTIMEZONE_SOURCE_OFFSET,
} DateStage;

typedef struct {
    DateOperation operation;
    DatePhase phase;
    DateStage stage;
    int compare_op;
    PyObject *left;
    PyObject *right;
    PyObject *tz;
    PyObject *args;
    PyObject *kwargs;
    PyObject *left_offset;
    PyObject *right_offset;
    PyObject *second_left_offset;
    PyObject *dst_value;
    PyObject *offset_value;
    PyObject *name_value;
} DateState;

static PyTypeObject *datetime_type;
static PyTypeObject *time_type;
static PyObject *datetime_module;
static PyObject *datetime_timezone;
static PyObject *original_methods[9];
static PyObject *original_time_methods[4];
static PyMethodDef method_defs[13];
static PyMethodDef time_method_defs[4];
static richcmpfunc original_datetime_richcompare;
static richcmpfunc original_time_richcompare;
static binaryfunc original_datetime_subtract;
static int datetime_strftime_inherited;
static int datetime_installed;

static PyObject *date_resume(const void *, PyObject *);
static PyObject *date_finish(DateState *, PyObject *);
static PyObject *date_astimezone_drive(DateState *, PyObject *);
static PyObject *date_call_right_offset(DateState *, DatePhase);
static PyObject *date_compare_result(DateState *, PyObject *, PyObject *);
static PyObject *date_subtract_result(DateState *, PyObject *, PyObject *);
static PyObject *date_datetime_compare_after_first_right(DateState *, PyObject *);
static PyObject *date_datetime_compare_after_second_left(DateState *, PyObject *);
static PyObject *date_datetime_compare_after_second_right(DateState *, PyObject *);
static PyObject *date_datetime_compare_after_fold_left(DateState *, PyObject *);
static PyObject *date_datetime_compare_after_fold_right(DateState *, PyObject *);

static void
date_state_clear(DateState *state)
{
    Py_XDECREF(state->left);
    Py_XDECREF(state->right);
    Py_XDECREF(state->tz);
    Py_XDECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_XDECREF(state->left_offset);
    Py_XDECREF(state->right_offset);
    Py_XDECREF(state->second_left_offset);
    Py_XDECREF(state->dst_value);
    Py_XDECREF(state->offset_value);
    Py_XDECREF(state->name_value);
}

static void *
date_copy_state(const void *raw_state)
{
    const DateState *source = raw_state;
    DateState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->left = Py_XNewRef(source->left);
    copy->right = Py_XNewRef(source->right);
    copy->tz = Py_XNewRef(source->tz);
    copy->args = Py_XNewRef(source->args);
    copy->kwargs = Py_XNewRef(source->kwargs);
    copy->left_offset = Py_XNewRef(source->left_offset);
    copy->right_offset = Py_XNewRef(source->right_offset);
    copy->second_left_offset = Py_XNewRef(source->second_left_offset);
    copy->dst_value = Py_XNewRef(source->dst_value);
    copy->offset_value = Py_XNewRef(source->offset_value);
    copy->name_value = Py_XNewRef(source->name_value);
    return copy;
}

static void
date_free_state(void *raw_state)
{
    DateState *state = raw_state;
    if (state == NULL) {
        return;
    }
    date_state_clear(state);
    PyMem_Free(state);
}

static const AleffAdapterVTable date_vtable = {
    .copy_state = date_copy_state,
    .free_state = date_free_state,
    .resume = date_resume,
};

static PyObject *
call_descriptor(PyObject *descriptor, PyObject *receiver, PyObject *args, PyObject *kwargs)
{
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    PyObject *call_args = PyTuple_New(count + 1);
    if (call_args == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(call_args, 0, Py_NewRef(receiver));
    for (Py_ssize_t index = 0; index < count; index++) {
        PyTuple_SET_ITEM(
            call_args,
            index + 1,
            Py_NewRef(PyTuple_GET_ITEM(args, index))
        );
    }
    PyObject *result = PyObject_Call(descriptor, call_args, kwargs);
    Py_DECREF(call_args);
    return result;
}

static int
delta_microseconds(PyObject *value, int64_t *result)
{
    if (value == Py_None) {
        if (result != NULL) {
            *result = 0;
        }
        return 1;
    }
    if (!PyDelta_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "tzinfo.utcoffset() must return None or a timedelta, not '%.200s'",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    int64_t days = PyDateTime_DELTA_GET_DAYS(value);
    int64_t seconds = PyDateTime_DELTA_GET_SECONDS(value);
    int64_t microseconds = PyDateTime_DELTA_GET_MICROSECONDS(value);
    int64_t total = ((days * 86400) + seconds) * 1000000 + microseconds;
    if (total <= -86400000000LL || total >= 86400000000LL) {
#if PY_VERSION_HEX >= 0x030e0000
        PyErr_Format(
            PyExc_ValueError,
            "offset must be a timedelta strictly between "
            "-timedelta(hours=24) and timedelta(hours=24), not %R",
            value
        );
#else
        PyErr_SetString(
            PyExc_ValueError,
            "offset must be a timedelta strictly between -timedelta(hours=24) and timedelta(hours=24)."
        );
#endif
        return -1;
    }
    if (result != NULL) {
        *result = total;
    }
    return 0;
}

static int
validate_callback(DateOperation operation, PyObject *value, int64_t *offset)
{
    if (operation == DATETIME_TIME_TZNAME ||
        operation == DATETIME_DATETIME_TZNAME) {
        if (value != Py_None && !PyUnicode_Check(value)) {
            PyErr_Format(
                PyExc_TypeError,
                "tzinfo.tzname() must return None or a string, not '%.200s'",
                Py_TYPE(value)->tp_name
            );
            return -1;
        }
        return 0;
    }
    if (operation == DATETIME_TIME_DST || operation == DATETIME_DATETIME_DST) {
        if (value != Py_None && !PyDelta_Check(value)) {
            PyErr_Format(
                PyExc_TypeError,
                "tzinfo.dst() must return None or a timedelta, not '%.200s'",
                Py_TYPE(value)->tp_name
            );
            return -1;
        }
    }
    return delta_microseconds(value, offset) < 0 ? -1 : 0;
}

static PyObject *
date_naive_datetime(PyObject *value)
{
    return PyDateTime_FromDateAndTime(
        PyDateTime_GET_YEAR(value),
        PyDateTime_GET_MONTH(value),
        PyDateTime_GET_DAY(value),
        PyDateTime_DATE_GET_HOUR(value),
        PyDateTime_DATE_GET_MINUTE(value),
        PyDateTime_DATE_GET_SECOND(value),
        PyDateTime_DATE_GET_MICROSECOND(value)
    );
}

static PyObject *
date_naive_time(PyObject *value)
{
    return PyTime_FromTime(
        PyDateTime_TIME_GET_HOUR(value),
        PyDateTime_TIME_GET_MINUTE(value),
        PyDateTime_TIME_GET_SECOND(value),
        PyDateTime_TIME_GET_MICROSECOND(value)
    );
}

static PyObject *
date_make_offset_text(PyObject *value)
{
    int64_t total;
    if (delta_microseconds(value, &total) < 0) {
        return NULL;
    }
    if (value == Py_None) {
        return PyUnicode_New(0, 127);
    }
    int negative = total < 0;
    uint64_t absolute = (uint64_t)(negative ? -total : total);
    uint64_t minutes = absolute / 60000000U;
    uint64_t seconds = (absolute / 1000000U) % 60U;
    uint64_t micros = absolute % 1000000U;
    PyObject *result = PyUnicode_FromFormat(
        "%c%02u:%02u",
        negative ? '-' : '+',
        (unsigned)(minutes / 60U),
        (unsigned)(minutes % 60U)
    );
    if (result == NULL) {
        return NULL;
    }
    if (seconds != 0 || micros != 0) {
        PyObject *suffix = PyUnicode_FromFormat(":%02u", (unsigned)seconds);
        if (suffix == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        PyUnicode_Append(&result, suffix);
        Py_DECREF(suffix);
        if (result == NULL) {
            return NULL;
        }
        if (micros != 0) {
            suffix = PyUnicode_FromFormat(".%06u", (unsigned)micros);
            if (suffix == NULL) {
                Py_DECREF(result);
                return NULL;
            }
            PyUnicode_Append(&result, suffix);
            Py_DECREF(suffix);
        }
    }
    return result;
}

static PyObject *
date_append_offset(PyObject *base, PyObject *offset)
{
    if (offset == Py_None) {
        return Py_NewRef(base);
    }
    PyObject *text = date_make_offset_text(offset);
    if (text == NULL) {
        return NULL;
    }
    PyObject *result = PyUnicode_Concat(base, text);
    Py_DECREF(text);
    return result;
}

static PyObject *
date_isoformat(DateState *state, PyObject *offset)
{
    PyObject *naive = state->operation == DATETIME_TIME_ISOFORMAT
        ? date_naive_time(state->left)
        : date_naive_datetime(state->left);
    if (naive == NULL) {
        return NULL;
    }
    PyObject *descriptor = state->operation == DATETIME_TIME_ISOFORMAT
        ? original_time_methods[3]
        : original_methods[8];
    PyObject *empty_args = state->args == NULL ? PyTuple_New(0) : NULL;
    PyObject *call_args = state->args == NULL ? empty_args : state->args;
    PyObject *base = call_descriptor(descriptor, naive, call_args, state->kwargs);
    Py_XDECREF(empty_args);
    Py_DECREF(naive);
    if (base == NULL) {
        return NULL;
    }
    PyObject *result = date_append_offset(base, offset);
    Py_DECREF(base);
    return result;
}

static PyObject *
date_datetime_with_timezone(PyObject *value, PyObject *offset)
{
    PyObject *tz = PyObject_CallFunctionObjArgs(datetime_timezone, offset, NULL);
    if (tz == NULL) {
        return NULL;
    }
    PyObject *result = PyDateTimeAPI->DateTime_FromDateAndTime(
        PyDateTime_GET_YEAR(value),
        PyDateTime_GET_MONTH(value),
        PyDateTime_GET_DAY(value),
        PyDateTime_DATE_GET_HOUR(value),
        PyDateTime_DATE_GET_MINUTE(value),
        PyDateTime_DATE_GET_SECOND(value),
        PyDateTime_DATE_GET_MICROSECOND(value),
        tz,
        PyDateTimeAPI->DateTimeType
    );
    Py_DECREF(tz);
    return result;
}

static PyObject *
date_strftime(DateState *state, PyObject *callback_value)
{
    (void)callback_value;
    int64_t offset = 0;
    PyObject *offset_object = state->offset_value;
    if (delta_microseconds(offset_object, &offset) < 0) {
        return NULL;
    }
    (void)offset;

    PyObject *format = PyTuple_GET_SIZE(state->args) == 0
        ? NULL : PyTuple_GET_ITEM(state->args, 0);
    if (format == NULL) {
        PyErr_SetString(PyExc_TypeError, "strftime() argument 1 must be str, not missing");
        return NULL;
    }
    if (!PyUnicode_Check(format)) {
        PyErr_Format(
            PyExc_TypeError,
            "strftime() argument 1 must be str, not %.200s",
            Py_TYPE(format)->tp_name
        );
        return NULL;
    }
    PyObject *format_copy = Py_NewRef(format);
    if (state->name_value != NULL) {
        PyObject *name = state->name_value == Py_None
            ? PyUnicode_New(0, 127) : Py_NewRef(state->name_value);
        if (name == NULL) {
            Py_DECREF(format_copy);
            return NULL;
        }
        PyObject *replacement = PyObject_CallMethod(
            format_copy, "replace", "sO", "%Z", name
        );
        Py_DECREF(name);
        Py_DECREF(format_copy);
        format_copy = replacement;
        if (format_copy == NULL) {
            return NULL;
        }
    }
    PyObject *new_args = PyTuple_GetSlice(state->args, 1, PyTuple_GET_SIZE(state->args));
    if (new_args == NULL) {
        Py_DECREF(format_copy);
        return NULL;
    }
    PyObject *full_args = PyTuple_New(PyTuple_GET_SIZE(new_args) + 1);
    if (full_args == NULL) {
        Py_DECREF(format_copy);
        Py_DECREF(new_args);
        return NULL;
    }
    PyTuple_SET_ITEM(full_args, 0, format_copy);
    for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(new_args); index++) {
        PyTuple_SET_ITEM(full_args, index + 1, Py_NewRef(PyTuple_GET_ITEM(new_args, index)));
    }
    Py_DECREF(new_args);

    PyObject *receiver;
    PyObject *naive = NULL;
    PyObject *aware = NULL;
    if (offset_object == Py_None) {
        naive = date_naive_datetime(state->left);
        receiver = naive;
    }
    else {
        aware = date_datetime_with_timezone(state->left, offset_object);
        receiver = aware;
    }
    if (receiver == NULL) {
        Py_DECREF(full_args);
        return NULL;
    }
    PyObject *result = call_descriptor(original_methods[7], receiver, full_args, state->kwargs);
    Py_DECREF(full_args);
    Py_XDECREF(naive);
    Py_XDECREF(aware);
    return result;
}

static PyObject *
date_run_callback(DateState *state, const char *name, PyObject *argument)
{
    if (state->operation == DATETIME_TIME_UTCOFFSET ||
        state->operation == DATETIME_TIME_DST ||
        state->operation == DATETIME_TIME_TZNAME ||
        state->operation == DATETIME_TIME_ISOFORMAT ||
        state->operation == DATETIME_TIME_COMPARE) {
        argument = Py_None;
    }
    return PyObject_CallMethod(state->tz, name, "O", argument);
}

static PyObject *
date_iso_drive(DateState *state, PyObject *value)
{
    PyObject *offset = value;
    if (value == NULL) {
        state->stage = DATE_STAGE_OFFSET;
        offset = date_run_callback(state, "utcoffset", state->left);
        if (offset == NULL) {
            return NULL;
        }
    }
    PyObject *result = date_finish(state, offset);
    if (value == NULL) {
        Py_DECREF(offset);
    }
    return result;
}

static PyObject *
date_timestamp_drive(DateState *state, PyObject *value)
{
    PyObject *offset = value;
    if (value == NULL) {
        state->stage = DATE_STAGE_LEFT_OFFSET;
        offset = date_run_callback(state, "utcoffset", state->left);
        if (offset == NULL) {
            return NULL;
        }
    }
    PyObject *result = date_finish(state, offset);
    if (value == NULL) {
        Py_DECREF(offset);
    }
    return result;
}

static PyObject *
date_astimezone_drive(DateState *state, PyObject *value)
{
    PyObject *offset = value;
    if (offset == NULL) {
        state->stage = DATE_STAGE_ASTIMEZONE_SOURCE_OFFSET;
        PyObject *source_tz = PyDateTime_DATE_GET_TZINFO(state->left);
        offset = PyObject_CallMethod(source_tz, "utcoffset", "O", state->left);
        if (offset == NULL) return NULL;
    }
    int64_t ignored;
    if (validate_callback(DATETIME_DATETIME_UTCOFFSET, offset, &ignored) < 0) {
        if (value == NULL) Py_DECREF(offset);
        return NULL;
    }
    PyObject *source;
    if (offset == Py_None) {
        source = date_naive_datetime(state->left);
    }
    else {
        source = date_datetime_with_timezone(state->left, offset);
    }
    if (source == NULL) {
        if (value == NULL) Py_DECREF(offset);
        return NULL;
    }
    state->stage = DATE_STAGE_NONE;
    PyObject *result = call_descriptor(
        original_methods[2],
        source,
        state->args,
        state->kwargs
    );
    Py_DECREF(source);
    if (value == NULL) Py_DECREF(offset);
    return result;
}

static PyObject *
date_compare_drive(DateState *state, PyObject *value)
{
    PyObject *left_offset = value;
    if (value == NULL) {
        state->stage = DATE_STAGE_LEFT_OFFSET;
        left_offset = date_run_callback(state, "utcoffset", state->left);
        if (left_offset == NULL) {
            return NULL;
        }
    }
    if (validate_callback(DATETIME_DATETIME_UTCOFFSET, left_offset, NULL) < 0) {
        if (value == NULL) Py_DECREF(left_offset);
        return NULL;
    }
    Py_XSETREF(state->left_offset, Py_NewRef(left_offset));
    PyObject *right_offset = date_call_right_offset(
        state,
        DATE_WAIT_RIGHT_OFFSET
    );
    if (right_offset == NULL) {
        if (value == NULL) Py_DECREF(left_offset);
        return NULL;
    }
    PyObject *result;
    if (state->operation == DATETIME_DATETIME_SUBTRACT) {
        result = date_subtract_result(state, left_offset, right_offset);
    }
    else if (state->operation == DATETIME_DATETIME_COMPARE) {
        result = date_datetime_compare_after_first_right(state, right_offset);
    }
    else {
        result = date_compare_result(state, left_offset, right_offset);
    }
    Py_DECREF(right_offset);
    if (value == NULL) Py_DECREF(left_offset);
    return result;
}

static PyObject *
date_strftime_drive(DateState *state, PyObject *value)
{
    PyObject *current = value == NULL ? NULL : Py_NewRef(value);
    if (state->stage == DATE_STAGE_DST) {
        if (current == NULL) {
            current = date_run_callback(state, "dst", state->left);
            if (current == NULL) return NULL;
        }
        int64_t ignored;
        if (validate_callback(DATETIME_DATETIME_DST, current, &ignored) < 0) {
            Py_DECREF(current);
            return NULL;
        }
        Py_XSETREF(state->dst_value, Py_NewRef(current));
        state->stage = DATE_STAGE_OFFSET;
        Py_DECREF(current);
        current = date_run_callback(state, "utcoffset", state->left);
        if (current == NULL) return NULL;
    }
    if (state->stage == DATE_STAGE_OFFSET) {
        int64_t ignored;
        if (validate_callback(DATETIME_DATETIME_UTCOFFSET, current, &ignored) < 0) {
            Py_DECREF(current);
            return NULL;
        }
        Py_XSETREF(state->offset_value, Py_NewRef(current));
        state->stage = DATE_STAGE_NAME;
        Py_DECREF(current);
        current = date_run_callback(state, "tzname", state->left);
        if (current == NULL) return NULL;
    }
    if (state->stage == DATE_STAGE_NAME) {
        int64_t ignored;
        if (validate_callback(DATETIME_DATETIME_TZNAME, current, &ignored) < 0) {
            Py_DECREF(current);
            return NULL;
        }
        Py_XSETREF(state->name_value, Py_NewRef(current));
        Py_DECREF(current);
        return date_strftime(state, state->offset_value);
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid datetime strftime callback stage");
    return NULL;
}

static PyObject *
date_call_right_offset(DateState *state, DatePhase phase)
{
    PyObject *tz = state->operation == DATETIME_TIME_COMPARE
        ? PyDateTime_TIME_GET_TZINFO(state->right)
        : PyDateTime_DATE_GET_TZINFO(state->right);
    if (tz == Py_None) {
        return Py_NewRef(Py_None);
    }
    state->phase = phase;
    PyObject *argument = state->operation == DATETIME_TIME_COMPARE
        ? Py_None : state->right;
    PyObject *result = PyObject_CallMethod(tz, "utcoffset", "O", argument);
    if (result != NULL) {
        state->phase = DATE_WAIT_CALLBACK;
    }
    return result;
}

static PyObject *
date_compare_result(DateState *state, PyObject *left_offset, PyObject *right_offset)
{
    int64_t left_us;
    int64_t right_us;
    if (delta_microseconds(left_offset, &left_us) < 0 ||
        delta_microseconds(right_offset, &right_us) < 0) {
        return NULL;
    }
    if (left_offset == Py_None || right_offset == Py_None) {
        if (left_offset == Py_None && right_offset == Py_None) {
            /* Both values are offset-naive.  The original slot is not called
             * here because these temporary values have already been made
             * naive, but it supplies the exact rich-comparison semantics. */
        }
        else if (state->compare_op == Py_EQ) {
            Py_RETURN_FALSE;
        }
        else if (state->compare_op == Py_NE) {
            Py_RETURN_TRUE;
        }
        else {
            PyErr_Format(
                PyExc_TypeError,
                "can't compare offset-naive and offset-aware %s",
                state->operation == DATETIME_TIME_COMPARE ? "times" : "datetimes"
            );
            return NULL;
        }
    }
    PyObject *left_naive;
    PyObject *right_naive;
    if (state->operation == DATETIME_TIME_COMPARE) {
        left_naive = PyDateTime_FromDateAndTime(
            2000, 1, 1,
            PyDateTime_TIME_GET_HOUR(state->left),
            PyDateTime_TIME_GET_MINUTE(state->left),
            PyDateTime_TIME_GET_SECOND(state->left),
            PyDateTime_TIME_GET_MICROSECOND(state->left)
        );
        right_naive = PyDateTime_FromDateAndTime(
            2000, 1, 1,
            PyDateTime_TIME_GET_HOUR(state->right),
            PyDateTime_TIME_GET_MINUTE(state->right),
            PyDateTime_TIME_GET_SECOND(state->right),
            PyDateTime_TIME_GET_MICROSECOND(state->right)
        );
    }
    else {
        left_naive = date_naive_datetime(state->left);
        right_naive = date_naive_datetime(state->right);
    }
    if (left_naive == NULL || right_naive == NULL) {
        Py_XDECREF(left_naive);
        Py_XDECREF(right_naive);
        return NULL;
    }
    PyObject *left_delta = PyDelta_FromDSU(
        (int)(-left_us / 86400000000LL),
        (int)((-left_us % 86400000000LL) / 1000000LL),
        (int)(-left_us % 1000000LL)
    );
    PyObject *right_delta = PyDelta_FromDSU(
        (int)(-right_us / 86400000000LL),
        (int)((-right_us % 86400000000LL) / 1000000LL),
        (int)(-right_us % 1000000LL)
    );
    PyObject *left_adjusted = left_delta == NULL ? NULL : PyNumber_Add(left_naive, left_delta);
    PyObject *right_adjusted = right_delta == NULL ? NULL : PyNumber_Add(right_naive, right_delta);
    Py_XDECREF(left_delta);
    Py_XDECREF(right_delta);
    Py_DECREF(left_naive);
    Py_DECREF(right_naive);
    if (left_adjusted == NULL || right_adjusted == NULL) {
        Py_XDECREF(left_adjusted);
        Py_XDECREF(right_adjusted);
        return NULL;
    }
    PyObject *result = PyObject_RichCompare(
        left_adjusted,
        right_adjusted,
        state->compare_op
    );
    Py_DECREF(left_adjusted);
    Py_DECREF(right_adjusted);
    return result;
}

static PyObject *
date_subtract_result(DateState *state, PyObject *left_offset, PyObject *right_offset)
{
    if (delta_microseconds(left_offset, NULL) < 0 ||
        delta_microseconds(right_offset, NULL) < 0) {
        return NULL;
    }
    if ((left_offset == Py_None) != (right_offset == Py_None)) {
        PyErr_SetString(
            PyExc_TypeError,
            "can't subtract offset-naive and offset-aware datetimes"
        );
        return NULL;
    }
    PyObject *left = date_naive_datetime(state->left);
    PyObject *right = date_naive_datetime(state->right);
    if (left == NULL || right == NULL) {
        Py_XDECREF(left);
        Py_XDECREF(right);
        return NULL;
    }
    PyObject *result = PyNumber_Subtract(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    if (result == NULL) {
        return NULL;
    }
    PyObject *adjusted = PyNumber_Subtract(result, left_offset);
    Py_DECREF(result);
    if (adjusted == NULL) {
        return NULL;
    }
    result = PyNumber_Add(adjusted, right_offset);
    Py_DECREF(adjusted);
    return result;
}

static int
date_offsets_equal(PyObject *left, PyObject *right, int *equal)
{
    if (left == right) {
        *equal = 1;
        return 0;
    }
    if (!PyDelta_Check(left) || !PyDelta_Check(right)) {
        *equal = 0;
        return 0;
    }
    int64_t left_us;
    int64_t right_us;
    if (delta_microseconds(left, &left_us) < 0 ||
        delta_microseconds(right, &right_us) < 0) {
        return -1;
    }
    *equal = left_us == right_us;
    return 0;
}

static int
date_wall_times_equal(DateState *state, int *equal)
{
    PyObject *left = date_naive_datetime(state->left);
    PyObject *right = date_naive_datetime(state->right);
    if (left == NULL || right == NULL) {
        Py_XDECREF(left);
        Py_XDECREF(right);
        return -1;
    }
    int result = PyObject_RichCompareBool(left, right, Py_EQ);
    Py_DECREF(left);
    Py_DECREF(right);
    if (result < 0) {
        return -1;
    }
    *equal = result;
    return 0;
}

static PyObject *
date_flip_fold(PyObject *value)
{
    return PyDateTimeAPI->DateTime_FromDateAndTimeAndFold(
        PyDateTime_GET_YEAR(value),
        PyDateTime_GET_MONTH(value),
        PyDateTime_GET_DAY(value),
        PyDateTime_DATE_GET_HOUR(value),
        PyDateTime_DATE_GET_MINUTE(value),
        PyDateTime_DATE_GET_SECOND(value),
        PyDateTime_DATE_GET_MICROSECOND(value),
        PyDateTime_DATE_GET_TZINFO(value),
        !PyDateTime_DATE_GET_FOLD(value),
        Py_TYPE(value)
    );
}

static PyObject *
date_call_fold_offset(DateState *state, PyObject *value, DatePhase phase)
{
    PyObject *flipped = date_flip_fold(value);
    if (flipped == NULL) {
        return NULL;
    }
    PyObject *tz = PyDateTime_DATE_GET_TZINFO(value);
    state->phase = phase;
    PyObject *result = PyObject_CallMethod(tz, "utcoffset", "O", flipped);
    Py_DECREF(flipped);
    if (result != NULL) {
        state->phase = DATE_WAIT_CALLBACK;
    }
    return result;
}

static PyObject *
date_equality_result(DateState *state, int pep495_exception)
{
    int truth = state->compare_op == Py_NE
        ? pep495_exception : !pep495_exception;
    return PyBool_FromLong(truth);
}

static PyObject *
date_datetime_compare_start_fold_probe(DateState *state)
{
    PyObject *offset = date_call_fold_offset(
        state,
        state->left,
        DATE_WAIT_FOLD_LEFT_OFFSET
    );
    if (offset == NULL) {
        return NULL;
    }
    PyObject *result = date_datetime_compare_after_fold_left(state, offset);
    Py_DECREF(offset);
    return result;
}

static PyObject *
date_datetime_compare_maybe_fold(
    DateState *state,
    PyObject *result,
    int values_equal
)
{
    if ((state->compare_op != Py_EQ && state->compare_op != Py_NE) ||
        !values_equal) {
        return result;
    }
    Py_DECREF(result);
    return date_datetime_compare_start_fold_probe(state);
}

static PyObject *
date_datetime_compare_after_first_right(DateState *state, PyObject *right_offset)
{
    if (validate_callback(
            DATETIME_DATETIME_UTCOFFSET,
            right_offset,
            NULL
        ) < 0) {
        return NULL;
    }
    Py_XSETREF(state->right_offset, Py_NewRef(right_offset));

    int offsets_equal;
    if (date_offsets_equal(
            state->left_offset,
            state->right_offset,
            &offsets_equal
        ) < 0) {
        return NULL;
    }
    if (offsets_equal) {
        PyObject *result = date_compare_result(
            state,
            state->left_offset,
            state->right_offset
        );
        if (result == NULL) {
            return NULL;
        }
        int values_equal = 0;
        if ((state->compare_op == Py_EQ || state->compare_op == Py_NE) &&
            date_wall_times_equal(state, &values_equal) < 0) {
            Py_DECREF(result);
            return NULL;
        }
        return date_datetime_compare_maybe_fold(
            state,
            result,
            values_equal
        );
    }
    if (state->left_offset == Py_None || state->right_offset == Py_None) {
        return date_compare_result(
            state,
            state->left_offset,
            state->right_offset
        );
    }

    state->phase = DATE_WAIT_SECOND_LEFT_OFFSET;
    PyObject *tz = PyDateTime_DATE_GET_TZINFO(state->left);
    PyObject *second_left = PyObject_CallMethod(
        tz,
        "utcoffset",
        "O",
        state->left
    );
    if (second_left == NULL) {
        return NULL;
    }
    state->phase = DATE_WAIT_CALLBACK;
    PyObject *result = date_datetime_compare_after_second_left(
        state,
        second_left
    );
    Py_DECREF(second_left);
    return result;
}

static PyObject *
date_datetime_compare_after_second_left(DateState *state, PyObject *left_offset)
{
    if (validate_callback(
            DATETIME_DATETIME_UTCOFFSET,
            left_offset,
            NULL
        ) < 0) {
        return NULL;
    }
    Py_XSETREF(state->second_left_offset, Py_NewRef(left_offset));
    PyObject *right_offset = date_call_right_offset(
        state,
        DATE_WAIT_SECOND_RIGHT_OFFSET
    );
    if (right_offset == NULL) {
        return NULL;
    }
    PyObject *result = date_datetime_compare_after_second_right(
        state,
        right_offset
    );
    Py_DECREF(right_offset);
    return result;
}

static PyObject *
date_datetime_compare_after_second_right(DateState *state, PyObject *right_offset)
{
    if (validate_callback(
            DATETIME_DATETIME_UTCOFFSET,
            right_offset,
            NULL
        ) < 0) {
        return NULL;
    }
    PyObject *delta = date_subtract_result(
        state,
        state->second_left_offset,
        right_offset
    );
    if (delta == NULL) {
        return NULL;
    }
    int values_equal = PyDateTime_DELTA_GET_DAYS(delta) == 0 &&
        PyDateTime_DELTA_GET_SECONDS(delta) == 0 &&
        PyDateTime_DELTA_GET_MICROSECONDS(delta) == 0;
    PyObject *zero = PyDelta_FromDSU(0, 0, 0);
    if (zero == NULL) {
        Py_DECREF(delta);
        return NULL;
    }
    PyObject *result = PyObject_RichCompare(delta, zero, state->compare_op);
    Py_DECREF(delta);
    Py_DECREF(zero);
    if (result == NULL) {
        return NULL;
    }
    return date_datetime_compare_maybe_fold(state, result, values_equal);
}

static PyObject *
date_datetime_compare_after_fold_left(DateState *state, PyObject *offset)
{
    if (validate_callback(
            DATETIME_DATETIME_UTCOFFSET,
            offset,
            NULL
        ) < 0) {
        return NULL;
    }
    int equal;
    if (date_offsets_equal(offset, state->left_offset, &equal) < 0) {
        return NULL;
    }
    if (!equal) {
        return date_equality_result(state, 1);
    }
    PyObject *right_offset = date_call_fold_offset(
        state,
        state->right,
        DATE_WAIT_FOLD_RIGHT_OFFSET
    );
    if (right_offset == NULL) {
        return NULL;
    }
    PyObject *result = date_datetime_compare_after_fold_right(
        state,
        right_offset
    );
    Py_DECREF(right_offset);
    return result;
}

static PyObject *
date_datetime_compare_after_fold_right(DateState *state, PyObject *offset)
{
    if (validate_callback(
            DATETIME_DATETIME_UTCOFFSET,
            offset,
            NULL
        ) < 0) {
        return NULL;
    }
    int equal;
    if (date_offsets_equal(offset, state->right_offset, &equal) < 0) {
        return NULL;
    }
    return date_equality_result(state, !equal);
}

static PyObject *
date_finish(DateState *state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    int64_t offset;
    if (state->operation == DATETIME_NOW ||
        state->operation == DATETIME_FROMTIMESTAMP ||
        state->operation == DATETIME_ASTIMEZONE) {
        return Py_NewRef(value);
    }
    if (state->operation == DATETIME_TIME_UTCOFFSET ||
        state->operation == DATETIME_TIME_DST ||
        state->operation == DATETIME_TIME_TZNAME ||
        state->operation == DATETIME_DATETIME_UTCOFFSET ||
        state->operation == DATETIME_DATETIME_DST ||
        state->operation == DATETIME_DATETIME_TZNAME) {
        if (validate_callback(state->operation, value, &offset) < 0) {
            return NULL;
        }
        return Py_NewRef(value);
    }
    if (state->operation == DATETIME_TIMESTAMP) {
        if (validate_callback(state->operation, value, &offset) < 0 || value == Py_None) {
            if (value == Py_None && !PyErr_Occurred()) {
                PyErr_SetString(PyExc_NotImplementedError, "a tzinfo subclass must implement utcoffset()");
            }
            return NULL;
        }
        PyObject *left = date_naive_datetime(state->left);
        PyObject *epoch = PyDateTime_FromDateAndTime(1970, 1, 1, 0, 0, 0, 0);
        if (left == NULL || epoch == NULL) {
            Py_XDECREF(left);
            Py_XDECREF(epoch);
            return NULL;
        }
        PyObject *delta = PyNumber_Subtract(left, epoch);
        Py_DECREF(left);
        Py_DECREF(epoch);
        if (delta == NULL) {
            return NULL;
        }
        PyObject *adjusted = PyNumber_Subtract(delta, value);
        Py_DECREF(delta);
        if (adjusted == NULL) {
            return NULL;
        }
        PyObject *result = PyObject_CallMethod(adjusted, "total_seconds", NULL);
        Py_DECREF(adjusted);
        return result;
    }
    if (state->operation == DATETIME_TIME_ISOFORMAT ||
        state->operation == DATETIME_DATETIME_ISOFORMAT) {
        if (validate_callback(state->operation, value, &offset) < 0) {
            return NULL;
        }
        return date_isoformat(state, value);
    }
    if (state->operation == DATETIME_DATETIME_STRFTIME) {
        if (state->compare_op == 1) {
            if (validate_callback(DATETIME_DATETIME_DST, value, &offset) < 0) {
                return NULL;
            }
            PyObject *tz = PyDateTime_DATE_GET_TZINFO(state->left);
            PyObject *utcoffset = PyObject_CallMethod(tz, "utcoffset", "O", state->left);
            if (utcoffset == NULL) {
                return NULL;
            }
            PyObject *result = date_strftime(state, utcoffset);
            Py_DECREF(utcoffset);
            return result;
        }
        if (state->compare_op == 3) {
            if (validate_callback(DATETIME_DATETIME_TZNAME, value, &offset) < 0) {
                return NULL;
            }
            PyObject *tz = PyDateTime_DATE_GET_TZINFO(state->left);
            PyObject *utcoffset = PyObject_CallMethod(tz, "utcoffset", "O", state->left);
            if (utcoffset == NULL) {
                return NULL;
            }
            PyObject *result = date_strftime(state, utcoffset);
            Py_DECREF(utcoffset);
            return result;
        }
        if (validate_callback(DATETIME_DATETIME_UTCOFFSET, value, &offset) < 0) {
            return NULL;
        }
        return date_strftime(state, value);
    }
    if (state->operation == DATETIME_TIME_COMPARE ||
        state->operation == DATETIME_DATETIME_COMPARE) {
        if (validate_callback(DATETIME_DATETIME_UTCOFFSET, value, &offset) < 0) {
            return NULL;
        }
        PyObject *right_offset = date_call_right_offset(
            state,
            DATE_WAIT_RIGHT_OFFSET
        );
        if (right_offset == NULL) {
            return NULL;
        }
        PyObject *result = state->operation == DATETIME_DATETIME_COMPARE
            ? date_datetime_compare_after_first_right(state, right_offset)
            : date_compare_result(state, value, right_offset);
        Py_DECREF(right_offset);
        return result;
    }
    if (state->operation == DATETIME_DATETIME_SUBTRACT) {
        if (validate_callback(DATETIME_DATETIME_UTCOFFSET, value, &offset) < 0) {
            return NULL;
        }
        PyObject *right_offset = date_call_right_offset(
            state,
            DATE_WAIT_RIGHT_OFFSET
        );
        if (right_offset == NULL) {
            return NULL;
        }
        PyObject *result = date_subtract_result(state, value, right_offset);
        Py_DECREF(right_offset);
        return result;
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown datetime adapter operation");
    return NULL;
}

static PyObject *
date_resume(const void *raw_state, PyObject *value)
{
    const DateState *source = raw_state;
    if (value == NULL) {
        return NULL;
    }
    DateState *state = date_copy_state(source);
    if (state == NULL) {
        return NULL;
    }
    PyObject *result;
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &date_vtable, state) < 0) {
        date_free_state(state);
        return NULL;
    }
    if (state->phase == DATE_WAIT_RIGHT_OFFSET) {
        if (state->operation == DATETIME_DATETIME_SUBTRACT) {
            result = date_subtract_result(state, state->left_offset, value);
        }
        else if (state->operation == DATETIME_DATETIME_COMPARE) {
            result = date_datetime_compare_after_first_right(state, value);
        }
        else {
            result = date_compare_result(state, state->left_offset, value);
        }
    }
    else if (state->phase == DATE_WAIT_SECOND_LEFT_OFFSET) {
        result = date_datetime_compare_after_second_left(state, value);
    }
    else if (state->phase == DATE_WAIT_SECOND_RIGHT_OFFSET) {
        result = date_datetime_compare_after_second_right(state, value);
    }
    else if (state->phase == DATE_WAIT_FOLD_LEFT_OFFSET) {
        result = date_datetime_compare_after_fold_left(state, value);
    }
    else if (state->phase == DATE_WAIT_FOLD_RIGHT_OFFSET) {
        result = date_datetime_compare_after_fold_right(state, value);
    }
    else if (state->operation == DATETIME_TIME_ISOFORMAT ||
        state->operation == DATETIME_DATETIME_ISOFORMAT) {
        result = date_iso_drive(state, value);
    }
    else if (state->operation == DATETIME_TIMESTAMP) {
        result = date_timestamp_drive(state, value);
    }
    else if (state->operation == DATETIME_TIME_COMPARE ||
             state->operation == DATETIME_DATETIME_COMPARE ||
             state->operation == DATETIME_DATETIME_SUBTRACT) {
        result = date_compare_drive(state, value);
    }
    else if (state->operation == DATETIME_DATETIME_STRFTIME) {
        result = date_strftime_drive(state, value);
    }
    else if (state->operation == DATETIME_ASTIMEZONE &&
             state->stage == DATE_STAGE_ASTIMEZONE_SOURCE_OFFSET) {
        result = date_astimezone_drive(state, value);
    }
    else {
        result = date_finish(state, value);
    }
    adapter_leave(&frame);
    date_free_state(state);
    return result;
}

static PyObject *
date_start(DateOperation operation, PyObject *receiver, PyObject *args, PyObject *kwargs)
{
    DateState state = {
        .operation = operation,
        .phase = DATE_WAIT_CALLBACK,
        .compare_op = 0,
        .left = Py_NewRef(receiver),
        .right = NULL,
        .tz = NULL,
        .args = Py_NewRef(args),
        .kwargs = Py_XNewRef(kwargs),
        .left_offset = NULL,
    };
    if (operation == DATETIME_NOW || operation == DATETIME_FROMTIMESTAMP ||
        operation == DATETIME_ASTIMEZONE) {
        PyObject *target = NULL;
        Py_ssize_t target_index = operation == DATETIME_FROMTIMESTAMP ? 1 : 0;
        if (PyTuple_GET_SIZE(args) > target_index) {
            target = PyTuple_GET_ITEM(args, target_index);
        }
        else if (kwargs != NULL) {
            target = PyDict_GetItemString(kwargs, "tz");
        }
        if (target == NULL) target = Py_None;
        state.tz = Py_NewRef(target);
    }
    else if (PyDateTime_Check(receiver)) {
        state.tz = Py_NewRef(PyDateTime_DATE_GET_TZINFO(receiver));
    }
    else if (PyTime_Check(receiver)) {
        state.tz = Py_NewRef(PyDateTime_TIME_GET_TZINFO(receiver));
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &date_vtable, &state) < 0) {
        date_state_clear(&state);
        return NULL;
    }
    PyObject *descriptor;
    switch (operation) {
        case DATETIME_NOW: descriptor = original_methods[0]; break;
        case DATETIME_FROMTIMESTAMP: descriptor = original_methods[1]; break;
        case DATETIME_ASTIMEZONE: descriptor = original_methods[2]; break;
        case DATETIME_TIMESTAMP: descriptor = original_methods[3]; break;
        case DATETIME_DATETIME_UTCOFFSET: descriptor = original_methods[4]; break;
        case DATETIME_DATETIME_DST: descriptor = original_methods[5]; break;
        case DATETIME_DATETIME_TZNAME: descriptor = original_methods[6]; break;
        case DATETIME_DATETIME_STRFTIME: descriptor = original_methods[7]; break;
        case DATETIME_DATETIME_ISOFORMAT: descriptor = original_methods[8]; break;
        case DATETIME_TIME_UTCOFFSET: descriptor = original_time_methods[0]; break;
        case DATETIME_TIME_DST: descriptor = original_time_methods[1]; break;
        case DATETIME_TIME_TZNAME: descriptor = original_time_methods[2]; break;
        case DATETIME_TIME_ISOFORMAT: descriptor = original_time_methods[3]; break;
        default: descriptor = NULL; break;
    }
    PyObject *result;
    if (operation == DATETIME_TIME_ISOFORMAT ||
        operation == DATETIME_DATETIME_ISOFORMAT) {
        result = state.tz == Py_None
            ? call_descriptor(descriptor, receiver, args, kwargs)
            : NULL;
        if (result == NULL && state.tz != Py_None) {
            PyObject *naive = operation == DATETIME_TIME_ISOFORMAT
                ? date_naive_time(receiver)
                : date_naive_datetime(receiver);
            if (naive == NULL) {
                result = NULL;
            }
            else {
                result = call_descriptor(descriptor, naive, args, kwargs);
                Py_DECREF(naive);
                if (result != NULL) {
                    Py_DECREF(result);
                    result = date_iso_drive(&state, NULL);
                }
            }
        }
    }
    else if (operation == DATETIME_TIMESTAMP) {
        result = state.tz == Py_None
            ? call_descriptor(descriptor, receiver, args, kwargs)
            : date_timestamp_drive(&state, NULL);
    }
    else if (operation == DATETIME_DATETIME_STRFTIME) {
        result = state.tz == Py_None
            ? call_descriptor(descriptor, receiver, args, kwargs)
            : (state.stage = DATE_STAGE_DST,
               date_strftime_drive(&state, NULL));
    }
    else if (operation == DATETIME_ASTIMEZONE &&
             PyDateTime_DATE_GET_TZINFO(receiver) != Py_None &&
             (state.tz == Py_None ||
              PyDateTime_DATE_GET_TZINFO(receiver) != state.tz)) {
        result = date_astimezone_drive(&state, NULL);
    }
    else {
        result = descriptor == NULL
            ? NULL : call_descriptor(descriptor, receiver, args, kwargs);
    }
    adapter_leave(&frame);
    date_state_clear(&state);
    return result;
}

#define DATE_VARARGS_WRAPPER(name, operation) \
    static PyObject *name(PyObject *self, PyObject *args, PyObject *kwargs) \
    { return date_start(operation, self, args, kwargs); }

static PyObject *
date_start_noargs(DateOperation operation, PyObject *self)
{
    PyObject *args = PyTuple_New(0);
    if (args == NULL) return NULL;
    PyObject *result = date_start(operation, self, args, NULL);
    Py_DECREF(args);
    return result;
}

DATE_VARARGS_WRAPPER(datetime_now, DATETIME_NOW)
DATE_VARARGS_WRAPPER(datetime_fromtimestamp, DATETIME_FROMTIMESTAMP)
DATE_VARARGS_WRAPPER(datetime_astimezone, DATETIME_ASTIMEZONE)
static PyObject *datetime_timestamp(PyObject *self)
{ return date_start_noargs(DATETIME_TIMESTAMP, self); }
static PyObject *datetime_utcoffset(PyObject *self)
{ return date_start_noargs(DATETIME_DATETIME_UTCOFFSET, self); }
static PyObject *datetime_dst(PyObject *self)
{ return date_start_noargs(DATETIME_DATETIME_DST, self); }
static PyObject *datetime_tzname(PyObject *self)
{ return date_start_noargs(DATETIME_DATETIME_TZNAME, self); }
DATE_VARARGS_WRAPPER(datetime_strftime, DATETIME_DATETIME_STRFTIME)
DATE_VARARGS_WRAPPER(datetime_isoformat, DATETIME_DATETIME_ISOFORMAT)
static PyObject *time_utcoffset(PyObject *self)
{ return date_start_noargs(DATETIME_TIME_UTCOFFSET, self); }
static PyObject *time_dst(PyObject *self)
{ return date_start_noargs(DATETIME_TIME_DST, self); }
static PyObject *time_tzname(PyObject *self)
{ return date_start_noargs(DATETIME_TIME_TZNAME, self); }
DATE_VARARGS_WRAPPER(time_isoformat, DATETIME_TIME_ISOFORMAT)

static PyObject *
date_richcompare(PyObject *left, PyObject *right, int op, int is_time)
{
    if ((is_time && (!PyTime_Check(left) || !PyTime_Check(right))) ||
        (!is_time && (!PyDateTime_Check(left) || !PyDateTime_Check(right)))) {
        return is_time
            ? original_time_richcompare(left, right, op)
            : original_datetime_richcompare(left, right, op);
    }
    DateState state = {
        .operation = is_time ? DATETIME_TIME_COMPARE : DATETIME_DATETIME_COMPARE,
        .phase = DATE_WAIT_CALLBACK,
        .compare_op = op,
        .left = Py_NewRef(left),
        .right = Py_NewRef(right),
        .tz = NULL,
        .args = NULL,
        .kwargs = NULL,
        .left_offset = NULL,
    };
    PyObject *left_tz = is_time
        ? PyDateTime_TIME_GET_TZINFO(left)
        : PyDateTime_DATE_GET_TZINFO(left);
    PyObject *right_tz = is_time
        ? PyDateTime_TIME_GET_TZINFO(right)
        : PyDateTime_DATE_GET_TZINFO(right);
    if (left_tz == Py_None || right_tz == Py_None || left_tz == right_tz) {
        date_state_clear(&state);
        return is_time
            ? original_time_richcompare(left, right, op)
            : original_datetime_richcompare(left, right, op);
    }
    state.tz = Py_NewRef(left_tz);
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &date_vtable, &state) < 0) {
        date_state_clear(&state);
        return NULL;
    }
    PyObject *result = date_compare_drive(&state, NULL);
    adapter_leave(&frame);
    date_state_clear(&state);
    return result;
}

static PyObject *datetime_richcompare_wrapper(PyObject *left, PyObject *right, int op)
{ return date_richcompare(left, right, op, 0); }

static PyObject *time_richcompare_wrapper(PyObject *left, PyObject *right, int op)
{ return date_richcompare(left, right, op, 1); }

static PyObject *
datetime_subtract_wrapper(PyObject *left, PyObject *right)
{
    if (!PyDateTime_Check(left) || !PyDateTime_Check(right) ||
        PyDateTime_DATE_GET_TZINFO(left) == Py_None ||
        PyDateTime_DATE_GET_TZINFO(right) == Py_None ||
        PyDateTime_DATE_GET_TZINFO(left) == PyDateTime_DATE_GET_TZINFO(right)) {
        return original_datetime_subtract(left, right);
    }
    DateState state = {
        .operation = DATETIME_DATETIME_SUBTRACT,
        .phase = DATE_WAIT_CALLBACK,
        .compare_op = 0,
        .left = Py_NewRef(left),
        .right = Py_NewRef(right),
        .tz = Py_NewRef(PyDateTime_DATE_GET_TZINFO(left)),
        .args = NULL,
        .kwargs = NULL,
        .left_offset = NULL,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &date_vtable, &state) < 0) {
        date_state_clear(&state);
        return NULL;
    }
    PyObject *result = date_compare_drive(&state, NULL);
    adapter_leave(&frame);
    date_state_clear(&state);
    return result;
}

static int
replace_method(PyTypeObject *type, const char *name, PyObject *original,
               PyMethodDef *method, PyCFunction function, int flags)
{
    if (!Py_IS_TYPE(original, &PyMethodDescr_Type) &&
        !Py_IS_TYPE(original, &PyClassMethodDescr_Type)) {
        PyErr_Format(PyExc_RuntimeError, "datetime.%s is not a C method", name);
        return -1;
    }
    PyMethodDef *source = ((PyMethodDescrObject *)original)->d_method;
    *method = *source;
    method->ml_name = name;
    method->ml_meth = function;
    method->ml_flags = flags;
    PyObject *descriptor = (flags & METH_CLASS) != 0
        ? PyDescr_NewClassMethod(type, method)
        : PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
        return -1;
    }
    PyObject *dict = PyType_GetDict(type);
    int result = dict == NULL ? -1 : aleff_adapter_register_callable(descriptor);
    if (result == 0) {
        result = PyDict_SetItemString(dict, name, descriptor);
    }
    Py_XDECREF(dict);
    Py_DECREF(descriptor);
    if (result == 0) {
        PyType_Modified(type);
    }
    return result;
}

int
adapter_datetime_install(PyObject *module)
{
    if (datetime_installed) {
        return 0;
    }
    PyDateTime_IMPORT;
    datetime_module = Py_NewRef(module);
    PyObject *datetime_object = PyObject_GetAttrString(module, "datetime");
    PyObject *time_object = PyObject_GetAttrString(module, "time");
    datetime_timezone = PyObject_GetAttrString(module, "timezone");
    if (datetime_object == NULL || time_object == NULL || datetime_timezone == NULL ||
        !PyType_Check(datetime_object) || !PyType_Check(time_object)) {
        Py_XDECREF(datetime_object);
        Py_XDECREF(time_object);
        adapter_datetime_rollback();
        return -1;
    }
    datetime_type = (PyTypeObject *)datetime_object;
    time_type = (PyTypeObject *)time_object;
    PyObject *dict = PyType_GetDict(datetime_type);
    PyObject *time_dict = PyType_GetDict(time_type);
    const char *names[] = {"now", "fromtimestamp", "astimezone", "timestamp", "utcoffset", "dst", "tzname", "strftime", "isoformat"};
    const char *time_names[] = {"utcoffset", "dst", "tzname", "isoformat"};
    for (int index = 0; index < 9; index++) {
        original_methods[index] = dict == NULL ? NULL : Py_XNewRef(PyDict_GetItemString(dict, names[index]));
    }
    if (original_methods[7] == NULL) {
        PyObject *date_dict = PyType_GetDict(PyDateTimeAPI->DateType);
        original_methods[7] = date_dict == NULL
            ? NULL
            : Py_XNewRef(PyDict_GetItemString(date_dict, names[7]));
        Py_XDECREF(date_dict);
        datetime_strftime_inherited = original_methods[7] != NULL;
    }
    for (int index = 0; index < 4; index++) {
        original_time_methods[index] = time_dict == NULL ? NULL : Py_XNewRef(PyDict_GetItemString(time_dict, time_names[index]));
    }
    Py_XDECREF(dict);
    Py_XDECREF(time_dict);
    original_datetime_richcompare = datetime_type->tp_richcompare;
    original_time_richcompare = time_type->tp_richcompare;
    original_datetime_subtract = datetime_type->tp_as_number == NULL ? NULL : datetime_type->tp_as_number->nb_subtract;
    int ok = original_methods[0] != NULL && original_methods[1] != NULL &&
        original_methods[2] != NULL && original_methods[3] != NULL &&
        original_methods[4] != NULL && original_methods[5] != NULL &&
        original_methods[6] != NULL && original_methods[7] != NULL &&
        original_methods[8] != NULL && original_time_methods[0] != NULL &&
        original_time_methods[1] != NULL && original_time_methods[2] != NULL &&
        original_time_methods[3] != NULL && original_datetime_subtract != NULL;
    if (!ok || replace_method(datetime_type, "now", original_methods[0], &method_defs[0], _PyCFunction_CAST(datetime_now), METH_VARARGS | METH_KEYWORDS | METH_CLASS) < 0 ||
        replace_method(datetime_type, "fromtimestamp", original_methods[1], &method_defs[1], _PyCFunction_CAST(datetime_fromtimestamp), METH_VARARGS | METH_KEYWORDS | METH_CLASS) < 0 ||
        replace_method(datetime_type, "astimezone", original_methods[2], &method_defs[2], _PyCFunction_CAST(datetime_astimezone), METH_VARARGS | METH_KEYWORDS) < 0 ||
        replace_method(datetime_type, "timestamp", original_methods[3], &method_defs[3], _PyCFunction_CAST(datetime_timestamp), METH_NOARGS) < 0 ||
        replace_method(datetime_type, "utcoffset", original_methods[4], &method_defs[4], _PyCFunction_CAST(datetime_utcoffset), METH_NOARGS) < 0 ||
        replace_method(datetime_type, "dst", original_methods[5], &method_defs[5], _PyCFunction_CAST(datetime_dst), METH_NOARGS) < 0 ||
        replace_method(datetime_type, "tzname", original_methods[6], &method_defs[6], _PyCFunction_CAST(datetime_tzname), METH_NOARGS) < 0 ||
        replace_method(datetime_type, "strftime", original_methods[7], &method_defs[7], _PyCFunction_CAST(datetime_strftime), METH_VARARGS | METH_KEYWORDS) < 0 ||
        replace_method(datetime_type, "isoformat", original_methods[8], &method_defs[8], _PyCFunction_CAST(datetime_isoformat), METH_VARARGS | METH_KEYWORDS) < 0 ||
        replace_method(time_type, "utcoffset", original_time_methods[0], &time_method_defs[0], _PyCFunction_CAST(time_utcoffset), METH_NOARGS) < 0 ||
        replace_method(time_type, "dst", original_time_methods[1], &time_method_defs[1], _PyCFunction_CAST(time_dst), METH_NOARGS) < 0 ||
        replace_method(time_type, "tzname", original_time_methods[2], &time_method_defs[2], _PyCFunction_CAST(time_tzname), METH_NOARGS) < 0 ||
        replace_method(time_type, "isoformat", original_time_methods[3], &time_method_defs[3], _PyCFunction_CAST(time_isoformat), METH_VARARGS | METH_KEYWORDS) < 0) {
        adapter_datetime_rollback();
        Py_DECREF(datetime_object);
        Py_DECREF(time_object);
        return -1;
    }
    datetime_type->tp_richcompare = datetime_richcompare_wrapper;
    time_type->tp_richcompare = time_richcompare_wrapper;
    datetime_type->tp_as_number->nb_subtract = datetime_subtract_wrapper;
    PyType_Modified(datetime_type);
    PyType_Modified(time_type);
    datetime_installed = 1;
    Py_DECREF(datetime_object);
    Py_DECREF(time_object);
    return 0;
}

void
adapter_datetime_rollback(void)
{
    if (datetime_type != NULL) {
        PyObject *dict = PyType_GetDict(datetime_type);
        const char *names[] = {"now", "fromtimestamp", "astimezone", "timestamp", "utcoffset", "dst", "tzname", "strftime", "isoformat"};
        for (int index = 0; index < 9; index++) {
            if (dict != NULL && index == 7 && datetime_strftime_inherited) {
                if (PyDict_GetItemString(dict, names[index]) != NULL) {
                    PyDict_DelItemString(dict, names[index]);
                }
            }
            else if (dict != NULL && original_methods[index] != NULL) {
                PyDict_SetItemString(dict, names[index], original_methods[index]);
            }
        }
        if (original_datetime_richcompare != NULL) {
            datetime_type->tp_richcompare = original_datetime_richcompare;
        }
        if (datetime_type->tp_as_number != NULL && original_datetime_subtract != NULL) {
            datetime_type->tp_as_number->nb_subtract = original_datetime_subtract;
        }
        PyType_Modified(datetime_type);
        Py_XDECREF(dict);
    }
    if (time_type != NULL) {
        PyObject *dict = PyType_GetDict(time_type);
        const char *names[] = {"utcoffset", "dst", "tzname", "isoformat"};
        for (int index = 0; index < 4; index++) {
            if (dict != NULL && original_time_methods[index] != NULL) {
                PyDict_SetItemString(dict, names[index], original_time_methods[index]);
            }
        }
        PyType_Modified(time_type);
        Py_XDECREF(dict);
    }
    for (int index = 0; index < 9; index++) Py_CLEAR(original_methods[index]);
    for (int index = 0; index < 4; index++) Py_CLEAR(original_time_methods[index]);
    Py_CLEAR(datetime_module);
    Py_CLEAR(datetime_timezone);
    datetime_type = NULL;
    time_type = NULL;
    original_datetime_richcompare = NULL;
    original_time_richcompare = NULL;
    original_datetime_subtract = NULL;
    datetime_strftime_inherited = 0;
    datetime_installed = 0;
}
