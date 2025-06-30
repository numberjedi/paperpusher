CC = clang
# default to prod flags
CFLAGS = -std=c99 -Wall -Wextra -I. -Isrc -O3
LDFLAGS =
# check for "asan" arg and add debug flags if set
ifeq ($(MAKECMDGOALS),asan)
	CFLAGS += -g -O0 -fsanitize=address -fno-omit-frame-pointer
	LDFLAGS += -fsanitize=address
endif
# check for "lsan" arg and add debug flags if set
ifeq ($(MAKECMDGOALS),gdb)
	CFLAGS += -g -O0 -fno-omit-frame-pointer
	LDFLAGS +=
endif

PKG_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0 poppler-glib)
PKG_LIBS    := $(shell pkg-config --libs gtk+-3.0 poppler-glib)

CFLAGS += $(PKG_CFLAGS)
LDFLAGS += $(PKG_LIBS) -lcjson -lm

SOURCES = $(wildcard src/*.c src/gui/*.c src/cJSON/cJSON.c)
OBJECTS = $(patsubst src/%,build/%, $(SOURCES:.c=.o))

APP_SOURCES := $(filter-out src/main.c, $(SOURCES))
TEST_SOURCES = $(wildcard tests/*.c)
TEST_BINS = $(patsubst tests/%.c, build/test_%, $(TEST_SOURCES))

TARGET = paperpusher

all: $(TARGET)

asan: $(TARGET)

gdb: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

build/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

build/gui/%.o: src/gui/%.c
	$(CC) $(CFLAGS) -c $< -o $@

build/cJSON/cJSON.o: src/cJSON/cJSON.c
	$(CC) $(CFLAGS) -c $< -o $@

# tests

# add mocka flags for tests only
CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka)
CMOCKA_LDFLAGS := $(shell pkg-config --libs cmocka)
TEST_CFLAGS := $(CFLAGS) $(CMOCKA_CFLAGS)
TEST_LDFLAGS := $(LDFLAGS) $(CMOCKA_LDFLAGS)

# build & run all test binaries
test: $(TEST_BINS)
	@echo "Running tests..."
	@for bin in $^; do \
		echo $$bin; \
		LSAN_OPTIONS="suppressions=lsan.supp" ./$$bin; \
		echo ""; \
	done

#compile each test binary with the app sources
build/test_%: tests/%.c $(APP_SOURCES)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

# end tests
clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean asan gdb

