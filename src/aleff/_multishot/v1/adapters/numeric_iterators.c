#include "numeric_iterators.h"
#include "numeric.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef enum {
    NUMERIC_ITER_FSUM,
    NUMERIC_ITER_DIST,
    NUMERIC_ITER_PROD,
    NUMERIC_ITER_SUMPROD,
} NumericIteratorOperation;

typedef enum {
    NUMERIC_ITER_WAIT_ITER,
    NUMERIC_ITER_WAIT_NEXT,
    NUMERIC_ITER_WAIT_CONVERSION,
    NUMERIC_ITER_WAIT_MULTIPLY,
    NUMERIC_ITER_WAIT_MULTIPLY_LEFT_BIND,
    NUMERIC_ITER_WAIT_MULTIPLY_LEFT,
    NUMERIC_ITER_WAIT_MULTIPLY_RIGHT_BIND,
    NUMERIC_ITER_WAIT_MULTIPLY_RIGHT,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_RIGHT,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_LEFT,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT_BIND,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT_BIND,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH_BIND,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX_BIND,
    NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX,
    NUMERIC_ITER_WAIT_ADD,
    NUMERIC_ITER_WAIT_INT_FLUSH,
    NUMERIC_ITER_WAIT_FLOAT_FLUSH,
} NumericIteratorPhase;

typedef struct {
    double hi;
    double lo;
} NumericIteratorDoubleLength;

typedef struct {
    double hi;
    double lo;
    double tiny;
} NumericIteratorTripleLength;

typedef struct {
    PyObject *args;
    PyObject *kwargs;
    PyObject *start;
    PyObject *iterators[2];
    PyObject *values[2];
    PyObject *items[2];
    PyObject *pending;
    PyObject *result;
    PyObject *term;
    PyObject *multiply_left_method;
    PyObject *multiply_right_method;
    PyObject *multiply_left_reflected;
    PyObject *multiply_right_reflected;
    PyObject *multiply_compare_left_method;
    PyObject *multiply_compare_right_method;
    PyObject *multiply_truth_object;
    PyObject *multiply_truth_descriptor;
    double *partials;
    Py_ssize_t partial_count;
    Py_ssize_t partial_capacity;
    double special_sum;
    double inf_sum;
    double fsum_x;
    double fsum_xsave;
    double converted[2];
    double dist_max;
    Py_ssize_t dist_index;
    int dist_side;
    int dist_found_nan;
    int dist_collected[2];
    NumericIteratorTripleLength flt_total;
    long int_total;
    int int_path_enabled;
    int int_total_in_use;
    int flt_path_enabled;
    int flt_total_in_use;
    int stopped[2];
    int flush_after_pair;
    NumericIteratorOperation operation;
    NumericIteratorPhase phase;
    int iterator_index;
    int pending_float_from_index;
    int multiply_reflected_first;
    int multiply_truth_is_length;
    int multiply_try_left;
    int multiply_try_right;
    int multiply_compare_reflected_first;
} NumericIteratorState;

typedef struct {
    PyObject *module;
    PyObject *original;
    const char *name;
} NumericIteratorInstallation;

#define NUMERIC_ITER_INSTALLATION_MAX 8
#define NUMERIC_ITER_FSUM_PARTIALS 32
static NumericIteratorInstallation numeric_iterator_installations[
    NUMERIC_ITER_INSTALLATION_MAX
];
static Py_ssize_t numeric_iterator_installation_count;
static PyMethodDef numeric_iterator_methods[NUMERIC_ITER_INSTALLATION_MAX];

static PyObject *numeric_iterator_resume(const void *, PyObject *);
static void numeric_iterator_free_state(void *raw_state);

static int
numeric_iterator_check_long_mult_overflow(long left, long right)
{
    if (left == 0 || right == 0) {
        return 0;
    }
    if (left > 0) {
        return right > 0
            ? left > LONG_MAX / right
            : right < LONG_MIN / left;
    }
    return right > 0
        ? left < LONG_MIN / right
        : left < LONG_MAX / right;
}

static int
numeric_iterator_long_add_would_overflow(long left, long right)
{
    return (right > 0 && left > LONG_MAX - right) ||
        (right < 0 && left < LONG_MIN - right);
}

static NumericIteratorDoubleLength
numeric_iterator_dl_sum(double a, double b)
{
    double x = a + b;
    double z = x - a;
    double y = (a - (x - z)) + (b - z);
    return (NumericIteratorDoubleLength){x, y};
}

static NumericIteratorDoubleLength
numeric_iterator_dl_fast_sum(double a, double b)
{
    double x = a + b;
    double y = (a - x) + b;
    return (NumericIteratorDoubleLength){x, y};
}

static NumericIteratorDoubleLength
numeric_iterator_dl_mul(double x, double y)
{
    double z = x * y;
    double zz = fma(x, y, -z);
    return (NumericIteratorDoubleLength){z, zz};
}

static NumericIteratorTripleLength
numeric_iterator_tl_fma(
    double x,
    double y,
    NumericIteratorTripleLength total
)
{
    NumericIteratorDoubleLength pr = numeric_iterator_dl_mul(x, y);
    NumericIteratorDoubleLength sm = numeric_iterator_dl_sum(total.hi, pr.hi);
    NumericIteratorDoubleLength r1 = numeric_iterator_dl_sum(total.lo, pr.lo);
    NumericIteratorDoubleLength r2 = numeric_iterator_dl_sum(r1.hi, sm.lo);
    return (NumericIteratorTripleLength){
        sm.hi,
        r2.hi,
        total.tiny + r1.lo + r2.lo,
    };
}

static double
numeric_iterator_tl_to_double(NumericIteratorTripleLength total)
{
    NumericIteratorDoubleLength last = numeric_iterator_dl_sum(total.lo, total.hi);
    return total.tiny + last.lo + last.hi;
}

static void
numeric_iterator_clear_state(NumericIteratorState *state)
{
    Py_XDECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_XDECREF(state->start);
    for (int index = 0; index < 2; index++) {
        Py_XDECREF(state->iterators[index]);
        Py_XDECREF(state->values[index]);
        Py_XDECREF(state->items[index]);
    }
    Py_XDECREF(state->pending);
    Py_XDECREF(state->result);
    Py_XDECREF(state->term);
    Py_XDECREF(state->multiply_left_method);
    Py_XDECREF(state->multiply_right_method);
    Py_XDECREF(state->multiply_left_reflected);
    Py_XDECREF(state->multiply_right_reflected);
    Py_XDECREF(state->multiply_compare_left_method);
    Py_XDECREF(state->multiply_compare_right_method);
    Py_XDECREF(state->multiply_truth_object);
    Py_XDECREF(state->multiply_truth_descriptor);
    PyMem_Free(state->partials);
}

static void
numeric_iterator_free_state(void *raw_state)
{
    NumericIteratorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    numeric_iterator_clear_state(state);
    PyMem_Free(state);
}

