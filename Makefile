TOPDIR = $(shell pwd)
PJNAME = livestream_segmenter

CROSS = $(CP)
DEBUG = 0
BINARY = exec
TARGET = $(PJNAME)
INSTALL_PATH = $(TOPDIR)
SRCPATH = $(TOPDIR)
INCPATH = $(TOPDIR)
LIBDIR = /usr/local/lib
LIBS=pthread dl
CFLAGS = -Wall -Wno-deprecated-declarations
LDFLAGS=
START_CMD = chmod +x configure; ./configure

include common.mk