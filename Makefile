SUBDIRS = src/aleff/_multishot/v1

.PHONY: build debug clean install test

PYTHON ?= python3
export PYTHON

build:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir build; done

debug:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir debug; done

clean:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean; done

install:
	uv pip install -e ".[dev]"

test:
	./run_tests.sh