static void *
numeric_iterator_copy_state(const void *raw_state)
{
    const NumericIteratorState *source = raw_state;
    NumericIteratorState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->args = NULL;
    copy->kwargs = NULL;
    copy->start = NULL;
    copy->pending = NULL;
    copy->result = NULL;
    copy->term = NULL;
    copy->multiply_left_method = NULL;
    copy->multiply_right_method = NULL;
    copy->multiply_left_reflected = NULL;
    copy->multiply_right_reflected = NULL;
    copy->multiply_compare_left_method = NULL;
    copy->multiply_compare_right_method = NULL;
    copy->multiply_truth_object = NULL;
    copy->multiply_truth_descriptor = NULL;
    copy->partials = NULL;
    for (int index = 0; index < 2; index++) {
        copy->iterators[index] = NULL;
        copy->values[index] = NULL;
        copy->items[index] = NULL;
    }
    copy->args = Py_NewRef(source->args);
    copy->kwargs = Py_XNewRef(source->kwargs);
    copy->start = Py_XNewRef(source->start);
    copy->pending = Py_XNewRef(source->pending);
    copy->result = Py_XNewRef(source->result);
    copy->term = Py_XNewRef(source->term);
    copy->multiply_left_method = Py_XNewRef(source->multiply_left_method);
    copy->multiply_right_method = Py_XNewRef(source->multiply_right_method);
    copy->multiply_left_reflected = Py_XNewRef(source->multiply_left_reflected);
    copy->multiply_right_reflected = Py_XNewRef(source->multiply_right_reflected);
    copy->multiply_compare_left_method = Py_XNewRef(source->multiply_compare_left_method);
    copy->multiply_compare_right_method = Py_XNewRef(source->multiply_compare_right_method);
    copy->multiply_truth_object = Py_XNewRef(source->multiply_truth_object);
    copy->multiply_truth_descriptor = Py_XNewRef(source->multiply_truth_descriptor);
    for (int index = 0; index < 2; index++) {
        copy->iterators[index] = source->iterators[index] == NULL
            ? NULL
            : clone_iterator_for_snapshot(source->iterators[index]);
        copy->values[index] = source->values[index] == NULL
            ? NULL
            : PyList_GetSlice(
                source->values[index],
                0,
                PyList_GET_SIZE(source->values[index])
            );
        copy->items[index] = Py_XNewRef(source->items[index]);
        if ((source->iterators[index] != NULL &&
             copy->iterators[index] == NULL) ||
            (source->values[index] != NULL && copy->values[index] == NULL)) {
            numeric_iterator_free_state(copy);
            return NULL;
        }
    }
    if (source->partials != NULL) {
        copy->partials = PyMem_Malloc(
            (size_t)source->partial_capacity * sizeof(double)
        );
        if (copy->partials == NULL) {
            PyErr_NoMemory();
            numeric_iterator_free_state(copy);
            return NULL;
        }
        memcpy(
            copy->partials,
            source->partials,
            (size_t)source->partial_count * sizeof(double)
        );
    }
    return copy;
}

static const AleffAdapterVTable numeric_iterator_vtable = {
    .copy_state = numeric_iterator_copy_state,
    .free_state = numeric_iterator_free_state,
    .resume = numeric_iterator_resume,
    .prepare_resume = NULL,
};

static int
numeric_iterator_count(NumericIteratorOperation operation)
{
    return operation == NUMERIC_ITER_DIST || operation == NUMERIC_ITER_SUMPROD
        ? 2 : 1;
}

static PyObject *
numeric_iterator_source(const NumericIteratorState *state, int index)
{
    return PyTuple_GET_ITEM(state->args, index);
}

static PyObject *
numeric_iterator_get_iterator(NumericIteratorState *state, int index)
{
    state->iterator_index = index;
    state->phase = NUMERIC_ITER_WAIT_ITER;
    PyObject *iterator = PyObject_GetIter(numeric_iterator_source(state, index));
    if (iterator == NULL) {
        return NULL;
    }
    state->iterators[index] = iterator;
    return iterator;
}

static int
numeric_iterator_has_protocol(PyObject *object, const char *name)
{
    PyObject *descriptor = lookup_raw_special(object, name);
    if (descriptor == NULL) return 0;
    Py_DECREF(descriptor);
    return 1;
}

