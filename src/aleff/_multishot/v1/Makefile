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
ADAPTER_SRC = adapters/framework.c adapters/iterators.c adapters/builtins.c \
	adapters/containers.c adapters/adapters_bootstrap.c adapters/api.h
SRC = _aleff.c adapters/framework.c

.PHONY: build debug clean

build: $(TARGET)

debug: $(SRC) $(ADAPTER_SRC)
	$(CC) $(DEBUG_CFLAGS) -shared $(LDFLAGS) -I$(INCLUDE) -o $(TARGET) $(SRC)

$(TARGET): $(SRC) $(ADAPTER_SRC)
	$(CC) $(CFLAGS) -shared $(LDFLAGS) -I$(INCLUDE) -o $@ $(SRC)

clean:
	rm -f _aleff*.so
