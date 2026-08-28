#include "sort_engine.h"

#include <stdbool.h>
#include <string.h>

#define ALEFF_MIN_GALLOP 7
#define ALEFF_MAX_MINRUN 64
#define ALEFF_MAX_PENDING ((int)(sizeof(Py_ssize_t) * 8))

typedef struct {
    Py_ssize_t base;
    Py_ssize_t len;
    int power;
} PendingRun;

typedef enum {
    ENGINE_KEY_NEXT,
    ENGINE_PREPARE,
    ENGINE_RUN_START,

    ENGINE_RUN312_FIRST,
    ENGINE_RUN312_SCAN,

    ENGINE_RUN313_ASC,
    ENGINE_RUN313_PREFIX,
    ENGINE_RUN313_DESC_SMALLER,
    ENGINE_RUN313_DESC_LARGER,
    ENGINE_RUN313_SUFFIX,

    ENGINE_RUN_READY,
    ENGINE_BINARY_START,
    ENGINE_BINARY_PROBE,
    ENGINE_BINARY_INSERT,
    ENGINE_RUN_COLLAPSE,
    ENGINE_RUN_AFTER_COLLAPSE,
    ENGINE_RUN_PUSH,
    ENGINE_FORCE_COLLAPSE,
    ENGINE_FORCE_AFTER_MERGE,

    ENGINE_MERGE_AT_START,
    ENGINE_MERGE_AT_LEFT,
    ENGINE_MERGE_AT_RIGHT,

    ENGINE_GALLOP_INITIAL,
    ENGINE_GALLOP_EXPONENTIAL,
    ENGINE_GALLOP_BINARY,

    ENGINE_MERGE_LO_START,
    ENGINE_MERGE_LO_STRAIGHT,
    ENGINE_MERGE_LO_GALLOP_A,
    ENGINE_MERGE_LO_GALLOP_B,

    ENGINE_MERGE_HI_START,
    ENGINE_MERGE_HI_STRAIGHT,
    ENGINE_MERGE_HI_GALLOP_A,
    ENGINE_MERGE_HI_GALLOP_B,

    ENGINE_FINISH,
    ENGINE_DONE,
    ENGINE_FAILED,
} EnginePhase;

typedef enum {
    GALLOP_LEFT,
    GALLOP_RIGHT,
} GallopKind;

typedef enum {
    GALLOP_SOURCE_MAIN,
    GALLOP_SOURCE_TEMP,
} GallopSource;

typedef enum {
    GALLOP_DIRECTION_LEFT,
    GALLOP_DIRECTION_RIGHT,
} GallopDirection;

typedef enum {
    MERGE_NONE,
    MERGE_LO,
    MERGE_HI,
} MergeKind;

struct AleffSortEngine {
    PyObject **items;
    PyObject **keys;
    Py_ssize_t *order;
    Py_ssize_t *temp;
    Py_ssize_t size;
    Py_ssize_t key_count;
    Py_ssize_t temp_alloced;

    int has_key_function;
    int reverse;
    int pre_reversed;
    int sort_started;
    int cleaned_keys;
    int aborted;

    EnginePhase phase;
    AleffSortRequestKind awaiting;
    Py_ssize_t request_left;
    Py_ssize_t request_right;
    int compare_result;

    Py_ssize_t minrun;
    Py_ssize_t min_gallop;
    Py_ssize_t run_base;
    Py_ssize_t nremaining;
    Py_ssize_t run_n;
    Py_ssize_t run_neq;
    int run_descending;

    Py_ssize_t binary_n;
    Py_ssize_t binary_ok;
    Py_ssize_t binary_l;
    Py_ssize_t binary_r;
    Py_ssize_t binary_m;
    Py_ssize_t binary_pivot;

    PendingRun pending[ALEFF_MAX_PENDING];
    int pending_count;
    int collapse_power;

    int merge_at_index;
    EnginePhase merge_return;
    Py_ssize_t merge_a_base;
    Py_ssize_t merge_b_base;
    Py_ssize_t merge_na;
    Py_ssize_t merge_nb;

    GallopKind gallop_kind;
    GallopSource gallop_source;
    GallopDirection gallop_direction;
    EnginePhase gallop_return;
    Py_ssize_t gallop_key;
    Py_ssize_t gallop_base;
    Py_ssize_t gallop_n;
    Py_ssize_t gallop_hint;
    Py_ssize_t gallop_ofs;
    Py_ssize_t gallop_lastofs;
    Py_ssize_t gallop_maxofs;
    Py_ssize_t gallop_m;
    Py_ssize_t gallop_result;

    MergeKind merge_kind;
    Py_ssize_t merge_dest;
    Py_ssize_t merge_a;
    Py_ssize_t merge_b;
    Py_ssize_t merge_base_a;
    Py_ssize_t merge_local_min_gallop;
    Py_ssize_t merge_acount;
    Py_ssize_t merge_bcount;
};

static Py_ssize_t
compute_minrun(Py_ssize_t n)
{
    Py_ssize_t r = 0;
    while (n >= ALEFF_MAX_MINRUN) {
        r |= n & 1;
        n >>= 1;
    }
    return n + r;
}

static int
powerloop(Py_ssize_t s1, Py_ssize_t n1, Py_ssize_t n2, Py_ssize_t n)
{
    int result = 0;
    Py_ssize_t a = 2 * s1 + n1;
    Py_ssize_t b = a + n1 + n2;
    for (;;) {
        result++;
        if (a >= n) {
            a -= n;
            b -= n;
        }
        else if (b >= n) {
            break;
        }
        a <<= 1;
        b <<= 1;
    }
    return result;
}

static void
reverse_order(AleffSortEngine *engine, Py_ssize_t base, Py_ssize_t n)
{
    Py_ssize_t left = base;
    Py_ssize_t right = base + n - 1;
    while (left < right) {
        Py_ssize_t item = engine->order[left];
        engine->order[left++] = engine->order[right];
        engine->order[right--] = item;
    }
}

static PyObject *
key_for_id(const AleffSortEngine *engine, Py_ssize_t id)
{
    return engine->keys == NULL ? engine->items[id] : engine->keys[id];
}

static Py_ssize_t
source_id(const AleffSortEngine *engine, GallopSource source, Py_ssize_t index)
{
    return source == GALLOP_SOURCE_MAIN
        ? engine->order[index]
        : engine->temp[index];
}