static int
numeric_iterator_finish_double(
    NumericIteratorState *state,
    PyObject *value,
    double *result
)
{
    if (state->pending_float_from_index) {
        PyObject *index = adapter_numeric_validate_index_result(value);
        if (index == NULL) {
            return -1;
        }
        *result = PyLong_AsDouble(index);
        Py_DECREF(index);
        return *result == -1.0 && PyErr_Occurred() ? -1 : 0;
    }
    if (!PyFloat_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "%.50s.__float__ returned non-float (type %.50s)",
            state->pending == NULL ? "object" : Py_TYPE(state->pending)->tp_name,
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    if (!PyFloat_CheckExact(value) && PyErr_WarnFormat(
            PyExc_DeprecationWarning,
            1,
            "%.200s.__float__ returned non-float (type %.200s).  The ability "
            "to return an instance of a strict subclass of float is "
            "deprecated, and may be removed in a future version of Python.",
            state->pending == NULL ? "object" : Py_TYPE(state->pending)->tp_name,
            Py_TYPE(value)->tp_name
        ) < 0) return -1;
    *result = PyFloat_AS_DOUBLE(value);
    return 0;
}

static int
numeric_iterator_fsum_realloc(NumericIteratorState *state)
{
    Py_ssize_t capacity = state->partial_capacity * 2;
    if (state->partial_count >= capacity ||
        (size_t)capacity >= (size_t)PY_SSIZE_T_MAX / sizeof(double)) {
        PyErr_SetString(PyExc_MemoryError, "math.fsum partials");
        return -1;
    }
    double *partials = PyMem_Realloc(
        state->partials,
        (size_t)capacity * sizeof(double)
    );
    if (partials == NULL) {
        PyErr_SetString(PyExc_MemoryError, "math.fsum partials");
        return -1;
    }
    state->partials = partials;
    state->partial_capacity = capacity;
    return 0;
}

static int
numeric_iterator_assign_double(
    NumericIteratorState *state,
    PyObject *item,
    double *result
)
{
    if (PyFloat_CheckExact(item)) {
        *result = PyFloat_AS_DOUBLE(item);
        return 0;
    }
    if (PyLong_CheckExact(item)) {
        *result = PyLong_AsDouble(item);
        return *result == -1.0 && PyErr_Occurred() ? -1 : 0;
    }
    state->pending = Py_NewRef(item);
    state->phase = NUMERIC_ITER_WAIT_CONVERSION;
    state->pending_float_from_index = 0;
    PyObject *converted_object = NULL;
    if (numeric_iterator_has_protocol(item, "__float__")) {
        converted_object = PyNumber_Float(item);
    }
    else if (numeric_iterator_has_protocol(item, "__index__")) {
        state->pending_float_from_index = 1;
        converted_object = PyNumber_Index(item);
    }
    if (converted_object != NULL) {
        int status = numeric_iterator_finish_double(
            state,
            converted_object,
            result
        );
        Py_DECREF(converted_object);
        Py_CLEAR(state->pending);
        return status;
    }
    if (PyErr_Occurred()) return -1;
    double converted = PyFloat_AsDouble(item);
    if (converted == -1.0 && PyErr_Occurred()) return -1;
    *result = converted;
    Py_CLEAR(state->pending);
    return 0;
}

static int
numeric_iterator_fsum_add(NumericIteratorState *state, double x)
{
    double y, t, hi, yr, lo = 0.0;
    Py_ssize_t i, j;
    state->fsum_xsave = x;
    for (i = j = 0; j < state->partial_count; j++) {
        y = state->partials[j];
        if (fabs(x) < fabs(y)) {
            t = x;
            x = y;
            y = t;
        }
        hi = x + y;
        yr = hi - x;
        lo = y - yr;
        if (lo != 0.0) {
            state->partials[i++] = lo;
        }
        x = hi;
    }
    state->partial_count = i;
    if (x == 0.0) {
        return 0;
    }
    if (!isfinite(x)) {
        if (isfinite(state->fsum_xsave)) {
            PyErr_SetString(PyExc_OverflowError, "intermediate overflow in fsum");
            return -1;
        }
        if (isinf(state->fsum_xsave)) {
            state->inf_sum += state->fsum_xsave;
        }
        state->special_sum += state->fsum_xsave;
        state->partial_count = 0;
        return 0;
    }
    if (state->partial_count >= state->partial_capacity &&
        numeric_iterator_fsum_realloc(state) < 0) {
        return -1;
    }
    state->partials[state->partial_count++] = x;
    return 0;
}

static PyObject *
numeric_iterator_fsum_finish(NumericIteratorState *state)
{
    if (state->special_sum != 0.0) {
        if (isnan(state->inf_sum)) {
            PyErr_SetString(PyExc_ValueError, "-inf + inf in fsum");
            return NULL;
        }
        return PyFloat_FromDouble(state->special_sum);
    }
    double hi = 0.0;
    double lo = 0.0;
    if (state->partial_count > 0) {
        Py_ssize_t n = state->partial_count;
        hi = state->partials[--n];
        while (n > 0) {
            double x = hi;
            double y = state->partials[--n];
            hi = x + y;
            double yr = hi - x;
            lo = y - yr;
            if (lo != 0.0) {
                break;
            }
        }
        if (n > 0 &&
            ((lo < 0.0 && state->partials[n - 1] < 0.0) ||
             (lo > 0.0 && state->partials[n - 1] > 0.0))) {
            double y = lo * 2.0;
            double x = hi + y;
            double yr = x - hi;
            if (y == yr) {
                hi = x;
            }
        }
    }
    return PyFloat_FromDouble(hi);
}

static double
numeric_iterator_vector_norm(
    Py_ssize_t n,
    double *vector,
    double max,
    int found_nan
)
{
    double x, h, scale, csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    NumericIteratorDoubleLength pr, sm;
    int max_e;
    if (isinf(max)) {
        return max;
    }
    if (found_nan) {
        return Py_NAN;
    }
    if (max == 0.0 || n <= 1) {
        return max;
    }
    frexp(max, &max_e);
    if (max_e < -1023) {
        for (Py_ssize_t i = 0; i < n; i++) {
            vector[i] /= DBL_MIN;
        }
        return DBL_MIN * numeric_iterator_vector_norm(
            n, vector, max / DBL_MIN, found_nan
        );
    }
    scale = ldexp(1.0, -max_e);
    for (Py_ssize_t i = 0; i < n; i++) {
        x = vector[i] * scale;
        pr = numeric_iterator_dl_mul(x, x);
        sm = numeric_iterator_dl_fast_sum(csum, pr.hi);
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    h = sqrt(csum - 1.0 + (frac1 + frac2));
    pr = numeric_iterator_dl_mul(-h, h);
    sm = numeric_iterator_dl_fast_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);
    return h / scale;
}

static int
numeric_iterator_dist_add_diff(NumericIteratorState *state)
{
    double x = fabs(state->converted[0] - state->converted[1]);
    if (state->dist_index >= state->partial_capacity) {
        Py_ssize_t capacity = state->partial_capacity * 2;
        double *diffs = PyMem_Realloc(
            state->partials, (size_t)capacity * sizeof(double)
        );
        if (diffs == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        state->partials = diffs;
        state->partial_capacity = capacity;
    }
    state->partials[state->dist_index++] = x;
    state->dist_found_nan |= isnan(x);
    if (x > state->dist_max) {
        state->dist_max = x;
    }
    return 0;
}

static PyObject *
numeric_iterator_dist_continue(
    NumericIteratorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            if (state->phase == NUMERIC_ITER_WAIT_NEXT &&
                PyErr_ExceptionMatches(PyExc_StopIteration)) {
                PyErr_Clear();
                int index = state->iterator_index;
                state->dist_collected[index] = 1;
                state->iterator_index = index + 1;
            }
            else {
                return NULL;
            }
        }
        else if (state->phase == NUMERIC_ITER_WAIT_ITER) {
            if (!PyIter_Check(resumed_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(resumed_value)->tp_name
                );
                return NULL;
            }
            Py_XSETREF(
                state->iterators[state->iterator_index],
                Py_NewRef(resumed_value)
            );
        }
        else if (state->phase == NUMERIC_ITER_WAIT_NEXT) {
            state->pending = Py_NewRef(resumed_value);
        }
        else if (state->phase == NUMERIC_ITER_WAIT_CONVERSION) {
            if (numeric_iterator_finish_double(
                    state,
                    resumed_value,
                    &state->converted[state->dist_side]
                ) < 0) {
                return NULL;
            }
            Py_CLEAR(state->pending);
            if (state->dist_side == 1) {
                if (numeric_iterator_dist_add_diff(state) < 0) {
                    return NULL;
                }
                state->dist_side = 0;
            }
            else {
                state->dist_side = 1;
            }
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator state");
            return NULL;
        }
    }
    for (;;) {
        int index = state->iterator_index;
        if (!state->dist_collected[0] || !state->dist_collected[1]) {
            if (index > 1) {
                index = 0;
            }
            if (state->dist_collected[index]) {
                state->iterator_index = ++index;
                continue;
            }
            if (PyTuple_Check(numeric_iterator_source(state, index))) {
                PyObject *tuple = numeric_iterator_source(state, index);
                Py_ssize_t size = PyTuple_GET_SIZE(tuple);
                for (Py_ssize_t i = 0; i < size; i++) {
                    if (PyList_Append(
                            state->values[index], PyTuple_GET_ITEM(tuple, i)
                        ) < 0) {
                        return NULL;
                    }
                }
                state->dist_collected[index] = 1;
                state->iterator_index = index + 1;
                continue;
            }
            if (state->iterators[index] == NULL &&
                numeric_iterator_get_iterator(state, index) == NULL) {
                return NULL;
            }
            state->phase = NUMERIC_ITER_WAIT_NEXT;
            PyObject *item = state->pending;
            if (item == NULL) {
                item = PyIter_Next(state->iterators[index]);
                if (item == NULL) {
                    if (PyErr_Occurred()) {
                        return NULL;
                    }
                    state->dist_collected[index] = 1;
                    state->iterator_index = index + 1;
                    continue;
                }
            }
            else {
                state->pending = NULL;
            }
            if (PyList_Append(state->values[index], item) < 0) {
                Py_DECREF(item);
                return NULL;
            }
            Py_DECREF(item);
            continue;
        }

        Py_ssize_t p_size = PyList_GET_SIZE(state->values[0]);
        Py_ssize_t q_size = PyList_GET_SIZE(state->values[1]);
        if (p_size != q_size) {
            PyErr_SetString(
                PyExc_ValueError,
                "both points must have the same number of dimensions"
            );
            return NULL;
        }
        if (state->dist_index >= p_size) {
            return PyFloat_FromDouble(numeric_iterator_vector_norm(
                p_size, state->partials, state->dist_max, state->dist_found_nan
            ));
        }
        PyObject *item = PyList_GET_ITEM(
            state->values[state->dist_side], state->dist_index
        );
        state->phase = NUMERIC_ITER_WAIT_CONVERSION;
        if (numeric_iterator_assign_double(
                state, item, &state->converted[state->dist_side]
            ) < 0) {
            return NULL;
        }
        if (state->dist_side == 1) {
            if (numeric_iterator_dist_add_diff(state) < 0) {
                return NULL;
            }
            state->dist_side = 0;
        }
        else {
            state->dist_side = 1;
        }
    }
}

static PyObject *
numeric_iterator_fsum_continue(
    NumericIteratorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            if (state->phase == NUMERIC_ITER_WAIT_NEXT &&
                PyErr_ExceptionMatches(PyExc_StopIteration)) {
                PyErr_Clear();
                return numeric_iterator_fsum_finish(state);
            }
            return NULL;
        }
        if (state->phase == NUMERIC_ITER_WAIT_ITER) {
            if (!PyIter_Check(resumed_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(resumed_value)->tp_name
                );
                return NULL;
            }
            state->iterators[0] = Py_NewRef(resumed_value);
        }
        else if (state->phase == NUMERIC_ITER_WAIT_NEXT) {
            state->pending = Py_NewRef(resumed_value);
        }
        else if (state->phase == NUMERIC_ITER_WAIT_CONVERSION) {
            double converted;
            if (numeric_iterator_finish_double(
                    state,
                    resumed_value,
                    &converted
                ) < 0) return NULL;
            Py_CLEAR(state->pending);
            if (numeric_iterator_fsum_add(state, converted) < 0) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator state");
            return NULL;
        }
    }
    if (state->iterators[0] == NULL &&
        numeric_iterator_get_iterator(state, 0) == NULL) {
        return NULL;
    }
    for (;;) {
        state->phase = NUMERIC_ITER_WAIT_NEXT;
        PyObject *item = state->pending;
        if (item == NULL) {
            item = PyIter_Next(state->iterators[0]);
            if (item == NULL) {
                if (PyErr_Occurred()) {
                    return NULL;
                }
                return numeric_iterator_fsum_finish(state);
            }
        }
        else {
            state->pending = NULL;
        }
        state->phase = NUMERIC_ITER_WAIT_CONVERSION;
        if (numeric_iterator_assign_double(state, item, &state->fsum_x) < 0) {
            Py_DECREF(item);
            return NULL;
        }
        Py_DECREF(item);
        if (numeric_iterator_fsum_add(state, state->fsum_x) < 0) {
            return NULL;
        }
    }
}

