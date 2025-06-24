CC = gcc
CFLAGS = -Wall -Wextra -I. -Isrc -g -O0 -fsanitize=address -fno-omit-frame-pointer
LDFLAGS = -fsanitize=address

PKG_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0 poppler-glib)
PKG_LIBS    := $(shell pkg-config --libs gtk+-3.0 poppler-glib)

CFLAGS += $(PKG_CFLAGS)
LDFLAGS += $(PKG_LIBS) -lcjson -lm

SOURCES = $(wildcard src/*.c src/gui/*.c src/cJSON/cJSON.c)
OBJECTS = $(SOURCES:.c=.o)
TARGET = paperpusher

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/gui/%.o: src/gui/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/cJSON/cJSON.o: src/cJSON/cJSON.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean

