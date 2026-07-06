# Samsung SDI ELPM482-00005 — Victron Cerbo GX Integration
# Build the C driver (Samsung SDI → Victron CAN-BMS protocol translator)
#
# The driver translates Samsung SDI CAN PDOs (0x500-0x504, 0x5F0-0x5F4)
# into Victron CAN-bus BMS frames (0x351, 0x355, 0x356, 0x35A).
# The stock Venus OS CAN-BMS driver handles D-Bus publishing.

.PHONY: all clean install

CC      := gcc
CFLAGS  := -Os -s -Wall -Wextra -std=c99 -D_GNU_SOURCE
LDLIBS  := -lm
TARGET  := samsung-sdi-bms
SRCDIR  := src

all: $(TARGET)

$(TARGET): $(SRCDIR)/samsung-sdi-bms.c
	$(CC) $(CFLAGS) $(LDLIBS) -o $@ $<

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d /data/samsung-sdi
	install -m 755 $(TARGET) /data/samsung-sdi/
	@echo "Installed $(TARGET) to /data/samsung-sdi/"
