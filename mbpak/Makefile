# ===========================================================================
# mbpak — Makefile (Linux / macOS)
# Builds the standalone .mbpak archive CLI.
#
# mbpak reuses the AES-256-GCM / zlib primitives from ../mbcrypt/mbcrypt_lib.c
# as-is (that file is compiled here into a *local* object, ../mbcrypt/ itself
# is never modified or written to).
#
# Requirements:
#   zlib            (always required)
#   OpenSSL >= 1.1.0 (required — .mbpak is AES-256-GCM only, no legacy mode)
#
# Targets:
#   make            Build mbpak
#   make debug      Debug build with sanitizers
#   make test       Build then run the end-to-end shell test (test/run.sh)
#   make clean      Remove build artefacts
# ===========================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I../mbcrypt
LDFLAGS = -lz

MBCRYPT_DIR = ../mbcrypt

SRCS = mbpak_lib.c mbpak_cli.c
OBJS = $(SRCS:.c=.o) mbcrypt_lib.o

# ---------------------------------------------------------------------------
# Auto-detect OpenSSL via pkg-config — mandatory here (unlike mbcrypt, which
# can fall back to legacy XOR, .mbpak archives have no non-AES mode).
# ---------------------------------------------------------------------------
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)

ifeq ($(OPENSSL_LIBS),)
    $(error [mbpak] OpenSSL not found -- install libssl-dev/openssl-devel and rerun make)
endif
CFLAGS  += $(OPENSSL_CFLAGS) -DMBCRYPT_HAS_OPENSSL
LDFLAGS += $(OPENSSL_LIBS)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all debug test clean

all: mbpak

mbpak: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

mbpak_lib.o: mbpak_lib.c mbpak_lib.h $(MBCRYPT_DIR)/mbcrypt_lib.h
	$(CC) $(CFLAGS) -c -o $@ $<

mbpak_cli.o: mbpak_cli.c mbpak_lib.h
	$(CC) $(CFLAGS) -c -o $@ $<

mbcrypt_lib.o: $(MBCRYPT_DIR)/mbcrypt_lib.c $(MBCRYPT_DIR)/mbcrypt_lib.h
	$(CC) $(CFLAGS) -c -o $@ $<

debug: CFLAGS  += -g -O0 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: mbpak

test: mbpak
	@bash test/run.sh

clean:
	rm -f $(OBJS) mbpak
	rm -rf test/tmp
