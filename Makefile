CC      ?= gcc
CFLAGS  = -std=c17 -Wall -Wextra -pedantic -O2 -pthread -Iinclude
LDFLAGS = -pthread
LDLIBS  = -lm

BUILD = build

CORE_SRC = src/barrier.c src/status.c src/snapshot_buffer.c \
           src/fdtd_solver.c src/barbershop.c src/net_util.c
CORE_OBJ = $(CORE_SRC:src/%.c=$(BUILD)/%.o)

.PHONY: all test clean

all: fdtd_server fdtd_client

fdtd_server: $(CORE_OBJ) $(BUILD)/fdtd_server.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fdtd_client: $(BUILD)/fdtd_client.o $(BUILD)/net_util.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_snapshot_buffer: tests/test_snapshot_buffer.c src/snapshot_buffer.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: all $(BUILD)/test_snapshot_buffer
	$(BUILD)/test_snapshot_buffer
	bash tests/test_protocol.sh

clean:
	rm -rf $(BUILD) fdtd_server fdtd_client