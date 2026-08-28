#include <structmember.h>

typedef struct {
    PyObject *compare;
    PyObject *left;
    PyObject *right;
    int operation;
} FunctoolsCompareState;

static void *
functools_compare_copy_state(const void *raw_state)
{
    const FunctoolsCompareState *state = raw_state;
    FunctoolsCompareState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->compare = Py_NewRef(state->compare);
    copy->left = Py_NewRef(state->left);
    copy->right = Py_NewRef(state->right);
    return copy;
}

static void
functools_compare_free_state(void *raw_state)
{
    FunctoolsCompareState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->compare);
    Py_DECREF(state->left);
    Py_DECREF(state->right);
    PyMem_Free(state);
}

static PyObject *functools_compare_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable functools_compare_vtable = {
    .copy_state = functools_compare_copy_state,
    .free_state = functools_compare_free_state,
    .resume = functools_compare_resume,
};

static PyObject *
functools_compare_result(PyObject *value, int operation)
{
    PyObject *zero = PyLong_FromLong(0);
    if (zero == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_RichCompare(value, zero, operation);
    Py_DECREF(zero);
    return result;
}

static PyObject *
functools_compare_continue(
    FunctoolsCompareState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        return functools_compare_result(resumed_value, state->operation);
    }
    PyObject *comparison = PyObject_CallFunctionObjArgs(
        state->compare,
        state->left,
        state->right,
        NULL
    );
    if (comparison == NULL) {
        return NULL;
    }
    PyObject *result = functools_compare_result(comparison, state->operation);
    Py_DECREF(comparison);
    return result;
}

static PyObject *
functools_compare_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    FunctoolsCompareState *state = functools_compare_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &functools_compare_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = functools_compare_continue(state, value, 1);
    adapter_leave(&frame);
    functools_compare_free_state(state);
    return result;
}

typedef struct {
    PyObject_HEAD
    PyObject *compare;
} FunctoolsKeyFactory;

typedef struct {
    PyObject_HEAD
    PyObject *compare;
    PyObject *object;
} FunctoolsKeyObject;

static PyTypeObject FunctoolsKeyFactoryType;
static PyTypeObject FunctoolsKeyObjectType;

static void
functools_key_factory_dealloc(FunctoolsKeyFactory *self)
{
    Py_DECREF(self->compare);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
functools_key_factory_call(
    FunctoolsKeyFactory *self,
    PyObject *args,
    PyObject *kwargs
)
{
    if (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) {
        PyErr_SetString(PyExc_TypeError, "KeyWrapper() takes no keyword arguments");
        return NULL;
    }
    PyObject *object;
    if (!PyArg_ParseTuple(args, "O:KeyWrapper", &object)) {
        return NULL;
    }
    FunctoolsKeyObject *result = PyObject_New(
        FunctoolsKeyObject,
        &FunctoolsKeyObjectType
    );
    if (result == NULL) {
        return NULL;
    }
    result->compare = Py_NewRef(self->compare);
    result->object = Py_NewRef(object);
    return (PyObject *)result;
}

static PyTypeObject FunctoolsKeyFactoryType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "functools.KeyWrapperFactory",
    .tp_basicsize = sizeof(FunctoolsKeyFactory),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)functools_key_factory_dealloc,
    .tp_call = (ternaryfunc)functools_key_factory_call,
};

