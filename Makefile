PREFIX ?= $(HOME)/.local
PKGS    = wayland-client wayland-cursor cairo pangocairo fontconfig gio-unix-2.0 gdk-pixbuf-2.0 libpipewire-0.3
CFLAGS += -O2 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm
XDG    ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
LS      = protocol/wlr-layer-shell-unstable-v1.xml
GEN     = wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c

spotglass: spotglass.c $(GEN)
	$(CC) $(CFLAGS) -o $@ spotglass.c wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c $(LDFLAGS) $(LDLIBS)

wlr-layer-shell-unstable-v1-client-protocol.h: $(LS)
	wayland-scanner client-header $< $@
wlr-layer-shell-unstable-v1-protocol.c: $(LS)
	wayland-scanner private-code $< $@
xdg-shell-protocol.c: $(XDG)
	wayland-scanner private-code $< $@

install: spotglass
	install -Dm755 spotglass $(DESTDIR)$(PREFIX)/bin/spotglass

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/spotglass

clean:
	rm -f spotglass $(GEN)

.PHONY: install uninstall clean
