PREFIX ?= /usr/local
DEVICE := ../device
LIB := $(DEVICE)/target/release/libverdant_memory.a
LDLIBS := -lpthread -ldl -lm

all: build/vram-host build/vram-scale

runtime:
	cd $(DEVICE) && python3 build.py

build/vram-host: vram_host.c $(DEVICE)/native/pager.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

build/vram-scale: $(DEVICE)/validation/scale.c $(DEVICE)/validation/capacity_pattern.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

install: build/vram-host
	install -Dm755 $< $(DESTDIR)$(PREFIX)/sbin/vram-host

.PHONY: all runtime install
