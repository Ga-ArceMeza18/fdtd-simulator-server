
set -u
cd "$(dirname "$0")/.."

ENV_NAME=${ENV_NAME:-$(hostname -s)}
REPS=${REPS:-5}
SCEN=${SCEN:-"E1 E2 E3"}
QUICK=${QUICK:-0}
BIN=./fdtd_bench
OUTDIR=${OUTDIR:-evidencia/$ENV_NAME}
SNAPDIR=${SNAPDIR:-${TMPDIR:-/tmp}/fdtd_bench_snap_$$}
BUFFER=4

if [ "$QUICK" = "1" ]; then
    BIG=${BIG:-512}; SMALL=${SMALL:-128}; STEPS=${STEPS:-200}
else
    BIG=${BIG:-2048}; SMALL=${SMALL:-256}; STEPS=${STEPS:-1000}
fi

# Orden de las columnas de fdtd_bench (debe coincidir con src/fdtd_bench.c)
HEADER="env,scenario,rep,nx,ny,steps,threads,k,B,init_s,run_s,barrier_mean_s,barrier_max_s,io_push_s,compute_s,flush_s,total_s,sum_abs_ez,hash_ez"

#  hardware: núcleos físicos disponibles
physical_cores() {
    local phys avail
    phys=$(lscpu -p=CORE,SOCKET 2>/dev/null | grep -v '^#' | sort -u | wc -l)
    avail=$(nproc)
    [ "$phys" -ge 1 ] 2>/dev/null || phys=$avail
    [ "$avail" -lt "$phys" ] && phys=$avail   # respeta la asignación de Slurm / taskset
    echo "$phys"
}
CORES=${CORES:-$(physical_cores)}

#  lista de hilos
if [ -z "${THREADS:-}" ]; then
    THREADS=""
    t=1
    while [ "$t" -le "$CORES" ]; do THREADS="$THREADS $t"; t=$((t * 2)); done
    last=${THREADS##* }
    [ "$last" -ne "$CORES" ] && THREADS="$THREADS $CORES"
    THREADS="$THREADS $((CORES * 2))"          # sobresuscripción
    THREADS=$(echo $THREADS)
fi

scenario_params() {   # -> "nx ny pasos k"
    case "$1" in
        E1) echo "$BIG $BIG $STEPS 0" ;;
        E2) echo "$BIG $BIG $STEPS 10" ;;
        E3) echo "$SMALL $SMALL $STEPS 0" ;;
        *)  return 1 ;;
    esac
}

# ---- comprobaciones
if [ ! -x "$BIN" ]; then
    echo "Compilando..."; make fdtd_bench >/dev/null || { echo "No se pudo compilar fdtd_bench"; exit 1; }
fi
mkdir -p "$OUTDIR" "$SNAPDIR" || exit 1
cleanup() { rmdir "$SNAPDIR" 2>/dev/null; }
trap cleanup EXIT

if [[ " $SCEN " == *" E2 "* ]]; then
    need_kb=$(( BIG * BIG * 8 / 1024 * (STEPS / 10) + 1024 ))
    avail_kb=$(df -k --output=avail "$SNAPDIR" 2>/dev/null | tail -1 | tr -d ' ')
    if [ -n "$avail_kb" ] && [ "$avail_kb" -lt "$need_kb" ]; then
        echo "AVISO: E2 necesita ~$((need_kb / 1024)) MB libres en $SNAPDIR y hay $((avail_kb / 1024)) MB."
        echo "       Use SNAPDIR=<otro directorio> (en Kabré, un directorio de /scratch)."
    fi
fi

scripts/env_info.sh "$OUTDIR/hardware.txt" >/dev/null
{
    echo "env=$ENV_NAME"
    echo "cores=$CORES"
    echo "threads=$THREADS"
    echo "reps=$REPS"
    echo "scenarios=$SCEN"
    echo "big=$BIG"; echo "small=$SMALL"; echo "steps=$STEPS"
    echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
} > "$OUTDIR/meta.txt"

echo "Ambiente: $ENV_NAME | núcleos: $CORES | hilos: $THREADS | repeticiones: $REPS"
echo "Resultados en: $OUTDIR"

failures=0
for s in $SCEN; do
    params=$(scenario_params "$s") || { echo "Escenario desconocido: $s"; failures=$((failures + 1)); continue; }
    read -r nx ny steps k <<<"$params"
    csv="$OUTDIR/bench_$s.csv"
    echo "$HEADER" > "$csv"
    total=$(( REPS * $(echo $THREADS | wc -w) ))
    n=0
    echo
    echo "== $s: ${nx}x${ny}, $steps pasos, k=$k"

    # Las repeticiones son el lazo externo: así una perturbación pasajera del
    # equipo no cae sobre todas las repeticiones de un mismo número de hilos.
    for rep in $(seq 1 "$REPS"); do
        for t in $THREADS; do
            n=$((n + 1))
            if [ "$t" -gt $((nx - 2)) ]; then continue; fi
            if out=$("$BIN" --no-header "$nx" "$ny" "$steps" "$t" "$k" "$BUFFER" "$SNAPDIR"); then
                echo "$ENV_NAME,$s,$rep,$out" >> "$csv"
                tot=$(echo "$out" | awk -F, '{printf "%.3f", $14}')
                printf "  [%d/%d] rep %d, %2d hilos: %s s\n" "$n" "$total" "$rep" "$t" "$tot"
            else
                echo "  [$n/$total] rep $rep, $t hilos: FALLO"
                failures=$((failures + 1))
            fi
        done
    done
done

echo
if [ "$failures" -eq 0 ]; then
    echo "Listo. Para los gráficos: python3 scripts/plot_bench.py"
else
    echo "Terminó con $failures corrida(s) fallida(s)."
    exit 1
fi