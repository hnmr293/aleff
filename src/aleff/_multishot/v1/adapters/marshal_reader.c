#include "marshal_reader.h"

#include <stdint.h>

#include <Python.h>
#if PY_VERSION_HEX >= 0x030d0000 && defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

/* The wire-format parser follows CPython's Python/marshal.c.  CPython's
 * license terms are included in LICENSES/CPython.txt. */

#define MARSHAL_FLAG_REF 0x80
#define MARSHAL_SIZE32_MAX INT32_MAX
#if defined(MS_WINDOWS)
#  define MARSHAL_MAX_STACK_DEPTH 1000
#elif defined(__wasi__)
#  define MARSHAL_MAX_STACK_DEPTH 1500
#elif PY_VERSION_HEX >= 0x030d0000 && defined(__APPLE__) && \
    defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#  define MARSHAL_MAX_STACK_DEPTH 1500
#else
#  define MARSHAL_MAX_STACK_DEPTH 2000
#endif

#define TYPE_NULL '0'
#define TYPE_NONE 'N'
#define TYPE_FALSE 'F'
#define TYPE_TRUE 'T'
#define TYPE_STOPITER 'S'
#define TYPE_ELLIPSIS '.'
#define TYPE_INT 'i'
#define TYPE_INT64 'I'
#define TYPE_FLOAT 'f'
#define TYPE_BINARY_FLOAT 'g'
#define TYPE_COMPLEX 'x'
#define TYPE_BINARY_COMPLEX 'y'
#define TYPE_LONG 'l'
#define TYPE_STRING 's'
#define TYPE_INTERNED 't'
#define TYPE_REF 'r'
#define TYPE_TUPLE '('
#define TYPE_LIST '['
#define TYPE_DICT '{'
#define TYPE_CODE 'c'
#define TYPE_UNICODE 'u'
#define TYPE_UNKNOWN '?'
#define TYPE_SET '<'
#define TYPE_FROZENSET '>'
#define TYPE_ASCII 'a'
#define TYPE_ASCII_INTERNED 'A'
#define TYPE_SMALL_TUPLE ')'
#define TYPE_SHORT_ASCII 'z'
#define TYPE_SHORT_ASCII_INTERNED 'Z'
#define TYPE_SLICE ':'

typedef enum {
    PARSE_OK,
    PARSE_NULL,
    PARSE_NEED,
    PARSE_BAD,
} ParseResult;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t position;
    size_t next_read_size;
    size_t reference_count;
    size_t pending_references[MARSHAL_MAX_STACK_DEPTH];
    size_t pending_reference_count;
    unsigned int depth;
    int allow_code;
} MarshalReader;

static ParseResult
marshal_reader_need(MarshalReader *reader, size_t size)
{
    reader->next_read_size = size;
    return PARSE_NEED;
}

static ParseResult
marshal_reader_take(MarshalReader *reader, size_t size)
{
    if (size > reader->size - reader->position) {
        return marshal_reader_need(reader, size);
    }
    reader->position += size;
    return PARSE_OK;
}

static ParseResult
marshal_reader_byte(MarshalReader *reader, unsigned int *value)
{
    ParseResult result = marshal_reader_take(reader, 1);
    if (result != PARSE_OK) {
        return result;
    }
    *value = reader->data[reader->position - 1];
    return PARSE_OK;
}

static ParseResult
marshal_reader_i32(MarshalReader *reader, int64_t *value)
{
    ParseResult result;
    uint32_t unsigned_value;

    result = marshal_reader_take(reader, 4);
    if (result != PARSE_OK) {
        return result;
    }
    unsigned_value = (uint32_t)reader->data[reader->position - 4] |
        ((uint32_t)reader->data[reader->position - 3] << 8) |
        ((uint32_t)reader->data[reader->position - 2] << 16) |
        ((uint32_t)reader->data[reader->position - 1] << 24);
    *value = unsigned_value <= INT32_MAX
        ? (int64_t)unsigned_value
        : (int64_t)unsigned_value - ((int64_t)INT32_MAX + 1) * 2;
    return PARSE_OK;
}

