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
ADAPTER_SRC = adapters/framework.c adapters/bisect.c adapters/heapq.c \
	adapters/iterators.c adapters/itertools.c \
	adapters/builtins.c adapters/protocols.c adapters/containers.c adapters/mappings.c \
	adapters/sets.c adapters/text.c adapters/operator.c adapters/functools.c \
	adapters/adapters_bootstrap.c adapters/sort_engine.c
ADAPTER_HEADERS = adapters/api.h adapters/bisect.h adapters/builtins.h adapters/containers.h \
	adapters/functools.h adapters/internal.h adapters/iterators.h adapters/itertools.h \
	adapters/heapq.h adapters/mappings.h adapters/operator.h adapters/protocols.h adapters/sets.h \
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