static void
functools_key_object_dealloc(FunctoolsKeyObject *self)
{
    Py_DECREF(self->compare);
    Py_DECREF(self->object);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
functools_key_object_richcompare(
    FunctoolsKeyObject *self,
    PyObject *other,
    int operation
)
{
    if (!PyObject_TypeCheck(other, &FunctoolsKeyObjectType)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    FunctoolsKeyObject *right = (FunctoolsKeyObject *)other;
    FunctoolsCompareState state = {
        .compare = self->compare,
        .left = self->object,
        .right = right->object,
        .operation = operation,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &functools_compare_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = functools_compare_continue(&state, NULL, 0);
    adapter_leave(&frame);
    return result;
}

static PyMemberDef functools_key_object_members[] = {
    {"obj", T_OBJECT, offsetof(FunctoolsKeyObject, object), READONLY, NULL},
    {NULL, 0, 0, 0, NULL}
};

static PyTypeObject FunctoolsKeyObjectType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "functools.KeyWrapper",
    .tp_basicsize = sizeof(FunctoolsKeyObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)functools_key_object_dealloc,
    .tp_richcompare = (richcmpfunc)functools_key_object_richcompare,
    .tp_members = functools_key_object_members,
    .tp_hash = PyObject_HashNotImplemented,
};

static PyObject *
adapter_cmp_to_key(PyObject *Py_UNUSED(self), PyObject *compare)
{
    if (!PyCallable_Check(compare)) {
        PyErr_SetString(PyExc_TypeError, "the comparison function must be callable");
        return NULL;
    }
    FunctoolsKeyFactory *factory = PyObject_New(
        FunctoolsKeyFactory,
        &FunctoolsKeyFactoryType
    );
    if (factory == NULL) {
        return NULL;
    }
    factory->compare = Py_NewRef(compare);
    return (PyObject *)factory;
}

/* The adapter keeps CPython's actual _lru_cache_wrapper as the cache owner.
 * These private layouts are identical in CPython 3.12, 3.13, and 3.14. */
typedef struct FunctoolsLruListElem {
    PyObject_HEAD
    struct FunctoolsLruListElem *prev;
    struct FunctoolsLruListElem *next;
    Py_hash_t hash;
    PyObject *key;
    PyObject *result;
} FunctoolsLruListElem;

typedef struct {
    FunctoolsLruListElem root;
    void *wrapper;
    int typed;
    PyObject *cache;
    Py_ssize_t hits;
    PyObject *func;
    Py_ssize_t maxsize;
    Py_ssize_t misses;
    PyObject *kwd_mark;
    PyTypeObject *lru_list_elem_type;
    PyObject *cache_info_type;
    PyObject *dict;
    PyObject *weakreflist;
} FunctoolsLruCacheObject;

typedef struct {
    PyObject *key;
    PyObject *value;
    Py_hash_t hash;
} FunctoolsCacheEntry;

typedef struct {
    FunctoolsCacheEntry *entries;
    Py_ssize_t count;
    Py_ssize_t hits;
    Py_ssize_t misses;
} FunctoolsCacheSnapshot;

typedef struct {
    PyObject *wrapper;
    PyObject *args;
    PyObject *kwargs;
    FunctoolsCacheSnapshot *snapshot;
} FunctoolsCacheState;

typedef struct {
    PyObject_HEAD
    PyObject *func;
    PyObject *wrapper_ref;
} FunctoolsCacheCallable;

static PyObject *functools_cache_resume(const void *raw_state, PyObject *value);

static void
functools_cache_snapshot_free(FunctoolsCacheSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        Py_DECREF(snapshot->entries[i].key);
        Py_DECREF(snapshot->entries[i].value);
    }
    PyMem_Free(snapshot->entries);
    PyMem_Free(snapshot);
}

static FunctoolsCacheSnapshot *
functools_cache_snapshot_copy(const FunctoolsCacheSnapshot *source)
{
    FunctoolsCacheSnapshot *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    if (source != NULL) {
        copy->hits = source->hits;
        copy->misses = source->misses;
    }
    if (source == NULL || source->count == 0) {
        return copy;
    }
    copy->entries = PyMem_Calloc(
        (size_t)source->count, sizeof(*copy->entries)
    );
    if (copy->entries == NULL) {
        PyMem_Free(copy);
        PyErr_NoMemory();
        return NULL;
    }
    for (Py_ssize_t i = 0; i < source->count; i++) {
        copy->entries[i].hash = source->entries[i].hash;
        copy->entries[i].key = Py_NewRef(source->entries[i].key);
        copy->entries[i].value = Py_NewRef(source->entries[i].value);
        copy->count++;
    }
    return copy;
}

static FunctoolsCacheSnapshot *
functools_cache_snapshot_capture(const FunctoolsLruCacheObject *cache)
{
    FunctoolsCacheSnapshot *snapshot = PyMem_Calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    snapshot->hits = cache->hits;
    snapshot->misses = cache->misses;
    Py_ssize_t count = PyDict_GET_SIZE(cache->cache);
    if (count == 0) {
        return snapshot;
    }
    snapshot->entries = PyMem_Calloc((size_t)count, sizeof(*snapshot->entries));
    if (snapshot->entries == NULL) {
        PyMem_Free(snapshot);
        PyErr_NoMemory();
        return NULL;
    }
    if (cache->maxsize > 0) {
        for (FunctoolsLruListElem *link = cache->root.next;
             link != &cache->root;
             link = link->next) {
            snapshot->entries[snapshot->count].hash = link->hash;
            snapshot->entries[snapshot->count].key = Py_NewRef(link->key);
            snapshot->entries[snapshot->count].value = Py_NewRef(link->result);
            snapshot->count++;
        }
    }
    else {
        PyObject *key;
        PyObject *value;
        Py_ssize_t position = 0;
        while (PyDict_Next(cache->cache, &position, &key, &value)) {
            snapshot->entries[snapshot->count].key = Py_NewRef(key);
            snapshot->entries[snapshot->count].value = Py_NewRef(value);
            snapshot->entries[snapshot->count].hash = PyObject_Hash(key);
            if (snapshot->entries[snapshot->count].hash == -1) {
                snapshot->count++;
                functools_cache_snapshot_free(snapshot);
                return NULL;
            }
            snapshot->count++;
        }
    }
    return snapshot;
}

static void *
functools_cache_copy_state(const void *raw_state)
{
    const FunctoolsCacheState *state = raw_state;
    FunctoolsCacheState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->wrapper = Py_NewRef(state->wrapper);
    copy->args = Py_NewRef(state->args);
    copy->kwargs = Py_XNewRef(state->kwargs);
    copy->snapshot = state->snapshot != NULL
        ? functools_cache_snapshot_copy(state->snapshot)
        : functools_cache_snapshot_capture(
            (FunctoolsLruCacheObject *)state->wrapper
        );
    if (copy->snapshot == NULL) {
        Py_DECREF(copy->wrapper);
        Py_DECREF(copy->args);
        Py_XDECREF(copy->kwargs);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
functools_cache_free_state(void *raw_state)
{
    FunctoolsCacheState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->wrapper);
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    functools_cache_snapshot_free(state->snapshot);
    PyMem_Free(state);
}

static const AleffAdapterVTable functools_cache_vtable = {
    .copy_state = functools_cache_copy_state,
    .free_state = functools_cache_free_state,
    .resume = functools_cache_resume,
};

static PyObject *
functools_cache_key(
    PyObject *kwd_mark,
    PyObject *args,
    PyObject *kwargs,
    int typed
)
{
    Py_ssize_t positional = PyTuple_GET_SIZE(args);
    Py_ssize_t keyword_count = kwargs == NULL ? 0 : PyDict_GET_SIZE(kwargs);
    if (!typed && keyword_count == 0) {
        if (positional == 1) {
            PyObject *item = PyTuple_GET_ITEM(args, 0);
            if (PyUnicode_CheckExact(item) || PyLong_CheckExact(item)) {
                return Py_NewRef(item);
            }
        }
        return Py_NewRef(args);
    }
    Py_ssize_t extra = keyword_count == 0 ? 0 : 1 + keyword_count * 2;
    Py_ssize_t type_count = typed ? positional + keyword_count : 0;
    PyObject *key = PyTuple_New(positional + extra + type_count);
    if (key == NULL) {
        return NULL;
    }
    Py_ssize_t index = 0;
    for (; index < positional; index++) {
        PyTuple_SET_ITEM(key, index, Py_NewRef(PyTuple_GET_ITEM(args, index)));
    }
    if (keyword_count != 0) {
        PyTuple_SET_ITEM(key, index++, Py_NewRef(kwd_mark));
        PyObject *name;
        PyObject *value;
        Py_ssize_t position = 0;
        while (PyDict_Next(kwargs, &position, &name, &value)) {
            PyTuple_SET_ITEM(key, index++, Py_NewRef(name));
            PyTuple_SET_ITEM(key, index++, Py_NewRef(value));
        }
    }
    if (typed) {
        for (Py_ssize_t i = 0; i < positional; i++) {
            PyTuple_SET_ITEM(key, index++, Py_NewRef((PyObject *)Py_TYPE(PyTuple_GET_ITEM(args, i))));
        }
        if (kwargs != NULL) {
            PyObject *value;
            Py_ssize_t position = 0;
            while (PyDict_Next(kwargs, &position, NULL, &value)) {
                PyTuple_SET_ITEM(key, index++, Py_NewRef((PyObject *)Py_TYPE(value)));
            }
        }
    }
    return key;
}

static void
functools_lru_extract_link(FunctoolsLruListElem *link)
{
    FunctoolsLruListElem *prev = link->prev;
    FunctoolsLruListElem *next = link->next;
    prev->next = next;
    next->prev = prev;
}

static void
functools_lru_append_link(
    FunctoolsLruCacheObject *cache,
    FunctoolsLruListElem *link
)
{
    FunctoolsLruListElem *root = &cache->root;
    FunctoolsLruListElem *last = root->prev;
    last->next = root->prev = link;
    link->prev = last;
    link->next = root;
}

static void
functools_lru_prepend_link(
    FunctoolsLruCacheObject *cache,
    FunctoolsLruListElem *link
)
{
    FunctoolsLruListElem *root = &cache->root;
    FunctoolsLruListElem *first = root->next;
    first->prev = root->next = link;
    link->prev = root;
    link->next = first;
}

static void
functools_lru_clear(FunctoolsLruCacheObject *cache)
{
    FunctoolsLruListElem *link = cache->root.next;
    cache->root.next = &cache->root;
    cache->root.prev = &cache->root;
    /* Drop the list's ownership before clearing the dict.  The dictionary
     * owns the other reference to each link; walking links after PyDict_Clear
     * would therefore be unsafe. */
    while (link != &cache->root) {
        FunctoolsLruListElem *next = link->next;
        Py_DECREF(link);
        link = next;
    }
    PyDict_Clear(cache->cache);
}

static int
functools_cache_restore_snapshot(
    FunctoolsLruCacheObject *cache,
    const FunctoolsCacheSnapshot *snapshot
)
{
    functools_lru_clear(cache);
    cache->hits = snapshot->hits;
    cache->misses = snapshot->misses;
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        const FunctoolsCacheEntry *entry = &snapshot->entries[i];
        if (cache->maxsize == -1) {
            if (PyDict_SetItem(cache->cache, entry->key, entry->value) < 0) {
                return -1;
            }
            continue;
        }
        FunctoolsLruListElem *link = PyObject_New(
            FunctoolsLruListElem, cache->lru_list_elem_type
        );
        if (link == NULL) {
            return -1;
        }
        link->hash = entry->hash;
        link->key = Py_NewRef(entry->key);
        link->result = Py_NewRef(entry->value);
        if (PyDict_SetItem(cache->cache, entry->key, (PyObject *)link) < 0) {
            Py_DECREF(link);
            return -1;
        }
        functools_lru_append_link(cache, link);
    }
    return 0;
}

static int
functools_cache_store(
    FunctoolsLruCacheObject *cache,
    PyObject *args,
    PyObject *kwargs,
    PyObject *value
)
{
    if (cache->maxsize == 0) {
        return 0;
    }
    PyObject *key = functools_cache_key(
        cache->kwd_mark, args, kwargs, cache->typed
    );
    if (key == NULL) {
        return -1;
    }
    Py_hash_t hash = PyObject_Hash(key);
    if (hash == -1) {
        Py_DECREF(key);
        return -1;
    }
    if (cache->maxsize == -1) {
        int result = PyDict_SetItem(cache->cache, key, value);
        Py_DECREF(key);
        return result;
    }

    /* This is the post-call check from CPython's bounded implementation. */
    PyObject *cached = PyDict_GetItemWithError(cache->cache, key);
    if (cached != NULL) {
        FunctoolsLruListElem *link = (FunctoolsLruListElem *)cached;
        Py_XSETREF(link->result, Py_NewRef(value));
        Py_DECREF(key);
        return 0;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(key);
        return -1;
    }

    if (PyDict_GET_SIZE(cache->cache) < cache->maxsize ||
        cache->root.next == &cache->root) {
        FunctoolsLruListElem *link = PyObject_New(
            FunctoolsLruListElem, cache->lru_list_elem_type
        );
        if (link == NULL) {
            Py_DECREF(key);
            return -1;
        }
        link->hash = hash;
        link->key = key;
        link->result = Py_NewRef(value);
        if (PyDict_SetItem(cache->cache, key, (PyObject *)link) < 0) {
            Py_DECREF(link);
            return -1;
        }
        functools_lru_append_link(cache, link);
        return 0;
    }

    /* Reuse the oldest link, as CPython does, to preserve LRU recency. */
    FunctoolsLruListElem *link = cache->root.next;
    functools_lru_extract_link(link);
    if (PyDict_DelItem(cache->cache, link->key) < 0) {
        functools_lru_prepend_link(cache, link);
        Py_DECREF(key);
        return -1;
    }
    PyObject *old_key = link->key;
    PyObject *old_result = link->result;
    link->hash = hash;
    link->key = key;
    link->result = Py_NewRef(value);
    if (PyDict_SetItem(cache->cache, key, (PyObject *)link) < 0) {
        Py_DECREF(old_key);
        Py_DECREF(old_result);
        Py_DECREF(link);
        return -1;
    }
    functools_lru_append_link(cache, link);
    Py_DECREF(old_key);
    Py_DECREF(old_result);
    return 0;
}

static PyObject *
functools_cache_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    FunctoolsCacheState *state = functools_cache_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    FunctoolsLruCacheObject *cache =
        (FunctoolsLruCacheObject *)state->wrapper;
    PyObject *result = NULL;
    if (functools_cache_restore_snapshot(cache, state->snapshot) == 0 &&
        functools_cache_store(cache, state->args, state->kwargs, value) == 0) {
        result = Py_NewRef(value);
    }
    functools_cache_free_state(state);
    return result;
}