static PyObject *
numeric_iterator_call_binary_method(
    NumericIteratorState *state,
    PyObject *descriptor,
    PyObject *receiver,
    PyObject *argument,
    int reflected
)
{
    state->phase = reflected
        ? NUMERIC_ITER_WAIT_MULTIPLY_RIGHT_BIND
        : NUMERIC_ITER_WAIT_MULTIPLY_LEFT_BIND;
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, receiver, (PyObject *)Py_TYPE(receiver));
    if (callable == NULL) {
        return NULL;
    }
    state->phase = reflected
        ? NUMERIC_ITER_WAIT_MULTIPLY_RIGHT
        : NUMERIC_ITER_WAIT_MULTIPLY_LEFT;
    PyObject *result = PyObject_CallOneArg(callable, argument);
    Py_DECREF(callable);
    return result;
}

static void
numeric_iterator_clear_multiply_methods(NumericIteratorState *state)
{
    Py_CLEAR(state->multiply_left_method);
    Py_CLEAR(state->multiply_right_method);
    state->multiply_reflected_first = 0;
    state->multiply_try_left = 0;
    state->multiply_try_right = 0;
}

static PyObject *numeric_iterator_manual_multiply(
    NumericIteratorState *,
    PyObject *,
    int
);

static void
numeric_iterator_clear_overload_state(NumericIteratorState *state)
{
    Py_CLEAR(state->multiply_left_reflected);
    Py_CLEAR(state->multiply_right_reflected);
    Py_CLEAR(state->multiply_compare_left_method);
    Py_CLEAR(state->multiply_compare_right_method);
    Py_CLEAR(state->multiply_truth_object);
    Py_CLEAR(state->multiply_truth_descriptor);
    state->multiply_truth_is_length = 0;
    state->multiply_compare_reflected_first = 0;
}

static PyObject *
numeric_iterator_finish_overload_check(
    NumericIteratorState *state,
    int overloaded
)
{
    state->multiply_reflected_first = overloaded;
    numeric_iterator_clear_overload_state(state);
    return numeric_iterator_manual_multiply(state, NULL, 0);
}

static int
numeric_iterator_length_truth(PyObject *index, int *truth)
{
    if (Py_SIZE((PyLongObject *)index) < 0) {
        PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        return -1;
    }
    Py_ssize_t length = PyLong_AsSsize_t(index);
    if (length < 0 && PyErr_Occurred()) {
        return -1;
    }
    *truth = length != 0;
    return 0;
}

static PyObject *
numeric_iterator_finish_overload_length(
    NumericIteratorState *state,
    PyObject *value,
    int from_index_method
)
{
    PyObject *index;
    if (from_index_method) {
        index = adapter_numeric_validate_index_result(value);
    }
    else if (PyLong_Check(value)) {
        index = PyNumber_Index(value);
    }
    else {
        PyObject *descriptor = lookup_raw_special(value, "__index__");
        if (descriptor != NULL &&
            !Py_IS_TYPE(descriptor, &PyWrapperDescr_Type) &&
            !Py_IS_TYPE(descriptor, &PyMethodDescr_Type)) {
            Py_XSETREF(state->multiply_truth_object, Py_NewRef(value));
            Py_XSETREF(state->multiply_truth_descriptor, descriptor);
            state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX_BIND;
            descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
            PyObject *callable = get == NULL
                ? Py_NewRef(descriptor)
                : get(descriptor, value, (PyObject *)Py_TYPE(value));
            if (callable == NULL) {
                return NULL;
            }
            state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX;
            PyObject *result = PyObject_CallNoArgs(callable);
            Py_DECREF(callable);
            if (result == NULL) {
                return NULL;
            }
            PyObject *finished = numeric_iterator_finish_overload_length(
                state,
                result,
                1
            );
            Py_DECREF(result);
            return finished;
        }
        Py_XDECREF(descriptor);
        index = PyNumber_Index(value);
    }
    if (index == NULL) {
        return NULL;
    }
    int truth;
    int status = numeric_iterator_length_truth(index, &truth);
    Py_DECREF(index);
    if (status < 0) {
        return NULL;
    }
    return numeric_iterator_finish_overload_check(state, truth);
}

static PyObject *
numeric_iterator_finish_overload_truth(
    NumericIteratorState *state,
    PyObject *value
)
{
    if (state->multiply_truth_is_length) {
        return numeric_iterator_finish_overload_length(state, value, 0);
    }
    if (!PyBool_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__bool__ should return bool, returned %.200s",
            Py_TYPE(value)->tp_name
        );
        return NULL;
    }
    return numeric_iterator_finish_overload_check(state, value == Py_True);
}

static PyObject *
numeric_iterator_call_overload_truth(
    NumericIteratorState *state,
    PyObject *value,
    PyObject *descriptor,
    int is_length
)
{
    Py_XSETREF(state->multiply_truth_object, Py_NewRef(value));
    Py_XSETREF(state->multiply_truth_descriptor, descriptor);
    state->multiply_truth_is_length = is_length;
    state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH_BIND;
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, value, (PyObject *)Py_TYPE(value));
    if (callable == NULL) {
        return NULL;
    }
    state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH;
    PyObject *result = PyObject_CallNoArgs(callable);
    Py_DECREF(callable);
    if (result == NULL) {
        return NULL;
    }
    PyObject *finished = numeric_iterator_finish_overload_truth(state, result);
    Py_DECREF(result);
    return finished;
}

static PyObject *
numeric_iterator_finish_overload_comparison(
    NumericIteratorState *state,
    PyObject *comparison
)
{
    if (PyBool_Check(comparison)) {
        return numeric_iterator_finish_overload_check(
            state,
            comparison == Py_True
        );
    }
    PyObject *descriptor = lookup_raw_special(comparison, "__bool__");
    if (descriptor != NULL) {
        return numeric_iterator_call_overload_truth(
            state,
            comparison,
            descriptor,
            0
        );
    }
    descriptor = lookup_raw_special(comparison, "__len__");
    if (descriptor != NULL) {
        return numeric_iterator_call_overload_truth(
            state,
            comparison,
            descriptor,
            1
        );
    }
    return numeric_iterator_finish_overload_check(state, 1);
}

static PyObject *
numeric_iterator_call_compare_method(
    NumericIteratorState *state,
    int reflected
)
{
    PyObject *receiver = reflected
        ? state->multiply_right_reflected
        : state->multiply_left_reflected;
    PyObject *argument = reflected
        ? state->multiply_left_reflected
        : state->multiply_right_reflected;
    PyObject *descriptor = lookup_raw_special(receiver, "__ne__");
    if (reflected) {
        Py_XSETREF(state->multiply_compare_right_method, descriptor);
    }
    else {
        Py_XSETREF(state->multiply_compare_left_method, descriptor);
    }
    if (descriptor == NULL) {
        state->phase = reflected
            ? NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT
            : NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT;
        return Py_NewRef(Py_NotImplemented);
    }
    state->phase = reflected
        ? NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT_BIND
        : NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT_BIND;
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, receiver, (PyObject *)Py_TYPE(receiver));
    if (callable == NULL) {
        return NULL;
    }
    state->phase = reflected
        ? NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT
        : NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT;
    PyObject *result = PyObject_CallOneArg(callable, argument);
    Py_DECREF(callable);
    return result;
}

