#include "api.h"
#include "csv.h"

#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The reader and writer objects below intentionally mirror the private
 * prefixes used by CPython's _csv module.  The public type is left intact;
 * only its next slot and its two writer method descriptors are replaced. */

#define NOT_SET ((Py_UCS4)-1)
#define EOL ((Py_UCS4)-2)

typedef enum {
    START_RECORD,
    START_FIELD,
    ESCAPED_CHAR,
    IN_FIELD,
    IN_QUOTED_FIELD,
    ESCAPE_IN_QUOTED_FIELD,
    QUOTE_IN_QUOTED_FIELD,
    EAT_CRNL,
    AFTER_ESCAPED_CRNL,
} ParserState;

typedef enum {
    QUOTE_MINIMAL,
    QUOTE_ALL,
    QUOTE_NONNUMERIC,
    QUOTE_NONE,
    QUOTE_STRINGS,
    QUOTE_NOTNULL,
} QuoteStyle;

typedef struct {
    PyObject_HEAD
    char doublequote;
    char skipinitialspace;
    char strict;
    int quoting;
    Py_UCS4 delimiter;
    Py_UCS4 quotechar;
    Py_UCS4 escapechar;
    PyObject *lineterminator;
} CsvDialectObject;

typedef struct {
    PyObject_HEAD
    PyObject *input_iter;
    CsvDialectObject *dialect;
    PyObject *fields;
    ParserState state;
    Py_UCS4 *field;
    Py_ssize_t field_size;
    Py_ssize_t field_len;
#if PY_VERSION_HEX >= 0x030d0000
    bool unquoted_field;
#else
    int numeric_field;
#endif
    unsigned long line_num;
} CsvReaderObject;

typedef struct {
    PyObject_HEAD
    PyObject *write;
    CsvDialectObject *dialect;
    Py_UCS4 *rec;
    Py_ssize_t rec_size;
    Py_ssize_t rec_len;
    int num_fields;
    PyObject *error_obj;
} CsvWriterObject;

typedef enum {
    CSV_READER_LIVE,
    CSV_READER_SNAPSHOT,
} CsvReaderStateKind;

typedef struct {
    CsvReaderObject *reader;
    PyObject *module;
    PyObject *error;
    PyObject *input_iter;
    PyObject *fields;
    Py_UCS4 *field;
    Py_ssize_t field_size;
    Py_ssize_t field_len;
    ParserState parser_state;
#if PY_VERSION_HEX >= 0x030d0000
    bool unquoted_field;
#else
    int numeric_field;
#endif
    unsigned long line_num;
    CsvReaderStateKind kind;
} CsvReaderState;

typedef enum {
    CSV_WRITEROW,
    CSV_WRITEROWS,
} CsvWriterMode;

typedef enum {
    CSV_WRITER_WAIT_ROWS_ITER,
    CSV_WRITER_WAIT_ROWS_NEXT,
    CSV_WRITER_WAIT_ROW_ITER,
    CSV_WRITER_WAIT_ROW_NEXT,
    CSV_WRITER_WAIT_FIELD_STR,
    CSV_WRITER_WAIT_WRITE,
} CsvWriterPhase;

typedef struct {
    CsvWriterObject *writer;
    PyObject *rows_source;
    PyObject *rows_iter;
    PyObject *row_source;
    PyObject *field_iter;
    PyObject *field;
    Py_UCS4 *record;
    Py_ssize_t record_size;
    Py_ssize_t record_len;
    int num_fields;
    int quoted;
    int null_field;
    CsvWriterMode mode;
    CsvWriterPhase phase;
} CsvWriterState;

static const AleffAdapterVTable csv_reader_vtable;
static const AleffAdapterVTable csv_writer_vtable;
static void csv_writer_free_state(void *raw_state);

static PyTypeObject *installed_reader_type;
static PyTypeObject *installed_writer_type;
static PyObject *installed_csv_module;
static PyObject *installed_csv_error;
static PyObject *original_writerow;
static PyObject *original_writerows;
static PyObject *original_reader_next_method;
static iternextfunc original_reader_next;
static PyMethodDef reader_next_method;
static PyMethodDef writerow_method;
static PyMethodDef writerows_method;
static int csv_installed;

static PyObject *
csv_clone_position_iterator(PyObject *iterator)
{
    const char *name = Py_TYPE(iterator)->tp_name;
    if (strcmp(name, "list_iterator") != 0 &&
        strcmp(name, "tuple_iterator") != 0 &&
        strcmp(name, "range_iterator") != 0 &&
        strcmp(name, "longrange_iterator") != 0) {
        return Py_NewRef(iterator);
    }

    PyObject *reduced = PyObject_CallMethod(iterator, "__reduce__", NULL);
    if (reduced == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(reduced) || PyTuple_GET_SIZE(reduced) < 2) {
        Py_DECREF(reduced);
        return Py_NewRef(iterator);
    }
    PyObject *copy = PyObject_Call(
        PyTuple_GET_ITEM(reduced, 0),
        PyTuple_GET_ITEM(reduced, 1),
        NULL
    );
    if (copy != NULL && PyTuple_GET_SIZE(reduced) >= 3) {
        PyObject *result = PyObject_CallMethod(
            copy,
            "__setstate__",
            "O",
            PyTuple_GET_ITEM(reduced, 2)
        );
        if (result == NULL) {
            Py_CLEAR(copy);
        }
        else {
            Py_DECREF(result);
        }
    }
    Py_DECREF(reduced);
    return copy;
}

static void
csv_reader_clear_snapshot(CsvReaderState *state)
{
    Py_XDECREF(state->input_iter);
    Py_XDECREF(state->fields);
    PyMem_Free(state->field);
    Py_XDECREF(state->error);
    Py_XDECREF(state->module);
    PyMem_Free(state);
}