static int
marshal_reader_add_reference(MarshalReader *reader)
{
    if (reader->reference_count == SIZE_MAX) {
        return 0;
    }
    reader->reference_count++;
    return 1;
}

static int
marshal_reader_reserve_delayed_reference(MarshalReader *reader, int flag)
{
    if (!flag) {
        return 1;
    }
    if (reader->pending_reference_count >= MARSHAL_MAX_STACK_DEPTH ||
        !marshal_reader_add_reference(reader)) {
        return 0;
    }
    reader->pending_references[reader->pending_reference_count++] =
        reader->reference_count - 1;
    return 1;
}

static void
marshal_reader_finish_delayed_reference(MarshalReader *reader, int flag)
{
    if (flag && reader->pending_reference_count != 0) {
        reader->pending_reference_count--;
    }
}

static int
marshal_reader_reference_is_pending(
    const MarshalReader *reader,
    size_t reference
)
{
    size_t index;
    for (index = 0; index < reader->pending_reference_count; index++) {
        if (reader->pending_references[index] == reference) {
            return 1;
        }
    }
    return 0;
}

static int
marshal_reader_equal_ascii(
    const unsigned char *data,
    size_t size,
    const char *text
)
{
    size_t index = 0;
    while (text[index] != '\0') {
        unsigned char value;
        if (index == size) {
            return 0;
        }
        value = data[index];
        if (value >= 'A' && value <= 'Z') {
            value = (unsigned char)(value + ('a' - 'A'));
        }
        if (value != (unsigned char)text[index]) {
            return 0;
        }
        index++;
    }
    return index == size;
}

/* PyOS_string_to_double accepts decimal spellings and the ordinary special
 * values.  This validates the spelling without materializing a Python float. */
static int
marshal_reader_valid_float(const unsigned char *data, size_t size)
{
    size_t index = 0;
    size_t digits_before = 0;
    size_t digits_after = 0;

    if (size == 0) {
        return 0;
    }
    if (data[index] == '+' || data[index] == '-') {
        index++;
        if (index == size) {
            return 0;
        }
    }
    if (marshal_reader_equal_ascii(data + index, size - index, "nan") ||
        marshal_reader_equal_ascii(data + index, size - index, "inf") ||
        marshal_reader_equal_ascii(data + index, size - index, "infinity")) {
        return 1;
    }

    while (index < size && data[index] >= '0' && data[index] <= '9') {
        index++;
        digits_before++;
    }
    if (index < size && data[index] == '.') {
        index++;
        while (index < size && data[index] >= '0' && data[index] <= '9') {
            index++;
            digits_after++;
        }
    }
    if (digits_before == 0 && digits_after == 0) {
        return 0;
    }
    if (index < size && (data[index] == 'e' || data[index] == 'E')) {
        index++;
        if (index < size && (data[index] == '+' || data[index] == '-')) {
            index++;
        }
        if (index == size) {
            return 0;
        }
        while (index < size && data[index] >= '0' && data[index] <= '9') {
            index++;
        }
    }
    return index == size;
}

static int
marshal_reader_valid_utf8(const unsigned char *data, size_t size)
{
    size_t index = 0;
    while (index < size) {
        unsigned char first = data[index++];
        if (first <= 0x7f) {
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            if (index == size || (data[index++] & 0xc0) != 0x80) {
                return 0;
            }
            continue;
        }
        if (first >= 0xe0 && first <= 0xef) {
            if (index + 1 >= size ||
                (data[index] & 0xc0) != 0x80 ||
                (data[index + 1] & 0xc0) != 0x80 ||
                (first == 0xe0 && data[index] < 0xa0)) {
                return 0;
            }
            index += 2;
            continue;
        }
        if (first >= 0xf0 && first <= 0xf4) {
            if (index + 2 >= size ||
                (data[index] & 0xc0) != 0x80 ||
                (data[index + 1] & 0xc0) != 0x80 ||
                (data[index + 2] & 0xc0) != 0x80 ||
                (first == 0xf0 && data[index] < 0x90) ||
                (first == 0xf4 && data[index] > 0x8f)) {
                return 0;
            }
            index += 3;
            continue;
        }
        return 0;
    }
    return 1;
}