static int
request_lt(
    AleffSortEngine *engine,
    Py_ssize_t left,
    Py_ssize_t right,
    EnginePhase after
)
{
    engine->request_left = left;
    engine->request_right = right;
    engine->phase = after;
    engine->awaiting = ALEFF_SORT_REQUEST_LT;
    return 1;
}

static int
ensure_temp(AleffSortEngine *engine, Py_ssize_t need)
{
    if (need <= engine->temp_alloced) {
        return 0;
    }
    if ((size_t)need > SIZE_MAX / sizeof(*engine->temp)) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t *replacement = PyMem_Realloc(
        engine->temp,
        (size_t)need * sizeof(*replacement)
    );
    if (replacement == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    engine->temp = replacement;
    engine->temp_alloced = need;
    return 0;
}

static void
cleanup_partial_keys(AleffSortEngine *engine)
{
    if (engine->keys == NULL || engine->cleaned_keys) {
        return;
    }
    while (engine->key_count > 0) {
        engine->key_count--;
        Py_CLEAR(engine->keys[engine->key_count]);
    }
    engine->cleaned_keys = 1;
}

static void
cleanup_sorted_keys(AleffSortEngine *engine)
{
    if (engine->keys == NULL || engine->cleaned_keys) {
        return;
    }
    for (Py_ssize_t i = 0; i < engine->size; i++) {
        Py_ssize_t id = engine->order[i];
        Py_CLEAR(engine->keys[id]);
    }
    engine->cleaned_keys = 1;
}

static void
reverse_last_equal(AleffSortEngine *engine)
{
    if (engine->run_neq != 0) {
        Py_ssize_t length = engine->run_neq + 1;
        reverse_order(
            engine,
            engine->run_base + engine->run_n - length,
            length
        );
        engine->run_neq = 0;
    }
}

static int
start_gallop(
    AleffSortEngine *engine,
    GallopKind kind,
    Py_ssize_t key,
    GallopSource source,
    Py_ssize_t base,
    Py_ssize_t n,
    Py_ssize_t hint,
    EnginePhase return_phase
)
{
    engine->gallop_kind = kind;
    engine->gallop_key = key;
    engine->gallop_source = source;
    engine->gallop_base = base;
    engine->gallop_n = n;
    engine->gallop_hint = hint;
    engine->gallop_lastofs = 0;
    engine->gallop_ofs = 1;
    engine->gallop_return = return_phase;

    Py_ssize_t hint_id = source_id(engine, source, base + hint);
    if (kind == GALLOP_LEFT) {
        return request_lt(engine, hint_id, key, ENGINE_GALLOP_INITIAL);
    }
    return request_lt(engine, key, hint_id, ENGINE_GALLOP_INITIAL);
}

static int
continue_gallop_exponential(AleffSortEngine *engine)
{
    if (engine->gallop_ofs >= engine->gallop_maxofs) {
        if (engine->gallop_ofs > engine->gallop_maxofs) {
            engine->gallop_ofs = engine->gallop_maxofs;
        }
        goto translate;
    }

    Py_ssize_t index = engine->gallop_direction == GALLOP_DIRECTION_RIGHT
        ? engine->gallop_base + engine->gallop_hint + engine->gallop_ofs
        : engine->gallop_base + engine->gallop_hint - engine->gallop_ofs;
    Py_ssize_t probe = source_id(engine, engine->gallop_source, index);

    if (engine->gallop_kind == GALLOP_LEFT) {
        return request_lt(
            engine,
            probe,
            engine->gallop_key,
            ENGINE_GALLOP_EXPONENTIAL
        );
    }
    return request_lt(
        engine,
        engine->gallop_key,
        probe,
        ENGINE_GALLOP_EXPONENTIAL
    );

translate:
    if (engine->gallop_direction == GALLOP_DIRECTION_RIGHT) {
        engine->gallop_lastofs += engine->gallop_hint;
        engine->gallop_ofs += engine->gallop_hint;
    }
    else {
        Py_ssize_t saved = engine->gallop_lastofs;
        engine->gallop_lastofs = engine->gallop_hint - engine->gallop_ofs;
        engine->gallop_ofs = engine->gallop_hint - saved;
    }
    engine->gallop_lastofs++;
    if (engine->gallop_lastofs >= engine->gallop_ofs) {
        engine->gallop_result = engine->gallop_ofs;
        engine->phase = engine->gallop_return;
        return 1;
    }
    engine->gallop_m = engine->gallop_lastofs +
        ((engine->gallop_ofs - engine->gallop_lastofs) >> 1);
    Py_ssize_t binary_probe = source_id(
        engine,
        engine->gallop_source,
        engine->gallop_base + engine->gallop_m
    );
    if (engine->gallop_kind == GALLOP_LEFT) {
        return request_lt(
            engine,
            binary_probe,
            engine->gallop_key,
            ENGINE_GALLOP_BINARY
        );
    }
    return request_lt(
        engine,
        engine->gallop_key,
        binary_probe,
        ENGINE_GALLOP_BINARY
    );
}

static void
merge_lo_succeed(AleffSortEngine *engine)
{
    if (engine->merge_na != 0) {
        memcpy(
            &engine->order[engine->merge_dest],
            &engine->temp[engine->merge_a],
            (size_t)engine->merge_na * sizeof(*engine->order)
        );
    }
    engine->merge_kind = MERGE_NONE;
    engine->phase = engine->merge_return;
}

static void
merge_lo_copy_b(AleffSortEngine *engine)
{
    memmove(
        &engine->order[engine->merge_dest],
        &engine->order[engine->merge_b],
        (size_t)engine->merge_nb * sizeof(*engine->order)
    );
    engine->order[engine->merge_dest + engine->merge_nb] =
        engine->temp[engine->merge_a];
    engine->merge_kind = MERGE_NONE;
    engine->phase = engine->merge_return;
}

static void
merge_hi_succeed(AleffSortEngine *engine)
{
    if (engine->merge_nb != 0) {
        memcpy(
            &engine->order[engine->merge_dest - (engine->merge_nb - 1)],
            engine->temp,
            (size_t)engine->merge_nb * sizeof(*engine->order)
        );
    }
    engine->merge_kind = MERGE_NONE;
    engine->phase = engine->merge_return;
}

static void
merge_hi_copy_a(AleffSortEngine *engine)
{
    memmove(
        &engine->order[engine->merge_dest + 1 - engine->merge_na],
        &engine->order[engine->merge_a + 1 - engine->merge_na],
        (size_t)engine->merge_na * sizeof(*engine->order)
    );
    engine->merge_dest -= engine->merge_na;
    engine->merge_a -= engine->merge_na;
    engine->order[engine->merge_dest] = engine->temp[engine->merge_b];
    engine->merge_kind = MERGE_NONE;
    engine->phase = engine->merge_return;
}

AleffSortEngine *
aleff_sort_engine_new(PyObject *items, int has_key_function, int reverse)
{
    if (!PyList_Check(items)) {
        PyErr_SetString(PyExc_TypeError, "sort engine requires a list");
        return NULL;
    }
    Py_ssize_t size = PyList_GET_SIZE(items);
    AleffSortEngine *engine = PyMem_Calloc(1, sizeof(*engine));
    if (engine == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    engine->size = size;
    engine->has_key_function = has_key_function;
    engine->reverse = reverse;
    engine->min_gallop = ALEFF_MIN_GALLOP;
    engine->phase = has_key_function ? ENGINE_KEY_NEXT : ENGINE_PREPARE;
    engine->awaiting = ALEFF_SORT_REQUEST_NONE;

    if (size != 0) {
        engine->items = PyMem_Calloc((size_t)size, sizeof(*engine->items));
        engine->order = PyMem_Malloc((size_t)size * sizeof(*engine->order));
        if (has_key_function) {
            engine->keys = PyMem_Calloc((size_t)size, sizeof(*engine->keys));
        }
        if (engine->items == NULL || engine->order == NULL ||
            (has_key_function && engine->keys == NULL)) {
            PyErr_NoMemory();
            aleff_sort_engine_free(engine);
            return NULL;
        }
        for (Py_ssize_t i = 0; i < size; i++) {
            engine->items[i] = Py_NewRef(PyList_GET_ITEM(items, i));
            engine->order[i] = i;
        }
    }
    return engine;
}

AleffSortEngine *
aleff_sort_engine_copy(const AleffSortEngine *source)
{
    AleffSortEngine *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->items = NULL;
    copy->keys = NULL;
    copy->order = NULL;
    copy->temp = NULL;

    if (source->size != 0) {
        copy->items = PyMem_Calloc((size_t)source->size, sizeof(*copy->items));
        copy->order = PyMem_Malloc(
            (size_t)source->size * sizeof(*copy->order)
        );
        if (source->keys != NULL) {
            copy->keys = PyMem_Calloc(
                (size_t)source->size,
                sizeof(*copy->keys)
            );
        }
        if (copy->items == NULL || copy->order == NULL ||
            (source->keys != NULL && copy->keys == NULL)) {
            PyErr_NoMemory();
            aleff_sort_engine_free(copy);
            return NULL;
        }
        memcpy(
            copy->order,
            source->order,
            (size_t)source->size * sizeof(*copy->order)
        );
        for (Py_ssize_t i = 0; i < source->size; i++) {
            copy->items[i] = Py_NewRef(source->items[i]);
            if (source->keys != NULL) {
                copy->keys[i] = Py_XNewRef(source->keys[i]);
            }
        }
    }
    if (source->temp_alloced != 0) {
        copy->temp = PyMem_Malloc(
            (size_t)source->temp_alloced * sizeof(*copy->temp)
        );
        if (copy->temp == NULL) {
            PyErr_NoMemory();
            aleff_sort_engine_free(copy);
            return NULL;
        }
        memcpy(
            copy->temp,
            source->temp,
            (size_t)source->temp_alloced * sizeof(*copy->temp)
        );
    }
    return copy;
}

void
aleff_sort_engine_free(AleffSortEngine *engine)
{
    if (engine == NULL) {
        return;
    }
    if (engine->items != NULL) {
        for (Py_ssize_t i = 0; i < engine->size; i++) {
            Py_XDECREF(engine->items[i]);
        }
    }
    if (engine->keys != NULL) {
        for (Py_ssize_t i = 0; i < engine->size; i++) {
            Py_XDECREF(engine->keys[i]);
        }
    }
    PyMem_Free(engine->items);
    PyMem_Free(engine->keys);
    PyMem_Free(engine->order);
    PyMem_Free(engine->temp);
    PyMem_Free(engine);
}

int
aleff_sort_engine_accept_key(AleffSortEngine *engine, PyObject *key)
{
    if (engine->awaiting != ALEFF_SORT_REQUEST_KEY || key == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "sort engine is not awaiting a key");
        return -1;
    }
    engine->keys[engine->key_count++] = Py_NewRef(key);
    engine->awaiting = ALEFF_SORT_REQUEST_NONE;
    engine->phase = ENGINE_KEY_NEXT;
    return 0;
}

int
aleff_sort_engine_accept_lt(AleffSortEngine *engine, int result)
{
    if (engine->awaiting != ALEFF_SORT_REQUEST_LT) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "sort engine is not awaiting a comparison"
        );
        return -1;
    }
    engine->compare_result = result != 0;
    engine->awaiting = ALEFF_SORT_REQUEST_NONE;
    return 0;
}

