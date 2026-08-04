# jackoviz — JACK + FFTW3 + Datoviz spectrogram surface
#
# Dependencies:
#   - JACK (pkg-config: jack)
#   - FFTW3 (pkg-config: fftw3)
#   - Datoviz 0.4 (set DATOVIZ_ROOT or rely on datoviz-config)

CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-typedef-redefinition
LDFLAGS  ?=

PKG_CFLAGS := $(shell pkg-config --cflags jack fftw3 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs jack fftw3 2>/dev/null) -lm

# Prefer an explicit Datoviz source/build tree (common during v0.4 RC development).
DATOVIZ_ROOT ?= $(HOME)/work/datoviz

ifeq ($(wildcard $(DATOVIZ_ROOT)/include/datoviz.h),$(DATOVIZ_ROOT)/include/datoviz.h)
  DVZ_CFLAGS := -I$(DATOVIZ_ROOT)/include
  DVZ_LIBS   := -L$(DATOVIZ_ROOT)/build/src -ldatoviz -Wl,-rpath,$(DATOVIZ_ROOT)/build/src
else ifneq ($(shell command -v datoviz-config 2>/dev/null),)
  DVZ_CFLAGS := $(shell datoviz-config --cflags)
  DVZ_LIBS   := $(shell datoviz-config --libs)
else
  $(warning Datoviz not found. Set DATOVIZ_ROOT or install datoviz-config on PATH.)
  DVZ_CFLAGS :=
  DVZ_LIBS   := -ldatoviz
endif

# Linux may need -lrt for shm_open on older glibc; harmless to try via linker group.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  EXTRA_LIBS := -lrt -lpthread
else
  EXTRA_LIBS :=
endif

.PHONY: all clean run

all: jackoviz

jackoviz: jackoviz.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $(DVZ_CFLAGS) -o $@ $< $(LDFLAGS) $(PKG_LIBS) $(DVZ_LIBS) $(EXTRA_LIBS)

run: jackoviz
	./jackoviz -s system:capture_1

clean:
	rm -f jackoviz