static ParseResult marshal_reader_object(MarshalReader *reader, int *is_null);

static ParseResult
marshal_reader_float_text(MarshalReader *reader)
{
    ParseResult result;
    unsigned int size;
    size_t start;

    result = marshal_reader_byte(reader, &size);
    if (result != PARSE_OK) {
        return result;
    }
    start = reader->position;
    result = marshal_reader_take(reader, size);
    if (result != PARSE_OK) {
        return result;
    }
    return marshal_reader_valid_float(reader->data + start, size)
        ? PARSE_OK : PARSE_BAD;
}

static ParseResult
marshal_reader_sized_bytes(
    MarshalReader *reader,
    int validate_utf8
)
{
    ParseResult result;
    int64_t signed_size;
    size_t start;

    result = marshal_reader_i32(reader, &signed_size);
    if (result != PARSE_OK) {
        return result;
    }
    if (signed_size < 0 || signed_size > MARSHAL_SIZE32_MAX) {
        return PARSE_BAD;
    }
    start = reader->position;
    result = marshal_reader_take(reader, (size_t)signed_size);
    if (result != PARSE_OK) {
        return result;
    }
    if (validate_utf8 && !marshal_reader_valid_utf8(
            reader->data + start,
            (size_t)signed_size
        )) {
        return PARSE_BAD;
    }
    return PARSE_OK;
}

static ParseResult
marshal_reader_long(MarshalReader *reader)
{
    ParseResult result;
    int64_t signed_count;
    uint64_t count;
    size_t index;

    result = marshal_reader_i32(reader, &signed_count);
    if (result != PARSE_OK) {
        return result;
    }
    if (signed_count == INT32_MIN) {
        return PARSE_BAD;
    }
    count = signed_count < 0
        ? (uint64_t)(-signed_count)
        : (uint64_t)signed_count;
    if (count == 0) {
        return PARSE_OK;
    }
    for (index = 0; index < (size_t)count; index++) {
        size_t offset;
        unsigned int digit;

        result = marshal_reader_take(reader, 2);
        if (result != PARSE_OK) {
            return result;
        }
        offset = reader->position - 2;
        digit = reader->data[offset] |
            ((unsigned int)reader->data[offset + 1] << 8);
        if (digit > 0x7fff || (index + 1 == (size_t)count && digit == 0)) {
            return PARSE_BAD;
        }
    }
    return PARSE_OK;
}

static ParseResult
marshal_reader_sequence(
    MarshalReader *reader,
    int64_t count,
    int flag
)
{
    int64_t index;
    ParseResult result;
    int is_null;

    if (count < 0 || count > MARSHAL_SIZE32_MAX) {
        return PARSE_BAD;
    }
    if (flag && !marshal_reader_add_reference(reader)) {
        return PARSE_BAD;
    }
    for (index = 0; index < count; index++) {
        result = marshal_reader_object(reader, &is_null);
        if (result != PARSE_OK) {
            return result;
        }
        if (is_null) {
            return PARSE_BAD;
        }
    }
    return PARSE_OK;
}

static ParseResult
marshal_reader_dict(MarshalReader *reader, int flag)
{
    ParseResult result;
    int is_null;

    if (flag && !marshal_reader_add_reference(reader)) {
        return PARSE_BAD;
    }
    for (;;) {
        result = marshal_reader_object(reader, &is_null);
        if (result == PARSE_NEED || result == PARSE_BAD) {
            return result;
        }
        if (result == PARSE_NULL) {
            return PARSE_OK;
        }
        result = marshal_reader_object(reader, &is_null);
        if (result == PARSE_NEED || result == PARSE_BAD) {
            return result;
        }
        if (result == PARSE_NULL) {
            return PARSE_OK;
        }
    }
}

