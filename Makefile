#!/usr/bin/make -f

CC=gcc
LIBS=purple json-glib-1.0 glib-2.0
CFLAGS+=-DPURPLE_PLUGINS $(shell pkg-config --cflags $(LIBS))
CFLAGS+=-fPIC -DPIC
CFLAGS+=-Wall -g -O0 -Werror
LDLIBS+=$(shell pkg-config --libs $(LIBS))
LDLIBS+=-lhttp_parser

# generate .d files when compiling
CPPFLAGS+=-MMD

OBJECTS=libmatrix.o matrix-api.o matrix-json.o matrix-login.o matrix-room.o \
    matrix-sync.o
TARGET=libmatrix.so

all: $(TARGET)
clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(TARGET)

install:
	mkdir -p $(DESTDIR)/usr/lib/purple-2
	install -m 664 $(TARGET) $(DESTDIR)/usr/lib/purple-2/


$(TARGET): $(OBJECTS)
	$(LINK.o) -shared $^ $(LOADLIBES) $(LDLIBS) -o $@

-include $(OBJECTS:.o=.d)