static PyObject *
functools_cache_callable_call(
    FunctoolsCacheCallable *callable,
    PyObject *args,
    PyObject *kwargs
)
{
    PyObject *wrapper = PyWeakref_GetObject(callable->wrapper_ref);
    if (wrapper == Py_None) {
        PyErr_SetString(
            PyExc_RuntimeError, "lru cache wrapper is no longer alive"
        );
        return NULL;
    }
    FunctoolsCacheState state = {
        .wrapper = wrapper,
        .args = args,
        .kwargs = kwargs,
        .snapshot = NULL,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &functools_cache_vtable, &state) < 0) {
        return NULL;
    }
    if (frame.node == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_Call(callable->func, args, kwargs);
    adapter_leave(&frame);
    return result;
}

static void
functools_cache_callable_dealloc(FunctoolsCacheCallable *self)
{
    Py_DECREF(self->func);
    Py_XDECREF(self->wrapper_ref);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject FunctoolsCacheCallableType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._functools_continuation_callable",
    .tp_basicsize = sizeof(FunctoolsCacheCallable),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)functools_cache_callable_dealloc,
    .tp_call = (ternaryfunc)functools_cache_callable_call,
};

static PyObject *
functools_cache_callable_new(PyObject *func)
{
    FunctoolsCacheCallable *callable = PyObject_New(
        FunctoolsCacheCallable, &FunctoolsCacheCallableType
    );
    if (callable == NULL) {
        return NULL;
    }
    callable->func = Py_NewRef(func);
    callable->wrapper_ref = NULL;
    return (PyObject *)callable;
}