static ParseResult
marshal_reader_set(
    MarshalReader *reader,
    int64_t count,
    int flag,
    int delayed_reference
)
{
    int64_t index;
    ParseResult result;
    int is_null;

    if (count < 0 || count > MARSHAL_SIZE32_MAX) {
        return PARSE_BAD;
    }
    if (delayed_reference &&
        !marshal_reader_reserve_delayed_reference(reader, flag)) {
        return PARSE_BAD;
    }
    if (!delayed_reference && flag && !marshal_reader_add_reference(reader)) {
        return PARSE_BAD;
    }
    for (index = 0; index < count; index++) {
        result = marshal_reader_object(reader, &is_null);
        if (result != PARSE_OK) {
            return result;
        }
        if (is_null) {
            return PARSE_BAD;
        }
    }
    marshal_reader_finish_delayed_reference(reader, delayed_reference && flag);
    return PARSE_OK;
}

static ParseResult
marshal_reader_code(MarshalReader *reader, int flag)
{
    ParseResult result;
    int64_t ignored;
    int is_null;
    int index;

    if (!marshal_reader_reserve_delayed_reference(reader, flag)) {
        return PARSE_BAD;
    }
    for (index = 0; index < 5; index++) {
        result = marshal_reader_i32(reader, &ignored);
        if (result != PARSE_OK) {
            return result;
        }
    }
    for (index = 0; index < 8; index++) {
        result = marshal_reader_object(reader, &is_null);
        if (result != PARSE_OK) {
            return result;
        }
        if (is_null) {
            return PARSE_BAD;
        }
    }
    result = marshal_reader_i32(reader, &ignored);
    if (result != PARSE_OK) {
        return result;
    }
    for (index = 0; index < 2; index++) {
        result = marshal_reader_object(reader, &is_null);
        if (result != PARSE_OK) {
            return result;
        }
        if (is_null) {
            return PARSE_BAD;
        }
    }
    marshal_reader_finish_delayed_reference(reader, flag);
    return PARSE_OK;
}

