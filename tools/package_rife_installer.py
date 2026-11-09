"""Package a dedicated Vulkan installer from an already sealed OFXR payload."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(base, output):
    if output.exists() or Path(str(output) + ".zip").exists():
        raise RuntimeError("Previous installer is preserved; choose a new --output")
    for row in (base / "SHA256SUMS.txt").read_text().splitlines():
        digest, name = row.split("  ", 1)
        path = (base / name).resolve()
        if not path.is_relative_to(base.resolve()) or sha(path) != digest:
            raise RuntimeError("Sealed OFXR package integrity mismatch: " + name)
    provenance = json.loads((base / "BUILD_PROVENANCE.json").read_text(encoding="utf-8-sig"))
    output.mkdir(parents=True)
    for folder in ("tools", "python", "rife-vulkan", "licenses"):
        shutil.copytree(base / folder, output / folder)
    shutil.copy2(base / "LICENSE", output / "LICENSE")
    shutil.copy2(base / "Install-RIFE-Vulkan.cmd", output / "Install-RIFE-Vulkan.cmd")
    shutil.copy2(base / "docs/RIFE_VULKAN.md", output / "RIFE_VULKAN.md")
    (output / "LEGGIMI.txt").write_text(
        "INSTALLER RIFE VULKAN - SPERIMENTALE\n\n"
        "Estrarre tutta la ZIP, poi avviare Install-RIFE-Vulkan.cmd.\n"
        "Installa in %LOCALAPPDATA%/ofxr-bridge/RIFE/runtime senza Python globale.\n"
        "Download fissati e verificati; runtime validato prima dell'attivazione.\n"
        "Un runtime precedente viene conservato automaticamente: non occorre rinominarlo.\n"
        "Servono 2 GB liberi per l'installazione, oltre allo spazio per le cache.\n"
        f"Radeon/visore devono ancora essere validati. Usare la build OFXR Vulkan V{provenance['version']} o una successiva compatibile.\n"
        "Per le prove locali usare il kit di switch: evita di reinstallare il runtime NVIDIA.\n",
        encoding="utf-8", newline="\n")
    receipt = dict(schema=1, binary_version=provenance["version"], binary_source_commit=provenance["source_commit"],
                   installer_tools_commit=provenance["source_commit"],
                   model_runtime_bundled=False, default_backend="vulkan", default_destination="%LOCALAPPDATA%/ofxr-bridge/RIFE/runtime")
    (output / "INSTALLER_PROVENANCE.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    entries = sorted(p for p in output.rglob("*") if p.is_file())
    hashes = {p.relative_to(output).as_posix(): sha(p) for p in entries}
    (output / "SHA256SUMS.txt").write_text("".join(value + "  " + name + "\n" for name, value in hashes.items()), encoding="ascii")
    archive = Path(str(output) + ".zip")
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as zipped:
        for path in sorted(p for p in output.rglob("*") if p.is_file()):
            zipped.write(path, output.name + "/" + path.relative_to(output).as_posix())
    with zipfile.ZipFile(archive) as zipped:
        if zipped.testzip() is not None:
            raise RuntimeError("Installer ZIP CRC mismatch")
    Path(str(archive) + ".sha256").write_text(sha(archive) + "  " + archive.name + "\n", encoding="ascii")
    print("Verified standalone installer:", archive, sha(archive))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.package.resolve(), args.output.resolve())
