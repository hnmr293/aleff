#include "io_text.h"

#ifndef MS_WINDOWS
#  include <sys/types.h>
#endif
#include <stdio.h>
#include <limits.h>
#include <string.h>

#ifdef MS_WINDOWS
typedef long long AleffTextOff_t;
#else
typedef off_t AleffTextOff_t;
#endif

/* CPython keeps TextIOWrapper's structure private.  This is its stable
 * 3.12--3.14 layout; PyObject_HEAD remains version-specific for 3.14t. */
typedef PyObject *(*text_encodefunc)(PyObject *, PyObject *);
typedef struct {
    PyObject_HEAD
    int ok;
    int detached;
    Py_ssize_t chunk_size;
    PyObject *buffer;
    PyObject *encoding;
    PyObject *encoder;
    PyObject *decoder;
    PyObject *readnl;
    PyObject *errors;
    const char *writenl;
    char line_buffering;
    char write_through;
    char readuniversal;
    char readtranslate;
    char writetranslate;
    char seekable;
    char has_read1;
    char telling;
    char finalizing;
    text_encodefunc encodefunc;
    char encoding_start_of_stream;
    PyObject *decoded_chars;
    Py_ssize_t decoded_chars_used;
    PyObject *pending_bytes;
    Py_ssize_t pending_bytes_count;
    PyObject *snapshot;
    double b2cratio;
    PyObject *raw;
    PyObject *weakreflist;
    PyObject *dict;
    PyObject *state;
} AleffTextIO;

typedef enum {
    TEXT_READ,
    TEXT_READLINE,
    TEXT_WRITE,
    TEXT_SEEK,
    TEXT_TELL,
    TEXT_FLUSH,
    TEXT_CLOSE,
    TEXT_ITER,
} TextOperation;

typedef enum {
    TEXT_PHASE_READ_ALL,
    TEXT_PHASE_READ_CHUNK,
    TEXT_PHASE_READ_AFTER_WRITE,
    TEXT_PHASE_LINE_CHUNK,
    TEXT_PHASE_LINE_AFTER_WRITE,
    TEXT_PHASE_WRITE_ENCODE,
    TEXT_PHASE_WRITE_BUFFER,
    TEXT_PHASE_WRITE_FLUSH,
    TEXT_PHASE_WRITE_RESET,
    TEXT_PHASE_SEEK_TELL,
    TEXT_PHASE_SEEK_BUFFER,
    TEXT_PHASE_SEEK_FLUSH,
    TEXT_PHASE_SEEK_RESET,
    TEXT_PHASE_SEEK_RESTORE_READ,
    TEXT_PHASE_SEEK_RESTORE_DECODE,
    TEXT_PHASE_SEEK_ENCODER_RESET,
    TEXT_PHASE_TELL_BUFFER,
    TEXT_PHASE_TELL_FLUSH,
    TEXT_PHASE_FLUSH_BUFFER,
    TEXT_PHASE_FLUSH_WRITE,
    TEXT_PHASE_CLOSE_BUFFER,
} TextPhase;

typedef struct {
    PyObject *original;
    PyObject *receiver;
    PyObject *arguments;
    PyObject *saved_buffer;
    PyObject *buffer_state;
    PyObject *saved_decoder;
    PyObject *saved_encoder;
    PyObject *saved_decoded_chars;
    PyObject *saved_pending_bytes;
    PyObject *saved_snapshot;
    PyObject *decoder_state;
    PyObject *encoder_state;
    PyObject *line;
    PyObject *result;
    PyObject *seek_cookie;
    PyObject *seek_start;
    PyObject *seek_flags;
    PyObject *seek_input;
    PyObject *close_exception;
    Py_ssize_t saved_decoded_chars_used;
    Py_ssize_t saved_pending_bytes_count;
    Py_ssize_t read_size;
    Py_ssize_t text_length;
    int seek_bytes_to_feed;
    int seek_chars_to_skip;
    int seek_need_eof;
    double saved_b2cratio;
    char saved_telling;
    char saved_encoding_start_of_stream;
    int need_flush;
    int write_through;
    int operation;
    int phase;
    int snapshot_state;
    int nested_parent;
    int close_buffer_started;
    int close_flush_expected;
    int close_flush_done;
} TextCallState;

typedef struct {
    PyTypeObject *type;
    const char *name;
    PyObject *original;
} TextMethodBackup;

static const AleffAdapterVTable text_call_vtable;
static PyObject *installed_io;
static PyObject *installed_text_type;
static TextMethodBackup backups[7];
static PyMethodDef replacement_methods[7];
static Py_ssize_t backup_count;
static iternextfunc original_text_next;
static int text_installed;
static int text_nested_marker;
static const char text_nested_marker_name[] = "aleff.text.nested";
#define TEXT_NESTED_SEEK_TELL 1
#define TEXT_NESTED_TELL_FLUSH 2
#define TEXT_NESTED_SEEK_FLUSH 3
#define TEXT_NESTED_ITER_READLINE 4
#define TEXT_NESTED_CLOSE_FLUSH 5

static AleffTextIO *
text_object(PyObject *object)
{
    return (AleffTextIO *)object;
}

static PyObject *
text_codec_state(PyObject *codec)
{
    PyObject *name = PyUnicode_FromString("getstate");
    PyObject *state;
    if (name == NULL) {
        return NULL;
    }
    state = PyObject_CallMethodObjArgs(codec, name, NULL);
    Py_DECREF(name);
    return state;
}