static PyObject *
numeric_iterator_manual_compare_reflected(
    NumericIteratorState *state,
    PyObject *completed,
    int is_resumed
)
{
    PyObject *comparison;
    if (is_resumed) {
        if (state->phase ==
            NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT_BIND) {
            state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT;
            comparison = PyObject_CallOneArg(
                completed,
                state->multiply_right_reflected
            );
        }
        else if (state->phase ==
                 NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT_BIND) {
            state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT;
            comparison = PyObject_CallOneArg(
                completed,
                state->multiply_left_reflected
            );
        }
        else {
            comparison = Py_NewRef(completed);
        }
    }
    else if (state->multiply_compare_reflected_first) {
        comparison = numeric_iterator_call_compare_method(state, 1);
    }
    else {
        comparison = numeric_iterator_call_compare_method(state, 0);
    }
    if (comparison == NULL) {
        return NULL;
    }
    if (comparison != Py_NotImplemented) {
        PyObject *result = numeric_iterator_finish_overload_comparison(
            state,
            comparison
        );
        Py_DECREF(comparison);
        return result;
    }
    Py_DECREF(comparison);

    if (state->phase ==
            NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT &&
        state->multiply_compare_reflected_first) {
        comparison = numeric_iterator_call_compare_method(state, 0);
    }
    else if (state->phase ==
                 NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT &&
             !state->multiply_compare_reflected_first &&
             Py_TYPE(state->multiply_right_reflected)->tp_richcompare !=
                 NULL) {
        comparison = numeric_iterator_call_compare_method(state, 1);
    }
    else {
        return numeric_iterator_finish_overload_check(state, 1);
    }
    if (comparison == NULL) {
        return NULL;
    }
    if (comparison == Py_NotImplemented) {
        Py_DECREF(comparison);
        return numeric_iterator_finish_overload_check(state, 1);
    }
    PyObject *result = numeric_iterator_finish_overload_comparison(
        state,
        comparison
    );
    Py_DECREF(comparison);
    return result;
}

static PyObject *
numeric_iterator_compare_reflected_methods(NumericIteratorState *state)
{
    if (state->multiply_left_reflected ==
        state->multiply_right_reflected) {
        return numeric_iterator_finish_overload_check(state, 0);
    }
    PyObject *left_method = lookup_raw_special(
        state->multiply_left_reflected,
        "__ne__"
    );
    PyObject *right_method = lookup_raw_special(
        state->multiply_right_reflected,
        "__ne__"
    );
    int manual = (left_method != NULL &&
                  !Py_IS_TYPE(left_method, &PyWrapperDescr_Type) &&
                  !Py_IS_TYPE(left_method, &PyMethodDescr_Type)) ||
        (right_method != NULL &&
         !Py_IS_TYPE(right_method, &PyWrapperDescr_Type) &&
         !Py_IS_TYPE(right_method, &PyMethodDescr_Type));
    if (manual) {
        state->multiply_compare_left_method = left_method;
        state->multiply_compare_right_method = right_method;
        state->multiply_compare_reflected_first =
            Py_TYPE(state->multiply_left_reflected) !=
                Py_TYPE(state->multiply_right_reflected) &&
            PyType_IsSubtype(
                Py_TYPE(state->multiply_right_reflected),
                Py_TYPE(state->multiply_left_reflected)
            ) &&
            Py_TYPE(state->multiply_right_reflected)->tp_richcompare !=
                Py_TYPE(state->multiply_left_reflected)->tp_richcompare;
        return numeric_iterator_manual_compare_reflected(state, NULL, 0);
    }
    Py_XDECREF(left_method);
    Py_XDECREF(right_method);
    state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE;
    PyObject *comparison = PyObject_RichCompare(
        state->multiply_left_reflected,
        state->multiply_right_reflected,
        Py_NE
    );
    if (comparison == NULL) {
        return NULL;
    }
    PyObject *result = numeric_iterator_finish_overload_comparison(
        state,
        comparison
    );
    Py_DECREF(comparison);
    return result;
}

static PyObject *
numeric_iterator_get_left_reflected(NumericIteratorState *state)
{
    state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_LEFT;
    PyObject *method = PyObject_GetAttrString(
        (PyObject *)Py_TYPE(state->result),
        "__rmul__"
    );
    if (method == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            return NULL;
        }
        PyErr_Clear();
        return numeric_iterator_finish_overload_check(state, 1);
    }
    state->multiply_left_reflected = method;
    return numeric_iterator_compare_reflected_methods(state);
}

static PyObject *
numeric_iterator_start_overload_check(NumericIteratorState *state)
{
    if (!state->multiply_try_left || !state->multiply_try_right ||
        !PyType_IsSubtype(Py_TYPE(state->items[0]), Py_TYPE(state->result))) {
        return numeric_iterator_finish_overload_check(state, 0);
    }
    state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_RIGHT;
    PyObject *method = PyObject_GetAttrString(
        (PyObject *)Py_TYPE(state->items[0]),
        "__rmul__"
    );
    if (method == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            return NULL;
        }
        PyErr_Clear();
        return numeric_iterator_finish_overload_check(state, 0);
    }
    state->multiply_right_reflected = method;
    return numeric_iterator_get_left_reflected(state);
}

static PyObject *
numeric_iterator_resume_overload_check(
    NumericIteratorState *state,
    PyObject *value
)
{
    if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_RIGHT) {
        if (value == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
                return NULL;
            }
            PyErr_Clear();
            return numeric_iterator_finish_overload_check(state, 0);
        }
        state->multiply_right_reflected = Py_NewRef(value);
        return numeric_iterator_get_left_reflected(state);
    }
    if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_LEFT) {
        if (value == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
                return NULL;
            }
            PyErr_Clear();
            return numeric_iterator_finish_overload_check(state, 1);
        }
        state->multiply_left_reflected = Py_NewRef(value);
        return numeric_iterator_compare_reflected_methods(state);
    }
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE) {
        return numeric_iterator_finish_overload_comparison(state, value);
    }
    if (state->phase ==
            NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT_BIND ||
        state->phase ==
            NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_LEFT ||
        state->phase ==
            NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT_BIND ||
        state->phase ==
            NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_COMPARE_RIGHT) {
        return numeric_iterator_manual_compare_reflected(state, value, 1);
    }
    if (state->phase ==
        NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH_BIND) {
        state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH;
        PyObject *result = PyObject_CallNoArgs(value);
        if (result == NULL) {
            return NULL;
        }
        PyObject *finished = numeric_iterator_finish_overload_truth(
            state,
            result
        );
        Py_DECREF(result);
        return finished;
    }
    if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_TRUTH) {
        return numeric_iterator_finish_overload_truth(state, value);
    }
    if (state->phase ==
        NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX_BIND) {
        state->phase = NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX;
        PyObject *result = PyObject_CallNoArgs(value);
        if (result == NULL) {
            return NULL;
        }
        PyObject *finished = numeric_iterator_finish_overload_length(
            state,
            result,
            1
        );
        Py_DECREF(result);
        return finished;
    }
    if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX) {
        return numeric_iterator_finish_overload_length(state, value, 1);
    }
    PyErr_SetString(
        PyExc_RuntimeError,
        "invalid numeric multiply overload state"
    );
    return NULL;
}

static PyObject *
numeric_iterator_unsupported_multiply(
    NumericIteratorState *state
)
{
    PyErr_Format(
        PyExc_TypeError,
        "unsupported operand type(s) for *: '%.100s' and '%.100s'",
        Py_TYPE(state->result)->tp_name,
        Py_TYPE(state->items[0])->tp_name
    );
    return NULL;
}

static PyObject *
numeric_iterator_call_current_binary_method(
    NumericIteratorState *state,
    int reflected
)
{
    PyObject *receiver = reflected ? state->items[0] : state->result;
    PyObject *argument = reflected ? state->result : state->items[0];
    PyObject *descriptor = lookup_raw_special(
        receiver,
        reflected ? "__rmul__" : "__mul__"
    );
    if (reflected) {
        Py_XSETREF(state->multiply_right_method, descriptor);
    }
    else {
        Py_XSETREF(state->multiply_left_method, descriptor);
    }
    if (descriptor == NULL) {
        state->phase = reflected
            ? NUMERIC_ITER_WAIT_MULTIPLY_RIGHT
            : NUMERIC_ITER_WAIT_MULTIPLY_LEFT;
        return Py_NewRef(Py_NotImplemented);
    }
    return numeric_iterator_call_binary_method(
        state,
        descriptor,
        receiver,
        argument,
        reflected
    );
}

