
CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -pedantic -O2 -pthread
CPPFLAGS = -Iinclude
LDLIBS = -lm

BUILD_DIR = build
SRC_DIR = src

COMMON = $(BUILD_DIR)/barrier.o \
         $(BUILD_DIR)/fdtd_solver.o \
         $(BUILD_DIR)/status.o \
         $(BUILD_DIR)/barbershop.o

NET = $(BUILD_DIR)/net_util.o

TEST_DIR = tests
TESTS = $(BUILD_DIR)/test_barbershop

.PHONY: all clean sin-barrera test

all: $(BUILD_DIR)/fdtd_server $(BUILD_DIR)/load_client

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fdtd_server: $(BUILD_DIR)/server_safe.o $(COMMON) $(NET)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/load_client: $(BUILD_DIR)/load_client.o $(NET)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_%.o: $(TEST_DIR)/test_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_barbershop: $(BUILD_DIR)/test_barbershop.o $(COMMON)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

sin-barrera:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -DSIN_BARRERA" all

clean:
	rm -rf $(BUILD_DIR)