static int
text_restore_codec_state(PyObject *codec, PyObject *state)
{
    PyObject *name;
    PyObject *result;
    if (codec == NULL || state == NULL) {
        return 0;
    }
    name = PyUnicode_FromString("setstate");
    if (name == NULL) {
        return -1;
    }
    result = PyObject_CallMethodObjArgs(codec, name, state, NULL);
    Py_DECREF(name);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static void
text_clear_state(TextCallState *state)
{
    Py_XDECREF(state->original);
    Py_XDECREF(state->receiver);
    Py_XDECREF(state->arguments);
    Py_XDECREF(state->saved_buffer);
    Py_XDECREF(state->buffer_state);
    Py_XDECREF(state->saved_decoder);
    Py_XDECREF(state->saved_encoder);
    Py_XDECREF(state->saved_decoded_chars);
    Py_XDECREF(state->saved_pending_bytes);
    Py_XDECREF(state->saved_snapshot);
    Py_XDECREF(state->decoder_state);
    Py_XDECREF(state->encoder_state);
    Py_XDECREF(state->line);
    Py_XDECREF(state->result);
    Py_XDECREF(state->seek_cookie);
    Py_XDECREF(state->seek_start);
    Py_XDECREF(state->seek_flags);
    Py_XDECREF(state->seek_input);
    Py_XDECREF(state->close_exception);
    memset(state, 0, sizeof(*state));
}

static void *
text_copy_state(const void *raw_state)
{
    const TextCallState *source = raw_state;
    TextCallState *copy = PyMem_Calloc(1, sizeof(*copy));
    AleffTextIO *receiver;
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->original = Py_XNewRef(source->original);
    copy->receiver = Py_XNewRef(source->receiver);
    copy->arguments = Py_XNewRef(source->arguments);
    copy->line = Py_XNewRef(source->line);
    copy->result = Py_XNewRef(source->result);
    copy->seek_cookie = Py_XNewRef(source->seek_cookie);
    copy->seek_start = Py_XNewRef(source->seek_start);
    copy->seek_flags = Py_XNewRef(source->seek_flags);
    copy->seek_input = Py_XNewRef(source->seek_input);
    copy->close_exception = Py_XNewRef(source->close_exception);
    copy->read_size = source->read_size;
    copy->text_length = source->text_length;
    copy->seek_bytes_to_feed = source->seek_bytes_to_feed;
    copy->seek_chars_to_skip = source->seek_chars_to_skip;
    copy->seek_need_eof = source->seek_need_eof;
    copy->need_flush = source->need_flush;
    copy->write_through = source->write_through;
    copy->operation = source->operation;
    copy->phase = source->phase;
    copy->nested_parent = source->nested_parent;
    copy->close_buffer_started = source->close_buffer_started;
    copy->close_flush_expected = source->close_flush_expected;
    copy->close_flush_done = source->close_flush_done;
    copy->snapshot_state = 1;
    if (source->snapshot_state) {
        copy->saved_buffer = Py_XNewRef(source->saved_buffer);
        copy->buffer_state = source->buffer_state == NULL
            ? NULL : PyDict_Copy(source->buffer_state);
        if (source->buffer_state != NULL && copy->buffer_state == NULL) {
            text_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
        copy->saved_decoder = Py_XNewRef(source->saved_decoder);
        copy->saved_encoder = Py_XNewRef(source->saved_encoder);
        copy->saved_decoded_chars = Py_XNewRef(source->saved_decoded_chars);
        if (source->saved_pending_bytes != NULL &&
            PyList_Check(source->saved_pending_bytes)) {
            copy->saved_pending_bytes =
                PySequence_List(source->saved_pending_bytes);
        }
        else {
            copy->saved_pending_bytes = Py_XNewRef(source->saved_pending_bytes);
        }
        if (source->saved_pending_bytes != NULL &&
            copy->saved_pending_bytes == NULL) {
            text_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
        copy->saved_snapshot = Py_XNewRef(source->saved_snapshot);
        copy->decoder_state = Py_XNewRef(source->decoder_state);
        copy->encoder_state = Py_XNewRef(source->encoder_state);
        copy->saved_decoded_chars_used = source->saved_decoded_chars_used;
        copy->saved_pending_bytes_count = source->saved_pending_bytes_count;
        copy->saved_b2cratio = source->saved_b2cratio;
        copy->saved_telling = source->saved_telling;
        copy->saved_encoding_start_of_stream =
            source->saved_encoding_start_of_stream;
        return copy;
    }
    receiver = text_object(source->receiver);
    copy->saved_buffer = Py_XNewRef(receiver->buffer);
    if (receiver->buffer != NULL) {
        PyObject *dictionary = PyObject_GetAttrString(
            receiver->buffer, "__dict__"
        );
        if (dictionary == NULL) {
            PyErr_Clear();
        }
        else if (PyDict_Check(dictionary)) {
            copy->buffer_state = PyDict_Copy(dictionary);
            Py_DECREF(dictionary);
            if (copy->buffer_state == NULL) {
                text_clear_state(copy);
                PyMem_Free(copy);
                return NULL;
            }
        }
        else {
            Py_DECREF(dictionary);
        }
    }
    copy->saved_decoder = Py_XNewRef(receiver->decoder);
    copy->saved_encoder = Py_XNewRef(receiver->encoder);
    copy->saved_decoded_chars = Py_XNewRef(receiver->decoded_chars);
    if (receiver->pending_bytes != NULL &&
        PyList_Check(receiver->pending_bytes)) {
        copy->saved_pending_bytes = PySequence_List(receiver->pending_bytes);
    }
    else {
        copy->saved_pending_bytes = Py_XNewRef(receiver->pending_bytes);
    }
    if (receiver->pending_bytes != NULL && copy->saved_pending_bytes == NULL) {
        text_clear_state(copy);
        PyMem_Free(copy);
        return NULL;
    }
    copy->saved_snapshot = Py_XNewRef(receiver->snapshot);
    copy->saved_decoded_chars_used = receiver->decoded_chars_used;
    copy->saved_pending_bytes_count = receiver->pending_bytes_count;
    copy->saved_b2cratio = receiver->b2cratio;
    copy->saved_telling = receiver->telling;
    copy->saved_encoding_start_of_stream = receiver->encoding_start_of_stream;
    if (receiver->decoder != NULL) {
        copy->decoder_state = text_codec_state(receiver->decoder);
        if (copy->decoder_state == NULL) {
            text_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
    }
    if (receiver->encoder != NULL) {
        copy->encoder_state = text_codec_state(receiver->encoder);
        if (copy->encoder_state == NULL) {
            text_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
    }
    return copy;
}

static void
text_free_state(void *raw_state)
{
    TextCallState *state = raw_state;
    if (state == NULL) {
        return;
    }
    text_clear_state(state);
    PyMem_Free(state);
}

static void
text_set_ref(PyObject **target, PyObject *value)
{
    Py_XINCREF(value);
    Py_XDECREF(*target);
    *target = value;
}

static PyObject *
text_wrap_nested(int parent, PyObject *value)
{
    PyObject *marker;
    PyObject *parent_value;
    PyObject *result;
    marker = PyCapsule_New(&text_nested_marker, text_nested_marker_name, NULL);
    parent_value = PyLong_FromLong(parent);
    if (marker == NULL || parent_value == NULL) {
        Py_XDECREF(marker);
        Py_XDECREF(parent_value);
        return NULL;
    }
    result = PyTuple_Pack(3, marker, parent_value, value);
    Py_DECREF(marker);
    Py_DECREF(parent_value);
    return result;
}

static PyObject *
text_unwrap_nested(PyObject *value, int parent)
{
    if (PyTuple_Check(value) && PyTuple_GET_SIZE(value) == 3 &&
        PyCapsule_IsValid(PyTuple_GET_ITEM(value, 0), text_nested_marker_name) &&
        PyLong_Check(PyTuple_GET_ITEM(value, 1)) &&
        PyLong_AsLong(PyTuple_GET_ITEM(value, 1)) == parent) {
        return Py_NewRef(PyTuple_GET_ITEM(value, 2));
    }
    return NULL;
}

static PyObject *
text_cookie_build(PyObject *start, PyObject *flags, int bytes_to_feed,
                  int chars_to_skip, int need_eof)
{
    PyObject *result = PyNumber_Index(start);
    PyObject *component;
    PyObject *shift;
    PyObject *combined;
    int offset = (int)(sizeof(AleffTextOff_t) * CHAR_BIT);
    int fields[] = {bytes_to_feed, chars_to_skip, need_eof};
    if (result == NULL) {
        return NULL;
    }
    component = PyNumber_Index(flags);
    if (component == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    shift = PyLong_FromLong(offset);
    if (shift == NULL) {
        Py_DECREF(component);
        Py_DECREF(result);
        return NULL;
    }
    combined = PyNumber_Lshift(component, shift);
    Py_DECREF(component);
    Py_DECREF(shift);
    if (combined == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    component = PyNumber_Or(result, combined);
    Py_DECREF(result);
    Py_DECREF(combined);
    result = component;
    if (result == NULL) {
        return NULL;
    }
    offset += (int)(sizeof(int) * CHAR_BIT);
    for (int index = 0; index < 3; index++) {
        component = PyLong_FromLong(fields[index]);
        shift = PyLong_FromLong(offset);
        if (component == NULL || shift == NULL) {
            Py_XDECREF(component);
            Py_XDECREF(shift);
            Py_DECREF(result);
            return NULL;
        }
        combined = PyNumber_Lshift(component, shift);
        Py_DECREF(component);
        Py_DECREF(shift);
        if (combined == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        component = PyNumber_Or(result, combined);
        Py_DECREF(result);
        Py_DECREF(combined);
        result = component;
        if (result == NULL) {
            return NULL;
        }
        offset += (int)(sizeof(int) * CHAR_BIT);
    }
    return result;
}

static int
text_cookie_parse(TextCallState *state)
{
    PyObject *cookie = PyNumber_Index(state->seek_cookie);
    PyObject *one;
    PyObject *shift;
    PyObject *mask;
    PyObject *part;
    long field;
    const int start_bits = (int)(sizeof(AleffTextOff_t) * CHAR_BIT);
    const int int_bits = (int)(sizeof(int) * CHAR_BIT);
    if (cookie == NULL) {
        return -1;
    }
    one = PyLong_FromLong(1);
    shift = PyLong_FromLong(start_bits);
    if (one == NULL || shift == NULL) {
        Py_XDECREF(one);
        Py_XDECREF(shift);
        Py_DECREF(cookie);
        return -1;
    }
    mask = PyNumber_Lshift(one, shift);
    Py_DECREF(shift);
    if (mask == NULL) {
        Py_DECREF(one);
        Py_DECREF(cookie);
        return -1;
    }
    shift = PyNumber_Subtract(mask, one);
    Py_DECREF(mask);
    Py_DECREF(one);
    if (shift == NULL) {
        Py_DECREF(cookie);
        return -1;
    }
    Py_XSETREF(state->seek_start, PyNumber_And(cookie, shift));
    Py_DECREF(shift);
    if (state->seek_start == NULL) {
        Py_DECREF(cookie);
        return -1;
    }
    for (int index = 0; index < 4; index++) {
        PyObject *word_shift = PyLong_FromLong(
            start_bits + index * int_bits);
        PyObject *word;
        if (word_shift == NULL) {
            Py_DECREF(cookie);
            return -1;
        }
        word = PyNumber_Rshift(cookie, word_shift);
        Py_DECREF(word_shift);
        if (word == NULL) {
            Py_DECREF(cookie);
            return -1;
        }
        one = PyLong_FromLong(1);
        shift = PyLong_FromLong(int_bits);
        if (one == NULL || shift == NULL) {
            Py_XDECREF(one);
            Py_XDECREF(shift);
            Py_DECREF(word);
            Py_DECREF(cookie);
            return -1;
        }
        mask = PyNumber_Lshift(one, shift);
        Py_DECREF(one);
        Py_DECREF(shift);
        if (mask == NULL) {
            Py_DECREF(word);
            Py_DECREF(cookie);
            return -1;
        }
        one = PyLong_FromLong(1);
        if (one == NULL) {
            Py_DECREF(mask);
            Py_DECREF(word);
            Py_DECREF(cookie);
            return -1;
        }
        shift = PyNumber_Subtract(mask, one);
        Py_DECREF(mask);
        Py_DECREF(one);
        if (shift == NULL) {
            Py_DECREF(word);
            Py_DECREF(cookie);
            return -1;
        }
        part = PyNumber_And(word, shift);
        Py_DECREF(shift);
        Py_DECREF(word);
        if (part == NULL) {
            Py_DECREF(cookie);
            return -1;
        }
        field = PyLong_AsLong(part);
        if (field == -1 && PyErr_Occurred()) {
            Py_DECREF(part);
            Py_DECREF(cookie);
            return -1;
        }
        if (index == 0) {
            Py_XSETREF(state->seek_flags, part);
        }
        else {
            Py_DECREF(part);
            if (index == 1) {
                state->seek_bytes_to_feed = (int)field;
            }
            else if (index == 2) {
                state->seek_chars_to_skip = (int)field;
            }
            else {
                state->seek_need_eof = (int)field;
            }
        }
    }
    Py_DECREF(cookie);
    return 0;
}

static int
text_restore_state(TextCallState *state)
{
    AleffTextIO *receiver = text_object(state->receiver);
    text_set_ref(&receiver->buffer, state->saved_buffer);
    text_set_ref(&receiver->decoder, state->saved_decoder);
    text_set_ref(&receiver->encoder, state->saved_encoder);
    text_set_ref(&receiver->decoded_chars, state->saved_decoded_chars);
    text_set_ref(&receiver->pending_bytes, state->saved_pending_bytes);
    text_set_ref(&receiver->snapshot, state->saved_snapshot);
    receiver->decoded_chars_used = state->saved_decoded_chars_used;
    receiver->pending_bytes_count = state->saved_pending_bytes_count;
    receiver->b2cratio = state->saved_b2cratio;
    receiver->telling = state->saved_telling;
    receiver->encoding_start_of_stream = state->saved_encoding_start_of_stream;
    if (text_restore_codec_state(receiver->decoder, state->decoder_state) < 0) {
        return -1;
    }
    return text_restore_codec_state(receiver->encoder, state->encoder_state);
}

static int
text_restore_buffer_state(TextCallState *state)
{
    PyObject *dictionary;
    int result;
    if (state->saved_buffer == NULL || state->buffer_state == NULL) {
        return 0;
    }
    dictionary = PyObject_GetAttrString(state->saved_buffer, "__dict__");
    if (dictionary == NULL) {
        return -1;
    }
    if (!PyDict_Check(dictionary)) {
        Py_DECREF(dictionary);
        return 0;
    }
    PyDict_Clear(dictionary);
    result = PyDict_Update(dictionary, state->buffer_state);
    Py_DECREF(dictionary);
    return result;
}

static int
text_prepare_resume(void *raw_state)
{
    TextCallState *state = raw_state;
    if (text_restore_state(state) < 0) {
        return -1;
    }
    return text_restore_buffer_state(state);
}

static PyObject *
text_buffer_call(AleffTextIO *receiver, const char *name, PyObject *argument)
{
    PyObject *method = PyUnicode_FromString(name);
    PyObject *result;
    if (method == NULL) {
        return NULL;
    }
    result = argument == NULL
        ? PyObject_CallMethodObjArgs(receiver->buffer, method, NULL)
        : PyObject_CallMethodObjArgs(receiver->buffer, method, argument, NULL);
    Py_DECREF(method);
    return result;
}

static PyObject *
text_buffer_seek(AleffTextIO *receiver, PyObject *position, int whence)
{
    PyObject *method = PyUnicode_FromString("seek");
    PyObject *which = PyLong_FromLong(whence);
    PyObject *result;
    if (method == NULL || which == NULL) {
        Py_XDECREF(method);
        Py_XDECREF(which);
        return NULL;
    }
    result = PyObject_CallMethodObjArgs(receiver->buffer, method, position,
                                        which, NULL);
    Py_DECREF(method);
    Py_DECREF(which);
    return result;
}

static PyObject *
text_empty(void)
{
    return PyUnicode_FromStringAndSize(NULL, 0);
}

static PyObject *
text_decoded_get(AleffTextIO *receiver, Py_ssize_t count)
{
    Py_ssize_t available;
    PyObject *result;
    if (receiver->decoded_chars == NULL) {
        return text_empty();
    }
    if (PyUnicode_READY(receiver->decoded_chars) < 0) {
        return NULL;
    }
    available = PyUnicode_GET_LENGTH(receiver->decoded_chars) -
        receiver->decoded_chars_used;
    if (count < 0 || count > available) {
        count = available;
    }
    if (receiver->decoded_chars_used > 0 || count < available) {
        result = PyUnicode_Substring(receiver->decoded_chars,
                                     receiver->decoded_chars_used,
                                     receiver->decoded_chars_used + count);
    }
    else {
        result = Py_NewRef(receiver->decoded_chars);
    }
    if (result != NULL) {
        receiver->decoded_chars_used += count;
    }
    return result;
}

static int
text_append(PyObject **target, PyObject *part)
{
    PyObject *joined;
    if (*target == NULL) {
        *target = Py_NewRef(part);
        return 0;
    }
    joined = PyUnicode_Concat(*target, part);
    if (joined == NULL) {
        return -1;
    }
    Py_SETREF(*target, joined);
    return 0;
}

static int
text_save_read_decoder_state(TextCallState *state, AleffTextIO *receiver)
{
    Py_XDECREF(state->decoder_state);
    state->decoder_state = NULL;
    if (!receiver->telling || receiver->decoder == NULL) {
        return 0;
    }
    state->decoder_state = text_codec_state(receiver->decoder);
    return state->decoder_state == NULL ? -1 : 0;
}

static PyObject *text_read_continue(TextCallState *, PyObject *);
static PyObject *text_line_continue(TextCallState *, PyObject *);
static PyObject *text_write_continue(TextCallState *, PyObject *);
static PyObject *text_seek_continue(TextCallState *, PyObject *);
static PyObject *text_tell_continue(TextCallState *, PyObject *);

static PyObject *
text_read_request(TextCallState *state, Py_ssize_t size_hint)
{
    AleffTextIO *receiver = text_object(state->receiver);
    Py_ssize_t size_value = size_hint > receiver->chunk_size
        ? size_hint : receiver->chunk_size;
    PyObject *size = PyLong_FromSsize_t(size_value);
    PyObject *result;
    if (size == NULL) {
        return NULL;
    }
    if (text_save_read_decoder_state(state, receiver) < 0) {
        Py_DECREF(size);
        return NULL;
    }
    state->phase = state->operation == TEXT_READ
        ? TEXT_PHASE_READ_CHUNK : TEXT_PHASE_LINE_CHUNK;
    result = text_buffer_call(receiver, receiver->has_read1 ? "read1" : "read",
                              size);
    Py_DECREF(size);
    if (result == NULL) {
        return NULL;
    }
    {
        PyObject *processed = state->operation == TEXT_READ
            ? text_read_continue(state, result)
            : text_line_continue(state, result);
        Py_DECREF(result);
        return processed;
    }
}

static PyObject *
text_decode_chunk(TextCallState *state, PyObject *value, int *eof)
{
    AleffTextIO *receiver = text_object(state->receiver);
    Py_buffer view;
    PyObject *name;
    PyObject *final;
    PyObject *decoded;
    Py_ssize_t nbytes;
    if (PyObject_GetBuffer(value, &view, 0) < 0) {
        PyErr_Format(PyExc_TypeError,
                     "underlying %s() should have returned a bytes-like object, not '%.200s'",
                     receiver->has_read1 ? "read1" : "read",
                     Py_TYPE(value)->tp_name);
        return NULL;
    }
    nbytes = view.len;
    *eof = nbytes == 0;
    PyBuffer_Release(&view);
    name = PyUnicode_FromString("decode");
    final = PyBool_FromLong(*eof);
    if (name == NULL || final == NULL) {
        Py_XDECREF(name);
        Py_XDECREF(final);
        return NULL;
    }
    decoded = PyObject_CallMethodObjArgs(receiver->decoder, name, value, final,
                                         NULL);
    Py_DECREF(name);
    Py_DECREF(final);
    if (decoded == NULL) {
        return NULL;
    }
    if (!PyUnicode_Check(decoded) || PyUnicode_READY(decoded) < 0) {
        PyErr_Format(PyExc_TypeError,
                     "decoder should return a string result, not '%.200s'",
                     Py_TYPE(decoded)->tp_name);
        Py_DECREF(decoded);
        return NULL;
    }
    Py_XSETREF(receiver->decoded_chars, decoded);
    receiver->decoded_chars_used = 0;
    if (PyUnicode_GET_LENGTH(decoded) > 0) {
        receiver->b2cratio = (double)nbytes / PyUnicode_GET_LENGTH(decoded);
        *eof = 0;
    }
    else {
        receiver->b2cratio = 0.0;
    }
    if (receiver->telling && state->decoder_state != NULL) {
        PyObject *buffered;
        PyObject *flags;
        PyObject *next_input;
        PyObject *snapshot;
        PyObject *input_bytes;
        if (!PyTuple_Check(state->decoder_state) ||
            !PyArg_ParseTuple(state->decoder_state, "OO", &buffered, &flags) ||
            !PyBytes_Check(buffered)) {
            PyErr_SetString(PyExc_TypeError, "illegal decoder state");
            return NULL;
        }
        next_input = Py_NewRef(buffered);
        input_bytes = PyBytes_FromObject(value);
        if (input_bytes == NULL) {
            Py_DECREF(next_input);
            return NULL;
        }
        PyBytes_Concat(&next_input, input_bytes);
        Py_DECREF(input_bytes);
        if (next_input == NULL) {
            return NULL;
        }
        snapshot = PyTuple_Pack(2, flags, next_input);
        Py_DECREF(next_input);
        if (snapshot == NULL) {
            return NULL;
        }
        Py_XSETREF(receiver->snapshot, snapshot);
    }
    return Py_NewRef(receiver->decoded_chars);
}

static PyObject *
text_read_continue(TextCallState *state, PyObject *value)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *decoded;
    PyObject *part;
    int eof;
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == TEXT_PHASE_READ_AFTER_WRITE) {
        if (state->read_size < 0) {
            state->phase = TEXT_PHASE_READ_ALL;
            {
                PyObject *next = text_buffer_call(receiver, "read", NULL);
                PyObject *processed;
                if (next == NULL) {
                    return NULL;
                }
                processed = text_read_continue(state, next);
                Py_DECREF(next);
                return processed;
            }
        }
        return text_read_request(state, state->read_size);
    }
    if (state->phase == TEXT_PHASE_READ_ALL) {
        decoded = text_decode_chunk(state, value, &eof);
        if (decoded == NULL) {
            return NULL;
        }
        Py_DECREF(decoded);
        part = text_decoded_get(receiver, -1);
        if (part == NULL || text_append(&state->result, part) < 0) {
            Py_XDECREF(part);
            return NULL;
        }
        Py_DECREF(part);
        Py_CLEAR(receiver->snapshot);
        Py_CLEAR(receiver->decoded_chars);
        return state->result == NULL ? text_empty() : Py_NewRef(state->result);
    }
    decoded = text_decode_chunk(state, value, &eof);
    if (decoded == NULL) {
        return NULL;
    }
    Py_DECREF(decoded);
    part = text_decoded_get(receiver, state->read_size -
                            (state->result == NULL ? 0 :
                             PyUnicode_GET_LENGTH(state->result)));
    if (part == NULL || text_append(&state->result, part) < 0) {
        Py_XDECREF(part);
        return NULL;
    }
    Py_DECREF(part);
    if (state->result != NULL &&
        PyUnicode_GET_LENGTH(state->result) >= state->read_size) {
        return Py_NewRef(state->result);
    }
    if (eof) {
        return state->result == NULL ? text_empty() : Py_NewRef(state->result);
    }
    return text_read_request(state, state->read_size -
                             (state->result == NULL ? 0 :
                              PyUnicode_GET_LENGTH(state->result)));
}

static Py_ssize_t
text_line_end(AleffTextIO *receiver, Py_ssize_t start, Py_ssize_t end)
{
    for (Py_ssize_t index = start; index < end; index++) {
        Py_UCS4 character = PyUnicode_ReadChar(receiver->decoded_chars, index);
        if (receiver->readtranslate) {
            if (character == '\n') {
                return index + 1 - start;
            }
        }
        else if (receiver->readuniversal) {
            if (character == '\n') {
                return index + 1 - start;
            }
            if (character == '\r') {
                return index + 1 - start +
                    (index + 1 < end &&
                     PyUnicode_ReadChar(receiver->decoded_chars, index + 1) == '\n');
            }
        }
        else if (receiver->readnl != NULL) {
            Py_ssize_t newline_length = PyUnicode_GET_LENGTH(receiver->readnl);
            if (index + newline_length <= end &&
                PyUnicode_Tailmatch(receiver->decoded_chars, receiver->readnl,
                                    index, index + newline_length, -1) == 1) {
                return index + newline_length - start;
            }
        }
    }
    return -1;
}

static PyObject *
text_line_continue(TextCallState *state, PyObject *value)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *decoded;
    PyObject *part;
    Py_ssize_t start;
    Py_ssize_t end;
    Py_ssize_t available;
    Py_ssize_t line_end;
    Py_ssize_t take;
    int eof;
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == TEXT_PHASE_LINE_AFTER_WRITE) {
        return text_read_request(state, 0);
    }
    decoded = text_decode_chunk(state, value, &eof);
    if (decoded == NULL) {
        return NULL;
    }
    Py_DECREF(decoded);
    start = receiver->decoded_chars_used;
    end = PyUnicode_GET_LENGTH(receiver->decoded_chars);
    available = end - start;
    line_end = text_line_end(receiver, start, end);
    take = line_end < 0 ? available : line_end;
    if (state->read_size >= 0) {
        Py_ssize_t already = state->line == NULL ? 0 :
            PyUnicode_GET_LENGTH(state->line);
        if (take > state->read_size - already) {
            take = state->read_size - already;
        }
    }
    if (take < 0) {
        take = 0;
    }
    if (take > 0) {
        part = PyUnicode_Substring(receiver->decoded_chars, start, start + take);
        if (part == NULL || text_append(&state->line, part) < 0) {
            Py_XDECREF(part);
            return NULL;
        }
        Py_DECREF(part);
        receiver->decoded_chars_used += take;
    }
    if ((line_end >= 0 && take == line_end) ||
        (state->read_size >= 0 && state->line != NULL &&
         PyUnicode_GET_LENGTH(state->line) >= state->read_size)) {
        PyObject *line = state->line == NULL ? text_empty() : Py_NewRef(state->line);
        if (line != NULL && state->nested_parent == TEXT_NESTED_ITER_READLINE) {
            PyObject *nested = text_wrap_nested(state->nested_parent, line);
            Py_DECREF(line);
            return nested;
        }
        return line;
    }
    if (eof) {
        Py_CLEAR(receiver->decoded_chars);
        Py_CLEAR(receiver->snapshot);
        {
            PyObject *line = state->line == NULL ? text_empty() : Py_NewRef(state->line);
            if (line != NULL && state->nested_parent == TEXT_NESTED_ITER_READLINE) {
                PyObject *nested = text_wrap_nested(state->nested_parent, line);
                Py_DECREF(line);
                return nested;
            }
            return line;
        }
    }
    return text_read_request(state, 0);
}

static PyObject *
text_write_reset(TextCallState *state)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *name;
    PyObject *reset;
    if (receiver->snapshot != NULL) {
        Py_CLEAR(receiver->decoded_chars);
        receiver->decoded_chars_used = 0;
        Py_CLEAR(receiver->snapshot);
    }
    if (receiver->decoder == NULL) {
        return PyLong_FromSsize_t(state->text_length);
    }
    state->phase = TEXT_PHASE_WRITE_RESET;
    name = PyUnicode_FromString("reset");
    if (name == NULL) {
        return NULL;
    }
    reset = PyObject_CallMethodObjArgs(receiver->decoder, name, NULL);
    Py_DECREF(name);
    if (reset == NULL) {
        return NULL;
    }
    Py_DECREF(reset);
    return PyLong_FromSsize_t(state->text_length);
}

static PyObject *
text_pending_to_bytes(PyObject *pending)
{
    PyObject *result;
    if (PyBytes_Check(pending)) {
        return Py_NewRef(pending);
    }
    if (PyUnicode_Check(pending)) {
        return PyUnicode_AsASCIIString(pending);
    }
    if (!PyList_Check(pending)) {
        PyErr_Format(PyExc_TypeError,
                     "pending text data must be bytes or ASCII str, not '%.200s'",
                     Py_TYPE(pending)->tp_name);
        return NULL;
    }
    result = PyBytes_FromStringAndSize(NULL, 0);
    if (result == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < PyList_GET_SIZE(pending); index++) {
        PyObject *part = PyList_GET_ITEM(pending, index);
        PyObject *part_bytes = text_pending_to_bytes(part);
        PyObject *joined;
        if (part_bytes == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        joined = PyBytes_FromStringAndSize(NULL,
                                           PyBytes_GET_SIZE(result) +
                                           PyBytes_GET_SIZE(part_bytes));
        if (joined == NULL) {
            Py_DECREF(part_bytes);
            Py_DECREF(result);
            return NULL;
        }
        memcpy(PyBytes_AS_STRING(joined), PyBytes_AS_STRING(result),
               (size_t)PyBytes_GET_SIZE(result));
        memcpy(PyBytes_AS_STRING(joined) + PyBytes_GET_SIZE(result),
               PyBytes_AS_STRING(part_bytes),
               (size_t)PyBytes_GET_SIZE(part_bytes));
        Py_DECREF(part_bytes);
        Py_SETREF(result, joined);
    }
    return result;
}

static PyObject *
text_write_after_encode(TextCallState *state, PyObject *encoded)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *bytes;
    PyObject *part;
    Py_ssize_t bytes_len;
    if (PyBytes_Check(encoded) || PyUnicode_Check(encoded)) {
        bytes = encoded;
    }
    else if (PyTuple_Check(encoded) && PyTuple_GET_SIZE(encoded) == 2) {
        bytes = PyTuple_GET_ITEM(encoded, 0);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "encoder should return a bytes object, not '%.200s'",
                     Py_TYPE(encoded)->tp_name);
        return NULL;
    }
    part = text_pending_to_bytes(bytes);
    if (part == NULL) {
        return NULL;
    }
    receiver->encoding_start_of_stream = 0;
    bytes_len = PyBytes_GET_SIZE(part);
    if (receiver->pending_bytes == NULL) {
        receiver->pending_bytes = Py_NewRef(part);
    }
    else if (!PyList_CheckExact(receiver->pending_bytes)) {
        PyObject *list = PyList_New(2);
        if (list == NULL) {
            return NULL;
        }
        PyList_SET_ITEM(list, 0, receiver->pending_bytes);
        PyList_SET_ITEM(list, 1, Py_NewRef(part));
        receiver->pending_bytes = list;
    }
    else if (PyList_Append(receiver->pending_bytes, part) < 0) {
        Py_DECREF(part);
        return NULL;
    }
    Py_DECREF(part);
    receiver->pending_bytes_count += bytes_len;
    if (receiver->pending_bytes_count >= receiver->chunk_size ||
        state->need_flush || state->write_through) {
        PyObject *payload = PyBytes_FromStringAndSize(NULL,
                                                       receiver->pending_bytes_count);
        PyObject *result;
        if (payload == NULL) {
            return NULL;
        }
        {
            PyObject *flattened = text_pending_to_bytes(
                receiver->pending_bytes);
            if (flattened == NULL) {
                Py_DECREF(payload);
                return NULL;
            }
            memcpy(PyBytes_AS_STRING(payload), PyBytes_AS_STRING(flattened),
                   (size_t)receiver->pending_bytes_count);
            Py_DECREF(flattened);
        }
        receiver->pending_bytes_count = 0;
        Py_CLEAR(receiver->pending_bytes);
        state->phase = TEXT_PHASE_WRITE_BUFFER;
        result = text_buffer_call(receiver, "write", payload);
        Py_DECREF(payload);
        if (result == NULL) {
            return NULL;
        }
        {
            PyObject *processed = text_write_continue(state, result);
            Py_DECREF(result);
            return processed;
        }
    }
    return text_write_reset(state);
}

static PyObject *
text_write_continue(TextCallState *state, PyObject *value)
{
    AleffTextIO *receiver = text_object(state->receiver);
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == TEXT_PHASE_WRITE_ENCODE &&
        (PyBytes_Check(value) || PyTuple_Check(value))) {
        return text_write_after_encode(state, value);
    }
    if (state->phase == TEXT_PHASE_WRITE_ENCODE) {
        state->phase = TEXT_PHASE_WRITE_BUFFER;
    }
    if (state->phase == TEXT_PHASE_WRITE_BUFFER &&
        receiver->pending_bytes != NULL) {
        Py_CLEAR(receiver->pending_bytes);
        receiver->pending_bytes_count = 0;
    }
    if (state->phase == TEXT_PHASE_WRITE_FLUSH) {
        return text_write_reset(state);
    }
    if (state->phase == TEXT_PHASE_WRITE_RESET) {
        return PyLong_FromSsize_t(state->text_length);
    }
    if (state->need_flush && state->phase == TEXT_PHASE_WRITE_BUFFER) {
        state->phase = TEXT_PHASE_WRITE_FLUSH;
        {
            PyObject *next = text_buffer_call(receiver, "flush", NULL);
            PyObject *processed;
            if (next == NULL) {
                return NULL;
            }
            processed = text_write_continue(state, next);
            Py_DECREF(next);
            return processed;
        }
    }
    return text_write_reset(state);
}

static PyObject *
text_seek_position(TextCallState *state)
{
    PyObject *position;
    PyObject *result;
    if (state->seek_start == NULL && state->seek_cookie != NULL &&
        text_cookie_parse(state) < 0) {
        return NULL;
    }
    if (text_object(state->receiver)->decoder == NULL) {
        state->seek_chars_to_skip = 0;
    }
    position = state->seek_start != NULL
        ? Py_NewRef(state->seek_start) : PyLong_FromLong(0);
    if (position == NULL) {
        return NULL;
    }
    result = text_buffer_seek(text_object(state->receiver), position, SEEK_SET);
    Py_DECREF(position);
    if (result == NULL) {
        return NULL;
    }
    {
        PyObject *processed = text_seek_continue(state, result);
        Py_DECREF(result);
        return processed;
    }
}

static PyObject *
text_seek_final(TextCallState *state, PyObject *value)
{
    if (state->result != NULL) {
        return Py_NewRef(state->result);
    }
    if (state->seek_cookie != NULL) {
        return Py_NewRef(state->seek_cookie);
    }
    return Py_NewRef(value);
}

static PyObject *
text_seek_encoder_start(TextCallState *state, PyObject *value)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *name;
    PyObject *argument = NULL;
    PyObject *zero;
    PyObject *position;
    PyObject *result;
    int start_of_stream = 0;
    if (receiver->encoder == NULL) {
        return text_seek_final(state, value);
    }
    position = state->seek_start != NULL ? state->seek_start
        : (state->result != NULL ? state->result : state->seek_cookie);
    if (position != NULL) {
        zero = PyLong_FromLong(0);
        if (zero == NULL) {
            return NULL;
        }
        start_of_stream = PyObject_RichCompareBool(position, zero, Py_EQ);
        Py_DECREF(zero);
        if (start_of_stream < 0) {
            return NULL;
        }
        if (start_of_stream && state->seek_flags != NULL) {
            zero = PyLong_FromLong(0);
            if (zero == NULL) {
                return NULL;
            }
            start_of_stream = PyObject_RichCompareBool(state->seek_flags, zero,
                                                       Py_EQ);
            Py_DECREF(zero);
            if (start_of_stream < 0) {
                return NULL;
            }
        }
    }
    state->phase = TEXT_PHASE_SEEK_ENCODER_RESET;
    name = PyUnicode_FromString(start_of_stream ? "reset" : "setstate");
    if (name == NULL) {
        return NULL;
    }
    if (!start_of_stream) {
        argument = PyLong_FromLong(0);
        if (argument == NULL) {
            Py_DECREF(name);
            return NULL;
        }
    }
    result = argument == NULL
        ? PyObject_CallMethodObjArgs(receiver->encoder, name, NULL)
        : PyObject_CallMethodObjArgs(receiver->encoder, name, argument, NULL);
    Py_XDECREF(argument);
    Py_DECREF(name);
    if (result == NULL) {
        return NULL;
    }
    {
        PyObject *processed = text_seek_continue(state, result);
        Py_DECREF(result);
        return processed;
    }
}

