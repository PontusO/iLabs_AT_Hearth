# Host unit tests for the portable core. No SDK: plain gcc against the
# stub port. Run with `make -C test/host run`.

CORE   = ../../core
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I$(CORE)/include -DMT_HOST_TEST -DMT_COMBINED_IMAGE=0

TESTS = test_mt_composition test_mt_transport test_mt_cmdbox test_mt_rows test_mt_stores test_mt_store_index test_hearth_ring test_mt_ota

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

test_mt_ota: test_mt_ota.c $(CORE)/mt/mt_ota.c port_stub.c
	$(CC) $(CFLAGS) -DPORT_STUB_NO_LINK -o $@ $^

boundary:
	@! /usr/bin/grep -rnE '#include .*(freertos|FreeRTOS|esp_|nvs|sdkconfig|driver/|zephyr|sli?_|em_|gecko|nvm3|SEGGER|rail)' $(CORE) \
	  || (echo "SDK header inside core/" && exit 1)

SILABS_PORT = ../../platform/silabs/port

test_hearth_ring: test_hearth_ring.c $(SILABS_PORT)/hearth_ring.c $(SILABS_PORT)/hearth_kvid.c
	$(CC) $(CFLAGS) -o $@ $^

# Compile the Silabs skeleton's pure-C files against core/ headers on the
# host. Catches a signature drift between the stubs and the headers before
# a toolchain exists to link them, and the .slcp drifting away from
# core/sources.cmake, which no build catches at all.
silabs-stubs:
	$(CC) $(CFLAGS) -fsyntax-only -I$(CORE)/include $(SILABS_PORT)/mt_matter_stub.c
	$(CC) $(CFLAGS) -fsyntax-only -I$(CORE)/include $(SILABS_PORT)/mt_devtypes_stub.c
	python3 check_decls.py
	python3 check_slcp_sources.py

run: all boundary silabs-stubs
	./test_mt_composition
	./test_mt_transport
	./test_mt_cmdbox
	./test_mt_rows
	./test_mt_stores
	./test_mt_store_index
	./test_hearth_ring
	./test_mt_ota

clean:
	rm -f $(TESTS)

.PHONY: all run clean boundary silabs-stubs
