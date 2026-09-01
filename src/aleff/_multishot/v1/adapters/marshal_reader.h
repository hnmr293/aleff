#ifndef ALEFF_CONTINUATION_ADAPTERS_MARSHAL_READER_H
#define ALEFF_CONTINUATION_ADAPTERS_MARSHAL_READER_H

#include <stddef.h>

typedef enum {
    ALEFF_MARSHAL_READER_COMPLETE = 0,
    ALEFF_MARSHAL_READER_MALFORMED = 1,
    ALEFF_MARSHAL_READER_NEED_READ = 2,
} AleffMarshalReaderStatus;

typedef struct {
    AleffMarshalReaderStatus status;
    /* The first byte after the object, or the CPython stop boundary. */
    size_t boundary;
    /* The exact size of the next readinto request when status is NEED_READ. */
    size_t next_read_size;
} AleffMarshalReaderResult;

/* Examine one exact prefix of one marshal object.  data may be NULL only when
 * size is zero.  allow_code follows marshal.load's allow_code argument. */
AleffMarshalReaderResult aleff_marshal_reader_examine(
    const unsigned char *data,
    size_t size,
    int allow_code
);

#endif