static PyObject *
text_seek_restore_start(TextCallState *state)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *size;
    PyObject *result;
    if (state->seek_chars_to_skip <= 0) {
        return text_seek_encoder_start(state, state->seek_cookie);
    }
    if (state->seek_bytes_to_feed < 0) {
        PyErr_SetString(PyExc_OSError, "can't restore logical file position");
        return NULL;
    }
    size = PyLong_FromLong(state->seek_bytes_to_feed);
    if (size == NULL) {
        return NULL;
    }
    state->phase = TEXT_PHASE_SEEK_RESTORE_READ;
    result = text_buffer_call(receiver, "read", size);
    Py_DECREF(size);
    if (result == NULL) {
        return NULL;
    }
    {
        PyObject *processed = text_seek_continue(state, result);
        Py_DECREF(result);
        return processed;
    }
}

static PyObject *
text_seek_continue(TextCallState *state, PyObject *value)
{
    AleffTextIO *receiver = text_object(state->receiver);
    PyObject *nested_value;
    int whence = 0;
    if (state->arguments != NULL && PyTuple_GET_SIZE(state->arguments) > 1) {
        whence = (int)PyLong_AsLong(PyTuple_GET_ITEM(state->arguments, 1));
        if (whence == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == TEXT_PHASE_SEEK_RESTORE_READ) {
        PyObject *name;
        PyObject *final;
        PyObject *decoded;
        if (!PyBytes_Check(value)) {
            PyErr_Format(PyExc_TypeError,
                         "underlying read() should have returned a bytes object, "
                         "not '%.200s'", Py_TYPE(value)->tp_name);
            return NULL;
        }
        Py_XSETREF(state->seek_input, Py_NewRef(value));
        name = PyUnicode_FromString("decode");
        final = PyBool_FromLong(state->seek_need_eof);
        if (name == NULL || final == NULL) {
            Py_XDECREF(name);
            Py_XDECREF(final);
            return NULL;
        }
        state->phase = TEXT_PHASE_SEEK_RESTORE_DECODE;
        decoded = PyObject_CallMethodObjArgs(text_object(state->receiver)->decoder,
                                             name, value, final, NULL);
        Py_DECREF(name);
        Py_DECREF(final);
        if (decoded == NULL) {
            return NULL;
        }
        {
            PyObject *processed = text_seek_continue(state, decoded);
            Py_DECREF(decoded);
            return processed;
        }
    }
    if (state->phase == TEXT_PHASE_SEEK_RESTORE_DECODE) {
        AleffTextIO *receiver = text_object(state->receiver);
        PyObject *snapshot;
        if (!PyUnicode_Check(value) || PyUnicode_READY(value) < 0) {
            if (PyUnicode_Check(value) && PyErr_Occurred()) {
                return NULL;
            }
            PyErr_Format(PyExc_TypeError,
                         "decoder should return a string result, not '%.200s'",
                         Py_TYPE(value)->tp_name);
            return NULL;
        }
        if (PyUnicode_GET_LENGTH(value) < state->seek_chars_to_skip) {
            PyErr_SetString(PyExc_OSError, "can't restore logical file position");
            return NULL;
        }
        Py_XSETREF(receiver->decoded_chars, Py_NewRef(value));
        receiver->decoded_chars_used = state->seek_chars_to_skip;
        snapshot = PyTuple_Pack(2, state->seek_flags, state->seek_input);
        if (snapshot == NULL) {
            return NULL;
        }
        Py_XSETREF(receiver->snapshot, snapshot);
        return text_seek_encoder_start(state, value);
    }
    nested_value = text_unwrap_nested(value, TEXT_NESTED_SEEK_TELL);
    if (nested_value != NULL) {
        Py_DECREF(value);
        Py_XSETREF(state->seek_cookie, nested_value);
        Py_CLEAR(state->seek_start);
        state->phase = TEXT_PHASE_SEEK_BUFFER;
        return text_seek_position(state);
    }
    nested_value = text_unwrap_nested(value, TEXT_NESTED_SEEK_FLUSH);
    if (nested_value != NULL) {
        PyObject *zero;
        PyObject *result;
        Py_DECREF(nested_value);
        Py_DECREF(value);
        state->phase = TEXT_PHASE_SEEK_BUFFER;
        if (whence == SEEK_END) {
            zero = PyLong_FromLong(0);
            if (zero == NULL) {
                return NULL;
            }
            result = text_buffer_seek(receiver, zero, SEEK_END);
            Py_DECREF(zero);
            if (result == NULL) {
                return NULL;
            }
            {
                PyObject *processed = text_seek_continue(state, result);
                Py_DECREF(result);
                return processed;
            }
        }
        return text_seek_position(state);
    }
    if (state->phase == TEXT_PHASE_SEEK_TELL) {
        Py_XSETREF(state->seek_cookie, Py_NewRef(value));
        Py_CLEAR(state->seek_start);
        state->phase = TEXT_PHASE_SEEK_BUFFER;
        return text_seek_position(state);
    }
    if (state->phase == TEXT_PHASE_SEEK_FLUSH) {
        state->phase = TEXT_PHASE_SEEK_BUFFER;
        if (whence == SEEK_END) {
            PyObject *zero = PyLong_FromLong(0);
            PyObject *result;
            if (zero == NULL) {
                return NULL;
            }
            result = text_buffer_seek(receiver, zero, SEEK_END);
            Py_DECREF(zero);
            if (result == NULL) {
                return NULL;
            }
            {
                PyObject *processed = text_seek_continue(state, result);
                Py_DECREF(result);
                return processed;
            }
        }
        return text_seek_position(state);
    }
    if (state->phase == TEXT_PHASE_SEEK_RESET) {
        return text_seek_restore_start(state);
    }
    if (state->phase == TEXT_PHASE_SEEK_ENCODER_RESET) {
        return text_seek_final(state, value);
    }
    Py_CLEAR(receiver->decoded_chars);
    receiver->decoded_chars_used = 0;
    Py_CLEAR(receiver->snapshot);
    if (whence == SEEK_END) {
        Py_XSETREF(state->result, Py_NewRef(value));
    }
    if (receiver->decoder != NULL) {
        PyObject *name;
        PyObject *argument = NULL;
        PyObject *reset;
        int use_state = state->seek_flags != NULL &&
            !(state->seek_start == NULL && whence == SEEK_END);
        if (use_state) {
            PyObject *zero = PyLong_FromLong(0);
            int at_start;
            int start_cmp;
            int flags_cmp;
            if (zero == NULL) {
                return NULL;
            }
            start_cmp = state->seek_start == NULL ? 0 :
                PyObject_RichCompareBool(state->seek_start, zero, Py_EQ);
            flags_cmp = PyObject_RichCompareBool(state->seek_flags, zero, Py_EQ);
            Py_DECREF(zero);
            if (start_cmp < 0 || flags_cmp < 0) {
                return NULL;
            }
            at_start = start_cmp == 1 && flags_cmp == 1;
            use_state = !at_start;
        }
        name = PyUnicode_FromString(use_state ? "setstate" : "reset");
        if (name == NULL) {
            return NULL;
        }
        if (use_state) {
            argument = Py_BuildValue("yO", "", state->seek_flags);
            if (argument == NULL) {
                Py_DECREF(name);
                return NULL;
            }
        }
        state->phase = TEXT_PHASE_SEEK_RESET;
        reset = argument == NULL
            ? PyObject_CallMethodObjArgs(receiver->decoder, name, NULL)
            : PyObject_CallMethodObjArgs(receiver->decoder, name, argument, NULL);
        Py_XDECREF(argument);
        Py_DECREF(name);
        if (reset == NULL) {
            return NULL;
        }
        Py_DECREF(reset);
    }
    return text_seek_encoder_start(state, value);
}

static PyObject *
text_tell_continue(TextCallState *state, PyObject *value)
{
    PyObject *nested;
    PyObject *nested_value;
    if (value == NULL) {
        return NULL;
    }
    nested_value = text_unwrap_nested(value, TEXT_NESTED_TELL_FLUSH);
    if (nested_value != NULL) {
        Py_DECREF(nested_value);
        Py_DECREF(value);
        state->phase = TEXT_PHASE_TELL_BUFFER;
        {
            PyObject *next = text_buffer_call(text_object(state->receiver), "tell", NULL);
            PyObject *processed;
            if (next == NULL) {
                return NULL;
            }
            processed = text_tell_continue(state, next);
            Py_DECREF(next);
            return processed;
        }
    }
    if (state->phase == TEXT_PHASE_TELL_FLUSH) {
        state->phase = TEXT_PHASE_TELL_BUFFER;
        {
            PyObject *next = text_buffer_call(text_object(state->receiver), "tell", NULL);
            PyObject *processed;
            if (next == NULL) {
                return NULL;
            }
            processed = text_tell_continue(state, next);
            Py_DECREF(next);
            return processed;
        }
    }
    if (state->nested_parent != 0) {
        nested = text_wrap_nested(state->nested_parent, value);
        return nested;
    }
    {
        AleffTextIO *receiver = text_object(state->receiver);
        if (receiver->snapshot != NULL && receiver->decoded_chars != NULL) {
            PyObject *next_input;
            PyObject *length;
            PyObject *position;
            PyObject *flags;
            Py_ssize_t chars_to_skip = receiver->decoded_chars_used;
            Py_ssize_t bytes_to_feed;
            if (!PyTuple_Check(receiver->snapshot) ||
                PyTuple_GET_SIZE(receiver->snapshot) < 2) {
                return Py_NewRef(value);
            }
            next_input = PyTuple_GET_ITEM(receiver->snapshot, 1);
            if (!PyBytes_Check(next_input)) {
                return Py_NewRef(value);
            }
            length = PyLong_FromSsize_t(PyBytes_GET_SIZE(next_input));
            if (length == NULL) {
                Py_XDECREF(length);
                return NULL;
            }
            position = PyNumber_Subtract(value, length);
            Py_DECREF(length);
            if (position == NULL) {
                return NULL;
            }
            flags = PyTuple_GET_ITEM(receiver->snapshot, 0);
            bytes_to_feed = chars_to_skip == 0 ? 0 :
                PyBytes_GET_SIZE(next_input);
            {
                PyObject *cookie = text_cookie_build(
                    position, flags, (int)bytes_to_feed,
                    (int)chars_to_skip, 0);
                Py_DECREF(position);
                return cookie;
            }
        }
    }
    return Py_NewRef(value);
}

static PyObject *
text_flush_continue(TextCallState *state, PyObject *value)
{
    PyObject *nested;
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == TEXT_PHASE_FLUSH_WRITE) {
        state->phase = TEXT_PHASE_FLUSH_BUFFER;
        {
            PyObject *next = text_buffer_call(text_object(state->receiver), "flush", NULL);
            if (next == NULL) {
                return NULL;
            }
            if (state->nested_parent != 0) {
                PyObject *processed = text_wrap_nested(state->nested_parent, next);
                Py_DECREF(next);
                return processed;
            }
            return next;
        }
    }
    if (state->nested_parent != 0) {
        nested = text_wrap_nested(state->nested_parent, value);
        return nested;
    }
    return Py_NewRef(value);
}

