# Host unit tests for the pure-C parts of the AT+MT firmware. No IDF, no
# framework: plain gcc. Run with `make -C test/host run`.

CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../../main/include -DMT_HOST_TEST

TESTS = test_mt_composition test_mt_transport test_mt_cmdbox

all: $(TESTS)

test_mt_composition: test_mt_composition.c ../../main/mt_composition.c
	$(CC) $(CFLAGS) -o $@ $^

test_mt_transport: test_mt_transport.c ../../main/mt_transport.c
	$(CC) $(CFLAGS) -o $@ $^

test_mt_cmdbox: test_mt_cmdbox.c ../../main/mt_cmdbox.c
	$(CC) $(CFLAGS) -o $@ $^

run: all
	./test_mt_composition
	./test_mt_transport
	./test_mt_cmdbox

clean:
	rm -f $(TESTS)

.PHONY: all run clean
