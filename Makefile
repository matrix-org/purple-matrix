#!/usr/bin/make -f

CC=gcc
LIBS=purple json-glib-1.0 glib-2.0 sqlite3

PKG_CONFIG=pkg-config
CFLAGS+=$(shell $(PKG_CONFIG) --cflags $(LIBS))
CFLAGS+=-fPIC -DPIC
LDLIBS+=$(shell $(PKG_CONFIG) --libs $(LIBS))
LDLIBS+=-lhttp_parser

ifndef MATRIX_NO_E2E
LDLIBS+=-lolm
endif

PLUGIN_DIR_PURPLE	=  $(shell $(PKG_CONFIG) --variable=plugindir purple)
DATA_ROOT_DIR_PURPLE	=  $(shell $(PKG_CONFIG) --variable=datarootdir purple)

TARGET=libmatrix.so

include Makefile.common