static PyObject *
text_close_continue(TextCallState *state, PyObject *value)
{
    PyObject *nested_value;
    PyObject *result;
    if (value == NULL) {
        if (state->close_buffer_started && state->close_exception != NULL &&
            PyErr_Occurred()) {
            PyObject *close_exception = PyErr_GetRaisedException();
            if (close_exception != NULL) {
                PyException_SetContext(close_exception, state->close_exception);
                state->close_exception = NULL;
                PyErr_SetRaisedException(close_exception);
            }
            return NULL;
        }
        if (state->close_flush_done && !state->close_buffer_started) {
            if (state->close_exception != NULL && PyErr_Occurred()) {
                PyObject *close_exception = PyErr_GetRaisedException();
                if (close_exception != NULL) {
                    PyException_SetContext(close_exception,
                                           state->close_exception);
                    state->close_exception = NULL;
                    PyErr_SetRaisedException(close_exception);
                }
            }
            return NULL;
        }
        if (state->close_flush_expected && !state->close_buffer_started &&
            PyErr_Occurred()) {
            state->close_exception = PyErr_GetRaisedException();
            state->close_buffer_started = 1;
            result = text_buffer_call(text_object(state->receiver), "close", NULL);
            if (result == NULL) {
                PyObject *close_exception = PyErr_GetRaisedException();
                if (close_exception != NULL) {
                    PyException_SetContext(close_exception, state->close_exception);
                    state->close_exception = NULL;
                    PyErr_SetRaisedException(close_exception);
                }
                else {
                    PyErr_SetRaisedException(state->close_exception);
                    state->close_exception = NULL;
                }
                return NULL;
            }
            Py_DECREF(result);
            PyErr_SetRaisedException(state->close_exception);
            state->close_exception = NULL;
        }
        return NULL;
    }
    nested_value = text_unwrap_nested(value, TEXT_NESTED_CLOSE_FLUSH);
    if (nested_value != NULL) {
        Py_DECREF(nested_value);
        Py_DECREF(value);
        state->close_flush_done = 1;
        state->close_buffer_started = 1;
        result = text_buffer_call(text_object(state->receiver), "close", NULL);
        if (result == NULL) {
            if (state->close_exception != NULL) {
                PyObject *close_exception = PyErr_GetRaisedException();
                if (close_exception != NULL) {
                    PyException_SetContext(close_exception, state->close_exception);
                    state->close_exception = NULL;
                    PyErr_SetRaisedException(close_exception);
                }
            }
            return NULL;
        }
        if (state->close_exception != NULL) {
            Py_DECREF(result);
            PyErr_SetRaisedException(state->close_exception);
            state->close_exception = NULL;
            return NULL;
        }
        return result;
    }
    if (state->close_flush_done && state->close_exception != NULL) {
        Py_DECREF(value);
        PyErr_SetRaisedException(state->close_exception);
        state->close_exception = NULL;
        return NULL;
    }
    /* The underlying close callback has already completed. */
    return Py_NewRef(value);
}

