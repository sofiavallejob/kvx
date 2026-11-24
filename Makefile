# Makefile for kvx  (pd-lib-builder: https://github.com/pure-data/pd-lib-builder)
lib.name = kvx

dict.class.sources = src/dict.c src/kv_core.c
table.class.sources = src/table.c src/csv_core.c src/csv_ops.c src/kv_core.c
stream.class.sources = src/stream.c

datafiles = \
	$(wildcard help/*) \
	$(wildcard examples/*) \
	kvx-meta.pd \
	README.md \
	LICENSE

cflags = -std=c99 -O3 -Isrc

PDLIBBUILDER_DIR = .
include $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder

# unit tests for the Pd-free core (needs a C compiler only)
test:
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O1 -g -Isrc tests/test_csv.c src/csv_core.c -lm -o tests/test_csv
	./tests/test_csv
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O1 -g -Isrc tests/test_ops.c src/csv_ops.c src/csv_core.c src/kv_core.c -lm -o tests/test_ops
	./tests/test_ops
	$(CC) -std=c99 -Wall -Wextra -O1 -g tests/test_core.c src/kv_core.c -lm -o tests/test_core
	./tests/test_core

# tests for the object itself, with the Pd API replaced by a mock (needs the Pd headers)
test-object:
	$(CC) -std=gnu99 -Wall -g -I$(PDINCLUDEDIR) -Isrc tests/mock_pd.c tests/test_dict.c src/dict.c src/kv_core.c -lm -o tests/test_dict
	./tests/test_dict

.PHONY: test test-object
