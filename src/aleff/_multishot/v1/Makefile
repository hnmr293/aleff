PYTHON ?= python3
EXT_SUFFIX := $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")
INCLUDE := $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_path('include'))")
CC ?= cc
CFLAGS ?= -std=c2x -Wall -Wextra -Wpedantic -O2 -fPIC
DEBUG_CFLAGS = -std=c2x -Wall -Wextra -Wpedantic -O0 -g3 -fPIC

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -undefined dynamic_lookup
endif

TARGET = _aleff$(EXT_SUFFIX)
ADAPTER_SRC = adapters/framework.c adapters/critical_sections.c \
	adapters/buffers.c adapters/binascii.c adapters/compression.c adapters/hashing.c \
	adapters/module_functions.c \
	adapters/bisect.c adapters/heapq.c adapters/regex.c \
	adapters/numeric.c adapters/numeric_iterators.c adapters/struct.c \
	adapters/datetime.c adapters/zoneinfo.c \
	adapters/io.c adapters/io_buffered.c adapters/io_text.c adapters/codecs.c \
	adapters/marshal.c adapters/marshal_reader.c adapters/marshal_stream.c \
	adapters/csv.c adapters/json.c adapters/json_encoder.c adapters/json_decoder.c adapters/pickle.c \
	adapters/iterators.c adapters/iterator_snapshots.c adapters/itertools.c \
	adapters/builtins.c adapters/protocols.c adapters/containers.c adapters/mappings.c \
	adapters/sets.c adapters/text.c adapters/operator.c adapters/functools.c \
	adapters/adapters_bootstrap.c adapters/sort_engine.c
ADAPTER_HEADERS = adapters/api.h adapters/critical_sections.h \
	adapters/binascii.h adapters/buffers.h adapters/compression.h adapters/hashing.h \
	adapters/module_functions.h \
	adapters/bisect.h adapters/builtins.h adapters/containers.h \
	adapters/functools.h adapters/internal.h adapters/iterators.h adapters/itertools.h \
	adapters/heapq.h adapters/mappings.h adapters/marshal.h adapters/marshal_reader.h \
	adapters/numeric.h adapters/numeric_iterators.h adapters/struct.h \
	adapters/datetime.h adapters/zoneinfo.h adapters/io.h adapters/io_buffered.h \
	adapters/io_text.h adapters/codecs.h \
	adapters/marshal_stream.h adapters/operator.h adapters/protocols.h adapters/regex.h adapters/sets.h \
	adapters/csv.h adapters/json.h adapters/json_encoder.h adapters/json_decoder.h adapters/pickle.h \
	adapters/sort_engine.h adapters/text.h
SRC = _aleff.c $(ADAPTER_SRC)

.PHONY: build debug clean

build: $(TARGET)

debug: $(SRC) $(ADAPTER_HEADERS)
	$(CC) $(DEBUG_CFLAGS) -shared $(LDFLAGS) -I$(INCLUDE) -o $(TARGET) $(SRC)

$(TARGET): $(SRC) $(ADAPTER_HEADERS)
	$(CC) $(CFLAGS) -shared $(LDFLAGS) -I$(INCLUDE) -o $@ $(SRC)

clean:
	rm -f _aleff*.so
