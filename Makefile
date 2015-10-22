#!/usr/bin/make -f

CC=gcc
LIBS=purple json-glib-1.0 glib-2.0

PKG_CONFIG=pkg-config
CFLAGS+=-DPURPLE_PLUGINS $(shell $(PKG_CONFIG) --cflags $(LIBS))
CFLAGS+=-fPIC -DPIC
CFLAGS+=-Wall -g -O0 -Werror
LDLIBS+=$(shell pkg-config --libs $(LIBS))
LDLIBS+=-lhttp_parser

PLUGIN_DIR_PURPLE	=  $(shell $(PKG_CONFIG) --variable=plugindir purple)
DATA_ROOT_DIR_PURPLE	=  $(shell $(PKG_CONFIG) --variable=datarootdir purple)


# generate .d files when compiling
CPPFLAGS+=-MMD

OBJECTS=libmatrix.o matrix-api.o matrix-json.o matrix-login.o matrix-room.o \
    matrix-sync.o
TARGET=libmatrix.so

all: $(TARGET)
clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(TARGET)

install:
	mkdir -p $(DESTDIR)$(PLUGIN_DIR_PURPLE)
	install -m 664 $(TARGET) $(DESTDIR)$(PLUGIN_DIR_PURPLE)
	for i in 16 22 48; do \
	    mkdir -p $(DESTDIR)$(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/$$i; \
	    install -m 664 matrix-$${i}px.png $(DESTDIR)$(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/$$i/matrix.png; \
	done


$(TARGET): $(OBJECTS)
	$(LINK.o) -shared $^ $(LOADLIBES) $(LDLIBS) -o $@

-include $(OBJECTS:.o=.d)
