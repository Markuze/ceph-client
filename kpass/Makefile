# Ceph Zero-Overhead Kernel Passthrough (kpass) - Complete Build System
#
# Targets:
#   all       - Build userspace library, demos, and kernel module
#   lib       - Build userspace shared and static libraries
#   demos     - Build demo applications
#   module    - Build kernel module (ceph_kpass.ko)
#   clean     - Remove build artifacts
#   test      - Run functionality test (requires module loaded)
#
# The kernel module provides /dev/ceph_kpass for true zero-overhead I/O.
# The userspace library talks to the kernel module via io_uring_cmd.
#

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?= $(realpath ../)

# Compiler and flags for userspace
CC := gcc
PKG_CONFIG ?= pkg-config
CFLAGS := -Wall -Wextra -Werror -O2 -g -fPIC
CPPFLAGS += -I$(CURDIR)/lib $(shell $(PKG_CONFIG) --cflags liburing)

# Link flags
LDLIBS += $(shell $(PKG_CONFIG) --libs liburing)

# Source files
LIB_SRC := lib/ceph_kpass.c
LIB_HDR := lib/ceph_kpass.h
UAPI_HDR := include/uapi/ceph_kpass.h

DEMO_SRC := demo/echo_server.c demo/test_client.c

# Output files
LIB_SO := lib/libceph_kpass.so
LIB_A := lib/libceph_kpass.a
LIB_OBJ := lib/ceph_kpass.o

ECHO_SERVER := demo/echo_server
TEST_CLIENT := demo/test_client
COMMAND_TEST := tests/command_layout
STREAM_TEST := tests/stream_objects

KERNEL_MODULE := kernel/ceph_kpass.ko

# Phony targets
.PHONY: all lib demos module clean test help load unload check check-liburing

# Default target
all: lib demos module

# Help
help:
	@echo "Ceph Zero-Overhead Kernel Passthrough (kpass) - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build everything (default)"
	@echo "  lib       - Build userspace libraries"
	@echo "  demos     - Build demo applications"
	@echo "  module    - Build kernel module (ceph_kpass.ko)"
	@echo "  clean     - Remove build artifacts"
	@echo "  load      - Load kernel module"
	@echo "  unload    - Unload kernel module"
	@echo "  test      - Run functionality test"
	@echo "  check     - Check command encoding (no module required)"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR      - Kernel build directory (default: /lib/modules/.../build)"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build everything"
	@echo "  make module             # Build only kernel module"
	@echo "  sudo make load          # Load the module"
	@echo "  make test               # Run test (module must be loaded)"

# Kernel module
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel modules

# Library targets
lib: $(LIB_SO) $(LIB_A)

check-liburing:
	@$(PKG_CONFIG) --atleast-version=2.15 liburing || { \
		echo "kpass requires liburing >= 2.15; set PKG_CONFIG_PATH for a local install." >&2; \
		exit 1; \
	}

$(LIB_OBJ): $(LIB_SRC) $(LIB_HDR) $(UAPI_HDR) | check-liburing
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(LIB_SO): $(LIB_OBJ)
	$(CC) $(LDFLAGS) -shared -o $@ $< $(LDLIBS)

$(LIB_A): $(LIB_OBJ)
	ar rcs $@ $<

# Demo targets
demos: $(ECHO_SERVER) $(TEST_CLIENT)

$(ECHO_SERVER): demo/echo_server.c $(LIB_A) $(LIB_HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_A) $(LDLIBS)

$(TEST_CLIENT): demo/test_client.c $(LIB_A) $(LIB_HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_A) $(LDLIBS)

$(COMMAND_TEST): tests/command_layout.c $(LIB_SRC) $(LIB_HDR) $(UAPI_HDR) | check-liburing
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(STREAM_TEST): tests/stream_objects.c lib/kpass_objects.h $(UAPI_HDR) | check-liburing
	$(CC) $(CPPFLAGS) $(CFLAGS) -static $(LDFLAGS) -o $@ $< $(LDLIBS)

check: $(COMMAND_TEST)
	./$(COMMAND_TEST)

# Clean
clean:
	rm -f $(LIB_OBJ) $(LIB_SO) $(LIB_A)
	rm -f $(ECHO_SERVER) $(TEST_CLIENT)
	rm -f $(COMMAND_TEST)
	rm -f $(STREAM_TEST)
	rm -f demo/*.o lib/*.o
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean 2>/dev/null || true

# Load/unload module
load: module
	@if lsmod | grep -q ceph_kpass; then \
		echo "Module already loaded"; \
	else \
		sudo insmod kernel/ceph_kpass.ko && \
		echo "Module loaded: /dev/ceph_kpass"; \
	fi

unload:
	@if lsmod | grep -q ceph_kpass; then \
		sudo rmmod ceph_kpass && \
		echo "Module unloaded"; \
	else \
		echo "Module not loaded"; \
	fi

# Test - runs a loopback test with the kernel module
test: demos
	@if ! lsmod | grep -q ceph_kpass; then \
		echo "ERROR: Kernel module not loaded. Run 'sudo make load' first."; \
		exit 1; \
	fi
	@echo "=== Running Zero-Overhead Echo Test (Kernel Module) ==="
	@echo ""
	@(nc -l 9999 > /tmp/echo_output.txt &) && sleep 0.3 && \
	(timeout 5 ./demo/echo_server 8888 127.0.0.1 9999 &) && sleep 0.5 && \
	echo "Hello, Zero-Overhead World!" | timeout 2 nc localhost 8888 && \
	sleep 1 && \
	echo "" && \
	echo "=== Output received at destination ===" && \
	cat /tmp/echo_output.txt && \
	echo "" && \
	echo "=== Test Complete ===" && \
	pkill -f "nc -l 9999" 2>/dev/null; \
	pkill -f "echo_server 8888" 2>/dev/null; \
	rm -f /tmp/echo_output.txt; true

# Test with mock (no kernel module required)
test-mock: demos
	@echo "=== Running Zero-Overhead Echo Test (Mock/Userspace) ==="
	@echo ""
	@(nc -l 9999 > /tmp/echo_output.txt &) && sleep 0.3 && \
	(timeout 5 ./demo/echo_server 8888 127.0.0.1 9999 &) && sleep 0.5 && \
	echo "Hello, Zero-Overhead World!" | timeout 2 nc localhost 8888 && \
	sleep 1 && \
	echo "" && \
	echo "=== Output received at destination ===" && \
	cat /tmp/echo_output.txt && \
	echo "" && \
	echo "=== Test Complete ===" && \
	pkill -f "nc -l 9999" 2>/dev/null; \
	pkill -f "echo_server 8888" 2>/dev/null; \
	rm -f /tmp/echo_output.txt; true
