# kpass - Zero-Copy Kernel Passthrough

Kernel module and userspace library for zero-copy TCP forwarding via
`tcp_read_sock` + `MSG_SPLICE_PAGES`. Userspace operates on handles only --
data never crosses the kernel-user boundary.

## Quick Start

```sh
# Build
cd kpass
make lib demos

# Load module
sudo make load

# Run test (3 terminals)
nc -l 9999                              # destination
./demo/echo_server 8888 127.0.0.1 9999  # forwarder
./demo/test_client 127.0.0.1 8888 "hello"  # client
```

## Documentation

See [KPASS.md](KPASS.md) for full documentation: architecture, API reference,
kernel internals, and build instructions.