typedef struct {
    PyObject_HEAD
    PyObject *decorator;
} FunctoolsCacheDecorator;

static PyObject *functools_cache_decorator_call(
    FunctoolsCacheDecorator *self,
    PyObject *args,
    PyObject *kwargs
);

static void
functools_cache_decorator_dealloc(FunctoolsCacheDecorator *self)
{
    Py_DECREF(self->decorator);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject FunctoolsCacheDecoratorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._functools_lru_decorator",
    .tp_basicsize = sizeof(FunctoolsCacheDecorator),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)functools_cache_decorator_dealloc,
    .tp_call = (ternaryfunc)functools_cache_decorator_call,
};

static PyObject *functools_original_lru_cache = NULL;
static PyObject *functools_update_wrapper = NULL;

static PyObject *
functools_cache_wrap(PyObject *func, PyObject *decorator)
{
    PyObject *callable = functools_cache_callable_new(func);
    if (callable == NULL) {
        return NULL;
    }
    PyObject *wrapper = PyObject_CallOneArg(decorator, callable);
    if (wrapper == NULL) {
        Py_DECREF(callable);
        return NULL;
    }
    FunctoolsCacheCallable *callable_object =
        (FunctoolsCacheCallable *)callable;
    callable_object->wrapper_ref = PyWeakref_NewRef(wrapper, NULL);
    if (callable_object->wrapper_ref == NULL) {
        Py_DECREF(wrapper);
        Py_DECREF(callable);
        return NULL;
    }
    PyObject *updated = PyObject_CallFunctionObjArgs(
        functools_update_wrapper, wrapper, func, NULL
    );
    Py_DECREF(callable);
    if (updated == NULL) {
        Py_DECREF(wrapper);
        return NULL;
    }
    Py_DECREF(updated);
    return wrapper;
}

