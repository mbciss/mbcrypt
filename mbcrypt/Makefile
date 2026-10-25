# ===========================================================================
# mbcrypt — Makefile (Linux / macOS)
# Builds the standalone PHP-file encryption tool.
#
# Requirements:
#   zlib   (always required)
#   OpenSSL >= 1.1.0  (optional — enables AES-256-GCM)
#
# Targets:
#   make            Build mbcrypt (auto-detect OpenSSL)
#   make static     Statically linked binary
#   make debug      Debug build with sanitizers
#   make valgrind   Run valgrind memory check
#   make install    Install to /usr/local/bin
#   make clean      Remove build artefacts
# ===========================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lz

SRCS = mbcrypt.c mbcrypt_lib.c
OBJS = $(SRCS:.c=.o)

# ---------------------------------------------------------------------------
# Auto-detect OpenSSL via pkg-config
# ---------------------------------------------------------------------------
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)

ifneq ($(OPENSSL_LIBS),)
    CFLAGS  += $(OPENSSL_CFLAGS) -DMBCRYPT_HAS_OPENSSL
    LDFLAGS += $(OPENSSL_LIBS)
    $(info [mbcrypt] AES-256-GCM: enabled (OpenSSL found via pkg-config))
else
    $(info [mbcrypt] AES-256-GCM: disabled -- install libssl-dev and rerun make)
endif

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all static debug valgrind install clean

all: mbcrypt

mbcrypt: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c mbcrypt_lib.h
	$(CC) $(CFLAGS) -c -o $@ $<

static: LDFLAGS += -static
static: mbcrypt

debug: CFLAGS  = -Wall -Wextra -g -O0 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: mbcrypt

valgrind: debug
	valgrind --tool=memcheck --leak-check=full --track-origins=yes \
		./mbcrypt --help

install: mbcrypt
	install -m 755 mbcrypt /usr/local/bin/mbcrypt
	@echo "Installed: /usr/local/bin/mbcrypt"

clean:
	rm -f $(OBJS) mbcrypt