static PyObject *
numeric_iterator_manual_multiply(
    NumericIteratorState *state,
    PyObject *completed,
    int is_resumed
)
{
    PyObject *result;
    if (is_resumed) {
        if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_LEFT_BIND) {
            state->phase = NUMERIC_ITER_WAIT_MULTIPLY_LEFT;
            result = PyObject_CallOneArg(completed, state->items[0]);
        }
        else if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_RIGHT_BIND) {
            state->phase = NUMERIC_ITER_WAIT_MULTIPLY_RIGHT;
            result = PyObject_CallOneArg(completed, state->result);
        }
        else {
            result = Py_NewRef(completed);
        }
    }
    else if (state->multiply_reflected_first) {
        result = numeric_iterator_call_current_binary_method(state, 1);
    }
    else if (state->multiply_try_left) {
        result = numeric_iterator_call_current_binary_method(state, 0);
    }
    else {
        result = numeric_iterator_call_current_binary_method(state, 1);
    }
    if (result == NULL) {
        return NULL;
    }
    if (result != Py_NotImplemented) {
        numeric_iterator_clear_multiply_methods(state);
        return result;
    }
    Py_DECREF(result);

    if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_RIGHT &&
        state->multiply_reflected_first &&
        state->multiply_try_left) {
        result = numeric_iterator_call_current_binary_method(state, 0);
    }
    else if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY_LEFT &&
             !state->multiply_reflected_first &&
             state->multiply_try_right) {
        result = numeric_iterator_call_current_binary_method(state, 1);
    }
    else {
        numeric_iterator_clear_multiply_methods(state);
        return numeric_iterator_unsupported_multiply(state);
    }
    if (result == NULL) {
        return NULL;
    }
    if (result == Py_NotImplemented) {
        Py_DECREF(result);
        numeric_iterator_clear_multiply_methods(state);
        return numeric_iterator_unsupported_multiply(state);
    }
    numeric_iterator_clear_multiply_methods(state);
    return result;
}

static PyObject *
numeric_iterator_multiply(
    NumericIteratorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase >=
                NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_RIGHT &&
            state->phase <= NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX) {
            return numeric_iterator_resume_overload_check(
                state,
                resumed_value
            );
        }
        if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY) {
            return Py_NewRef(resumed_value);
        }
        return numeric_iterator_manual_multiply(state, resumed_value, 1);
    }

    PyObject *left_method = lookup_raw_special(state->result, "__mul__");
    PyObject *right_method = Py_TYPE(state->result) == Py_TYPE(state->items[0])
        ? NULL
        : lookup_raw_special(state->items[0], "__rmul__");
    int left_is_python_slot = left_method != NULL &&
        !Py_IS_TYPE(left_method, &PyWrapperDescr_Type) &&
        !Py_IS_TYPE(left_method, &PyMethodDescr_Type);
    int right_is_python_slot = right_method != NULL &&
        !Py_IS_TYPE(right_method, &PyWrapperDescr_Type) &&
        !Py_IS_TYPE(right_method, &PyMethodDescr_Type);
    int manual = left_is_python_slot || right_is_python_slot;
    if (!manual) {
        Py_XDECREF(left_method);
        Py_XDECREF(right_method);
        state->phase = NUMERIC_ITER_WAIT_MULTIPLY;
        return PyNumber_Multiply(state->result, state->items[0]);
    }

    state->multiply_try_left = left_method != NULL ||
        (Py_TYPE(state->result)->tp_as_number != NULL &&
         Py_TYPE(state->result)->tp_as_number->nb_multiply != NULL);
    state->multiply_try_right =
        Py_TYPE(state->result) != Py_TYPE(state->items[0]) &&
        (right_method != NULL ||
         (Py_TYPE(state->items[0])->tp_as_number != NULL &&
          Py_TYPE(state->items[0])->tp_as_number->nb_multiply != NULL));
    state->multiply_left_method = left_method;
    state->multiply_right_method = right_method;
    return numeric_iterator_start_overload_check(state);
}

static PyObject *
numeric_iterator_prod_continue(
    NumericIteratorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase >=
                NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_RIGHT &&
            state->phase <= NUMERIC_ITER_WAIT_MULTIPLY_OVERLOAD_INDEX) {
            PyObject *product = numeric_iterator_multiply(
                state,
                resumed_value,
                1
            );
            if (product == NULL) {
                return NULL;
            }
            Py_SETREF(state->result, product);
            Py_CLEAR(state->items[0]);
        }
        else {
            if (resumed_value == NULL) {
                if (state->phase == NUMERIC_ITER_WAIT_NEXT &&
                    PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    PyErr_Clear();
                    return Py_NewRef(state->result);
                }
                return NULL;
            }
            if (state->phase == NUMERIC_ITER_WAIT_ITER) {
                if (!PyIter_Check(resumed_value)) {
                    PyErr_Format(
                        PyExc_TypeError,
                        "iter() returned non-iterator of type '%.200s'",
                        Py_TYPE(resumed_value)->tp_name
                    );
                    return NULL;
                }
                state->iterators[0] = Py_NewRef(resumed_value);
            }
            else if (state->phase == NUMERIC_ITER_WAIT_NEXT) {
                state->items[0] = Py_NewRef(resumed_value);
            }
            else if (state->phase == NUMERIC_ITER_WAIT_MULTIPLY ||
                     state->phase == NUMERIC_ITER_WAIT_MULTIPLY_LEFT_BIND ||
                     state->phase == NUMERIC_ITER_WAIT_MULTIPLY_LEFT ||
                     state->phase == NUMERIC_ITER_WAIT_MULTIPLY_RIGHT_BIND ||
                     state->phase == NUMERIC_ITER_WAIT_MULTIPLY_RIGHT) {
                PyObject *product = numeric_iterator_multiply(
                    state,
                    resumed_value,
                    1
                );
                if (product == NULL) {
                    return NULL;
                }
                Py_SETREF(state->result, product);
                Py_CLEAR(state->items[0]);
            }
            else {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "invalid numeric iterator state"
                );
                return NULL;
            }
        }
    }
    if (state->iterators[0] == NULL &&
        numeric_iterator_get_iterator(state, 0) == NULL) {
        return NULL;
    }
    for (;;) {
        state->phase = NUMERIC_ITER_WAIT_NEXT;
        PyObject *item = state->items[0];
        if (item != NULL) {
            state->items[0] = NULL;
        }
        else {
            item = PyIter_Next(state->iterators[0]);
        }
        if (item == NULL) {
            if (PyErr_Occurred()) {
                return NULL;
            }
            return Py_NewRef(state->result);
        }
        state->items[0] = item;
        PyObject *product = numeric_iterator_multiply(state, NULL, 0);
        if (product == NULL) {
            return NULL;
        }
        Py_SETREF(state->result, product);
        Py_CLEAR(state->items[0]);
    }
}

static int
numeric_iterator_sumprod_flush(
    NumericIteratorState *state,
    int float_path,
    int after_pair
)
{
    state->flush_after_pair = after_pair;
    if (float_path) {
        state->term = PyFloat_FromDouble(
            numeric_iterator_tl_to_double(state->flt_total)
        );
        if (state->term == NULL) {
            return -1;
        }
        state->phase = NUMERIC_ITER_WAIT_FLOAT_FLUSH;
    }
    else {
        state->term = PyLong_FromLong(state->int_total);
        if (state->term == NULL) {
            return -1;
        }
        state->phase = NUMERIC_ITER_WAIT_INT_FLUSH;
    }
    PyObject *new_total = PyNumber_Add(state->result, state->term);
    if (new_total == NULL) {
        return -1;
    }
    Py_SETREF(state->result, new_total);
    Py_CLEAR(state->term);
    if (float_path) {
        state->flt_total = (NumericIteratorTripleLength){0.0, 0.0, 0.0};
        state->flt_total_in_use = 0;
    }
    else {
        state->int_total = 0;
        state->int_total_in_use = 0;
    }
    return 0;
}

