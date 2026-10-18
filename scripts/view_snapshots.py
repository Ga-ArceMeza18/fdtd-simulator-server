#!/usr/bin/env python3

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np

NAME = re.compile(r"job(\d+)_step(\d+)\.bin$")


def read_snapshot(path):
    """Devuelve (nx, ny, paso, matriz Ez[nx][ny]); valida el tamaño del archivo."""
    with open(path, "rb") as f:
        header = np.fromfile(f, dtype="<i4", count=3)
        if header.size != 3:
            raise ValueError(f"{path}: encabezado incompleto")
        nx, ny, q = (int(v) for v in header)
        if nx <= 0 or ny <= 0:
            raise ValueError(f"{path}: dimensiones inválidas ({nx}, {ny})")
        data = np.fromfile(f, dtype="<f8", count=nx * ny)
        if data.size != nx * ny:
            raise ValueError(f"{path}: esperaba {nx * ny} valores y hay {data.size}")
    return nx, ny, q, data.reshape(nx, ny)


def find_files(target, job, step):
    """Lista [(job, paso, ruta)] ordenada por paso."""
    if os.path.isfile(target):
        m = NAME.search(os.path.basename(target))
        return [(int(m.group(1)) if m else 0, int(m.group(2)) if m else 0, target)]
    found = []
    for path in glob.glob(os.path.join(target, "job*_step*.bin")):
        m = NAME.search(os.path.basename(path))
        if m:
            found.append((int(m.group(1)), int(m.group(2)), path))
    jobs = sorted({j for j, _, _ in found})
    if job is None:
        if len(jobs) > 1:
            raise SystemExit(f"Hay varios trabajos en {target} ({', '.join(map(str, jobs))}); use --job <id>.")
        job = jobs[0] if jobs else None
    found = [t for t in found if t[0] == job]
    if step is not None:
        found = [t for t in found if t[1] == step]
    return sorted(found, key=lambda t: t[1])


def downsample(a, factor):
    return a[::factor, ::factor] if factor > 1 else a


def auto_stride(nx, ny, max_side):
    return max(1, int(np.ceil(max(nx, ny) / max_side)))


def global_vmax(files, stride, percentile):
    """Escala común a todos los cuadros para que se pueda comparar la amplitud."""
    best = 0.0
    for _, _, path in files:
        _, _, _, ez = read_snapshot(path)
        v = np.percentile(np.abs(downsample(ez, stride)), percentile)
        best = max(best, float(v))
    return best if best > 0 else 1.0


def render_rgb(ez, vmax, cmap_name, upscale):
    """Matriz -> imagen RGB uint8 (sin pasar por una figura de matplotlib: es rápido)."""
    import matplotlib

    cmap = matplotlib.colormaps[cmap_name]
    norm = np.clip((ez / vmax + 1.0) / 2.0, 0.0, 1.0)         # [-vmax, vmax] -> [0, 1]
    rgb = (cmap(norm)[:, :, :3] * 255).astype(np.uint8)
    if upscale > 1:
        rgb = np.repeat(np.repeat(rgb, upscale, axis=0), upscale, axis=1)
    return rgb


def make_gif(files, out, args):
    from PIL import Image, ImageDraw

    nx, ny, _, _ = read_snapshot(files[0][2])
    stride = args.stride or auto_stride(nx, ny, args.max_side)
    vmax = args.vmax or global_vmax(files, stride, args.percentile)
    frames = []
    for job, step, path in files:
        _, _, q, ez = read_snapshot(path)
        small = downsample(ez, stride)
        upscale = max(1, 400 // max(small.shape))
        img = Image.fromarray(render_rgb(small, vmax, args.cmap, upscale))
        draw = ImageDraw.Draw(img)
        draw.rectangle([0, 0, 150, 14], fill=(255, 255, 255))
        draw.text((3, 1), f"trabajo {job}  paso {q}", fill=(0, 0, 0))
        frames.append(img)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=int(1000 / args.fps), loop=0)
    print(f"{out}: {len(frames)} cuadros, escala ±{vmax:.3g}, submuestreo 1/{stride}")


