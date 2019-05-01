#!/usr/bin/make -f

CC=gcc
LIBS=purple json-glib-1.0 glib-2.0 sqlite3

PKG_CONFIG=pkg-config

PKG_CFLAGS=$(shell $(PKG_CONFIG) --cflags $(LIBS) || echo "FAILED")
ifeq ($(PKG_CFLAGS),FAILED)
$(error "$(PKG_CONFIG) failed")
else
CFLAGS+=$(PKG_CFLAGS)
endif
CFLAGS+=-fPIC -DPIC

PKG_LDLIBS=$(shell $(PKG_CONFIG) --libs $(LIBS) || echo "FAILED")
ifeq ($(PKG_LDLIBS),FAILED)
$(error "$(PKG_CONFIG) failed")
else
LDLIBS+=$(PKG_LDLIBS)
endif
LDLIBS+=-lhttp_parser

ifndef MATRIX_NO_E2E
LDLIBS+=-lolm -lgcrypt
endif

PLUGIN_DIR_PURPLE	=  $(shell $(PKG_CONFIG) --variable=plugindir purple)
DATA_ROOT_DIR_PURPLE	=  $(shell $(PKG_CONFIG) --variable=datarootdir purple)

TARGET=libmatrix.so

include Makefile.common
