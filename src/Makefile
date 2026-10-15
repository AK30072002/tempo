CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -Wall -Wextra -Wno-type-limits \
           -fno-stack-protector -ffreestanding
LDFLAGS ?=

all: test_tempo tempo_sync ledcap tempo_replay bench_truth libtempo.a

libtempo.a: tempo.o
	ar rcs $@ $^

tempo.o: tempo.c tempo.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_tempo: tempo.c test_tempo.c tempo.h
	$(CC) $(CFLAGS) -o $@ tempo.c test_tempo.c $(LDFLAGS)

tempo_replay: tempo.c tempo_replay.c tempo.h
	$(CC) $(CFLAGS) -o $@ tempo.c tempo_replay.c $(LDFLAGS)

bench_truth: tempo.c bench_truth.c tempo.h
	$(CC) -std=gnu99 -O2 -Wall -o $@ tempo.c bench_truth.c $(LDFLAGS)

tempo_sync: tempo.c v4l2cap.c tempo_sync.c tempo.h v4l2cap.h
	$(CC) $(CFLAGS) -o $@ tempo.c v4l2cap.c tempo_sync.c $(LDFLAGS)

ledcap: v4l2cap.c ledcap.c v4l2cap.h
	$(CC) -std=gnu99 -O2 -Wall -Wextra -o $@ v4l2cap.c ledcap.c $(LDFLAGS)

DS_ROOT ?= /opt/nvidia/deepstream/deepstream
nvmux_probe: nvmux_probe.c
	$(CC) -std=gnu99 -O2 -Wall -o $@ nvmux_probe.c \
	  $$(pkg-config --cflags --libs gstreamer-1.0) \
	  -I$(DS_ROOT)/sources/includes \
	  -L$(DS_ROOT)/lib -lnvdsgst_meta -lnvds_meta -lm

test: test_tempo
	./test_tempo

bench: bench_truth
	./bench_truth

size: tempo.o
	@size tempo.o
	@echo "--- freestanding check (no libc dependency in tempo.o) ---"
	@nm -u tempo.o || true

clean:
	rm -f *.o *.a test_tempo tempo_replay tempo_sync ledcap bench_truth nvmux_probe

.PHONY: all test bench size clean