static PyObject *
text_iterator_continue(TextCallState *state, PyObject *value)
{
    PyObject *line;
    if (value == NULL) {
        return NULL;
    }
    line = text_unwrap_nested(value, TEXT_NESTED_ITER_READLINE);
    if (line != NULL) {
        Py_DECREF(value);
    }
    else {
        line = text_line_continue(state, value);
    }
    if (line == NULL) {
        return NULL;
    }
    if (PyUnicode_GET_LENGTH(line) == 0) {
        AleffTextIO *receiver = text_object(state->receiver);
        Py_DECREF(line);
        Py_CLEAR(receiver->snapshot);
        receiver->telling = receiver->seekable;
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    return line;
}

static PyObject *
text_resume(const void *raw_state, PyObject *value)
{
    const TextCallState *source = raw_state;
    PyObject *pending_exception = value == NULL
        ? PyErr_GetRaisedException() : NULL;
    TextCallState *state = text_copy_state(raw_state);
    AleffAdapterFrame frame;
    PyObject *result;
    if (state == NULL) {
        if (pending_exception != NULL) {
            PyObject *copy_exception = PyErr_GetRaisedException();
            if (copy_exception != NULL) {
                PyException_SetContext(copy_exception, pending_exception);
                PyErr_SetRaisedException(copy_exception);
            }
            else {
                PyErr_SetRaisedException(pending_exception);
            }
        }
        return NULL;
    }
    if (adapter_enter(&frame, &text_call_vtable, state) < 0) {
        if (pending_exception != NULL) {
            PyObject *enter_exception = PyErr_GetRaisedException();
            PyException_SetContext(enter_exception, pending_exception);
            PyErr_SetRaisedException(enter_exception);
        }
        text_free_state(state);
        return NULL;
    }
    if (pending_exception != NULL) {
        PyErr_SetRaisedException(pending_exception);
    }
    if (value == NULL && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "text adapter resumed without a value or exception");
    }
    switch (source->operation) {
        case TEXT_READ:
            result = text_read_continue(state, value);
            break;
        case TEXT_READLINE:
            result = text_line_continue(state, value);
            break;
        case TEXT_WRITE:
            result = text_write_continue(state, value);
            break;
        case TEXT_SEEK:
            result = text_seek_continue(state, value);
            break;
        case TEXT_TELL:
            result = text_tell_continue(state, value);
            break;
        case TEXT_FLUSH:
            result = text_flush_continue(state, value);
            break;
        case TEXT_CLOSE:
            result = text_close_continue(state, value);
            break;
        case TEXT_ITER:
            result = text_iterator_continue(state, value);
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown text adapter operation");
            result = NULL;
            break;
    }
    adapter_leave(&frame);
    text_free_state(state);
    return result;
}

