# The uapi header lives in the dxgdrm repo; point DXGDRM_INCLUDE at a checkout.
DXGDRM_INCLUDE ?= ../../dxgdrm

PKGS    := egl glesv2 gbm libdrm libjpeg
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -pthread \
           -I$(DXGDRM_INCLUDE) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm -pthread

PREFIX  ?= /usr/local

weaselway-presenter: presenter.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: weaselway-presenter
	install -D -m 0755 weaselway-presenter $(DESTDIR)$(PREFIX)/bin/weaselway-presenter

clean:
	rm -f weaselway-presenter

.PHONY: install clean