def save_png(path, ez, q, job, vmax, cmap, show):
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(6, 5.2))
    im = ax.imshow(ez, cmap=cmap, vmin=-vmax, vmax=vmax, origin="upper", interpolation="nearest")
    ax.set_xlabel("j (columna)")
    ax.set_ylabel("i (fila)")
    ax.set_title(f"$E_z$ — trabajo {job}, paso {q}")
    fig.colorbar(im, ax=ax, label="$E_z$")
    fig.tight_layout()
    if show:
        plt.show()
    if path:
        fig.savefig(path, dpi=130)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="carpeta de instantáneas (o un archivo .bin)")
    ap.add_argument("--job", type=int, help="id del trabajo (obligatorio si hay varios)")
    ap.add_argument("--step", type=int, help="solo este paso")
    ap.add_argument("--list", action="store_true", help="solo listar las instantáneas")
    ap.add_argument("--frames", metavar="DIR", help="guardar un PNG por instantánea en DIR")
    ap.add_argument("--gif", metavar="ARCHIVO", help="animación GIF")
    ap.add_argument("--mp4", metavar="ARCHIVO", help="video MP4 (necesita ffmpeg)")
    ap.add_argument("--show", action="store_true", help="abrir una ventana (requiere entorno gráfico)")
    ap.add_argument("--fps", type=int, default=8)
    ap.add_argument("--cmap", default="RdBu_r", help="mapa de colores (por defecto RdBu_r)")
    ap.add_argument("--vmax", type=float, help="escala fija ±vmax (por defecto: percentil global)")
    ap.add_argument("--percentile", type=float, default=99.9, help="percentil para la escala automática")
    ap.add_argument("--stride", type=int, help="submuestreo espacial (por defecto automático)")
    ap.add_argument("--max-side", type=int, default=512, help="lado máximo de los cuadros del GIF/MP4")
    args = ap.parse_args()

    if not args.show:
        import matplotlib
        matplotlib.use("Agg")

    files = find_files(args.target, args.job, args.step)
    if not files:
        print("No se encontraron instantáneas.")
        return 1

    if args.list or not (args.frames or args.gif or args.mp4 or args.show):
        print(f"{len(files)} instantánea(s):")
        for job, step, path in files:
            nx, ny, q, _ = read_snapshot(path)
            print(f"  trabajo {job:>4}  paso {q:>6}  {nx}x{ny}  {os.path.getsize(path) / 1e6:8.2f} MB  {path}")
        if not (args.frames or args.gif or args.mp4 or args.show):
            print("\nUse --gif, --frames, --mp4 o --show para visualizarlas.")
        return 0

    if args.frames or args.show:
        nx, ny, _, _ = read_snapshot(files[0][2])
        stride = args.stride or 1
        vmax = args.vmax or global_vmax(files, stride, args.percentile)
        if args.frames:
            os.makedirs(args.frames, exist_ok=True)
        for job, step, path in files:
            _, _, q, ez = read_snapshot(path)
            out = os.path.join(args.frames, f"job{job}_step{q}.png") if args.frames else None
            save_png(out, downsample(ez, stride), q, job, vmax, args.cmap, args.show)
        if args.frames:
            print(f"{args.frames}/: {len(files)} imágenes PNG, escala ±{vmax:.3g}")

    if args.gif:
        make_gif(files, args.gif, args)

    if args.mp4:
        if shutil.which("ffmpeg") is None:
            print("ffmpeg no está instalado: use --gif en su lugar.")
            return 1
        with tempfile.TemporaryDirectory() as tmp:
            gif = os.path.join(tmp, "anim.gif")
            make_gif(files, gif, args)
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", gif, "-movflags", "faststart",
                            "-pix_fmt", "yuv420p", "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", args.mp4],
                           check=True)
        print(f"{args.mp4} listo")
    return 0


if __name__ == "__main__":
    sys.exit(main())