static PyObject *
functools_cache_decorator_call(
    FunctoolsCacheDecorator *self,
    PyObject *args,
    PyObject *kwargs
)
{
    if (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) {
        PyErr_SetString(PyExc_TypeError, "lru_cache() takes no keyword arguments");
        return NULL;
    }
    PyObject *func;
    if (!PyArg_ParseTuple(args, "O:lru_cache", &func)) {
        return NULL;
    }
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "the first argument must be callable");
        return NULL;
    }
    return functools_cache_wrap(func, self->decorator);
}

static PyObject *
adapter_lru_cache(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) == 1 &&
        (kwargs == NULL || PyDict_GET_SIZE(kwargs) == 0) &&
        PyCallable_Check(PyTuple_GET_ITEM(args, 0))) {
        return functools_cache_wrap(
            PyTuple_GET_ITEM(args, 0), functools_original_lru_cache
        );
    }
    PyObject *decorator = PyObject_Call(functools_original_lru_cache, args, kwargs);
    if (decorator == NULL) {
        return NULL;
    }
    FunctoolsCacheDecorator *result = PyObject_New(
        FunctoolsCacheDecorator,
        &FunctoolsCacheDecoratorType
    );
    if (result == NULL) {
        Py_DECREF(decorator);
        return NULL;
    }
    result->decorator = decorator;
    return (PyObject *)result;
}

