#!/usr/bin/env bash
# Pruebas de integración cliente-servidor (se ejecutan desde la raíz del repo):
#   1. flujo completo SUBMIT -> STATUS -> PEEK -> RESULT
#   2. solicitudes mal formadas (el servidor no se cae)
#   3. cola llena -> BUSY sin bloquear
#   4. estrés: 60 clientes concurrentes, sin trabajos perdidos ni duplicados
#   5. apagado ordenado con trabajo en curso, trabajos en cola y clientes conectados
set -u
cd "$(dirname "$0")/.."

SERVER=./fdtd_server
CLIENT=./fdtd_client
BENCH=./fdtd_bench
TMP=$(mktemp -d /tmp/fdtd_proto.XXXXXX)
FAILS=0
PIDS=()
PORT=0
SRV_PID=0

cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { echo "  ok    - $1"; }
fail() { echo "  FALLA - $1${2:+ ($2)}"; FAILS=$((FAILS + 1)); }

# expect <descripción> <regex> <valor>
expect() {
    if [[ "$3" =~ $2 ]]; then ok "$1"; else fail "$1" "obtuvo: '$3'"; fi
}

free_port() {
    local p
    while true; do
        p=$((20000 + RANDOM % 30000))
        if ! (exec 9<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; then
            echo "$p"
            return
        fi
    done
}

# start_server <sillas> <buffer>  -> deja PORT, SRV_PID y SNAP
start_server() {
    PORT=$(free_port)
    SNAP="$TMP/snap_$PORT"
    "$SERVER" "$PORT" "$1" "$2" "$SNAP" >"$TMP/server_$PORT.log" 2>&1 &
    SRV_PID=$!
    PIDS+=("$SRV_PID")
    local i
    for i in $(seq 1 100); do
        grep -q listening "$TMP/server_$PORT.log" 2>/dev/null && return 0
        sleep 0.05
    done
    fail "el servidor no arrancó" "$(cat "$TMP/server_$PORT.log")"
    return 1
}

cl() { timeout 20 "$CLIENT" 127.0.0.1 "$PORT" "$@"; }

# wait_status <id> <regex> <segundos>
wait_status() {
    local id=$1 re=$2 limit=$3 out i
    for i in $(seq 1 $((limit * 20))); do
        out=$(cl STATUS "$id")
        [[ "$out" =~ $re ]] && return 0
        sleep 0.05
    done
    return 1
}

near() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d < 1e-9) }'; }

[ -x "$SERVER" ] && [ -x "$CLIENT" ] && [ -x "$BENCH" ] || { echo "Faltan binarios: ejecute make primero"; exit 1; }

# ===================================================================== 1
echo "[1] Flujo completo SUBMIT -> STATUS -> PEEK -> RESULT"
start_server 2 4 || exit 1

out=$(cl SUBMIT 64 64 200 2 20 32 32)
expect "SUBMIT devuelve OK 1" '^OK 1$' "$out"
wait_status 1 '^DONE ' 30 && ok "STATUS llega a DONE" || fail "STATUS no llegó a DONE"
expect "STATUS DONE <segundos>" '^DONE [0-9]+\.[0-9]+$' "$(cl STATUS 1)"
peek=$(cl PEEK 1 32 32)
expect "PEEK devuelve VALUE <paso> <ez>" '^VALUE 200 [-+0-9.e]+$' "$peek"
res=$(cl RESULT 1)
expect "RESULT <suma> <archivo>" '^RESULT [-+0-9.e]+ .+job1_step200\.bin$' "$res"

sum=$(echo "$res" | awk '{print $2}')
file=$(echo "$res" | awk '{print $3}')
bench_sum=$("$BENCH" --no-header 64 64 200 2 0 | awk -F, '{print $15}')
[ "$sum" = "$bench_sum" ] && ok "suma_abs_ez coincide con fdtd_bench ($sum)" \
    || fail "suma_abs_ez distinta del bench" "$sum vs $bench_sum"

if [ -f "$file" ]; then
    size=$(stat -c %s "$file")
    [ "$size" -eq $((12 + 64 * 64 * 8)) ] && ok "archivo de instantánea con tamaño correcto ($size bytes)" \
        || fail "tamaño del archivo" "$size"
    n=$(ls "$SNAP" | grep -c '^job1_step.*\.bin$')
    [ "$n" -eq 10 ] && ok "10 instantáneas (cada 20 pasos)" || fail "número de instantáneas" "$n"
    off=$((12 + (32 * 64 + 32) * 8))
    disk=$(od -An -t f8 -j "$off" -N 8 "$file" | tr -d ' ')
    pv=$(echo "$peek" | awk '{print $3}')
    near "$disk" "$pv" && ok "PEEK coincide con el valor guardado en disco" \
        || fail "PEEK distinto del archivo" "$pv vs $disk"
else
    fail "el archivo de RESULT no existe" "$file"
fi

