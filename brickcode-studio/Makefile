# Build with:  make        Install with:  make install PREFIX=/usr/local
CC      ?= cc
PREFIX  ?= /usr/local
CFLAGS  ?= -O2
PKGS     = gtk4 libusb-1.0

brickcode-studio: brickcode-studio.c nqc_tables.inc i18n_es.inc
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ brickcode-studio.c \
	    $$(pkg-config --libs $(PKGS)) -lm

install: brickcode-studio
	install -Dm755 brickcode-studio $(DESTDIR)$(PREFIX)/bin/brickcode-studio
	for s in 48 64 128 256 512; do install -Dm644 icon-$$s.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/brickcode-studio.png; done
	install -Dm644 brickcode-studio.desktop $(DESTDIR)$(PREFIX)/share/applications/brickcode-studio.desktop
	install -Dm644 brickcode-studio.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/brickcode-studio.svg
	for f in ../firmware/*.lgo; do install -Dm644 $$f $(DESTDIR)$(PREFIX)/share/brickcode-studio/firmware/$$(basename $$f); done

clean:
	rm -f brickcode-studio