static int
adapter_functools_install(PyObject *functools)
{
    if (PyType_Ready(&FunctoolsKeyFactoryType) < 0 ||
        PyType_Ready(&FunctoolsKeyObjectType) < 0 ||
        PyType_Ready(&FunctoolsCacheCallableType) < 0 ||
        PyType_Ready(&FunctoolsCacheDecoratorType) < 0) {
        return -1;
    }
    functools_original_lru_cache = PyObject_GetAttrString(functools, "lru_cache");
    if (functools_original_lru_cache == NULL) {
        return -1;
    }
    functools_update_wrapper = PyObject_GetAttrString(functools, "update_wrapper");
    if (functools_update_wrapper == NULL) {
        return -1;
    }
    static PyMethodDef cmp_to_key_method = {
        "cmp_to_key", adapter_cmp_to_key, METH_O, NULL
    };
    PyObject *cmp_to_key = PyCFunction_NewEx(&cmp_to_key_method, NULL, functools);
    if (cmp_to_key == NULL ||
        PyObject_SetAttrString(functools, "cmp_to_key", cmp_to_key) < 0) {
        Py_XDECREF(cmp_to_key);
        return -1;
    }
    Py_DECREF(cmp_to_key);
    static PyMethodDef lru_cache_method = {
        "lru_cache", _PyCFunction_CAST(adapter_lru_cache), METH_VARARGS | METH_KEYWORDS, NULL
    };
    PyObject *lru_cache = PyCFunction_NewEx(&lru_cache_method, NULL, functools);
    if (lru_cache == NULL ||
        PyObject_SetAttrString(functools, "lru_cache", lru_cache) < 0) {
        Py_XDECREF(lru_cache);
        return -1;
    }
    Py_DECREF(lru_cache);
    return 0;
}
