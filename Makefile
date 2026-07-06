# Samsung SDI ELPM482-00005 — Victron Cerbo GX Integration
# Build the C driver (direct D-Bus publishing via libdbus-1)
#
# The driver publishes all 28 Samsung SDI fields directly to D-Bus,
# implementing the com.victronenergy.BusItem interface.  Optionally
# also translates to Victron CAN-bus BMS frames (--can-bms flag).

.PHONY: all clean install

CC      := gcc
CFLAGS  := -Os -s -Wall -Wextra -std=c99 -D_GNU_SOURCE
PKG_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
LDLIBS  := -lm $(shell pkg-config --libs dbus-1 2>/dev/null || echo '-ldbus-1')
TARGET  := samsung-sdi-bms
SRCDIR  := src

all: $(TARGET)

$(TARGET): $(SRCDIR)/samsung-sdi-bms.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d /data/samsung-sdi
	install -m 755 $(TARGET) /data/samsung-sdi/
	@echo "Installed $(TARGET) to /data/samsung-sdi/"
