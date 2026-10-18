

collect() {
    echo "# Fecha: $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "# Equipo: $(hostname)"
    if [ -n "${SLURM_JOB_ID:-}" ]; then
        echo "# Slurm: job=$SLURM_JOB_ID nodos=${SLURM_JOB_NODELIST:-?} cpus_por_tarea=${SLURM_CPUS_PER_TASK:-?}"
    fi
    echo
    echo "## lscpu";          lscpu 2>&1
    echo
    echo "## free -h";        free -h 2>&1
    echo
    echo "## uname -a";       uname -a 2>&1
    echo
    echo "## gcc --version";  gcc --version 2>&1
    echo
    echo "## nproc (CPUs disponibles para este proceso)"; nproc 2>&1
}

if [ $# -ge 1 ]; then
    collect | tee "$1"
else
    collect
fi