PREFIX ?= /usr/local
DEVICE := ../device
LIB := $(DEVICE)/target/release/libverdant_memory.a
LDLIBS := -lpthread -ldl -lm

all: build/vram-host build/vram-scale build/vram-ram-file

runtime:
	cd $(DEVICE) && python3 build.py

build/vram-host: vram_host.c managed.h $(DEVICE)/native/pager.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

build/vram-scale: $(DEVICE)/validation/scale.c $(DEVICE)/validation/capacity_pattern.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

FUSE_CFLAGS ?= $(shell pkg-config --cflags fuse3)
FUSE_LIBS ?= $(shell pkg-config --libs fuse3)

build/vram-ram-file: ram_file.c qualification.h projection.c projection_ioctl.h projection.h $(DEVICE)/native/pager.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 $(FUSE_CFLAGS) -I$(DEVICE)/native $< projection.c $(LIB) $(RAM_FILE_LDFLAGS) $(FUSE_LIBS) $(LDLIBS) -o $@

install:
	install -Dm644 ../site/assets/mark.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/io.github.afflom.vRAM.svg
	install -Dm644 packaging/io.github.afflom.vRAM.metainfo.xml $(DESTDIR)$(PREFIX)/share/metainfo/io.github.afflom.vRAM.metainfo.xml
	install -Dm644 projection.h $(DESTDIR)$(PREFIX)/include/vram/projection.h
	install -Dm755 build/vram-ram-file $(DESTDIR)$(PREFIX)/lib/vram/vram-ram-file
	install -Dm755 vram-machine $(DESTDIR)$(PREFIX)/bin/vram-machine
	install -Dm755 build/vram-host $(DESTDIR)$(PREFIX)/sbin/vram-host

.PHONY: all runtime install

# A local source installation keeps the administrator's existing configuration.
install-service: install
	install -d $(DESTDIR)/etc/systemd/system/multi-user.target.wants
	sed 's|/usr/bin/vram-machine|$(PREFIX)/bin/vram-machine|' packaging/vram-platform.service > $(DESTDIR)/etc/systemd/system/vram-platform.service
	ln -sf ../vram-platform.service $(DESTDIR)/etc/systemd/system/multi-user.target.wants/vram-platform.service
	install -d $(DESTDIR)/etc/systemd/system $(DESTDIR)/etc/default
	sed 's|/usr/sbin/vram-host|$(PREFIX)/sbin/vram-host|' packaging/vram@.service > $(DESTDIR)/etc/systemd/system/vram@.service.new
	chmod 644 $(DESTDIR)/etc/systemd/system/vram@.service.new
	mv $(DESTDIR)/etc/systemd/system/vram@.service.new $(DESTDIR)/etc/systemd/system/vram@.service
	test -e $(DESTDIR)/etc/default/vram || install -m644 packaging/vram.default $(DESTDIR)/etc/default/vram

.PHONY: install-service

# Same server and store as the release, with an observation at the two store calls.
build/vram-host-clock: vram_host.c managed.h tests/swap_clock.c $(DEVICE)/native/pager.h runtime
	mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -I$(DEVICE)/native $< tests/swap_clock.c $(LIB) $(LDFLAGS) \
		-Wl,--wrap=store_get,--wrap=store_put $(LDLIBS) -o $@
