ZIG     ?= zig
PREFIX  ?= /usr/local

.PHONY: all release debug test run install uninstall clean

all:
	$(ZIG) build

release:
	$(ZIG) build -Doptimize=ReleaseFast

debug:
	$(ZIG) build -Doptimize=Debug

test: all
	$(ZIG) build test

run: all
	$(ZIG) build run -- $(ARGS)

install: release
	install -d $(PREFIX)/bin $(PREFIX)/lib $(PREFIX)/include
	install -m 755 zig-out/bin/slop $(PREFIX)/bin/slop
	install -m 644 zig-out/lib/libslop.a $(PREFIX)/lib/libslop.a
	install -m 644 zig-out/include/slop.h $(PREFIX)/include/slop.h

uninstall:
	rm -f $(PREFIX)/bin/slop $(PREFIX)/lib/libslop.a $(PREFIX)/include/slop.h

clean:
	rm -rf zig-out .zig-cache