static ParseResult
marshal_reader_object(MarshalReader *reader, int *is_null)
{
    ParseResult result = PARSE_OK;
    unsigned int code;
    unsigned int type;
    int flag;
    int64_t count;
    unsigned int byte_count;
#if PY_VERSION_HEX >= 0x030e0000
    int nested_null;
#endif

    *is_null = 0;
    result = marshal_reader_byte(reader, &code);
    if (result != PARSE_OK) {
        return result;
    }
    reader->depth++;
    if (reader->depth > MARSHAL_MAX_STACK_DEPTH) {
        result = PARSE_BAD;
        goto done;
    }
    flag = (code & MARSHAL_FLAG_REF) != 0;
    type = code & ~MARSHAL_FLAG_REF;

    switch (type) {
    case TYPE_NULL:
        *is_null = 1;
        result = PARSE_NULL;
        break;
    case TYPE_NONE:
    case TYPE_FALSE:
    case TYPE_TRUE:
    case TYPE_STOPITER:
    case TYPE_ELLIPSIS:
        break;
    case TYPE_INT:
        result = marshal_reader_i32(reader, &count);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_INT64:
        result = marshal_reader_take(reader, 8);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_LONG:
        result = marshal_reader_long(reader);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_FLOAT:
        result = marshal_reader_float_text(reader);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_BINARY_FLOAT:
        result = marshal_reader_take(reader, 8);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_COMPLEX:
        result = marshal_reader_float_text(reader);
        if (result == PARSE_OK) {
            result = marshal_reader_float_text(reader);
        }
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_BINARY_COMPLEX:
        result = marshal_reader_take(reader, 8);
        if (result == PARSE_OK) {
            result = marshal_reader_take(reader, 8);
        }
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_STRING:
        result = marshal_reader_sized_bytes(reader, 0);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_INTERNED:
    case TYPE_UNICODE:
        result = marshal_reader_sized_bytes(reader, 1);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_ASCII:
    case TYPE_ASCII_INTERNED:
        result = marshal_reader_sized_bytes(reader, 0);
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_SHORT_ASCII:
    case TYPE_SHORT_ASCII_INTERNED:
        result = marshal_reader_byte(reader, &byte_count);
        if (result == PARSE_OK) {
            result = marshal_reader_take(reader, byte_count);
        }
        if (result == PARSE_OK && flag && !marshal_reader_add_reference(reader)) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_SMALL_TUPLE:
        result = marshal_reader_byte(reader, &byte_count);
        if (result == PARSE_OK) {
            result = marshal_reader_sequence(reader, byte_count, flag);
        }
        break;
    case TYPE_TUPLE:
    case TYPE_LIST:
        result = marshal_reader_i32(reader, &count);
        if (result == PARSE_OK) {
            result = marshal_reader_sequence(reader, count, flag);
        }
        break;
    case TYPE_DICT:
        result = marshal_reader_dict(reader, flag);
        break;
    case TYPE_SET:
    case TYPE_FROZENSET:
        result = marshal_reader_i32(reader, &count);
        if (result == PARSE_OK) {
            result = marshal_reader_set(
                reader,
                count,
                flag,
                type == TYPE_FROZENSET && count != 0
            );
        }
        break;
    case TYPE_CODE:
        if (!reader->allow_code) {
            result = PARSE_BAD;
            break;
        }
        result = marshal_reader_code(reader, flag);
        break;
#if PY_VERSION_HEX >= 0x030e0000
    case TYPE_SLICE:
        if (!marshal_reader_reserve_delayed_reference(reader, flag)) {
            result = PARSE_BAD;
            break;
        }
        result = marshal_reader_object(reader, &nested_null);
        if (result == PARSE_OK && nested_null) {
            result = PARSE_BAD;
        }
        if (result == PARSE_OK) {
            result = marshal_reader_object(reader, &nested_null);
        }
        if (result == PARSE_OK && nested_null) {
            result = PARSE_BAD;
        }
        if (result == PARSE_OK) {
            result = marshal_reader_object(reader, &nested_null);
        }
        if (result == PARSE_OK && nested_null) {
            result = PARSE_BAD;
        }
        if (result == PARSE_OK) {
            marshal_reader_finish_delayed_reference(reader, flag);
        }
        break;
#endif
    case TYPE_REF:
        result = marshal_reader_i32(reader, &count);
        if (result == PARSE_OK &&
            (count < 0 || (uint64_t)count >= reader->reference_count ||
             marshal_reader_reference_is_pending(reader, (size_t)count))) {
            result = PARSE_BAD;
        }
        break;
    case TYPE_UNKNOWN:
    default:
        result = PARSE_BAD;
        break;
    }

done:
    reader->depth--;
    return result;
}

AleffMarshalReaderResult
aleff_marshal_reader_examine(
    const unsigned char *data,
    size_t size,
    int allow_code
)
{
    MarshalReader reader = {
        .data = data,
        .size = size,
        .allow_code = allow_code != 0,
    };
    int is_null = 0;
    ParseResult parse_result;
    AleffMarshalReaderResult result;

    if (data == NULL && size != 0) {
        result.status = ALEFF_MARSHAL_READER_MALFORMED;
        result.boundary = 0;
        result.next_read_size = 0;
        return result;
    }
    parse_result = marshal_reader_object(&reader, &is_null);
    if (parse_result == PARSE_NEED) {
        result.status = ALEFF_MARSHAL_READER_NEED_READ;
        result.boundary = reader.position;
        result.next_read_size = reader.next_read_size;
    }
    else if (parse_result == PARSE_OK && !is_null) {
        result.status = ALEFF_MARSHAL_READER_COMPLETE;
        result.boundary = reader.position;
        result.next_read_size = 0;
    }
    else {
        result.status = ALEFF_MARSHAL_READER_MALFORMED;
        result.boundary = reader.position;
        result.next_read_size = 0;
    }
    return result;
}