static const AleffAdapterVTable text_call_vtable = {
    .copy_state = text_copy_state,
    .free_state = text_free_state,
    .resume = text_resume,
    .prepare_resume = text_prepare_resume,
};

static int
text_operation(const char *name, TextOperation *operation)
{
    static const char *const names[] = {
        "read", "readline", "write", "seek", "tell", "flush", "close",
    };
    for (int index = 0; index < 7; index++) {
        if (strcmp(name, names[index]) == 0) {
            *operation = (TextOperation)index;
            return 0;
        }
    }
    return -1;
}

static int
text_parse_size(PyObject *arguments, Py_ssize_t *size)
{
    PyObject *value;
    if (PyTuple_GET_SIZE(arguments) == 0) {
        *size = -1;
        return 0;
    }
    value = PyTuple_GET_ITEM(arguments, 0);
    if (value == Py_None) {
        *size = -1;
        return 0;
    }
    *size = PyLong_AsSsize_t(value);
    return *size == -1 && PyErr_Occurred() ? -1 : 0;
}

static int
text_setup_state(TextCallState *state, TextOperation operation)
{
    AleffTextIO *receiver = text_object(state->receiver);
    state->operation = operation;
    switch (operation) {
        case TEXT_READ:
            if (text_parse_size(state->arguments, &state->read_size) < 0) {
                return -1;
            }
            state->phase = state->read_size < 0 ? TEXT_PHASE_READ_ALL
                                                : TEXT_PHASE_READ_CHUNK;
            if (receiver->pending_bytes != NULL) {
                state->phase = TEXT_PHASE_READ_AFTER_WRITE;
            }
            break;
        case TEXT_READLINE:
            state->read_size = -1;
            if (PyTuple_GET_SIZE(state->arguments) > 0 &&
                text_parse_size(state->arguments, &state->read_size) < 0) {
                return -1;
            }
            state->phase = receiver->pending_bytes != NULL
                ? TEXT_PHASE_LINE_AFTER_WRITE : TEXT_PHASE_LINE_CHUNK;
            {
                TextCallState *parent = adapter_find_state(&text_call_vtable);
                if (parent != NULL && parent->operation == TEXT_ITER) {
                    state->nested_parent = TEXT_NESTED_ITER_READLINE;
                }
            }
            break;
        case TEXT_WRITE: {
            PyObject *text;
            if (PyTuple_GET_SIZE(state->arguments) == 0) {
                state->phase = TEXT_PHASE_WRITE_BUFFER;
                break;
            }
            text = PyTuple_GET_ITEM(state->arguments, 0);
            if (!PyUnicode_Check(text)) {
                state->text_length = 0;
            }
            else if (PyUnicode_READY(text) < 0) {
                return -1;
            }
            else {
                Py_ssize_t length = PyUnicode_GET_LENGTH(text);
                state->text_length = length;
                state->write_through = receiver->write_through;
                state->need_flush = receiver->line_buffering &&
                    (PyUnicode_FindChar(text, '\n', 0, length, 1) >= 0 ||
                     PyUnicode_FindChar(text, '\r', 0, length, 1) >= 0);
            }
            state->phase = receiver->encodefunc == NULL
                ? TEXT_PHASE_WRITE_ENCODE : TEXT_PHASE_WRITE_BUFFER;
            break;
        }
        case TEXT_SEEK: {
            int whence = 0;
            if (PyTuple_GET_SIZE(state->arguments) == 0) {
                state->phase = TEXT_PHASE_SEEK_BUFFER;
                break;
            }
            state->seek_cookie = Py_NewRef(PyTuple_GET_ITEM(state->arguments, 0));
            if (PyTuple_GET_SIZE(state->arguments) > 1) {
                whence = (int)PyLong_AsLong(PyTuple_GET_ITEM(state->arguments, 1));
                if (whence == -1 && PyErr_Occurred()) {
                    return -1;
                }
            }
            state->phase = whence == SEEK_CUR ? TEXT_PHASE_SEEK_TELL
                                               : TEXT_PHASE_SEEK_BUFFER;
            if (whence == SEEK_END ||
                (whence != SEEK_CUR && receiver->pending_bytes != NULL)) {
                state->phase = TEXT_PHASE_SEEK_FLUSH;
            }
            break;
        }
        case TEXT_TELL:
            state->phase = receiver->pending_bytes != NULL
                ? TEXT_PHASE_TELL_FLUSH : TEXT_PHASE_TELL_BUFFER;
            {
                TextCallState *parent = adapter_find_state(&text_call_vtable);
                if (parent != NULL && parent->operation == TEXT_SEEK &&
                    parent->phase == TEXT_PHASE_SEEK_TELL) {
                    state->nested_parent = TEXT_NESTED_SEEK_TELL;
                }
            }
            break;
        case TEXT_FLUSH:
            receiver->telling = receiver->seekable;
            state->phase = receiver->pending_bytes != NULL
                ? TEXT_PHASE_FLUSH_WRITE : TEXT_PHASE_FLUSH_BUFFER;
            {
                TextCallState *parent = adapter_find_state(&text_call_vtable);
                if (parent != NULL && parent->operation == TEXT_SEEK) {
                    state->nested_parent = TEXT_NESTED_SEEK_FLUSH;
                    parent->phase = TEXT_PHASE_SEEK_BUFFER;
                }
                else if (parent != NULL && parent->operation == TEXT_TELL) {
                    state->nested_parent = TEXT_NESTED_TELL_FLUSH;
                    parent->phase = TEXT_PHASE_TELL_BUFFER;
                }
                else if (parent != NULL && parent->operation == TEXT_CLOSE) {
                    state->nested_parent = TEXT_NESTED_CLOSE_FLUSH;
                }
            }
            break;
        case TEXT_CLOSE:
            state->phase = TEXT_PHASE_CLOSE_BUFFER;
            /* CPython close() always flushes through the TextIOWrapper before
             * closing the underlying buffer.  The nested flush is represented
             * by a transient marker when it reaches this adapter. */
            state->close_flush_expected = 1;
            break;
        case TEXT_ITER:
            receiver->telling = 0;
            state->read_size = -1;
            state->phase = receiver->pending_bytes != NULL
                ? TEXT_PHASE_LINE_AFTER_WRITE : TEXT_PHASE_LINE_CHUNK;
            break;
    }
    return 0;
}

