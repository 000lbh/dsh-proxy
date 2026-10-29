CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -pthread

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR ?= /etc/systemd/system
CONFDIR ?= $(SYSCONFDIR)/dsh-proxy

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)
TARGET := dsh-proxy

.PHONY: all clean install install-systemd install-user uninstall check

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c -o $@ $<

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -d "$(DESTDIR)$(CONFDIR)"
	@if [ -f "$(DESTDIR)$(CONFDIR)/dsh-proxy.conf" ]; then \
		echo "keeping existing $(DESTDIR)$(CONFDIR)/dsh-proxy.conf"; \
	else \
		install -m 0644 packaging/dsh-proxy.conf "$(DESTDIR)$(CONFDIR)/dsh-proxy.conf"; \
	fi
	@echo "Installed the binary and config."
	@echo "For the systemd unit (recommended), run: sudo ./scripts/install.sh"

install-systemd: $(TARGET)
	./scripts/install.sh

install-user: $(TARGET)
	./scripts/install-user.sh

uninstall:
	-systemctl disable --now dsh-proxy 2>/dev/null || true
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"
	rm -f "$(DESTDIR)$(UNITDIR)/dsh-proxy.service"
	@echo "Kept $(DESTDIR)$(CONFDIR)/dsh-proxy.conf on purpose."

clean:
	rm -f $(TARGET) $(OBJECTS)

check: $(TARGET)
	./tests/test_proxy.sh
	./tests/test_random_port.sh
