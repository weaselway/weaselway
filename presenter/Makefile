# The uapi header lives in the dxgdrm repo; point DXGDRM_INCLUDE at a checkout.
DXGDRM_INCLUDE ?= ../../dxgdrm

PKGS    := egl glesv2 gbm libdrm libjpeg freerdp3 freerdp-server3 winpr3 libcrypto
CFLAGS  ?= -O2 -g
# -isystem: FreeRDP's headers warn about their own deprecated declarations.
CFLAGS  += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -pthread \
           -I$(DXGDRM_INCLUDE) $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(PKGS)))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm -pthread

SOURCES := presenter.c rdp.c input.c
# The gfxredir server channel, which distributions build FreeRDP without; see
# gfxredir/README.md. Upstream code, so it is spared our warnings.
GFXREDIR := gfxredir/gfxredir_server.c gfxredir/gfxredir_common.c
HEADERS := rdp.h input.h $(wildcard gfxredir/*.h)
OBJECTS := $(SOURCES:.c=.o) $(GFXREDIR:.c=.o)

# Hardening. None of this makes C memory-safe; it turns the common mistakes
# into a crash instead of something exploitable.
#   _FORTIFY_SOURCE=3          bounds-checked memcpy/snprintf/read where the size is known
#   stack-protector-strong     canaries on functions with arrays or address-taken locals
#   stack-clash-protection     no jumping over the guard page with a large frame
#   trivial-auto-var-init      uninitialized locals read as zero, not as stack leftovers
#   wrapv, no-strict-aliasing  signed overflow and type punning are defined, so the
#                              optimizer cannot delete the checks that guard against them
#   PIE + relro + now          ASLR for the binary, read-only GOT
HARDEN  := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong \
           -fstack-clash-protection -ftrivial-auto-var-init=zero \
           -fwrapv -fno-strict-aliasing -fPIE \
           -Wformat=2 -Werror=format-security -Wshadow -Wvla
# Control-flow integrity for indirect branches and returns, per architecture.
ifeq ($(shell $(CC) -dumpmachine | cut -d- -f1),x86_64)
HARDEN  += -fcf-protection=full
else ifeq ($(shell $(CC) -dumpmachine | cut -d- -f1),aarch64)
HARDEN  += -mbranch-protection=standard
endif
CFLAGS  += $(HARDEN)
LDFLAGS += -pie -Wl,-z,relro,-z,now,-z,noexecstack

PREFIX  ?= /usr/local

weaselway-presenter: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

gfxredir/%.o: gfxredir/%.c $(HEADERS)
	$(CC) $(CFLAGS) -w -Igfxredir -c -o $@ $<

# For development: every out-of-bounds access, use-after-free and undefined
# operation aborts with a report. Slow; not for the image.
asan:
	$(CC) $(CFLAGS) -w -Igfxredir -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
		$(LDFLAGS) -o weaselway-presenter-asan $(SOURCES) $(GFXREDIR) $(LDLIBS)

# GCC's static analyzer; reports leaks, double frees and NULL dereferences on
# paths it can prove.
analyze:
	$(CC) $(CFLAGS) -fanalyzer -fsyntax-only $(SOURCES)

install: weaselway-presenter
	install -D -m 0755 weaselway-presenter $(DESTDIR)$(PREFIX)/bin/weaselway-presenter

clean:
	rm -f weaselway-presenter weaselway-presenter-asan $(OBJECTS)

.PHONY: install clean asan analyze
