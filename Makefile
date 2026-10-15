PROG    := bimg
CC      ?= cc
SCANNER ?= wayland-scanner
XDG_XML ?= /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
PC      := wayland-client egl glesv2 wayland-egl libjpeg libturbojpeg libpng libwebp libwebpdemux libwebpmux libtiff-4

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror
CPPFLAGS += $(shell pkg-config --cflags $(PC))
LDLIBS  += $(shell pkg-config --libs $(PC)) -lm

GEN := xdg-shell-protocol.h xdg-shell-protocol.c

.PHONY: all clean
all: $(PROG)

$(PROG): bimg.c image.c image.h $(GEN) Makefile
	$(CC) $(CFLAGS) $(CPPFLAGS) bimg.c image.c $(GEN) $(LDLIBS) -o $@

xdg-shell-protocol.h: $(XDG_XML)
	$(SCANNER) client-header $< $@

xdg-shell-protocol.c: $(XDG_XML)
	$(SCANNER) private-code $< $@

clean:
	rm -f $(PROG) $(GEN)