# ===================================================================== 2
echo "[2] Solicitudes mal formadas"
bad() { # bad <descripción> <comando...>
    local d=$1; shift
    expect "$d" '^ERROR' "$(cl "$@")"
}
bad "línea vacía" ""
bad "comando desconocido" FOO bar
bad "minúsculas no son comandos" submit 1 2
bad "SUBMIT sin argumentos" SUBMIT
bad "SUBMIT con pocos argumentos" SUBMIT 1 2
bad "SUBMIT con demasiados argumentos" SUBMIT 64 64 10 1 0 32 32 99
bad "SUBMIT no numérico" SUBMIT a b c d e f g
bad "SUBMIT negativo" SUBMIT -5 64 10 1 0 5 5
bad "SUBMIT con signo +" SUBMIT +64 64 10 1 0 5 5
bad "SUBMIT con decimal" SUBMIT 64.5 64 10 1 0 5 5
bad "SUBMIT desbordado" SUBMIT 99999999999999999999 64 10 1 0 5 5
bad "SUBMIT malla enorme" SUBMIT 100000 64 10 1 0 5 5
bad "SUBMIT más hilos que nx-2" SUBMIT 64 64 10 63 0 32 32
bad "SUBMIT 0 hilos" SUBMIT 64 64 10 0 0 32 32
bad "SUBMIT fuente en el borde" SUBMIT 64 64 10 2 0 0 32
bad "SUBMIT fuente fuera" SUBMIT 64 64 10 2 0 32 64
bad "SUBMIT k > pasos" SUBMIT 64 64 10 2 99 32 32
bad "SUBMIT 0 pasos" SUBMIT 64 64 0 2 0 32 32
bad "SUBMIT malla 2x2" SUBMIT 2 2 10 1 0 1 1
bad "STATUS sin id" STATUS
bad "STATUS no numérico" STATUS abc
bad "STATUS id inexistente" STATUS 999
bad "STATUS id 0" STATUS 0
bad "PEEK sin argumentos" PEEK 1
bad "PEEK fuera de la malla" PEEK 1 99999 0
bad "PEEK id inexistente" PEEK 999 0 0
bad "RESULT id inexistente" RESULT 999
bad "RESULT negativo" RESULT -1
bad "SHUTDOWN con argumentos" SHUTDOWN ahora
expect "línea demasiado larga" '^ERROR' "$(cl "$(printf 'A%.0s' $(seq 1 400))")"

# Bytes basura, cliente que conecta y se va, línea sin salto de línea
exec 3<>/dev/tcp/127.0.0.1/"$PORT"
printf '\x00\xff\xfe\x01 basura\n' >&3
read -t 5 -r line <&3
expect "bytes binarios" '^ERROR' "${line:-}"
exec 3>&- 3<&-

exec 3<>/dev/tcp/127.0.0.1/"$PORT"; exec 3>&- 3<&-            # conecta y cierra sin enviar
exec 3<>/dev/tcp/127.0.0.1/"$PORT"; printf 'STATUS' >&3; exec 3>&- 3<&-  # sin \n
sleep 0.3

kill -0 "$SRV_PID" 2>/dev/null && ok "el servidor sigue vivo tras todo lo anterior" \
    || fail "el servidor murió"
expect "STATUS sigue funcionando" '^DONE ' "$(cl STATUS 1)"
out=$(cl SUBMIT 24 24 10 2 0 12 12)
expect "ninguna solicitud inválida consumió un id (siguiente id = 2)" '^OK 2$' "$out"
wait_status 2 '^DONE ' 30 || fail "el trabajo 2 no terminó"

# ===================================================================== 3
echo "[3] Cola llena: BUSY sin bloquear (C = 2 sillas)"
out=$(cl SUBMIT 400 400 1500 1 0 200 200)
expect "trabajo largo aceptado" '^OK 3$' "$out"
wait_status 3 '^RUNNING [0-9]+/1500$' 20 && ok "STATUS RUNNING <paso>/<total>" || fail "no llegó a RUNNING"
expect "RESULT de un trabajo en ejecución" '^ERROR' "$(cl RESULT 3)"
expect "PEEK sobre un trabajo en ejecución" '^VALUE [0-9]+ [-+0-9.e]+$' "$(cl PEEK 3 200 200)"
expect "silla 1 ocupada" '^OK 4$' "$(cl SUBMIT 24 24 10 2 0 12 12)"
expect "silla 2 ocupada" '^OK 5$' "$(cl SUBMIT 24 24 10 2 0 12 12)"
expect "STATUS de trabajo en espera: posición 1" '^QUEUED 1$' "$(cl STATUS 4)"
expect "STATUS de trabajo en espera: posición 2" '^QUEUED 2$' "$(cl STATUS 5)"
t0=$(date +%s.%N)
out=$(cl SUBMIT 24 24 10 2 0 12 12)
t1=$(date +%s.%N)
expect "sin sillas -> BUSY" '^BUSY$' "$out"
awk -v a="$t0" -v b="$t1" 'BEGIN { exit !(b - a < 1.0) }' && ok "BUSY inmediato (no bloquea al cliente)" || fail "BUSY tardó demasiado"
wait_status 5 '^DONE ' 60 && ok "la cola se vacía" || fail "la cola no se vació"
expect "BUSY no consumió id (siguiente id = 6)" '^OK 6$' "$(cl SUBMIT 24 24 10 2 0 12 12)"
wait_status 6 '^DONE ' 30 || fail "el trabajo 6 no terminó"

