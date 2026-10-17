
CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -pedantic -O2 -pthread
CPPFLAGS = -Iinclude
LDLIBS = -lm

BUILD_DIR = build
SRC_DIR = src

COMMON = $(BUILD_DIR)/barrier.o \
         $(BUILD_DIR)/fdtd_solver.o \
         $(BUILD_DIR)/status.o

NET = $(BUILD_DIR)/net_util.o

.PHONY: all clean sin-barrera

all: $(BUILD_DIR)/fdtd_server $(BUILD_DIR)/load_client

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fdtd_server: $(BUILD_DIR)/server_safe.o $(COMMON) $(NET)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/load_client: $(BUILD_DIR)/load_client.o $(NET)
	$(CC) $(CFLAGS) $^ -o $@

sin-barrera:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -DSIN_BARRERA" all

clean:
	rm -rf $(BUILD_DIR)