static int
sort_engine_step(AleffSortEngine *engine, AleffSortRequest *request)
{
    request->kind = ALEFF_SORT_REQUEST_NONE;
    request->left = NULL;
    request->right = NULL;
    if (engine->awaiting != ALEFF_SORT_REQUEST_NONE) {
        request->kind = engine->awaiting;
        if (engine->awaiting == ALEFF_SORT_REQUEST_KEY) {
            request->left = engine->items[engine->key_count];
        }
        else {
            request->left = key_for_id(engine, engine->request_left);
            request->right = key_for_id(engine, engine->request_right);
        }
        return 1;
    }

    for (;;) {
        switch (engine->phase) {
            case ENGINE_KEY_NEXT:
                if (engine->key_count == engine->size) {
                    engine->phase = ENGINE_PREPARE;
                    continue;
                }
                engine->awaiting = ALEFF_SORT_REQUEST_KEY;
                request->kind = ALEFF_SORT_REQUEST_KEY;
                request->left = engine->items[engine->key_count];
                return 1;

            case ENGINE_PREPARE:
                engine->nremaining = engine->size;
                engine->run_base = 0;
                if (engine->size < 2) {
                    engine->phase = ENGINE_FINISH;
                    continue;
                }
                engine->sort_started = 1;
                if (engine->reverse) {
                    reverse_order(engine, 0, engine->size);
                    engine->pre_reversed = 1;
                }
                engine->minrun = compute_minrun(engine->size);
                engine->phase = ENGINE_RUN_START;
                continue;

            case ENGINE_RUN_START:
                if (engine->nremaining == 0) {
                    engine->phase = ENGINE_FORCE_COLLAPSE;
                    continue;
                }
                engine->run_n = 1;
                engine->run_neq = 0;
                engine->run_descending = 0;
                if (engine->nremaining == 1) {
                    engine->phase = ENGINE_RUN_READY;
                    continue;
                }
#if PY_VERSION_HEX < 0x030d0000
                return request_lt(
                    engine,
                    engine->order[engine->run_base + 1],
                    engine->order[engine->run_base],
                    ENGINE_RUN312_FIRST
                );
#else
                return request_lt(
                    engine,
                    engine->order[engine->run_base + 1],
                    engine->order[engine->run_base],
                    ENGINE_RUN313_ASC
                );
#endif

            case ENGINE_RUN312_FIRST:
                engine->run_descending = engine->compare_result;
                engine->run_n = 2;
                if (engine->run_n >= engine->nremaining) {
                    if (engine->run_descending) {
                        reverse_order(engine, engine->run_base, engine->run_n);
                    }
                    engine->phase = ENGINE_RUN_READY;
                    continue;
                }
                return request_lt(
                    engine,
                    engine->order[engine->run_base + engine->run_n],
                    engine->order[engine->run_base + engine->run_n - 1],
                    ENGINE_RUN312_SCAN
                );

            case ENGINE_RUN312_SCAN:
                if (engine->run_descending
                        ? !engine->compare_result
                        : engine->compare_result) {
                    if (engine->run_descending) {
                        reverse_order(engine, engine->run_base, engine->run_n);
                    }
                    engine->phase = ENGINE_RUN_READY;
                    continue;
                }
                engine->run_n++;
                if (engine->run_n >= engine->nremaining) {
                    if (engine->run_descending) {
                        reverse_order(engine, engine->run_base, engine->run_n);
                    }
                    engine->phase = ENGINE_RUN_READY;
                    continue;
                }
                return request_lt(
                    engine,
                    engine->order[engine->run_base + engine->run_n],
                    engine->order[engine->run_base + engine->run_n - 1],
                    ENGINE_RUN312_SCAN
                );

            case ENGINE_RUN313_ASC:
                if (!engine->compare_result) {
                    engine->run_n++;
                    if (engine->run_n >= engine->nremaining) {
                        engine->phase = ENGINE_RUN_READY;
                        continue;
                    }
                    return request_lt(
                        engine,
                        engine->order[engine->run_base + engine->run_n],
                        engine->order[engine->run_base + engine->run_n - 1],
                        ENGINE_RUN313_ASC
                    );
                }
                if (engine->run_n > 1) {
                    return request_lt(
                        engine,
                        engine->order[engine->run_base],
                        engine->order[engine->run_base + engine->run_n - 1],
                        ENGINE_RUN313_PREFIX
                    );
                }
                engine->run_n++;
                engine->phase = ENGINE_RUN313_DESC_SMALLER;
                continue;

            case ENGINE_RUN313_PREFIX:
                if (engine->compare_result) {
                    engine->phase = ENGINE_RUN_READY;
                    continue;
                }
                reverse_order(engine, engine->run_base, engine->run_n);
                engine->run_n++;
                engine->phase = ENGINE_RUN313_DESC_SMALLER;
                continue;

            case ENGINE_RUN313_DESC_SMALLER:
                if (engine->run_n >= engine->nremaining) {
                    reverse_last_equal(engine);
                    reverse_order(engine, engine->run_base, engine->run_n);
                    engine->phase = ENGINE_RUN_READY;
                    continue;
                }
                return request_lt(
                    engine,
                    engine->order[engine->run_base + engine->run_n],
                    engine->order[engine->run_base + engine->run_n - 1],
                    ENGINE_RUN313_DESC_LARGER
                );

            case ENGINE_RUN313_DESC_LARGER:
                if (engine->compare_result) {
                    reverse_last_equal(engine);
                    engine->run_n++;
                    engine->phase = ENGINE_RUN313_DESC_SMALLER;
                    continue;
                }
                return request_lt(
                    engine,
                    engine->order[engine->run_base + engine->run_n - 1],
                    engine->order[engine->run_base + engine->run_n],
                    ENGINE_RUN313_SUFFIX
                );

            case ENGINE_RUN313_SUFFIX:
                /* This phase is shared by the second descending comparison
                 * and by the ascending suffix scan.  run_descending marks
                 * which result is being consumed. */
                if (!engine->run_descending) {
                    if (!engine->compare_result) {
                        engine->run_neq++;
                        engine->run_n++;
                        engine->phase = ENGINE_RUN313_DESC_SMALLER;
                        continue;
                    }
                    reverse_last_equal(engine);
                    reverse_order(engine, engine->run_base, engine->run_n);
                    engine->run_descending = 1;
                    if (engine->run_n >= engine->nremaining) {
                        engine->phase = ENGINE_RUN_READY;
                        continue;
                    }
                }
                else {
                    if (engine->compare_result) {
                        engine->phase = ENGINE_RUN_READY;
                        continue;
                    }
                    engine->run_n++;
                    if (engine->run_n >= engine->nremaining) {
                        engine->phase = ENGINE_RUN_READY;
                        continue;
                    }
                }
                return request_lt(
                    engine,
                    engine->order[engine->run_base + engine->run_n],
                    engine->order[engine->run_base + engine->run_n - 1],
                    ENGINE_RUN313_SUFFIX
                );

            case ENGINE_RUN_READY: {
                Py_ssize_t force = engine->nremaining <= engine->minrun
                    ? engine->nremaining
                    : engine->minrun;
                if (engine->run_n < force) {
                    engine->binary_n = force;
                    engine->binary_ok = engine->run_n;
                    engine->phase = ENGINE_BINARY_START;
                }
                else {
                    engine->phase = ENGINE_RUN_COLLAPSE;
                }
                continue;
            }

            case ENGINE_BINARY_START:
                if (engine->binary_ok == 0) {
                    engine->binary_ok = 1;
                }
                if (engine->binary_ok >= engine->binary_n) {
                    engine->run_n = engine->binary_n;
                    engine->phase = ENGINE_RUN_COLLAPSE;
                    continue;
                }
                engine->binary_pivot =
                    engine->order[engine->run_base + engine->binary_ok];
                engine->binary_l = 0;
                engine->binary_r = engine->binary_ok;
                engine->phase = ENGINE_BINARY_PROBE;
                continue;

            case ENGINE_BINARY_PROBE:
                if (engine->binary_l >= engine->binary_r) {
                    engine->phase = ENGINE_BINARY_INSERT;
                    continue;
                }
                engine->binary_m =
                    (engine->binary_l + engine->binary_r) >> 1;
                return request_lt(
                    engine,
                    engine->binary_pivot,
                    engine->order[engine->run_base + engine->binary_m],
                    ENGINE_BINARY_INSERT
                );

            case ENGINE_BINARY_INSERT:
                if (engine->binary_l < engine->binary_r) {
                    if (engine->compare_result) {
                        engine->binary_r = engine->binary_m;
                    }
                    else {
                        engine->binary_l = engine->binary_m + 1;
                    }
                    engine->phase = ENGINE_BINARY_PROBE;
                    continue;
                }
                memmove(
                    &engine->order[engine->run_base + engine->binary_l + 1],
                    &engine->order[engine->run_base + engine->binary_l],
                    (size_t)(engine->binary_ok - engine->binary_l) *
                        sizeof(*engine->order)
                );
                engine->order[engine->run_base + engine->binary_l] =
                    engine->binary_pivot;
                engine->binary_ok++;
                engine->phase = ENGINE_BINARY_START;
                continue;

            case ENGINE_RUN_COLLAPSE:
                if (engine->pending_count != 0) {
                    PendingRun *top = &engine->pending[engine->pending_count - 1];
                    engine->collapse_power = powerloop(
                        top->base,
                        top->len,
                        engine->run_n,
                        engine->size
                    );
                    if (engine->pending_count > 1 &&
                        engine->pending[engine->pending_count - 2].power >
                            engine->collapse_power) {
                        engine->merge_at_index = engine->pending_count - 2;
                        engine->merge_return = ENGINE_RUN_AFTER_COLLAPSE;
                        engine->phase = ENGINE_MERGE_AT_START;
                        continue;
                    }
                    top->power = engine->collapse_power;
                }
                engine->phase = ENGINE_RUN_PUSH;
                continue;

            case ENGINE_RUN_AFTER_COLLAPSE:
                engine->phase = ENGINE_RUN_COLLAPSE;
                continue;

            case ENGINE_RUN_PUSH:
                if (engine->pending_count >= ALEFF_MAX_PENDING) {
                    PyErr_SetString(PyExc_RuntimeError, "sort run stack overflow");
                    engine->phase = ENGINE_FAILED;
                    return -1;
                }
                engine->pending[engine->pending_count].base = engine->run_base;
                engine->pending[engine->pending_count].len = engine->run_n;
                engine->pending[engine->pending_count].power = 0;
                engine->pending_count++;
                engine->run_base += engine->run_n;
                engine->nremaining -= engine->run_n;
                engine->phase = ENGINE_RUN_START;
                continue;

            case ENGINE_FORCE_COLLAPSE:
                if (engine->pending_count <= 1) {
                    engine->phase = ENGINE_FINISH;
                    continue;
                }
                engine->merge_at_index = engine->pending_count - 2;
                if (engine->merge_at_index > 0 &&
                    engine->pending[engine->merge_at_index - 1].len <
                        engine->pending[engine->merge_at_index + 1].len) {
                    engine->merge_at_index--;
                }
                engine->merge_return = ENGINE_FORCE_AFTER_MERGE;
                engine->phase = ENGINE_MERGE_AT_START;
                continue;

            case ENGINE_FORCE_AFTER_MERGE:
                engine->phase = ENGINE_FORCE_COLLAPSE;
                continue;

            case ENGINE_MERGE_AT_START: {
                int i = engine->merge_at_index;
                engine->merge_a_base = engine->pending[i].base;
                engine->merge_na = engine->pending[i].len;
                engine->merge_b_base = engine->pending[i + 1].base;
                engine->merge_nb = engine->pending[i + 1].len;
                engine->pending[i].len = engine->merge_na + engine->merge_nb;
                if (i == engine->pending_count - 3) {
                    engine->pending[i + 1] = engine->pending[i + 2];
                }
                engine->pending_count--;
                return start_gallop(
                    engine,
                    GALLOP_RIGHT,
                    engine->order[engine->merge_b_base],
                    GALLOP_SOURCE_MAIN,
                    engine->merge_a_base,
                    engine->merge_na,
                    0,
                    ENGINE_MERGE_AT_LEFT
                );
            }

            case ENGINE_MERGE_AT_LEFT:
                engine->merge_a_base += engine->gallop_result;
                engine->merge_na -= engine->gallop_result;
                if (engine->merge_na == 0) {
                    engine->phase = engine->merge_return;
                    continue;
                }
                return start_gallop(
                    engine,
                    GALLOP_LEFT,
                    engine->order[engine->merge_a_base + engine->merge_na - 1],
                    GALLOP_SOURCE_MAIN,
                    engine->merge_b_base,
                    engine->merge_nb,
                    engine->merge_nb - 1,
                    ENGINE_MERGE_AT_RIGHT
                );

            case ENGINE_MERGE_AT_RIGHT:
                engine->merge_nb = engine->gallop_result;
                if (engine->merge_nb == 0) {
                    engine->phase = engine->merge_return;
                    continue;
                }
                engine->phase = engine->merge_na <= engine->merge_nb
                    ? ENGINE_MERGE_LO_START
                    : ENGINE_MERGE_HI_START;
                continue;

            case ENGINE_GALLOP_INITIAL:
                if (engine->gallop_kind == GALLOP_LEFT) {
                    engine->gallop_direction = engine->compare_result
                        ? GALLOP_DIRECTION_RIGHT
                        : GALLOP_DIRECTION_LEFT;
                }
                else {
                    engine->gallop_direction = engine->compare_result
                        ? GALLOP_DIRECTION_LEFT
                        : GALLOP_DIRECTION_RIGHT;
                }
                engine->gallop_maxofs =
                    engine->gallop_direction == GALLOP_DIRECTION_RIGHT
                    ? engine->gallop_n - engine->gallop_hint
                    : engine->gallop_hint + 1;
                return continue_gallop_exponential(engine);

            case ENGINE_GALLOP_EXPONENTIAL: {
                int keep_going;
                if (engine->gallop_kind == GALLOP_LEFT) {
                    keep_going = engine->gallop_direction == GALLOP_DIRECTION_RIGHT
                        ? engine->compare_result
                        : !engine->compare_result;
                }
                else {
                    keep_going = engine->gallop_direction == GALLOP_DIRECTION_LEFT
                        ? engine->compare_result
                        : !engine->compare_result;
                }
                if (keep_going) {
                    engine->gallop_lastofs = engine->gallop_ofs;
                    engine->gallop_ofs = (engine->gallop_ofs << 1) + 1;
                }
                else {
                    engine->gallop_ofs = engine->gallop_maxofs < engine->gallop_ofs
                        ? engine->gallop_maxofs
                        : engine->gallop_ofs;
                    goto gallop_translate;
                }
                return continue_gallop_exponential(engine);

gallop_translate:
                if (engine->gallop_direction == GALLOP_DIRECTION_RIGHT) {
                    engine->gallop_lastofs += engine->gallop_hint;
                    engine->gallop_ofs += engine->gallop_hint;
                }
                else {
                    Py_ssize_t saved = engine->gallop_lastofs;
                    engine->gallop_lastofs =
                        engine->gallop_hint - engine->gallop_ofs;
                    engine->gallop_ofs = engine->gallop_hint - saved;
                }
                engine->gallop_lastofs++;
                if (engine->gallop_lastofs >= engine->gallop_ofs) {
                    engine->gallop_result = engine->gallop_ofs;
                    engine->phase = engine->gallop_return;
                    continue;
                }
                engine->gallop_m = engine->gallop_lastofs +
                    ((engine->gallop_ofs - engine->gallop_lastofs) >> 1);
                Py_ssize_t probe = source_id(
                    engine,
                    engine->gallop_source,
                    engine->gallop_base + engine->gallop_m
                );
                if (engine->gallop_kind == GALLOP_LEFT) {
                    return request_lt(
                        engine,
                        probe,
                        engine->gallop_key,
                        ENGINE_GALLOP_BINARY
                    );
                }
                return request_lt(
                    engine,
                    engine->gallop_key,
                    probe,
                    ENGINE_GALLOP_BINARY
                );
            }

            case ENGINE_GALLOP_BINARY:
                if (engine->gallop_kind == GALLOP_LEFT) {
                    if (engine->compare_result) {
                        engine->gallop_lastofs = engine->gallop_m + 1;
                    }
                    else {
                        engine->gallop_ofs = engine->gallop_m;
                    }
                }
                else {
                    if (engine->compare_result) {
                        engine->gallop_ofs = engine->gallop_m;
                    }
                    else {
                        engine->gallop_lastofs = engine->gallop_m + 1;
                    }
                }
                if (engine->gallop_lastofs >= engine->gallop_ofs) {
                    engine->gallop_result = engine->gallop_ofs;
                    engine->phase = engine->gallop_return;
                    continue;
                }
                engine->gallop_m = engine->gallop_lastofs +
                    ((engine->gallop_ofs - engine->gallop_lastofs) >> 1);
                {
                    Py_ssize_t probe = source_id(
                        engine,
                        engine->gallop_source,
                        engine->gallop_base + engine->gallop_m
                    );
                    if (engine->gallop_kind == GALLOP_LEFT) {
                        return request_lt(
                            engine,
                            probe,
                            engine->gallop_key,
                            ENGINE_GALLOP_BINARY
                        );
                    }
                    return request_lt(
                        engine,
                        engine->gallop_key,
                        probe,
                        ENGINE_GALLOP_BINARY
                    );
                }

            case ENGINE_MERGE_LO_START:
                if (ensure_temp(engine, engine->merge_na) < 0) {
                    engine->phase = ENGINE_FAILED;
                    return -1;
                }
                memcpy(
                    engine->temp,
                    &engine->order[engine->merge_a_base],
                    (size_t)engine->merge_na * sizeof(*engine->temp)
                );
                engine->merge_kind = MERGE_LO;
                engine->merge_dest = engine->merge_a_base;
                engine->merge_a = 0;
                engine->merge_b = engine->merge_b_base;
                engine->order[engine->merge_dest++] =
                    engine->order[engine->merge_b++];
                engine->merge_nb--;
                if (engine->merge_nb == 0) {
                    merge_lo_succeed(engine);
                    continue;
                }
                if (engine->merge_na == 1) {
                    merge_lo_copy_b(engine);
                    continue;
                }
                engine->merge_local_min_gallop = engine->min_gallop;
                engine->merge_acount = 0;
                engine->merge_bcount = 0;
                return request_lt(
                    engine,
                    engine->order[engine->merge_b],
                    engine->temp[engine->merge_a],
                    ENGINE_MERGE_LO_STRAIGHT
                );

            case ENGINE_MERGE_LO_STRAIGHT:
                if (engine->compare_result) {
                    engine->order[engine->merge_dest++] =
                        engine->order[engine->merge_b++];
                    engine->merge_bcount++;
                    engine->merge_acount = 0;
                    engine->merge_nb--;
                    if (engine->merge_nb == 0) {
                        merge_lo_succeed(engine);
                        continue;
                    }
                    if (engine->merge_bcount >= engine->merge_local_min_gallop) {
                        engine->merge_local_min_gallop++;
                        engine->phase = ENGINE_MERGE_LO_GALLOP_A;
                        continue;
                    }
                }
                else {
                    engine->order[engine->merge_dest++] =
                        engine->temp[engine->merge_a++];
                    engine->merge_acount++;
                    engine->merge_bcount = 0;
                    engine->merge_na--;
                    if (engine->merge_na == 1) {
                        merge_lo_copy_b(engine);
                        continue;
                    }
                    if (engine->merge_acount >= engine->merge_local_min_gallop) {
                        engine->merge_local_min_gallop++;
                        engine->phase = ENGINE_MERGE_LO_GALLOP_A;
                        continue;
                    }
                }
                return request_lt(
                    engine,
                    engine->order[engine->merge_b],
                    engine->temp[engine->merge_a],
                    ENGINE_MERGE_LO_STRAIGHT
                );

            case ENGINE_MERGE_LO_GALLOP_A:
                if (engine->gallop_return == ENGINE_MERGE_LO_GALLOP_A &&
                    engine->phase == ENGINE_MERGE_LO_GALLOP_A &&
                    engine->gallop_n == engine->merge_na) {
                    Py_ssize_t k = engine->gallop_result;
                    engine->merge_acount = k;
                    if (k != 0) {
                        memcpy(
                            &engine->order[engine->merge_dest],
                            &engine->temp[engine->merge_a],
                            (size_t)k * sizeof(*engine->order)
                        );
                        engine->merge_dest += k;
                        engine->merge_a += k;
                        engine->merge_na -= k;
                        if (engine->merge_na == 1) {
                            merge_lo_copy_b(engine);
                            continue;
                        }
                        if (engine->merge_na == 0) {
                            merge_lo_succeed(engine);
                            continue;
                        }
                    }
                    engine->order[engine->merge_dest++] =
                        engine->order[engine->merge_b++];
                    engine->merge_nb--;
                    if (engine->merge_nb == 0) {
                        merge_lo_succeed(engine);
                        continue;
                    }
                    engine->phase = ENGINE_MERGE_LO_GALLOP_B;
                    continue;
                }
                engine->merge_local_min_gallop -=
                    engine->merge_local_min_gallop > 1;
                engine->min_gallop = engine->merge_local_min_gallop;
                return start_gallop(
                    engine,
                    GALLOP_RIGHT,
                    engine->order[engine->merge_b],
                    GALLOP_SOURCE_TEMP,
                    engine->merge_a,
                    engine->merge_na,
                    0,
                    ENGINE_MERGE_LO_GALLOP_A
                );

            case ENGINE_MERGE_LO_GALLOP_B:
                if (engine->gallop_return == ENGINE_MERGE_LO_GALLOP_B &&
                    engine->gallop_n == engine->merge_nb) {
                    Py_ssize_t k = engine->gallop_result;
                    engine->merge_bcount = k;
                    if (k != 0) {
                        memmove(
                            &engine->order[engine->merge_dest],
                            &engine->order[engine->merge_b],
                            (size_t)k * sizeof(*engine->order)
                        );
                        engine->merge_dest += k;
                        engine->merge_b += k;
                        engine->merge_nb -= k;
                        if (engine->merge_nb == 0) {
                            merge_lo_succeed(engine);
                            continue;
                        }
                    }
                    engine->order[engine->merge_dest++] =
                        engine->temp[engine->merge_a++];
                    engine->merge_na--;
                    if (engine->merge_na == 1) {
                        merge_lo_copy_b(engine);
                        continue;
                    }
                    if (engine->merge_acount >= ALEFF_MIN_GALLOP ||
                        engine->merge_bcount >= ALEFF_MIN_GALLOP) {
                        engine->phase = ENGINE_MERGE_LO_GALLOP_A;
                        engine->gallop_n = -1;
                        continue;
                    }
                    engine->merge_local_min_gallop++;
                    engine->min_gallop = engine->merge_local_min_gallop;
                    engine->merge_acount = 0;
                    engine->merge_bcount = 0;
                    return request_lt(
                        engine,
                        engine->order[engine->merge_b],
                        engine->temp[engine->merge_a],
                        ENGINE_MERGE_LO_STRAIGHT
                    );
                }
                return start_gallop(
                    engine,
                    GALLOP_LEFT,
                    engine->temp[engine->merge_a],
                    GALLOP_SOURCE_MAIN,
                    engine->merge_b,
                    engine->merge_nb,
                    0,
                    ENGINE_MERGE_LO_GALLOP_B
                );

            case ENGINE_MERGE_HI_START:
                if (ensure_temp(engine, engine->merge_nb) < 0) {
                    engine->phase = ENGINE_FAILED;
                    return -1;
                }
                memcpy(
                    engine->temp,
                    &engine->order[engine->merge_b_base],
                    (size_t)engine->merge_nb * sizeof(*engine->temp)
                );
                engine->merge_kind = MERGE_HI;
                engine->merge_base_a = engine->merge_a_base;
                engine->merge_dest = engine->merge_b_base + engine->merge_nb - 1;
                engine->merge_a = engine->merge_a_base + engine->merge_na - 1;
                engine->merge_b = engine->merge_nb - 1;
                engine->order[engine->merge_dest--] =
                    engine->order[engine->merge_a--];
                engine->merge_na--;
                if (engine->merge_na == 0) {
                    merge_hi_succeed(engine);
                    continue;
                }
                if (engine->merge_nb == 1) {
                    merge_hi_copy_a(engine);
                    continue;
                }
                engine->merge_local_min_gallop = engine->min_gallop;
                engine->merge_acount = 0;
                engine->merge_bcount = 0;
                return request_lt(
                    engine,
                    engine->temp[engine->merge_b],
                    engine->order[engine->merge_a],
                    ENGINE_MERGE_HI_STRAIGHT
                );

            case ENGINE_MERGE_HI_STRAIGHT:
                if (engine->compare_result) {
                    engine->order[engine->merge_dest--] =
                        engine->order[engine->merge_a--];
                    engine->merge_acount++;
                    engine->merge_bcount = 0;
                    engine->merge_na--;
                    if (engine->merge_na == 0) {
                        merge_hi_succeed(engine);
                        continue;
                    }
                    if (engine->merge_acount >= engine->merge_local_min_gallop) {
                        engine->merge_local_min_gallop++;
                        engine->phase = ENGINE_MERGE_HI_GALLOP_A;
                        continue;
                    }
                }
                else {
                    engine->order[engine->merge_dest--] =
                        engine->temp[engine->merge_b--];
                    engine->merge_bcount++;
                    engine->merge_acount = 0;
                    engine->merge_nb--;
                    if (engine->merge_nb == 1) {
                        merge_hi_copy_a(engine);
                        continue;
                    }
                    if (engine->merge_bcount >= engine->merge_local_min_gallop) {
                        engine->merge_local_min_gallop++;
                        engine->phase = ENGINE_MERGE_HI_GALLOP_A;
                        continue;
                    }
                }
                return request_lt(
                    engine,
                    engine->temp[engine->merge_b],
                    engine->order[engine->merge_a],
                    ENGINE_MERGE_HI_STRAIGHT
                );

            case ENGINE_MERGE_HI_GALLOP_A:
                if (engine->gallop_return == ENGINE_MERGE_HI_GALLOP_A &&
                    engine->gallop_n == engine->merge_na) {
                    Py_ssize_t k = engine->merge_na - engine->gallop_result;
                    engine->merge_acount = k;
                    if (k != 0) {
                        engine->merge_dest -= k;
                        engine->merge_a -= k;
                        memmove(
                            &engine->order[engine->merge_dest + 1],
                            &engine->order[engine->merge_a + 1],
                            (size_t)k * sizeof(*engine->order)
                        );
                        engine->merge_na -= k;
                        if (engine->merge_na == 0) {
                            merge_hi_succeed(engine);
                            continue;
                        }
                    }
                    engine->order[engine->merge_dest--] =
                        engine->temp[engine->merge_b--];
                    engine->merge_nb--;
                    if (engine->merge_nb == 1) {
                        merge_hi_copy_a(engine);
                        continue;
                    }
                    engine->phase = ENGINE_MERGE_HI_GALLOP_B;
                    continue;
                }
                engine->merge_local_min_gallop -=
                    engine->merge_local_min_gallop > 1;
                engine->min_gallop = engine->merge_local_min_gallop;
                return start_gallop(
                    engine,
                    GALLOP_RIGHT,
                    engine->temp[engine->merge_b],
                    GALLOP_SOURCE_MAIN,
                    engine->merge_base_a,
                    engine->merge_na,
                    engine->merge_na - 1,
                    ENGINE_MERGE_HI_GALLOP_A
                );

            case ENGINE_MERGE_HI_GALLOP_B:
                if (engine->gallop_return == ENGINE_MERGE_HI_GALLOP_B &&
                    engine->gallop_n == engine->merge_nb) {
                    Py_ssize_t k = engine->merge_nb - engine->gallop_result;
                    engine->merge_bcount = k;
                    if (k != 0) {
                        engine->merge_dest -= k;
                        engine->merge_b -= k;
                        memcpy(
                            &engine->order[engine->merge_dest + 1],
                            &engine->temp[engine->merge_b + 1],
                            (size_t)k * sizeof(*engine->order)
                        );
                        engine->merge_nb -= k;
                        if (engine->merge_nb == 1) {
                            merge_hi_copy_a(engine);
                            continue;
                        }
                        if (engine->merge_nb == 0) {
                            merge_hi_succeed(engine);
                            continue;
                        }
                    }
                    engine->order[engine->merge_dest--] =
                        engine->order[engine->merge_a--];
                    engine->merge_na--;
                    if (engine->merge_na == 0) {
                        merge_hi_succeed(engine);
                        continue;
                    }
                    if (engine->merge_acount >= ALEFF_MIN_GALLOP ||
                        engine->merge_bcount >= ALEFF_MIN_GALLOP) {
                        engine->phase = ENGINE_MERGE_HI_GALLOP_A;
                        engine->gallop_n = -1;
                        continue;
                    }
                    engine->merge_local_min_gallop++;
                    engine->min_gallop = engine->merge_local_min_gallop;
                    engine->merge_acount = 0;
                    engine->merge_bcount = 0;
                    return request_lt(
                        engine,
                        engine->temp[engine->merge_b],
                        engine->order[engine->merge_a],
                        ENGINE_MERGE_HI_STRAIGHT
                    );
                }
                return start_gallop(
                    engine,
                    GALLOP_LEFT,
                    engine->order[engine->merge_a],
                    GALLOP_SOURCE_TEMP,
                    0,
                    engine->merge_nb,
                    engine->merge_nb - 1,
                    ENGINE_MERGE_HI_GALLOP_B
                );

            case ENGINE_FINISH:
                if (engine->sort_started) {
                    cleanup_sorted_keys(engine);
                }
                else {
                    cleanup_partial_keys(engine);
                }
                if (engine->pre_reversed) {
                    reverse_order(engine, 0, engine->size);
                    engine->pre_reversed = 0;
                }
                engine->phase = ENGINE_DONE;
                return 0;

            case ENGINE_DONE:
                return 0;

            case ENGINE_FAILED:
                return -1;
        }
    }
}