out=$(cl SHUTDOWN)
expect "SHUTDOWN responde BYE" '^BYE$' "$out"
wait "$SRV_PID" 2>/dev/null

# ===================================================================== 4
echo "[4] Estrés: 60 clientes concurrentes"
N=60
start_server 100 4 || exit 1
mkdir -p "$TMP/stress"

for i in $(seq 1 $N); do
    cl SUBMIT 32 32 50 2 10 16 16 >"$TMP/stress/$i" &
done

wait $(jobs -p | grep -v "^$SRV_PID$") 2>/dev/null
cat "$TMP"/stress/* > "$TMP/stress_all"
oks=$(grep -c '^OK ' "$TMP/stress_all")
busy=$(grep -c '^BUSY$' "$TMP/stress_all")
[ $((oks + busy)) -eq $N ] && ok "los $N clientes recibieron respuesta ($oks OK, $busy BUSY)" \
    || fail "respuestas incompletas" "$oks OK + $busy BUSY"
ids=$(grep '^OK ' "$TMP/stress_all" | awk '{print $2}' | sort -n)
dups=$(echo "$ids" | uniq -d | wc -l)
[ "$dups" -eq 0 ] && ok "ningún id duplicado" || fail "ids duplicados" "$dups"
maxid=$(echo "$ids" | tail -1)
[ "${maxid:-0}" -eq "$oks" ] && ok "ids consecutivos 1..$oks (ningún trabajo perdido)" \
    || fail "ids no consecutivos" "max=$maxid, OK=$oks"
all_done=1
for id in $ids; do
    wait_status "$id" '^DONE ' 60 || { all_done=0; fail "el trabajo $id no terminó"; }
done
[ "$all_done" -eq 1 ] && ok "todos los trabajos aceptados terminaron en DONE"
files=$(ls "$SNAP" | grep -c '^job[0-9]*_step[0-9]*\.bin$')
[ "$files" -eq $((oks * 5)) ] && ok "$files instantáneas en disco (5 por trabajo)" || fail "instantáneas" "$files != $((oks * 5))"
[ "$(ls "$SNAP" | grep -c '\.tmp$')" -eq 0 ] && ok "no quedaron archivos .tmp" || fail "archivos .tmp residuales"
expect "no existe un id extra" '^ERROR' "$(cl STATUS $((oks + 1)))"
cl SHUTDOWN >/dev/null
wait "$SRV_PID" 2>/dev/null

# ===================================================================== 5
echo "[5] Apagado ordenado con trabajos en cola y clientes conectados"
start_server 5 2 || exit 1
expect "trabajo largo" '^OK 1$' "$(cl SUBMIT 500 500 2000 2 500 250 250)"
wait_status 1 '^RUNNING ' 20 || fail "no llegó a RUNNING"
for i in 1 2 3; do
    expect "trabajo $((i + 1)) en cola" "^OK $((i + 1))\$" "$(cl SUBMIT 24 24 10 2 0 12 12)"
done
exec 3<>/dev/tcp/127.0.0.1/"$PORT"    # clientes conectados que no envían nada
exec 4<>/dev/tcp/127.0.0.1/"$PORT"

out=$(cl SHUTDOWN)
expect "SHUTDOWN responde BYE (tras terminar el trabajo en curso)" '^BYE$' "$out"

read -t 5 -r _ <&3; rc3=$?
read -t 5 -r _ <&4; rc4=$?
exec 3>&- 3<&- 4>&- 4<&-
[ "$rc3" -lt 128 ] && [ "$rc4" -lt 128 ] && ok "los clientes inactivos fueron desconectados" \
    || fail "un cliente inactivo quedó bloqueado" "rc=$rc3,$rc4"

for i in $(seq 1 100); do kill -0 "$SRV_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$SRV_PID" 2>/dev/null; then
    fail "el servidor no terminó"
else
    wait "$SRV_PID"; rc=$?
    [ "$rc" -eq 0 ] && ok "el servidor terminó con código 0" || fail "código de salida" "$rc"
fi
grep -q "servidor apagado" "$TMP/server_$PORT.log" && ok "mensaje 'servidor apagado'" || fail "falta mensaje de apagado"
[ -f "$SNAP/job1_step2000.bin" ] && ok "el trabajo en curso terminó y el búfer se vació a disco" \
    || fail "falta la última instantánea del trabajo en curso"
[ "$(ls "$SNAP" | grep -c '^job1_step')" -eq 4 ] && ok "4 instantáneas del trabajo 1" || fail "instantáneas del trabajo 1"
if cl STATUS 1 >/dev/null 2>&1; then fail "el servidor aún acepta conexiones"; else ok "ya no acepta conexiones"; fi

echo
if [ "$FAILS" -eq 0 ]; then echo "PROTOCOLO: todo OK"; exit 0; else echo "PROTOCOLO: $FAILS verificacion(es) fallaron"; exit 1; fi