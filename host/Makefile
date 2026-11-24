PREFIX ?= /usr/local
DEVICE := ../device
LIB := $(DEVICE)/target/release/libverdant_memory.a
LDLIBS := -lpthread -ldl -lm

all: build/vram-host build/vram-scale

runtime:
	cd $(DEVICE) && python3 build.py

build/vram-host: vram_host.c managed.h $(DEVICE)/native/pager.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

build/vram-scale: $(DEVICE)/validation/scale.c $(DEVICE)/validation/capacity_pattern.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

install:
	install -Dm755 build/vram-host $(DESTDIR)$(PREFIX)/sbin/vram-host

.PHONY: all runtime install

# A local source installation keeps the administrator's existing configuration.
install-service: install
	install -d $(DESTDIR)/etc/systemd/system $(DESTDIR)/etc/default
	sed 's|/usr/sbin/vram-host|$(PREFIX)/sbin/vram-host|' packaging/vram@.service > $(DESTDIR)/etc/systemd/system/vram@.service.new
	chmod 644 $(DESTDIR)/etc/systemd/system/vram@.service.new
	mv $(DESTDIR)/etc/systemd/system/vram@.service.new $(DESTDIR)/etc/systemd/system/vram@.service
	test -e $(DESTDIR)/etc/default/vram || install -m644 packaging/vram.default $(DESTDIR)/etc/default/vram

.PHONY: install-service
