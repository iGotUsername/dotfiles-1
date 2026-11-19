PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
PKG_CONFIG ?= pkg-config
CC = cc
CFLAGS ?= -O2
PACKAGES = dbus-1 libpipewire-0.3
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
WARN = -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm
OBJ = main.o status.o services.o

all: everbar

# Check even when copied/stale build artifacts appear up to date. Order-only
# prerequisites avoid rebuilding everything merely because this check is phony.
check-deps:
	@command -v $(PKG_CONFIG) >/dev/null 2>&1 || { \
		printf '%s\n' 'everbar: pkg-config is required to build.' >&2; exit 1; }
	@$(PKG_CONFIG) --print-errors --exists $(PACKAGES) || { \
		printf '%s\n' 'everbar: install the dbus and PipeWire development packages first.' >&2; exit 1; }

everbar $(OBJ) test-status test-services: | check-deps

everbar: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): everbar.h
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -c -o $@ $<

install: everbar
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 everbar "$(DESTDIR)$(BINDIR)/everbar"

test-status: ../../tests/status_driver.c status.o everbar.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 $(LDFLAGS) -I. -o $@ ../../tests/status_driver.c status.o -lm

test-services: ../../tests/services_driver.c services.c status.o everbar.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 $(LDFLAGS) -I. -o $@ ../../tests/services_driver.c status.o $(LDLIBS)

test: everbar test-status test-services
	./test-services
	python3 ../../tests/test_everbar.py

test-integration: everbar
	python3 ../../tests/test_services.py

clean:
	rm -f everbar $(OBJ) test-status test-services

.PHONY: all check-deps install test test-integration clean