static PyObject *
text_find_original(PyObject *receiver, const char *name)
{
    PyObject *mro = Py_TYPE(receiver)->tp_mro;
    if (mro == NULL) {
        return NULL;
    }
    for (Py_ssize_t mro_index = 0; mro_index < PyTuple_GET_SIZE(mro); mro_index++) {
        PyTypeObject *candidate = (PyTypeObject *)PyTuple_GET_ITEM(mro, mro_index);
        for (Py_ssize_t index = 0; index < backup_count; index++) {
            TextMethodBackup *backup = &backups[index];
            if (backup->type == candidate && strcmp(backup->name, name) == 0) {
                return Py_NewRef(backup->original);
            }
        }
    }
    PyErr_Format(PyExc_RuntimeError, "missing text adapter method %s", name);
    return NULL;
}

static PyObject *
text_call_method(PyObject *receiver, PyObject *arguments, const char *name)
{
    PyObject *original = text_find_original(receiver, name);
    TextOperation operation;
    PyObject *call_arguments;
    TextCallState state;
    AleffAdapterFrame frame;
    PyObject *result;
    if (original == NULL || text_operation(name, &operation) < 0) {
        Py_XDECREF(original);
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "unknown text adapter operation");
        }
        return NULL;
    }
    call_arguments = PyTuple_New(PyTuple_GET_SIZE(arguments) + 1);
    if (call_arguments == NULL) {
        Py_DECREF(original);
        return NULL;
    }
    PyTuple_SET_ITEM(call_arguments, 0, Py_NewRef(receiver));
    for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(arguments); index++) {
        PyTuple_SET_ITEM(call_arguments, index + 1,
                         Py_NewRef(PyTuple_GET_ITEM(arguments, index)));
    }
    memset(&state, 0, sizeof(state));
    state.original = original;
    state.receiver = Py_NewRef(receiver);
    state.arguments = Py_NewRef(arguments);
    if (text_setup_state(&state, operation) < 0) {
        text_clear_state(&state);
        Py_DECREF(call_arguments);
        return NULL;
    }
    if (adapter_enter(&frame, &text_call_vtable, &state) < 0) {
        text_clear_state(&state);
        Py_DECREF(call_arguments);
        return NULL;
    }
    result = PyObject_Call(state.original, call_arguments, NULL);
    adapter_leave(&frame);
    if (operation == TEXT_TELL && result != NULL &&
        state.nested_parent == TEXT_NESTED_SEEK_TELL) {
        TextCallState *parent = adapter_find_state(&text_call_vtable);
        if (parent != NULL && parent->operation == TEXT_SEEK) {
            parent->phase = TEXT_PHASE_SEEK_BUFFER;
        }
    }
    if (operation == TEXT_FLUSH && state.nested_parent == TEXT_NESTED_CLOSE_FLUSH) {
        TextCallState *parent = adapter_find_state(&text_call_vtable);
        if (parent != NULL && parent->operation == TEXT_CLOSE) {
            parent->close_flush_done = 1;
            if (result == NULL && PyErr_Occurred()) {
                PyObject *exception = PyErr_GetRaisedException();
                Py_XSETREF(parent->close_exception, exception);
                PyErr_SetRaisedException(Py_NewRef(parent->close_exception));
            }
        }
    }
    text_clear_state(&state);
    Py_DECREF(call_arguments);
    return result;
}

