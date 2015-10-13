#!/usr/bin/make -f

CC=gcc
GLIB_CFLAGS=$(shell pkg-config --cflags glib-2.0)
CFLAGS+=-I/usr/include/libpurple -DPURPLE_PLUGINS $(GLIB_CFLAGS)
CFLAGS+=-fPIC -DPIC
CFLAGS+=-Wall -g -O0

OBJECTS=libmatrix.o
TARGET=libmatrix.so

all: $(TARGET)
clean:
	rm -f $(OBJECTS) $(TARGET)

install:
	mkdir -p $(DESTDIR)/usr/lib/purple-2
	install -m 664 $(TARGET) $(DESTDIR)/usr/lib/purple-2/


$(TARGET): $(OBJECTS)
	$(LINK.o) -shared $^ $(LOADLIBES) $(LDLIBS) -o $@

