CC      ?= gcc
CFLAGS   = -std=c17 -Wall -Wextra -pedantic -O2 -pthread -Iinclude -MMD -MP
LDFLAGS  = -pthread
LDLIBS   = -lm

BUILD    = build
BUILD_SB = build_sb

CORE_SRC = src/barrier.c src/status.c src/snapshot_buffer.c \
           src/fdtd_solver.c src/barbershop.c src/net_util.c
CORE_OBJ    = $(CORE_SRC:src/%.c=$(BUILD)/%.o)
CORE_OBJ_SB = $(CORE_SRC:src/%.c=$(BUILD_SB)/%.o)

# Pruebas: cada tests/test_*.c se compila contra el núcleo y se ejecuta.
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=$(BUILD)/%)

.PHONY: all test test_sinbarrera bench bench-quick plots sinbarrera clean

all: fdtd_server fdtd_client fdtd_bench

# Variante -DSIN_BARRERA: elimina las barreras del solucionador (para mostrar
# la condición de carrera). Se compila en build_sb/ para no mezclar objetos.
sinbarrera: fdtd_server_sb fdtd_bench_sb

fdtd_server: $(CORE_OBJ) $(BUILD)/fdtd_server.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fdtd_client: $(BUILD)/fdtd_client.o $(BUILD)/net_util.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fdtd_bench: $(CORE_OBJ) $(BUILD)/fdtd_bench.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fdtd_server_sb: $(CORE_OBJ_SB) $(BUILD_SB)/fdtd_server.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fdtd_bench_sb: $(CORE_OBJ_SB) $(BUILD_SB)/fdtd_bench.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_SB)/%.o: CFLAGS += -DSIN_BARRERA
$(BUILD_SB)/%.o: src/%.c | $(BUILD_SB)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD) $(BUILD_SB):
	mkdir -p $@

# test_rwlock incluye status.c para probar las funciones static del candado,
# por eso no se enlaza con status.o.
$(BUILD)/test_rwlock: tests/test_rwlock.c src/status.c tests/test_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_rwlock.c $(LDLIBS)

$(BUILD)/test_%: tests/test_%.c tests/test_util.h $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJ) $(LDLIBS)

# El mismo test de determinismo, compilado SIN barreras: debe FALLAR.
$(BUILD_SB)/test_%: CFLAGS += -DSIN_BARRERA
$(BUILD_SB)/test_%: tests/test_%.c tests/test_util.h $(CORE_OBJ_SB) | $(BUILD_SB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJ_SB) $(LDLIBS)

test_sinbarrera: $(BUILD_SB)/test_determinism
	@echo "== determinismo con -DSIN_BARRERA (se espera que FALLE)"
	@if $(BUILD_SB)/test_determinism; then \
	@    echo "ERROR: sin barreras el test de determinismo NO fallo"; exit 1; \
	@else echo "  OK: la carrera fue detectada por el test"; fi

test: all $(TEST_BIN) test_sinbarrera
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done
	@if [ -f tests/test_protocol.sh ]; then echo "== tests/test_protocol.sh"; bash tests/test_protocol.sh; fi

# Mediciones: scripts/bench.sh si existe; si no, una corrida de muestra.
bench: fdtd_bench
	@if [ -f scripts/bench.sh ]; then bash scripts/bench.sh; else ./fdtd_bench 512 512 200 2 0; fi

# Prueba rápida del flujo de mediciones (mallas y pasos pequeños; NO sirve para el informe).
bench-quick: fdtd_bench
	@QUICK=1 REPS=2 bash scripts/bench.sh

# Gráficos de Amdahl, eficiencia y Karp-Flatt a partir de evidencia/*/bench_*.csv.
plots:
	@python3 scripts/plot_bench.py

clean:
	rm -rf $(BUILD) $(BUILD_SB) fdtd_server fdtd_client fdtd_bench fdtd_server_sb fdtd_bench_sb

-include $(wildcard $(BUILD)/*.d $(BUILD_SB)/*.d)