static int
csv_reader_copy_native(CsvReaderState *copy, const CsvReaderObject *reader)
{
    copy->input_iter = csv_clone_position_iterator(reader->input_iter);
    if (copy->input_iter == NULL) {
        return -1;
    }
    copy->fields = reader->fields == NULL
        ? NULL
        : PyList_GetSlice(reader->fields, 0, PyList_GET_SIZE(reader->fields));
    if (reader->fields != NULL && copy->fields == NULL) {
        return -1;
    }
    copy->field_size = reader->field_size;
    copy->field_len = reader->field_len;
    if (copy->field_size != 0) {
        copy->field = PyMem_Malloc(
            (size_t)copy->field_size * sizeof(*copy->field)
        );
        if (copy->field == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        if (copy->field_len != 0) {
            memcpy(
                copy->field,
                reader->field,
                (size_t)copy->field_len * sizeof(*copy->field)
            );
        }
    }
    copy->parser_state = reader->state;
#if PY_VERSION_HEX >= 0x030d0000
    copy->unquoted_field = reader->unquoted_field;
#else
    copy->numeric_field = reader->numeric_field;
#endif
    copy->line_num = reader->line_num;
    return 0;
}

static int
csv_reader_copy_snapshot(CsvReaderState *copy, const CsvReaderState *source)
{
    copy->input_iter = csv_clone_position_iterator(source->input_iter);
    if (copy->input_iter == NULL) {
        return -1;
    }
    copy->fields = source->fields == NULL
        ? NULL
        : PyList_GetSlice(source->fields, 0, PyList_GET_SIZE(source->fields));
    if (source->fields != NULL && copy->fields == NULL) {
        return -1;
    }
    copy->field_size = source->field_size;
    copy->field_len = source->field_len;
    if (copy->field_size != 0) {
        copy->field = PyMem_Malloc(
            (size_t)copy->field_size * sizeof(*copy->field)
        );
        if (copy->field == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        if (copy->field_len != 0) {
            memcpy(
                copy->field,
                source->field,
                (size_t)copy->field_len * sizeof(*copy->field)
            );
        }
    }
    copy->parser_state = source->parser_state;
#if PY_VERSION_HEX >= 0x030d0000
    copy->unquoted_field = source->unquoted_field;
#else
    copy->numeric_field = source->numeric_field;
#endif
    copy->line_num = source->line_num;
    return 0;
}

static void *
csv_reader_copy_state(const void *raw_state)
{
    const CsvReaderState *source = raw_state;
    CsvReaderState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->reader = (CsvReaderObject *)Py_NewRef((PyObject *)source->reader);
    copy->module = Py_NewRef(source->module);
    copy->error = Py_NewRef(source->error);
    copy->kind = CSV_READER_SNAPSHOT;
    int status = source->kind == CSV_READER_LIVE
        ? csv_reader_copy_native(copy, source->reader)
        : csv_reader_copy_snapshot(copy, source);
    if (status < 0) {
        Py_DECREF(copy->reader);
        copy->reader = NULL;
        csv_reader_clear_snapshot(copy);
        return NULL;
    }
    return copy;
}

static void
csv_reader_free_state(void *raw_state)
{
    CsvReaderState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->reader);
    csv_reader_clear_snapshot(state);
}

static int
csv_reader_restore(CsvReaderState *state)
{
    CsvReaderObject *reader = state->reader;
    Py_UCS4 *field = NULL;
    if (state->field_size != 0) {
        field = PyMem_Malloc(
            (size_t)state->field_size * sizeof(*field)
        );
        if (field == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        if (state->field_len != 0) {
            memcpy(
                field,
                state->field,
                (size_t)state->field_len * sizeof(*field)
            );
        }
    }

    Py_XSETREF(reader->input_iter, Py_NewRef(state->input_iter));
    Py_XSETREF(reader->fields, Py_XNewRef(state->fields));
    PyMem_Free(reader->field);
    reader->field = field;
    reader->field_size = state->field_size;
    reader->field_len = state->field_len;
    reader->state = state->parser_state;
#if PY_VERSION_HEX >= 0x030d0000
    reader->unquoted_field = state->unquoted_field;
#else
    reader->numeric_field = state->numeric_field;
#endif
    reader->line_num = state->line_num;
    return 0;
}

static int
csv_reader_field_limit(CsvReaderState *state, Py_ssize_t *limit)
{
    PyObject *value = PyObject_CallMethod(
        state->module,
        "field_size_limit",
        NULL
    );
    if (value == NULL) {
        return -1;
    }
    *limit = PyLong_AsSsize_t(value);
    Py_DECREF(value);
    return *limit == -1 && PyErr_Occurred() ? -1 : 0;
}

static int
csv_reader_grow_field(CsvReaderObject *reader)
{
    if ((size_t)reader->field_size >
        (size_t)PY_SSIZE_T_MAX / sizeof(*reader->field)) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t size = reader->field_size == 0
        ? 4096
        : reader->field_size * 2;
    if (size <= reader->field_size ||
        (size_t)size > (size_t)PY_SSIZE_T_MAX / sizeof(*reader->field)) {
        PyErr_NoMemory();
        return -1;
    }
    Py_UCS4 *field = PyMem_Realloc(
        reader->field,
        (size_t)size * sizeof(*field)
    );
    if (field == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    reader->field = field;
    reader->field_size = size;
    return 0;
}

static int
csv_reader_add_char(
    CsvReaderState *state,
    CsvReaderObject *reader,
    Py_UCS4 value
)
{
    Py_ssize_t limit;
    if (csv_reader_field_limit(state, &limit) < 0) {
        return -1;
    }
    if (reader->field_len >= limit) {
        PyErr_Format(
            state->error,
            "field larger than field limit (%zd)",
            limit
        );
        return -1;
    }
    if (reader->field_len == reader->field_size &&
        csv_reader_grow_field(reader) < 0) {
        return -1;
    }
    reader->field[reader->field_len++] = value;
    return 0;
}

static int
csv_reader_save_field(CsvReaderState *state, CsvReaderObject *reader)
{
    PyObject *field;
    (void)state;
#if PY_VERSION_HEX >= 0x030d0000
    if (reader->unquoted_field && reader->field_len == 0 &&
        (reader->dialect->quoting == QUOTE_NOTNULL ||
         reader->dialect->quoting == QUOTE_STRINGS)) {
        field = Py_NewRef(Py_None);
    }
    else {
        field = PyUnicode_FromKindAndData(
            PyUnicode_4BYTE_KIND,
            reader->field,
            reader->field_len
        );
        if (field == NULL) {
            return -1;
        }
        if (reader->unquoted_field && reader->field_len != 0 &&
            (reader->dialect->quoting == QUOTE_NONNUMERIC ||
             reader->dialect->quoting == QUOTE_STRINGS)) {
            PyObject *number = PyNumber_Float(field);
            Py_DECREF(field);
            if (number == NULL) {
                return -1;
            }
            field = number;
        }
        reader->field_len = 0;
    }
#else
    field = PyUnicode_FromKindAndData(
        PyUnicode_4BYTE_KIND,
        reader->field,
        reader->field_len
    );
    if (field == NULL) {
        return -1;
    }
    reader->field_len = 0;
    if (reader->numeric_field) {
        reader->numeric_field = 0;
        PyObject *number = PyNumber_Float(field);
        Py_DECREF(field);
        if (number == NULL) {
            return -1;
        }
        field = number;
    }
#endif
    if (PyList_Append(reader->fields, field) < 0) {
        Py_DECREF(field);
        return -1;
    }
    Py_DECREF(field);
    return 0;
}

static int
csv_reader_process_char(
    CsvReaderState *state,
    CsvReaderObject *reader,
    Py_UCS4 value
)
{
    CsvDialectObject *dialect = reader->dialect;
    switch (reader->state) {
        case START_RECORD:
            if (value == EOL) {
                break;
            }
            if (value == '\n' || value == '\r') {
                reader->state = EAT_CRNL;
                break;
            }
            reader->state = START_FIELD;
            /* fall through */
        case START_FIELD:
#if PY_VERSION_HEX >= 0x030d0000
            reader->unquoted_field = true;
#endif
            if (value == '\n' || value == '\r' || value == EOL) {
                if (csv_reader_save_field(state, reader) < 0) {
                    return -1;
                }
                reader->state = value == EOL ? START_RECORD : EAT_CRNL;
            }
            else if (value == dialect->quotechar &&
                     dialect->quoting != QUOTE_NONE) {
#if PY_VERSION_HEX >= 0x030d0000
                reader->unquoted_field = false;
#endif
                reader->state = IN_QUOTED_FIELD;
            }
            else if (value == dialect->escapechar) {
#if PY_VERSION_HEX < 0x030d0000
                if (dialect->quoting == QUOTE_NONNUMERIC) {
                    reader->numeric_field = 1;
                }
#endif
                reader->state = ESCAPED_CHAR;
            }
            else if (value == ' ' && dialect->skipinitialspace) {
                break;
            }
            else if (value == dialect->delimiter) {
                if (csv_reader_save_field(state, reader) < 0) {
                    return -1;
                }
            }
            else {
#if PY_VERSION_HEX < 0x030d0000
                if (dialect->quoting == QUOTE_NONNUMERIC) {
                    reader->numeric_field = 1;
                }
#endif
                if (csv_reader_add_char(state, reader, value) < 0) {
                    return -1;
                }
                reader->state = IN_FIELD;
            }
            break;

        case ESCAPED_CHAR:
            if (value == '\n' || value == '\r') {
                if (csv_reader_add_char(state, reader, value) < 0) {
                    return -1;
                }
                reader->state = AFTER_ESCAPED_CRNL;
                break;
            }
            if (value == EOL) {
                value = '\n';
            }
            if (csv_reader_add_char(state, reader, value) < 0) {
                return -1;
            }
            reader->state = IN_FIELD;
            break;

        case AFTER_ESCAPED_CRNL:
            if (value == EOL) {
                break;
            }
            /* fall through */
        case IN_FIELD:
            if (value == '\n' || value == '\r' || value == EOL) {
                if (csv_reader_save_field(state, reader) < 0) {
                    return -1;
                }
                reader->state = value == EOL ? START_RECORD : EAT_CRNL;
            }
            else if (value == dialect->escapechar) {
                reader->state = ESCAPED_CHAR;
            }
            else if (value == dialect->delimiter) {
                if (csv_reader_save_field(state, reader) < 0) {
                    return -1;
                }
                reader->state = START_FIELD;
            }
            else if (csv_reader_add_char(state, reader, value) < 0) {
                return -1;
            }
            break;

        case IN_QUOTED_FIELD:
            if (value == EOL) {
                break;
            }
            if (value == dialect->escapechar) {
                reader->state = ESCAPE_IN_QUOTED_FIELD;
            }
            else if (value == dialect->quotechar &&
                     dialect->quoting != QUOTE_NONE) {
                reader->state = dialect->doublequote
                    ? QUOTE_IN_QUOTED_FIELD
                    : IN_FIELD;
            }
            else if (csv_reader_add_char(state, reader, value) < 0) {
                return -1;
            }
            break;

        case ESCAPE_IN_QUOTED_FIELD:
            if (value == EOL) {
                value = '\n';
            }
            if (csv_reader_add_char(state, reader, value) < 0) {
                return -1;
            }
            reader->state = IN_QUOTED_FIELD;
            break;

        case QUOTE_IN_QUOTED_FIELD:
            if (dialect->quoting != QUOTE_NONE &&
                value == dialect->quotechar) {
                if (csv_reader_add_char(state, reader, value) < 0) {
                    return -1;
                }
                reader->state = IN_QUOTED_FIELD;
            }
            else if (value == dialect->delimiter) {
                if (csv_reader_save_field(state, reader) < 0) {
                    return -1;
                }
                reader->state = START_FIELD;
            }
            else if (value == '\n' || value == '\r' || value == EOL) {
                if (csv_reader_save_field(state, reader) < 0) {
                    return -1;
                }
                reader->state = value == EOL ? START_RECORD : EAT_CRNL;
            }
            else if (!dialect->strict) {
                if (csv_reader_add_char(state, reader, value) < 0) {
                    return -1;
                }
                reader->state = IN_FIELD;
            }
            else {
                PyErr_Format(
                    state->error,
                    "'%c' expected after '%c'",
                    dialect->delimiter,
                    dialect->quotechar
                );
                return -1;
            }
            break;

        case EAT_CRNL:
            if (value == '\n' || value == '\r') {
                break;
            }
            if (value == EOL) {
                reader->state = START_RECORD;
                break;
            }
            {
                PyErr_Format(
                    state->error,
                    "new-line character seen in unquoted field - "
                    "do you need to open the file with newline=''?"
                );
            }
            return -1;
    }
    return 0;
}

static PyObject *
csv_reader_finish(CsvReaderObject *reader)
{
    PyObject *fields = reader->fields;
    reader->fields = NULL;
    return fields;
}

static PyObject *
csv_reader_continue(
    CsvReaderState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    CsvReaderObject *reader = state->reader;
    PyObject *line;
    for (;;) {
        if (is_resumed) {
            line = Py_NewRef(resumed_value);
            is_resumed = 0;
        }
        else {
            line = PyIter_Next(reader->input_iter);
        }
        if (line == NULL) {
            if (PyErr_Occurred()) {
                return NULL;
            }
            if (reader->field_len != 0 || reader->state == IN_QUOTED_FIELD) {
                if (reader->dialect->strict) {
                    PyErr_SetString(state->error, "unexpected end of data");
                    return NULL;
                }
                if (csv_reader_save_field(state, reader) < 0) {
                    return NULL;
                }
            }
            return csv_reader_finish(reader);
        }
        if (!PyUnicode_Check(line)) {
            PyErr_Format(
                state->error,
                "iterator should return strings, not %.200s "
                "(the file should be opened in text mode)",
                Py_TYPE(line)->tp_name
            );
            Py_DECREF(line);
            return NULL;
        }
        if (reader->fields == NULL) {
            PyErr_SetString(state->error, "iterator has already advanced the reader");
            Py_DECREF(line);
            return NULL;
        }
        reader->line_num++;
        int kind = PyUnicode_KIND(line);
        const void *data = PyUnicode_DATA(line);
        Py_ssize_t length = PyUnicode_GET_LENGTH(line);
        for (Py_ssize_t pos = 0; pos < length; pos++) {
            if (csv_reader_process_char(
                    state,
                    reader,
                    PyUnicode_READ(kind, data, pos)
                ) < 0) {
                Py_DECREF(line);
                return NULL;
            }
        }
        Py_DECREF(line);
        if (csv_reader_process_char(state, reader, EOL) < 0) {
            return NULL;
        }
        if (reader->state == START_RECORD) {
            return csv_reader_finish(reader);
        }
    }
}

static PyObject *
csv_reader_resume(const void *raw_state, PyObject *value)
{
    CsvReaderState *state = csv_reader_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    if (csv_reader_restore(state) < 0) {
        csv_reader_free_state(state);
        return NULL;
    }
    state->kind = CSV_READER_LIVE;
    if (value == NULL) {
        csv_reader_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &csv_reader_vtable, state) < 0) {
        csv_reader_free_state(state);
        return NULL;
    }
    PyObject *result = csv_reader_continue(state, value, 1);
    adapter_leave(&frame);
    csv_reader_free_state(state);
    return result;
}

static const AleffAdapterVTable csv_reader_vtable = {
    .copy_state = csv_reader_copy_state,
    .free_state = csv_reader_free_state,
    .resume = csv_reader_resume,
    .prepare_resume = NULL,
};

static PyObject *
adapter_csv_reader_next(PyObject *object)
{
    CsvReaderState state = {
        .reader = (CsvReaderObject *)object,
        .module = installed_csv_module,
        .error = installed_csv_error,
        .kind = CSV_READER_LIVE,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &csv_reader_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = original_reader_next(object);
    adapter_leave(&frame);
    return result;
}

/* Record construction follows CPython's Modules/_csv.c writer.  Keeping the
 * record in adapter state makes each continuation shot independent while the
 * callback-free quoting and escaping rules remain identical.  CPython's
 * license terms are included in LICENSES/CPython.txt. */
static void *
csv_writer_copy_state(const void *raw_state)
{
    const CsvWriterState *source = raw_state;
    CsvWriterState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->writer = (CsvWriterObject *)Py_NewRef((PyObject *)source->writer);
    copy->rows_source = Py_XNewRef(source->rows_source);
    copy->row_source = Py_XNewRef(source->row_source);
    copy->field = Py_XNewRef(source->field);
    copy->mode = source->mode;
    copy->phase = source->phase;
    copy->record_size = source->record_size;
    copy->record_len = source->record_len;
    copy->num_fields = source->num_fields;
    copy->quoted = source->quoted;
    copy->null_field = source->null_field;
    copy->rows_iter = source->rows_iter == NULL
        ? NULL
        : csv_clone_position_iterator(source->rows_iter);
    copy->field_iter = source->field_iter == NULL
        ? NULL
        : csv_clone_position_iterator(source->field_iter);
    if ((source->rows_iter != NULL && copy->rows_iter == NULL) ||
        (source->field_iter != NULL && copy->field_iter == NULL)) {
        csv_writer_free_state(copy);
        return NULL;
    }
    if (source->record_size > 0) {
        copy->record = PyMem_Malloc(
            (size_t)source->record_size * sizeof(*copy->record)
        );
        if (copy->record == NULL) {
            PyErr_NoMemory();
            csv_writer_free_state(copy);
            return NULL;
        }
        if (source->record_len > 0) {
            memcpy(
                copy->record,
                source->record,
                (size_t)source->record_len * sizeof(*copy->record)
            );
        }
    }
    return copy;
}

static void
csv_writer_clear_state(CsvWriterState *state)
{
    Py_CLEAR(state->field);
    Py_CLEAR(state->field_iter);
    Py_CLEAR(state->row_source);
    Py_CLEAR(state->rows_iter);
    Py_CLEAR(state->rows_source);
    Py_CLEAR(state->writer);
    PyMem_Free(state->record);
    state->record = NULL;
    state->record_size = 0;
    state->record_len = 0;
}

static void
csv_writer_free_state(void *raw_state)
{
    CsvWriterState *state = raw_state;
    if (state == NULL) {
        return;
    }
    csv_writer_clear_state(state);
    PyMem_Free(state);
}

#define CSV_RECORD_INCREMENT 32768

static void
csv_writer_reset_record(CsvWriterState *state)
{
    state->record_len = 0;
    state->num_fields = 0;
    state->null_field = 0;
}

static Py_ssize_t
csv_writer_append_data(
    CsvWriterState *state,
    int field_kind,
    const void *field_data,
    Py_ssize_t field_len,
    int *quoted,
    int copy_phase
)
{
    CsvDialectObject *dialect = state->writer->dialect;
    Py_ssize_t record_len = state->record_len;

#define CSV_INCREMENT_LENGTH() \
    do { \
        if (!copy_phase && record_len == PY_SSIZE_T_MAX) { \
            PyErr_NoMemory(); \
            return -1; \
        } \
        record_len++; \
    } while (0)
#define CSV_ADD_CHARACTER(character) \
    do { \
        if (copy_phase) { \
            state->record[record_len] = (character); \
        } \
        CSV_INCREMENT_LENGTH(); \
    } while (0)

    if (state->num_fields > 0) {
        CSV_ADD_CHARACTER(dialect->delimiter);
    }
    if (copy_phase && *quoted) {
        CSV_ADD_CHARACTER(dialect->quotechar);
    }
    for (Py_ssize_t index = 0;
         field_data != NULL && index < field_len;
         index++) {
        Py_UCS4 character = PyUnicode_READ(field_kind, field_data, index);
        int escape = 0;
        if (character == dialect->delimiter ||
            character == dialect->escapechar ||
            character == dialect->quotechar ||
            character == '\n' ||
            character == '\r' ||
            PyUnicode_FindChar(
                dialect->lineterminator,
                character,
                0,
                PyUnicode_GET_LENGTH(dialect->lineterminator),
                1
            ) >= 0) {
            if (dialect->quoting == QUOTE_NONE) {
                escape = 1;
            }
            else {
                if (character == dialect->quotechar) {
                    if (dialect->doublequote) {
                        CSV_ADD_CHARACTER(dialect->quotechar);
                    }
                    else {
                        escape = 1;
                    }
                }
                else if (character == dialect->escapechar) {
                    escape = 1;
                }
                if (!escape) {
                    *quoted = 1;
                }
            }
            if (escape) {
                if (dialect->escapechar == NOT_SET) {
                    PyErr_Format(
                        state->writer->error_obj,
                        "need to escape, but no escapechar set"
                    );
                    return -1;
                }
                CSV_ADD_CHARACTER(dialect->escapechar);
            }
        }
        CSV_ADD_CHARACTER(character);
    }
    if (*quoted) {
        if (copy_phase) {
            CSV_ADD_CHARACTER(dialect->quotechar);
        }
        else {
            CSV_INCREMENT_LENGTH();
            CSV_INCREMENT_LENGTH();
        }
    }

#undef CSV_ADD_CHARACTER
#undef CSV_INCREMENT_LENGTH
    return record_len;
}

static int
csv_writer_grow_record(CsvWriterState *state, Py_ssize_t required)
{
    if (required <= state->record_size) {
        return 0;
    }
    if (required > PY_SSIZE_T_MAX - CSV_RECORD_INCREMENT) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t size =
        (required / CSV_RECORD_INCREMENT + 1) * CSV_RECORD_INCREMENT;
    if (size > PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(*state->record)) {
        PyErr_NoMemory();
        return -1;
    }
    Py_UCS4 *record = PyMem_Realloc(
        state->record,
        (size_t)size * sizeof(*state->record)
    );
    if (record == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    state->record = record;
    state->record_size = size;
    return 0;
}

static int
csv_writer_export_record(CsvWriterState *state)
{
    CsvWriterObject *writer = state->writer;
    if (state->record_len > writer->rec_size) {
        if (state->record_len >
            PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(*writer->rec)) {
            PyErr_NoMemory();
            return -1;
        }
        Py_UCS4 *record = PyMem_Realloc(
            writer->rec,
            (size_t)state->record_len * sizeof(*writer->rec)
        );
        if (record == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        writer->rec = record;
        writer->rec_size = state->record_len;
    }
    if (state->record_len > 0) {
        memcpy(
            writer->rec,
            state->record,
            (size_t)state->record_len * sizeof(*writer->rec)
        );
    }
    writer->rec_len = state->record_len;
    writer->num_fields = state->num_fields;
    return 0;
}

static int
csv_writer_import_record(CsvWriterState *state)
{
    CsvWriterObject *writer = state->writer;
    if (writer->rec_len > state->record_size) {
        if (writer->rec_len >
            PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(*state->record)) {
            PyErr_NoMemory();
            return -1;
        }
        Py_UCS4 *record = PyMem_Realloc(
            state->record,
            (size_t)writer->rec_len * sizeof(*state->record)
        );
        if (record == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        state->record = record;
        state->record_size = writer->rec_len;
    }
    if (writer->rec_len > 0) {
        memcpy(
            state->record,
            writer->rec,
            (size_t)writer->rec_len * sizeof(*state->record)
        );
    }
    state->record_len = writer->rec_len;
    state->num_fields = writer->num_fields;
    return 0;
}

static int
csv_writer_prepare_resume(void *raw_state)
{
    return csv_writer_export_record((CsvWriterState *)raw_state);
}

static int
csv_writer_import_after_callback(CsvWriterState *state)
{
    PyObject *raised = PyErr_GetRaisedException();
    if (csv_writer_import_record(state) < 0) {
        Py_XDECREF(raised);
        return -1;
    }
    PyErr_SetRaisedException(raised);
    return 0;
}

static int
csv_writer_append_field(CsvWriterState *state, PyObject *field, int quoted)
{
    CsvDialectObject *dialect = state->writer->dialect;
    int field_kind = -1;
    const void *field_data = NULL;
    Py_ssize_t field_len = 0;
    if (field != NULL) {
#if PY_VERSION_HEX < 0x030d0000
        if (PyUnicode_READY(field) < 0) {
            return -1;
        }
#endif
        field_kind = PyUnicode_KIND(field);
        field_data = PyUnicode_DATA(field);
        field_len = PyUnicode_GET_LENGTH(field);
    }
    if (field_len == 0 && dialect->delimiter == ' ' &&
        dialect->skipinitialspace) {
        if (dialect->quoting == QUOTE_NONE ||
            (field == NULL &&
             (dialect->quoting == QUOTE_STRINGS ||
              dialect->quoting == QUOTE_NOTNULL))) {
            PyErr_Format(
                state->writer->error_obj,
                "empty field must be quoted if delimiter is a space "
                "and skipinitialspace is true"
            );
            return -1;
        }
        quoted = 1;
    }
    Py_ssize_t required = csv_writer_append_data(
        state,
        field_kind,
        field_data,
        field_len,
        &quoted,
        0
    );
    if (required < 0 || csv_writer_grow_record(state, required) < 0) {
        return -1;
    }
    state->record_len = csv_writer_append_data(
        state,
        field_kind,
        field_data,
        field_len,
        &quoted,
        1
    );
    if (state->record_len < 0) {
        return -1;
    }
    state->num_fields++;
    return 0;
}

static int
csv_writer_append_lineterminator(CsvWriterState *state)
{
    PyObject *terminator = state->writer->dialect->lineterminator;
    Py_ssize_t length = PyUnicode_GET_LENGTH(terminator);
    if (length < 0 || state->record_len > PY_SSIZE_T_MAX - length) {
        if (length >= 0) {
            PyErr_NoMemory();
        }
        return -1;
    }
    Py_ssize_t required = state->record_len + length;
    if (csv_writer_grow_record(state, required) < 0) {
        return -1;
    }
    int kind = PyUnicode_KIND(terminator);
    const void *data = PyUnicode_DATA(terminator);
    for (Py_ssize_t index = 0; index < length; index++) {
        state->record[state->record_len + index] =
            PyUnicode_READ(kind, data, index);
    }
    state->record_len = required;
    return 0;
}

static int
csv_writer_accept_iterator(
    CsvWriterState *state,
    PyObject *value,
    int rows_iterator
)
{
    if (!PyIter_Check(value)) {
        if (rows_iterator) {
            PyErr_Format(
                PyExc_TypeError,
                "iter() returned non-iterator of type '%.200s'",
                Py_TYPE(value)->tp_name
            );
        }
        else {
            PyErr_Format(
                state->writer->error_obj,
                "iterable expected, not %.200s",
                Py_TYPE(state->row_source)->tp_name
            );
        }
        return -1;
    }
    if (rows_iterator) {
        Py_XSETREF(state->rows_iter, Py_NewRef(value));
        Py_CLEAR(state->rows_source);
        state->phase = CSV_WRITER_WAIT_ROWS_NEXT;
    }
    else {
        Py_XSETREF(state->field_iter, Py_NewRef(value));
        Py_CLEAR(state->row_source);
        csv_writer_reset_record(state);
        state->phase = CSV_WRITER_WAIT_ROW_NEXT;
    }
    return 0;
}

static int
csv_writer_accept_string(CsvWriterState *state, PyObject *value)
{
    if (!PyUnicode_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__str__ returned non-string (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    int result = csv_writer_append_field(state, value, state->quoted);
    Py_CLEAR(state->field);
    if (result < 0) {
        return -1;
    }
    state->phase = CSV_WRITER_WAIT_ROW_NEXT;
    return 0;
}

static int
csv_writer_process_field(CsvWriterState *state, PyObject *field)
{
    CsvDialectObject *dialect = state->writer->dialect;
    switch (dialect->quoting) {
        case QUOTE_NONNUMERIC:
            state->quoted = !PyNumber_Check(field);
            break;
        case QUOTE_ALL:
            state->quoted = 1;
            break;
        case QUOTE_STRINGS:
            state->quoted = PyUnicode_Check(field);
            break;
        case QUOTE_NOTNULL:
            state->quoted = field != Py_None;
            break;
        default:
            state->quoted = 0;
            break;
    }
    state->null_field = field == Py_None;
    if (PyUnicode_Check(field)) {
        int result = csv_writer_append_field(state, field, state->quoted);
        Py_DECREF(field);
        return result;
    }
    if (field == Py_None) {
        int result = csv_writer_append_field(state, NULL, state->quoted);
        Py_DECREF(field);
        return result;
    }
    Py_XSETREF(state->field, field);
    state->phase = CSV_WRITER_WAIT_FIELD_STR;
    if (csv_writer_export_record(state) < 0) return -1;
    PyObject *string = PyObject_Str(field);
    if (csv_writer_import_after_callback(state) < 0) {
        Py_XDECREF(string);
        return -1;
    }
    if (string == NULL) {
        return -1;
    }
    int result = csv_writer_accept_string(state, string);
    Py_DECREF(string);
    return result;
}

static PyObject *
csv_writer_finish_row(CsvWriterState *state)
{
    if (state->num_fields > 0 && state->record_len == 0) {
        CsvDialectObject *dialect = state->writer->dialect;
        if (dialect->quoting == QUOTE_NONE ||
            (state->null_field &&
             (dialect->quoting == QUOTE_STRINGS ||
              dialect->quoting == QUOTE_NOTNULL))) {
            PyErr_Format(
                state->writer->error_obj,
                "single empty field record must be quoted"
            );
            return NULL;
        }
        state->num_fields--;
        if (csv_writer_append_field(state, NULL, 1) < 0) return NULL;
    }
    if (csv_writer_append_lineterminator(state) < 0) return NULL;
    PyObject *line = PyUnicode_FromKindAndData(
        PyUnicode_4BYTE_KIND,
        state->record,
        state->record_len
    );
    if (line == NULL) return NULL;
    state->phase = CSV_WRITER_WAIT_WRITE;
    if (csv_writer_export_record(state) < 0) {
        Py_DECREF(line);
        return NULL;
    }
    PyObject *result = PyObject_CallOneArg(state->writer->write, line);
    Py_DECREF(line);
    if (csv_writer_import_after_callback(state) < 0) {
        Py_XDECREF(result);
        return NULL;
    }
    return result;
}

static PyObject *
csv_writer_continue(
    CsvWriterState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            if (state->phase == CSV_WRITER_WAIT_ROWS_NEXT &&
                PyErr_ExceptionMatches(PyExc_StopIteration)) {
                PyErr_Clear();
                Py_RETURN_NONE;
            }
            if (state->phase == CSV_WRITER_WAIT_ROW_NEXT &&
                PyErr_ExceptionMatches(PyExc_StopIteration)) {
                PyErr_Clear();
                PyObject *result = csv_writer_finish_row(state);
                if (result == NULL || state->mode == CSV_WRITEROW) {
                    return result;
                }
                Py_DECREF(result);
                Py_CLEAR(state->field_iter);
                state->phase = CSV_WRITER_WAIT_ROWS_NEXT;
            }
            else {
                if (state->phase == CSV_WRITER_WAIT_ROW_ITER &&
                    PyErr_ExceptionMatches(PyExc_TypeError)) {
                    PyErr_Format(
                        state->writer->error_obj,
                        "iterable expected, not %.200s",
                        Py_TYPE(state->row_source)->tp_name
                    );
                }
                return NULL;
            }
        }
        else {
        switch (state->phase) {
            case CSV_WRITER_WAIT_ROWS_ITER:
                if (csv_writer_accept_iterator(state, resumed_value, 1) < 0) {
                    return NULL;
                }
                break;
            case CSV_WRITER_WAIT_ROWS_NEXT:
                Py_XSETREF(state->row_source, Py_NewRef(resumed_value));
                state->phase = CSV_WRITER_WAIT_ROW_ITER;
                break;
            case CSV_WRITER_WAIT_ROW_ITER:
                if (csv_writer_accept_iterator(state, resumed_value, 0) < 0) {
                    return NULL;
                }
                break;
            case CSV_WRITER_WAIT_ROW_NEXT:
                if (csv_writer_process_field(
                        state,
                        Py_NewRef(resumed_value)
                    ) < 0) {
                    return NULL;
                }
                break;
            case CSV_WRITER_WAIT_FIELD_STR:
                if (csv_writer_accept_string(state, resumed_value) < 0) {
                    return NULL;
                }
                break;
            case CSV_WRITER_WAIT_WRITE:
                if (state->mode == CSV_WRITEROW) {
                    return Py_NewRef(resumed_value);
                }
                Py_CLEAR(state->field_iter);
                state->phase = CSV_WRITER_WAIT_ROWS_NEXT;
                break;
            default:
                PyErr_SetString(PyExc_RuntimeError, "invalid csv writer state");
                return NULL;
        }
        }
    }

    for (;;) {
        if (state->phase == CSV_WRITER_WAIT_ROWS_ITER) {
            if (csv_writer_export_record(state) < 0) return NULL;
            PyObject *iterator = PyObject_GetIter(state->rows_source);
            if (csv_writer_import_after_callback(state) < 0) {
                Py_XDECREF(iterator);
                return NULL;
            }
            if (iterator == NULL) {
                return NULL;
            }
            int result = csv_writer_accept_iterator(state, iterator, 1);
            Py_DECREF(iterator);
            if (result < 0) {
                return NULL;
            }
        }
        else if (state->phase == CSV_WRITER_WAIT_ROWS_NEXT) {
            if (csv_writer_export_record(state) < 0) return NULL;
            PyObject *row = PyIter_Next(state->rows_iter);
            if (csv_writer_import_after_callback(state) < 0) {
                Py_XDECREF(row);
                return NULL;
            }
            if (row == NULL) {
                if (PyErr_Occurred()) {
                    return NULL;
                }
                Py_RETURN_NONE;
            }
            Py_XSETREF(state->row_source, row);
            state->phase = CSV_WRITER_WAIT_ROW_ITER;
        }
        else if (state->phase == CSV_WRITER_WAIT_ROW_ITER) {
            if (csv_writer_export_record(state) < 0) return NULL;
            PyObject *iterator = PyObject_GetIter(state->row_source);
            if (csv_writer_import_after_callback(state) < 0) {
                Py_XDECREF(iterator);
                return NULL;
            }
            if (iterator == NULL) {
                if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                    PyErr_Format(
                        state->writer->error_obj,
                        "iterable expected, not %.200s",
                        Py_TYPE(state->row_source)->tp_name
                    );
                }
                return NULL;
            }
            int result = csv_writer_accept_iterator(state, iterator, 0);
            Py_DECREF(iterator);
            if (result < 0) {
                return NULL;
            }
        }
        else if (state->phase == CSV_WRITER_WAIT_ROW_NEXT) {
            if (csv_writer_export_record(state) < 0) return NULL;
            PyObject *field = PyIter_Next(state->field_iter);
            if (csv_writer_import_after_callback(state) < 0) {
                Py_XDECREF(field);
                return NULL;
            }
            if (field != NULL) {
                if (csv_writer_process_field(state, field) < 0) {
                    return NULL;
                }
                continue;
            }
            if (PyErr_Occurred()) {
                return NULL;
            }
            PyObject *result = csv_writer_finish_row(state);
            if (result == NULL) {
                return NULL;
            }
            if (state->mode == CSV_WRITEROW) {
                return result;
            }
            Py_DECREF(result);
            Py_CLEAR(state->field_iter);
            state->phase = CSV_WRITER_WAIT_ROWS_NEXT;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid csv writer state");
            return NULL;
        }
    }
}

static PyObject *
csv_writer_resume(const void *raw_state, PyObject *value)
{
    PyObject *raised = PyErr_GetRaisedException();
    CsvWriterState *state = csv_writer_copy_state(raw_state);
    if (state == NULL) {
        Py_XDECREF(raised);
        return NULL;
    }
    PyErr_SetRaisedException(raised);
    if (csv_writer_import_after_callback(state) < 0) {
        csv_writer_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &csv_writer_vtable, state) < 0) {
        csv_writer_free_state(state);
        return NULL;
    }
    PyObject *result = csv_writer_continue(state, value, 1);
    adapter_leave(&frame);
    csv_writer_free_state(state);
    return result;
}

static const AleffAdapterVTable csv_writer_vtable = {
    .copy_state = csv_writer_copy_state,
    .free_state = csv_writer_free_state,
    .resume = csv_writer_resume,
    .prepare_resume = csv_writer_prepare_resume,
};

static PyObject *
adapter_csv_writerow_impl(PyObject *self, PyObject *row)
{
    CsvWriterState state = {
        .writer = (CsvWriterObject *)Py_NewRef(self),
        .row_source = Py_NewRef(row),
        .mode = CSV_WRITEROW,
        .phase = CSV_WRITER_WAIT_ROW_ITER,
    };
    if (csv_writer_import_record(&state) < 0) {
        csv_writer_clear_state(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &csv_writer_vtable, &state) < 0) {
        csv_writer_clear_state(&state);
        return NULL;
    }
    PyObject *result = csv_writer_continue(&state, NULL, 0);
    adapter_leave(&frame);
    csv_writer_clear_state(&state);
    return result;
}

static PyObject *
adapter_csv_writerows_impl(PyObject *self, PyObject *rows)
{
    CsvWriterState state = {
        .writer = (CsvWriterObject *)Py_NewRef(self),
        .rows_source = Py_NewRef(rows),
        .mode = CSV_WRITEROWS,
        .phase = CSV_WRITER_WAIT_ROWS_ITER,
    };
    if (csv_writer_import_record(&state) < 0) {
        csv_writer_clear_state(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &csv_writer_vtable, &state) < 0) {
        csv_writer_clear_state(&state);
        return NULL;
    }
    PyObject *result = csv_writer_continue(&state, NULL, 0);
    adapter_leave(&frame);
    csv_writer_clear_state(&state);
    return result;
}

static PyObject *
adapter_csv_writerow(PyObject *self, PyObject *row)
{
#if PY_VERSION_HEX >= 0x030e0000
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(self);
    result = adapter_csv_writerow_impl(self, row);
    Py_END_CRITICAL_SECTION();
    return result;
#else
    return adapter_csv_writerow_impl(self, row);
#endif
}

static PyObject *
adapter_csv_writerows(PyObject *self, PyObject *rows)
{
#if PY_VERSION_HEX >= 0x030e0000
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(self);
    result = adapter_csv_writerows_impl(self, rows);
    Py_END_CRITICAL_SECTION();
    return result;
#else
    return adapter_csv_writerows_impl(self, rows);
#endif
}

#undef CSV_RECORD_INCREMENT

static PyObject *
adapter_csv_reader_next_method(
    PyObject *self,
    PyObject *Py_UNUSED(arguments)
)
{
    return adapter_csv_reader_next(self);
}

static int
csv_install_method(
    PyTypeObject *type,
    const char *name,
    PyObject *original,
    PyMethodDef *method,
    PyCFunction function
)
{
    if (!Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        PyErr_Format(PyExc_RuntimeError, "_csv.writer.%s is not a C method", name);
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
    int status = dict == NULL ? -1 : aleff_adapter_register_callable(descriptor);
    if (status == 0) {
        status = PyDict_SetItemString(dict, name, descriptor);
    }
    Py_XDECREF(dict);
    Py_DECREF(descriptor);
    if (status == 0) {
        PyType_Modified(type);
    }
    return status;
}

int
adapter_csv_install(PyObject *csv_module)
{
    if (csv_installed) {
        return 0;
    }
    PyObject *reader = PyObject_GetAttrString(csv_module, "Reader");
    PyObject *writer = PyObject_GetAttrString(csv_module, "Writer");
    if (reader == NULL || writer == NULL ||
        !PyType_Check(reader) || !PyType_Check(writer)) {
        Py_XDECREF(reader);
        Py_XDECREF(writer);
        PyErr_SetString(PyExc_RuntimeError, "cannot access _csv reader/writer types");
        return -1;
    }

    installed_reader_type = (PyTypeObject *)Py_NewRef(reader);
    installed_writer_type = (PyTypeObject *)Py_NewRef(writer);
    installed_csv_module = Py_NewRef(csv_module);
    installed_csv_error = PyObject_GetAttrString(csv_module, "Error");
    original_reader_next = installed_reader_type->tp_iternext;
    PyObject *reader_dict = PyType_GetDict(installed_reader_type);
    original_reader_next_method = reader_dict == NULL
        ? NULL : Py_XNewRef(PyDict_GetItemString(reader_dict, "__next__"));
    PyObject *writer_dict = PyType_GetDict(installed_writer_type);
    original_writerow = writer_dict == NULL
        ? NULL : Py_XNewRef(PyDict_GetItemString(writer_dict, "writerow"));
    original_writerows = writer_dict == NULL
        ? NULL : Py_XNewRef(PyDict_GetItemString(writer_dict, "writerows"));
    Py_XDECREF(reader_dict);
    Py_XDECREF(writer_dict);
    if (installed_csv_error == NULL || original_reader_next == NULL ||
        original_reader_next_method == NULL ||
        original_writerow == NULL ||
        original_writerows == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "cannot access _csv writer methods");
        }
        adapter_csv_rollback();
        Py_DECREF(reader);
        Py_DECREF(writer);
        return -1;
    }

    installed_reader_type->tp_iternext = adapter_csv_reader_next;
    PyType_Modified(installed_reader_type);
    reader_next_method = (PyMethodDef){
        .ml_name = "__next__",
        .ml_meth = adapter_csv_reader_next_method,
        .ml_flags = METH_NOARGS,
        .ml_doc = NULL,
    };
    PyObject *reader_next_descriptor = PyDescr_NewMethod(
        installed_reader_type,
        &reader_next_method
    );
    if (reader_next_descriptor == NULL ||
        PyDict_SetItemString(
            reader_dict,
            "__next__",
            reader_next_descriptor
        ) < 0) {
        Py_XDECREF(reader_next_descriptor);
        adapter_csv_rollback();
        Py_DECREF(reader);
        Py_DECREF(writer);
        return -1;
    }
    Py_DECREF(reader_next_descriptor);
    PyType_Modified(installed_reader_type);
    if (csv_install_method(
            installed_writer_type,
            "writerow",
            original_writerow,
            &writerow_method,
            (PyCFunction)adapter_csv_writerow
        ) < 0 ||
        csv_install_method(
            installed_writer_type,
            "writerows",
            original_writerows,
            &writerows_method,
            (PyCFunction)adapter_csv_writerows
        ) < 0) {
        adapter_csv_rollback();
        Py_DECREF(reader);
        Py_DECREF(writer);
        return -1;
    }
    csv_installed = 1;
    Py_DECREF(reader);
    Py_DECREF(writer);
    return 0;
}

void
adapter_csv_rollback(void)
{
    if (installed_reader_type != NULL && original_reader_next != NULL) {
        installed_reader_type->tp_iternext = original_reader_next;
        PyObject *dict = PyType_GetDict(installed_reader_type);
        if (dict != NULL && original_reader_next_method != NULL &&
            PyDict_SetItemString(
                dict,
                "__next__",
                original_reader_next_method
            ) < 0) {
            PyErr_Clear();
        }
        Py_XDECREF(dict);
        PyType_Modified(installed_reader_type);
    }
    if (installed_writer_type != NULL) {
        PyObject *dict = PyType_GetDict(installed_writer_type);
        if (dict != NULL) {
            if (original_writerow != NULL &&
                PyDict_SetItemString(dict, "writerow", original_writerow) < 0) {
                PyErr_Clear();
            }
            if (original_writerows != NULL &&
                PyDict_SetItemString(dict, "writerows", original_writerows) < 0) {
                PyErr_Clear();
            }
            PyType_Modified(installed_writer_type);
        }
        Py_XDECREF(dict);
    }
    Py_XDECREF(original_writerow);
    Py_XDECREF(original_writerows);
    Py_XDECREF(original_reader_next_method);
    Py_XDECREF(installed_csv_module);
    Py_XDECREF(installed_csv_error);
    Py_XDECREF((PyObject *)installed_reader_type);
    Py_XDECREF((PyObject *)installed_writer_type);
    original_writerow = NULL;
    original_writerows = NULL;
    original_reader_next_method = NULL;
    installed_csv_module = NULL;
    installed_csv_error = NULL;
    installed_reader_type = NULL;
    installed_writer_type = NULL;
    original_reader_next = NULL;
    csv_installed = 0;
}
