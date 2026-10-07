# The uapi header lives in the dxgdrm repo; point DXGDRM_INCLUDE at a checkout.
DXGDRM_INCLUDE ?= ../../dxgdrm

PKGS    := egl glesv2 gbm libdrm freerdp3 freerdp-server3 winpr3 libcrypto \
           glib-2.0 gio-2.0 gio-unix-2.0 libpng wayland-client
CFLAGS  ?= -O2 -g
# -isystem: FreeRDP's headers warn about their own deprecated declarations.
CFLAGS  += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -pthread \
           -I$(DXGDRM_INCLUDE) $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(PKGS)))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm -pthread

SELECTION := selection.c selection-mutter.c selection-wayland.c ext-data-control-v1-protocol.c
SOURCES := weaselwayd.c rdp.c input.c audio.c clipboard.c formats.c placeholder.c log.c \
           $(SELECTION)
# The gfxredir server channel, which distributions build FreeRDP without; see
# gfxredir/README.md. Upstream code, so it is spared our warnings.
GFXREDIR := gfxredir/gfxredir_server.c gfxredir/gfxredir_common.c
HEADERS := rdp.h input.h audio.h clipboard.h selection.h selection-backends.h formats.h \
           placeholder.h log.h ext-data-control-v1-client-protocol.h $(wildcard gfxredir/*.h)
OBJECTS := $(SOURCES:.c=.o) $(GFXREDIR:.c=.o)

# The clipboard protocol of KWin and the wlroots compositors.
DATA_CONTROL_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-data-control/ext-data-control-v1.xml
WAYLAND_SCANNER ?= wayland-scanner

ext-data-control-v1-client-protocol.h: $(DATA_CONTROL_XML)
	$(WAYLAND_SCANNER) client-header $< $@

ext-data-control-v1-protocol.c: $(DATA_CONTROL_XML)
	$(WAYLAND_SCANNER) private-code $< $@

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

weaselwayd: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

gfxredir/%.o: gfxredir/%.c $(HEADERS)
	$(CC) $(CFLAGS) -w -Igfxredir -c -o $@ $<

# For development: every out-of-bounds access, use-after-free and undefined
# operation aborts with a report. Slow; not for the image.
ASAN := -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
ASAN_OBJECTS := $(OBJECTS:.o=.asan.o)

asan: weaselwayd-asan

weaselwayd-asan: $(ASAN_OBJECTS)
	$(CC) $(CFLAGS) $(ASAN) $(LDFLAGS) -o $@ $(ASAN_OBJECTS) $(LDLIBS)

%.asan.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $(ASAN) -c -o $@ $<

gfxredir/%.asan.o: gfxredir/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(ASAN) -w -Igfxredir -c -o $@ $<

# GCC's static analyzer; reports leaks, double frees and NULL dereferences on
# paths it can prove.
analyze: ext-data-control-v1-client-protocol.h ext-data-control-v1-protocol.c
	$(CC) $(CFLAGS) -fanalyzer -fsyntax-only $(SOURCES)

# The clipboard's conversions, which need nothing but GLib and libpng to run.
TEST_PKGS := glib-2.0 gio-2.0 gio-unix-2.0 libpng wayland-client
TEST_CFLAGS := -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -g -I. \
               $(shell pkg-config --cflags $(TEST_PKGS))
TEST_LIBS := $(shell pkg-config --libs $(TEST_PKGS))

tests/test-formats: tests/test-formats.c formats.c formats.h
	$(CC) $(TEST_CFLAGS) -o $@ tests/test-formats.c formats.c $(TEST_LIBS)

# For trying the selection against a running compositor; see
# tests/selection-mutter.sh and tests/selection-kwin.sh.
tests/selection-tool: tests/selection-tool.c $(SELECTION) selection.h selection-backends.h \
                      ext-data-control-v1-client-protocol.h
	$(CC) $(TEST_CFLAGS) -o $@ tests/selection-tool.c $(SELECTION) $(TEST_LIBS)

# The RDP server and the clipboard without dxgdrm; see tests/clipboard-rdp.sh.
tests/rdp-harness: tests/rdp-harness.c $(filter-out weaselwayd.o,$(OBJECTS))
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ $^ $(LDLIBS)

check: tests/test-formats
	tests/test-formats

install: weaselwayd
	install -D -m 0755 weaselwayd $(DESTDIR)$(PREFIX)/bin/weaselwayd

clean:
	rm -f weaselwayd weaselwayd-asan $(OBJECTS) $(ASAN_OBJECTS) tests/test-formats tests/selection-tool \
		tests/rdp-harness ext-data-control-v1-client-protocol.h ext-data-control-v1-protocol.c

.PHONY: install clean asan analyze check