static int
numeric_iterator_sumprod_finalize_int(NumericIteratorState *state, int after_pair)
{
    state->int_path_enabled = 0;
    if (!state->int_total_in_use) {
        return 0;
    }
    return numeric_iterator_sumprod_flush(state, 0, after_pair);
}

static int
numeric_iterator_sumprod_finalize_float(NumericIteratorState *state, int after_pair)
{
    state->flt_path_enabled = 0;
    if (!state->flt_total_in_use) {
        return 0;
    }
    return numeric_iterator_sumprod_flush(state, 1, after_pair);
}

static bool
numeric_iterator_sumprod_float_pair(
    PyObject *p,
    PyObject *q,
    double *flt_p,
    double *flt_q
)
{
    bool p_float = PyFloat_CheckExact(p);
    bool q_float = PyFloat_CheckExact(q);
    if (p_float && q_float) {
        *flt_p = PyFloat_AS_DOUBLE(p);
        *flt_q = PyFloat_AS_DOUBLE(q);
        return true;
    }
    if (p_float && (PyLong_CheckExact(q) || PyBool_Check(q))) {
        *flt_p = PyFloat_AS_DOUBLE(p);
        *flt_q = PyLong_AsDouble(q);
        if (*flt_q == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
        return true;
    }
    if (q_float && (PyLong_CheckExact(p) || PyBool_Check(p))) {
        *flt_q = PyFloat_AS_DOUBLE(q);
        *flt_p = PyLong_AsDouble(p);
        if (*flt_p == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
        return true;
    }
    return false;
}

static PyObject *
numeric_iterator_sumprod_continue(
    NumericIteratorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            if (state->phase == NUMERIC_ITER_WAIT_NEXT &&
                PyErr_ExceptionMatches(PyExc_StopIteration)) {
                PyErr_Clear();
                state->stopped[state->iterator_index] = 1;
                state->iterator_index++;
            }
            else {
                return NULL;
            }
        }
        else switch (state->phase) {
            case NUMERIC_ITER_WAIT_ITER:
                if (!PyIter_Check(resumed_value)) {
                    PyErr_Format(
                        PyExc_TypeError,
                        "iter() returned non-iterator of type '%.200s'",
                        Py_TYPE(resumed_value)->tp_name
                    );
                    return NULL;
                }
                state->iterators[state->iterator_index] = Py_NewRef(resumed_value);
                break;
            case NUMERIC_ITER_WAIT_NEXT:
                state->items[state->iterator_index] = Py_NewRef(resumed_value);
                state->iterator_index++;
                break;
            case NUMERIC_ITER_WAIT_MULTIPLY:
                state->term = Py_NewRef(resumed_value);
                state->phase = NUMERIC_ITER_WAIT_ADD;
                {
                    PyObject *new_total = PyNumber_Add(state->result, state->term);
                    if (new_total == NULL) {
                        return NULL;
                    }
                    Py_SETREF(state->result, new_total);
                    Py_CLEAR(state->term);
                    Py_CLEAR(state->items[0]);
                    Py_CLEAR(state->items[1]);
                    state->iterator_index = 0;
                    state->stopped[0] = 0;
                    state->stopped[1] = 0;
                }
                break;
            case NUMERIC_ITER_WAIT_ADD:
                Py_SETREF(state->result, Py_NewRef(resumed_value));
                Py_CLEAR(state->term);
                Py_CLEAR(state->items[0]);
                Py_CLEAR(state->items[1]);
                state->iterator_index = 0;
                state->stopped[0] = 0;
                state->stopped[1] = 0;
                break;
            case NUMERIC_ITER_WAIT_INT_FLUSH:
                Py_SETREF(state->result, Py_NewRef(resumed_value));
                Py_CLEAR(state->term);
                state->int_total = 0;
                state->int_total_in_use = 0;
                break;
            case NUMERIC_ITER_WAIT_FLOAT_FLUSH:
                Py_SETREF(state->result, Py_NewRef(resumed_value));
                Py_CLEAR(state->term);
                state->flt_total = (NumericIteratorTripleLength){0.0, 0.0, 0.0};
                state->flt_total_in_use = 0;
                break;
            default:
                PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator state");
                return NULL;
        }
    }

    for (;;) {
        for (int index = 0; index < 2; index++) {
            if (state->iterators[index] == NULL &&
                numeric_iterator_get_iterator(state, index) == NULL) {
                return NULL;
            }
        }
        if (state->phase == NUMERIC_ITER_WAIT_ITER) {
            state->iterator_index = 0;
        }
        while (state->iterator_index < 2) {
            int index = state->iterator_index;
            state->phase = NUMERIC_ITER_WAIT_NEXT;
            PyObject *item = PyIter_Next(state->iterators[index]);
            if (item == NULL) {
                if (PyErr_Occurred()) {
                    return NULL;
                }
                state->stopped[index] = 1;
                state->iterator_index++;
                continue;
            }
            state->items[index] = item;
            state->iterator_index++;
        }

        if (state->stopped[0] != state->stopped[1]) {
            PyErr_SetString(PyExc_ValueError, "Inputs are not the same length");
            return NULL;
        }
        bool finished = state->stopped[0] && state->stopped[1];

        if (state->int_path_enabled) {
            if (!finished &&
                PyLong_CheckExact(state->items[0]) &&
                PyLong_CheckExact(state->items[1])) {
                int overflow;
                long p = PyLong_AsLongAndOverflow(state->items[0], &overflow);
                if (!overflow) {
                    long q = PyLong_AsLongAndOverflow(state->items[1], &overflow);
                    if (!overflow &&
                        !numeric_iterator_check_long_mult_overflow(p, q)) {
                        long product = p * q;
                        if (!numeric_iterator_long_add_would_overflow(
                                state->int_total, product
                            )) {
                            state->int_total += product;
                            state->int_total_in_use = 1;
                            Py_CLEAR(state->items[0]);
                            Py_CLEAR(state->items[1]);
                            state->iterator_index = 0;
                            state->stopped[0] = 0;
                            state->stopped[1] = 0;
                            continue;
                        }
                    }
                }
            }
            if (numeric_iterator_sumprod_finalize_int(state, 1) < 0) {
                return NULL;
            }
            if (state->phase == NUMERIC_ITER_WAIT_INT_FLUSH) {
                continue;
            }
        }

        if (state->flt_path_enabled) {
            if (!finished) {
                double p, q;
                if (numeric_iterator_sumprod_float_pair(
                        state->items[0], state->items[1], &p, &q
                    )) {
                    NumericIteratorTripleLength total =
                        numeric_iterator_tl_fma(p, q, state->flt_total);
                    if (isfinite(total.hi)) {
                        state->flt_total = total;
                        state->flt_total_in_use = 1;
                        Py_CLEAR(state->items[0]);
                        Py_CLEAR(state->items[1]);
                        state->iterator_index = 0;
                        state->stopped[0] = 0;
                        state->stopped[1] = 0;
                        continue;
                    }
                }
            }
            if (numeric_iterator_sumprod_finalize_float(state, 1) < 0) {
                return NULL;
            }
            if (state->phase == NUMERIC_ITER_WAIT_FLOAT_FLUSH) {
                continue;
            }
        }

        if (finished) {
            return Py_NewRef(state->result);
        }
        state->phase = NUMERIC_ITER_WAIT_MULTIPLY;
        PyObject *product = PyNumber_Multiply(
            state->items[0], state->items[1]
        );
        if (product == NULL) {
            return NULL;
        }
        state->term = product;
        state->phase = NUMERIC_ITER_WAIT_ADD;
        PyObject *new_total = PyNumber_Add(state->result, state->term);
        if (new_total == NULL) {
            return NULL;
        }
        Py_SETREF(state->result, new_total);
        Py_CLEAR(state->term);
        Py_CLEAR(state->items[0]);
        Py_CLEAR(state->items[1]);
        state->iterator_index = 0;
        state->stopped[0] = 0;
        state->stopped[1] = 0;
    }
}