int
aleff_sort_engine_advance(AleffSortEngine *engine, AleffSortRequest *request)
{
    for (;;) {
        int status = sort_engine_step(engine, request);
        if (status <= 0 || request->kind != ALEFF_SORT_REQUEST_NONE) {
            return status;
        }
    }
}

void
aleff_sort_engine_abort(AleffSortEngine *engine)
{
    if (engine == NULL || engine->aborted) {
        return;
    }
    engine->aborted = 1;
    if (engine->merge_kind == MERGE_LO) {
        if (engine->merge_na != 0) {
            memcpy(
                &engine->order[engine->merge_dest],
                &engine->temp[engine->merge_a],
                (size_t)engine->merge_na * sizeof(*engine->order)
            );
        }
        engine->merge_kind = MERGE_NONE;
    }
    else if (engine->merge_kind == MERGE_HI) {
        if (engine->merge_nb != 0) {
            memcpy(
                &engine->order[engine->merge_dest - (engine->merge_nb - 1)],
                engine->temp,
                (size_t)engine->merge_nb * sizeof(*engine->order)
            );
        }
        engine->merge_kind = MERGE_NONE;
    }
    if (engine->sort_started) {
        cleanup_sorted_keys(engine);
    }
    else {
        cleanup_partial_keys(engine);
    }
    if (engine->pre_reversed) {
        reverse_order(engine, 0, engine->size);
        engine->pre_reversed = 0;
    }
    engine->phase = ENGINE_FAILED;
}

PyObject *
aleff_sort_engine_materialize(const AleffSortEngine *engine)
{
    PyObject *result = PyList_New(engine->size);
    if (result == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < engine->size; i++) {
        PyList_SET_ITEM(
            result,
            i,
            Py_NewRef(engine->items[engine->order[i]])
        );
    }
    return result;
}