#if PY_VERSION_HEX >= 0x030d0000
#define TEXT_METHOD_WRAPPER(name) \
    static PyObject *text_##name(PyObject *self, PyObject *args) \
    { \
        PyObject *result; \
        /* TextIOWrapper methods are critical sections in 3.13+; retain the \
         * same object-locking contract for free-threaded Python. */ \
        /* NOLINTNEXTLINE(bugprone-macro-parentheses) */ \
        Py_BEGIN_CRITICAL_SECTION(self); \
        result = text_call_method(self, args, #name); \
        Py_END_CRITICAL_SECTION(); \
        return result; \
    }
#else
#define TEXT_METHOD_WRAPPER(name) \
    static PyObject *text_##name(PyObject *self, PyObject *args) \
    { \
        return text_call_method(self, args, #name); \
    }
#endif

TEXT_METHOD_WRAPPER(read)
TEXT_METHOD_WRAPPER(readline)
TEXT_METHOD_WRAPPER(write)
TEXT_METHOD_WRAPPER(seek)
TEXT_METHOD_WRAPPER(tell)
TEXT_METHOD_WRAPPER(flush)
TEXT_METHOD_WRAPPER(close)

static PyCFunction text_functions[] = {
    (PyCFunction)text_read, (PyCFunction)text_readline, (PyCFunction)text_write,
    (PyCFunction)text_seek, (PyCFunction)text_tell, (PyCFunction)text_flush,
    (PyCFunction)text_close,
};

static const char *const text_names[] = {
    "read", "readline", "write", "seek", "tell", "flush", "close",
};

static PyObject *
text_next_slot(PyObject *self)
{
    TextCallState state;
    AleffAdapterFrame frame;
    PyObject *result;
    memset(&state, 0, sizeof(state));
    state.original = Py_NewRef(Py_None);
    state.receiver = Py_NewRef(self);
    state.arguments = PyTuple_New(0);
    if (state.arguments == NULL || text_setup_state(&state, TEXT_ITER) < 0) {
        text_clear_state(&state);
        return NULL;
    }
    if (adapter_enter(&frame, &text_call_vtable, &state) < 0) {
        text_clear_state(&state);
        return NULL;
    }
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(self);
    result = original_text_next(self);
    Py_END_CRITICAL_SECTION();
#else
    result = original_text_next(self);
#endif
    adapter_leave(&frame);
    text_clear_state(&state);
    return result;
}

static int
text_replace_method(PyTypeObject *type, const char *name, PyCFunction function)
{
    PyObject *dict = PyType_GetDict(type);
    PyObject *original;
    TextMethodBackup *backup;
    PyMethodDef *replacement;
    PyObject *descriptor;
    if (dict == NULL) {
        return -1;
    }
    original = PyDict_GetItemString(dict, name);
    if (original == NULL) {
        Py_DECREF(dict);
        return 0;
    }
    if (!Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        Py_DECREF(dict);
        PyErr_Format(PyExc_RuntimeError,
                     "io.TextIOWrapper.%s is not a method descriptor", name);
        return -1;
    }
    if (backup_count >= (Py_ssize_t)(sizeof(backups) / sizeof(*backups))) {
        Py_DECREF(dict);
        PyErr_SetString(PyExc_RuntimeError, "too many text adapter methods");
        return -1;
    }
    backup = &backups[backup_count];
    backup->type = type;
    backup->name = name;
    backup->original = Py_NewRef(original);
    replacement = &replacement_methods[backup_count];
    *replacement = *((PyMethodDescrObject *)original)->d_method;
    replacement->ml_name = name;
    replacement->ml_meth = function;
    replacement->ml_flags = METH_VARARGS;
    descriptor = PyDescr_NewMethod(type, replacement);
    if (descriptor == NULL || PyDict_SetItemString(dict, name, descriptor) < 0) {
        Py_XDECREF(descriptor);
        Py_CLEAR(backup->original);
        Py_DECREF(dict);
        return -1;
    }
    Py_DECREF(descriptor);
    Py_DECREF(dict);
    PyType_Modified(type);
    backup_count++;
    return 0;
}

int
adapter_io_text_install(PyObject *io_module)
{
    if (text_installed) {
        return 0;
    }
    installed_io = Py_NewRef(io_module);
    installed_text_type = PyObject_GetAttrString(io_module, "TextIOWrapper");
    if (installed_text_type == NULL || !PyType_Check(installed_text_type)) {
        Py_XDECREF(installed_text_type);
        installed_text_type = NULL;
        PyErr_SetString(PyExc_RuntimeError, "io.TextIOWrapper is not a type");
        adapter_io_text_rollback();
        return -1;
    }
    for (int index = 0; index < 7; index++) {
        if (text_replace_method((PyTypeObject *)installed_text_type,
                                text_names[index], text_functions[index]) < 0) {
            adapter_io_text_rollback();
            return -1;
        }
    }
    original_text_next = ((PyTypeObject *)installed_text_type)->tp_iternext;
    if (original_text_next == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "io.TextIOWrapper has no iterator slot");
        adapter_io_text_rollback();
        return -1;
    }
    ((PyTypeObject *)installed_text_type)->tp_iternext = text_next_slot;
    PyType_Modified((PyTypeObject *)installed_text_type);
    text_installed = 1;
    return 0;
}

void
adapter_io_text_rollback(void)
{
    if (installed_text_type != NULL && original_text_next != NULL) {
        ((PyTypeObject *)installed_text_type)->tp_iternext = original_text_next;
        PyType_Modified((PyTypeObject *)installed_text_type);
    }
    for (Py_ssize_t index = backup_count - 1; index >= 0; index--) {
        TextMethodBackup *backup = &backups[index];
        PyObject *dict = PyType_GetDict(backup->type);
        if (dict != NULL && PyDict_SetItemString(dict, backup->name,
                                                 backup->original) < 0) {
            PyErr_Clear();
        }
        Py_XDECREF(dict);
        PyType_Modified(backup->type);
        Py_CLEAR(backup->original);
    }
    backup_count = 0;
    Py_CLEAR(installed_text_type);
    Py_CLEAR(installed_io);
    original_text_next = NULL;
    text_installed = 0;
}
