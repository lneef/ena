QEMU_DIR := $(CURDIR)/qemu
QEMU_BUILD_DIR := $(QEMU_DIR)/build
QEMU_URL := https://gitlab.com/qemu-project/qemu.git
QEMU_REF := v11.0.1
QEMU_TARGETS := x86_64-softmmu
QEMU_PRIMARY_TARGET := x86_64-softmmu

# ninja targets built for a clean build: the emulator plus the qtest harness.
# Building these directly avoids the qemu-nbd / selinux.h breakage hit by a
# full `make -C build`.
QEMU_NINJA_TARGETS := qemu-system-x86_64 tests/qtest/qos-test

# In-tree glue (meson/Kconfig/qtest wiring) is kept out of the submodule and
# applied as a patch at build time, so the qemu submodule stays clean across
# worktrees. ena.c and the tests themselves live in this repo.
ENA_PATCH := $(CURDIR)/qemu-ena.patch

QEMU_CFLAGS := \
	-I$(QEMU_DIR) \
	-I$(QEMU_DIR)/include \
	-I$(QEMU_BUILD_DIR) \
	-I$(QEMU_BUILD_DIR)/include \
	-I$(QEMU_BUILD_DIR)/qapi \
	-I$(QEMU_BUILD_DIR)/$(QEMU_PRIMARY_TARGET) \
	-I$(QEMU_BUILD_DIR)/$(QEMU_PRIMARY_TARGET)/qapi

.DEFAULT_GOAL := build
.PHONY: build clean setup print-qemu-cflags qemu-patch qemu-unpatch qemu-configure

# ---------------------------------------------------------------------------
# Primary entry points
# ---------------------------------------------------------------------------

# One-time setup: fetch/check out the pinned qemu submodule. Run this once
# after cloning; build/clean never touch the submodule themselves.
setup:
	@if [ -d "$(QEMU_DIR)/.git" ] && \
		[ "$$(git -C "$(QEMU_DIR)" describe --tags --exact-match 2>/dev/null)" = "$(QEMU_REF)" ]; then \
		:; \
	elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
		git submodule update --init --recursive qemu; \
	elif [ -d "$(QEMU_DIR)/.git" ]; then \
		git -C "$(QEMU_DIR)" fetch --tags --depth 1 origin "$(QEMU_REF)" && \
		git -C "$(QEMU_DIR)" checkout "$(QEMU_REF)"; \
	else \
		git clone --branch "$(QEMU_REF)" --depth 1 "$(QEMU_URL)" "$(QEMU_DIR)"; \
	fi

# Build everything: (re)apply the ENA glue, configure if needed, then build the
# emulator and qtest harness via ninja and refresh compile_commands.json.
# Requires `make setup` to have checked out the qemu submodule first.
build: qemu-configure
	ninja -C "$(QEMU_BUILD_DIR)" $(QEMU_NINJA_TARGETS)
	cp "$(QEMU_BUILD_DIR)/compile_commands.json" "compile_commands.json"

# Wipe everything: drop the build directory and revert the glue so the qemu
# submodule is left clean.
clean: qemu-unpatch
	rm -rf "$(QEMU_BUILD_DIR)"

# ---------------------------------------------------------------------------
# Internal machinery (used by build/clean)
# ---------------------------------------------------------------------------

# Apply the ENA glue into the submodule. Idempotent: a no-op if already
# applied, an error only if the tree has drifted so the patch no longer fits.
qemu-patch:
	@cd "$(QEMU_DIR)" && \
	if git apply --reverse --check "$(ENA_PATCH)" >/dev/null 2>&1; then \
		echo "ena glue already applied"; \
	elif git apply --check "$(ENA_PATCH)" >/dev/null 2>&1; then \
		git apply "$(ENA_PATCH)" && echo "applied ena glue"; \
	else \
		echo "ERROR: $(ENA_PATCH) does not apply cleanly to qemu submodule"; \
		exit 1; \
	fi

qemu-unpatch:
	@if [ -d "$(QEMU_DIR)/.git" ]; then \
		cd "$(QEMU_DIR)" && \
		if git apply --reverse --check "$(ENA_PATCH)" >/dev/null 2>&1; then \
			git apply --reverse "$(ENA_PATCH)" && echo "reverted ena glue"; \
		else \
			echo "ena glue not applied; nothing to revert"; \
		fi; \
	fi

$(QEMU_BUILD_DIR)/config-host.mak:
	mkdir -p "$(QEMU_BUILD_DIR)"
	cd "$(QEMU_BUILD_DIR)" && ../configure \
		--target-list="$(QEMU_TARGETS)" \
		--disable-werror \
		--disable-docs

# qemu-patch is a PHONY prerequisite listed first, so the glue is (re)applied on
# every configure/build even when config-host.mak already exists. meson detects
# the changed meson.build and regenerates the ninja graph on the next build.
qemu-configure: qemu-patch $(QEMU_BUILD_DIR)/config-host.mak

print-qemu-cflags:
	@printf '%s\n' "$(QEMU_CFLAGS)"
