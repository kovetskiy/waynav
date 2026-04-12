.PHONY: all build test clean install

BUILDDIR ?= build

all: build

build:
	@test -d $(BUILDDIR) || meson setup $(BUILDDIR)
	meson compile -C $(BUILDDIR)

test: build
	meson test -C $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

install: build
	meson install -C $(BUILDDIR)
