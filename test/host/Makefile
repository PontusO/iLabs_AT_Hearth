# Host unit tests for the portable core. No SDK: plain gcc against the
# stub port. Run with `make -C test/host run`.

CORE   = ../../core
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I$(CORE)/include -DMT_HOST_TEST -DMT_COMBINED_IMAGE=0

TESTS = test_mt_composition test_mt_transport test_mt_cmdbox test_mt_rows test_mt_stores

all: $(TESTS)

test_mt_composition: test_mt_composition.c $(CORE)/mt/mt_composition.c port_stub.c
	$(CC) $(CFLAGS) -o $@ $^

test_mt_transport: test_mt_transport.c $(CORE)/mt/mt_transport.c port_stub.c
	$(CC) $(CFLAGS) -o $@ $^

test_mt_cmdbox: test_mt_cmdbox.c $(CORE)/mt/mt_cmdbox.c port_stub.c
	$(CC) $(CFLAGS) -o $@ $^

test_mt_rows: test_mt_rows.c $(CORE)/mt/mt_rows.c port_stub.c
	$(CC) $(CFLAGS) -o $@ $^

test_mt_stores: test_mt_stores.c port_stub.c
	$(CC) $(CFLAGS) -o $@ $^

boundary:
	@! /usr/bin/grep -rnE '#include .*(freertos|esp_|nvs|sdkconfig|driver/|zephyr)' $(CORE) \
	  || (echo "SDK header inside core/" && exit 1)

run: all boundary
	./test_mt_composition
	./test_mt_transport
	./test_mt_cmdbox
	./test_mt_rows
	./test_mt_stores

clean:
	rm -f $(TESTS)

.PHONY: all run clean boundary