static PyObject *
numeric_iterator_resume(const void *raw_state, PyObject *value)
{
    NumericIteratorState *state = numeric_iterator_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &numeric_iterator_vtable, state) < 0) {
        numeric_iterator_free_state(state);
        return NULL;
    }
    PyObject *result;
    switch (state->operation) {
        case NUMERIC_ITER_FSUM:
            result = numeric_iterator_fsum_continue(state, value, 1);
            break;
        case NUMERIC_ITER_DIST:
            result = numeric_iterator_dist_continue(state, value, 1);
            break;
        case NUMERIC_ITER_PROD:
            result = numeric_iterator_prod_continue(state, value, 1);
            break;
        case NUMERIC_ITER_SUMPROD:
            result = numeric_iterator_sumprod_continue(state, value, 1);
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator operation");
            result = NULL;
            break;
    }
    adapter_leave(&frame);
    numeric_iterator_free_state(state);
    return result;
}

static PyObject *
numeric_iterator_function(PyObject *tag, PyObject *args, PyObject *kwargs)
{
    if (!PyTuple_Check(tag) || PyTuple_GET_SIZE(tag) != 2) {
        PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator adapter state");
        return NULL;
    }
    long operation = PyLong_AsLong(PyTuple_GET_ITEM(tag, 1));
    if (operation < 0 && PyErr_Occurred()) {
        return NULL;
    }
    if (operation < 0 || operation > NUMERIC_ITER_SUMPROD) {
        PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator operation");
        return NULL;
    }
    NumericIteratorOperation op = (NumericIteratorOperation)operation;
    int expected = numeric_iterator_count(op);
    if (PyTuple_GET_SIZE(args) != expected) {
        return PyObject_Call(PyTuple_GET_ITEM(tag, 0), args, kwargs);
    }

    PyObject *start = NULL;
    if (op == NUMERIC_ITER_PROD) {
        if (kwargs != NULL && PyDict_Size(kwargs) != 0) {
            PyObject *key, *value;
            Py_ssize_t position = 0;
            while (PyDict_Next(kwargs, &position, &key, &value)) {
                if (!PyUnicode_Check(key) ||
                    PyUnicode_CompareWithASCIIString(key, "start") != 0) {
                    return PyObject_Call(PyTuple_GET_ITEM(tag, 0), args, kwargs);
                }
                start = Py_NewRef(value);
            }
        }
        if (start == NULL) {
            start = PyLong_FromLong(1);
            if (start == NULL) {
                return NULL;
            }
        }
    }
    else if (kwargs != NULL && PyDict_Size(kwargs) != 0) {
        return PyObject_Call(PyTuple_GET_ITEM(tag, 0), args, kwargs);
    }

    NumericIteratorState state = {
        .args = Py_NewRef(args),
        .kwargs = Py_XNewRef(kwargs),
        .start = start,
        .operation = op,
        .phase = NUMERIC_ITER_WAIT_ITER,
        .iterator_index = 0,
        .int_path_enabled = 1,
        .flt_path_enabled = 1,
        .partial_capacity = NUMERIC_ITER_FSUM_PARTIALS,
        .dist_side = 0,
    };
    if (op == NUMERIC_ITER_FSUM || op == NUMERIC_ITER_DIST) {
        state.partials = PyMem_Malloc(
            (size_t)state.partial_capacity * sizeof(double)
        );
        if (state.partials == NULL) {
            PyErr_NoMemory();
            numeric_iterator_clear_state(&state);
            return NULL;
        }
    }
    if (op == NUMERIC_ITER_DIST) {
        state.values[0] = PyList_New(0);
        state.values[1] = PyList_New(0);
        if (state.values[0] == NULL || state.values[1] == NULL) {
            numeric_iterator_clear_state(&state);
            return NULL;
        }
    }
    if (op == NUMERIC_ITER_PROD) {
        state.result = Py_NewRef(start);
    }
    else if (op == NUMERIC_ITER_SUMPROD) {
        state.result = PyLong_FromLong(0);
        if (state.result == NULL) {
            numeric_iterator_clear_state(&state);
            return NULL;
        }
    }

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &numeric_iterator_vtable, &state) < 0) {
        numeric_iterator_clear_state(&state);
        return NULL;
    }
    PyObject *result;
    switch (op) {
        case NUMERIC_ITER_FSUM:
            result = numeric_iterator_fsum_continue(&state, NULL, 0);
            break;
        case NUMERIC_ITER_DIST:
            result = numeric_iterator_dist_continue(&state, NULL, 0);
            break;
        case NUMERIC_ITER_PROD:
            result = numeric_iterator_prod_continue(&state, NULL, 0);
            break;
        case NUMERIC_ITER_SUMPROD:
            result = numeric_iterator_sumprod_continue(&state, NULL, 0);
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "invalid numeric iterator operation");
            result = NULL;
            break;
    }
    adapter_leave(&frame);
    numeric_iterator_clear_state(&state);
    return result;
}

static int
numeric_iterator_replace(
    PyObject *module,
    const char *name,
    NumericIteratorOperation operation
)
{
    if (numeric_iterator_installation_count >= NUMERIC_ITER_INSTALLATION_MAX) {
        PyErr_SetString(PyExc_RuntimeError, "too many numeric iterator adapters");
        return -1;
    }
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        PyErr_Clear();
        return 0;
    }
    PyObject *kind = PyLong_FromLong((long)operation);
    PyObject *tag = kind == NULL ? NULL : PyTuple_Pack(2, original, kind);
    Py_XDECREF(kind);
    if (tag == NULL) {
        Py_DECREF(original);
        return -1;
    }
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    if (module_name == NULL) {
        Py_DECREF(tag);
        Py_DECREF(original);
        return -1;
    }
    PyMethodDef *method = &numeric_iterator_methods[numeric_iterator_installation_count];
    *method = (PyMethodDef){
        .ml_name = name,
        .ml_meth = (PyCFunction)(void(*)(void))numeric_iterator_function,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = PyCFunction_Check(original)
            ? ((PyCFunctionObject *)original)->m_ml->ml_doc
            : NULL,
    };
    PyObject *replacement = PyCFunction_NewEx(method, tag, module_name);
    Py_DECREF(module_name);
    Py_DECREF(tag);
    if (replacement == NULL) {
        Py_DECREF(original);
        return -1;
    }
    if (PyObject_SetAttrString(module, name, replacement) < 0) {
        Py_DECREF(replacement);
        Py_DECREF(original);
        return -1;
    }
    NumericIteratorInstallation *installation =
        &numeric_iterator_installations[numeric_iterator_installation_count++];
    installation->module = Py_NewRef(module);
    installation->original = original;
    installation->name = name;
    Py_DECREF(replacement);
    return 0;
}

int
adapter_numeric_iterators_install(PyObject *math_module)
{
    if (numeric_iterator_installation_count != 0) {
        return 0;
    }
    static const struct {
        const char *name;
        NumericIteratorOperation operation;
    } functions[] = {
        {"fsum", NUMERIC_ITER_FSUM},
        {"dist", NUMERIC_ITER_DIST},
        {"prod", NUMERIC_ITER_PROD},
#if PY_VERSION_HEX >= 0x030c0000
        {"sumprod", NUMERIC_ITER_SUMPROD},
#endif
    };
    Py_ssize_t count = (Py_ssize_t)(sizeof(functions) / sizeof(*functions));
    for (Py_ssize_t index = 0; index < count; index++) {
        if (numeric_iterator_replace(
                math_module,
                functions[index].name,
                functions[index].operation
            ) < 0) {
            adapter_numeric_iterators_rollback();
            return -1;
        }
    }
    return 0;
}

void
adapter_numeric_iterators_rollback(void)
{
    while (numeric_iterator_installation_count > 0) {
        NumericIteratorInstallation *installation =
            &numeric_iterator_installations[--numeric_iterator_installation_count];
        if (PyObject_SetAttrString(
                installation->module,
                installation->name,
                installation->original
            ) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(installation->module);
        Py_DECREF(installation->original);
        installation->module = NULL;
        installation->original = NULL;
    }
}
