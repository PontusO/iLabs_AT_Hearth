# Host unit tests for the portable core. No SDK: plain gcc against the
# stub port. Run with `make -C test/host run`.

CORE   = ../../core
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I$(CORE)/include -DMT_HOST_TEST -DMT_COMBINED_IMAGE=0

TESTS = test_mt_composition test_mt_transport test_mt_cmdbox test_mt_rows test_mt_stores test_mt_store_index

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

test_mt_store_index: test_mt_store_index.c ../../platform/esp32c6/main/mt_store_index.c
	$(CC) $(CFLAGS) -I../../platform/esp32c6/main -o $@ $^

boundary:
	@! /usr/bin/grep -rnE '#include .*(freertos|FreeRTOS|esp_|nvs|sdkconfig|driver/|zephyr|sl_|em_|gecko|nvm3|SEGGER|rail_)' $(CORE) \
	  || (echo "SDK header inside core/" && exit 1)

SILABS_PORT = ../../platform/silabs/port

# Compile the Silabs skeleton's pure-C files against core/ headers on the
# host. Catches a signature drift between the stubs and the headers before
# a toolchain exists to link them.
silabs-stubs:
	$(CC) $(CFLAGS) -fsyntax-only -I$(CORE)/include $(SILABS_PORT)/mt_matter_stub.c
	$(CC) $(CFLAGS) -fsyntax-only -I$(CORE)/include $(SILABS_PORT)/mt_devtypes_stub.c

run: all boundary silabs-stubs
	./test_mt_composition
	./test_mt_transport
	./test_mt_cmdbox
	./test_mt_rows
	./test_mt_stores
	./test_mt_store_index

clean:
	rm -f $(TESTS)

.PHONY: all run clean boundary silabs-stubs
