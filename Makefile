.PHONY: all build test clean install lint lint-tidy lint-scan lint-cppcheck

BUILDDIR ?= build

all: build

build:
	@test -d $(BUILDDIR) || meson setup $(BUILDDIR)
	meson compile -C $(BUILDDIR)

test: build
	meson test -C $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

lint: build
	@$(MAKE) -j3 --no-print-directory lint-tidy lint-scan lint-cppcheck

lint-tidy: build
	@echo "── clang-tidy ──"
	@run-clang-tidy -p $(BUILDDIR) -j$$(nproc) -quiet src/*.c

lint-scan: build
	@echo "── scan-build ──"
	@ninja -C $(BUILDDIR) scan-build

lint-cppcheck: build
	@echo "── cppcheck ──"
	@mkdir -p $(BUILDDIR)/cppcheck
	@cppcheck --enable=all --std=c11 \
		--suppress=missingIncludeSystem \
		--suppress=missingInclude \
		--suppress=normalCheckLevelMaxBranches \
		--suppress=checkersReport \
		--cppcheck-build-dir=$(BUILDDIR)/cppcheck \
		-j$$(nproc) \
		-I src/ src/*.c

install: build
	meson install -C $(BUILDDIR)
