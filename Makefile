# Samsung SDI ELPM482-00005 — Victron Cerbo GX Integration
# Build the C driver (native compilation on Cerbo GX)

.PHONY: all clean install

CC      := gcc
CFLAGS  := -Os -s -Wall -Wextra -std=c99
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
