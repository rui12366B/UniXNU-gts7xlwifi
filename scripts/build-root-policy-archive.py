#!/usr/bin/env python3
"""Compile the VerifiedRoot provider and two signed policies for the XNU link.

From unixnu-sm-t733 (4c679b4) scripts/build-root-policy-archive.py. gts7xlwifi changes: the policies are the
owner policies the earlier S7plus-iPadOS route generated for the same SHNGROOT image on sda39 (OS 04769D26,
SystemOS 8846FE78, VROOT01, one RSA-3072 anchor), copied to .cache/firmware/root-policies; the archive is named
libVerifiedRootProbe.a for CONFIG_XNU_VERIFIED_ROOT_ARCHIVE; no iPhoneOS SDK is needed for the policy object.
"""

import argparse
import json
from pathlib import Path
import struct
import subprocess


def compile_c(record, source, destination, include, log):
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(destination)])
            index += 1
        elif value in ("-MF", "-MJ"):
            index += 1
        elif value not in ("-MD", "-MP"):
            command.append(value)

        index += 1

    command.extend(["-I", str(include), "-Werror", "-Wno-missing-include-dirs"])
    log.with_suffix(".command.json").write_text(json.dumps(command, indent=2) + "\n")
    with log.open("w") as stream:
        subprocess.run(command, cwd=record["directory"], stdout=stream,
                       stderr=subprocess.STDOUT, check=True)


def assembler_path(path):
    return str(path).replace("\\", "\\\\").replace('"', '\\"')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--policies", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    tree = args.tree.resolve()
    policies = args.policies.resolve()
    output = args.output.resolve()
    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    kit = tree / "kits/VerifiedRoot"
    anchor = policies / "os/trusted-key.bin"
    key = anchor.read_bytes()
    if len(key) != 395 or key != (policies / "systemos/trusted-key.bin").read_bytes():
        raise ValueError("Both policies must use the same complete public anchor")

    manifests = []
    for component in ("os", "systemos"):
        path = policies / component / "manifest.bin"
        with path.open("rb") as stream:
            header = stream.read(160)

        size = path.stat().st_size
        if len(header) != 160 or header[:8] != b"VROOT01\0":
            raise ValueError("Missing signed policy header: " + component)

        image_bytes, block_count = struct.unpack_from("<QQ", header, 24)
        if size != 544 + block_count * 32:
            raise ValueError("Incomplete signed policy: " + component)

        manifests.append({"component": component, "path": path, "bytes": size,
                          "image_bytes": image_bytes, "block_count": block_count})

    output.mkdir(parents=True, exist_ok=True)
    assembly = output / "policies.S"
    lines = [
        ".section __DATA_CONST,__const", ".balign 8",
        ".globl _verified_root_key", "_verified_root_key:",
        '.incbin "' + assembler_path(anchor) + '"',
    ]
    for item in manifests:
        symbol = "_verified_root_" + item["component"] + "_manifest"
        lines.extend([
            ".balign 8", ".globl " + symbol, symbol + ":",
            '.incbin "' + assembler_path(item["path"]) + '"',
        ])

    lines.extend([".balign 8", ".globl _verified_root_policies", "_verified_root_policies:"])
    for item in manifests:
        lines.append(".quad _verified_root_" + item["component"] + "_manifest, " + str(item["bytes"]))

    lines.extend([
        ".globl _verified_root_policy_count", "_verified_root_policy_count:", ".quad 2", "",
    ])
    assembly.write_text("\n".join(lines))

    objects = []
    for relative in ("src/verified_root.c", "adapters/xnu_crypto.c"):
        source = kit / relative
        destination = output / (source.stem + ".o")
        compile_c(record, source, destination, kit / "include", output / (source.stem + ".log"))
        objects.append(destination)

    policy_object = output / "policies.o"
    with (output / "policies.log").open("w") as stream:
        subprocess.run([
            "clang", "-target", "arm64-apple-ios17.0",
            "-c", str(assembly), "-o", str(policy_object),
        ], stdout=stream, stderr=subprocess.STDOUT, check=True)
    objects.append(policy_object)

    archive = output / "libVerifiedRootProbe.a"
    archive.unlink(missing_ok=True)
    subprocess.run(["xcrun", "ar", "rcs", str(archive), *map(str, objects)], check=True)
    report = {
        "compile_recipe": str(recipe),
        "public_anchor": str(anchor),
        "archive": str(archive),
        "archive_bytes": archive.stat().st_size,
        "policies": [{**item, "path": str(item["path"])} for item in manifests],
        "exports": ["vr_open", "vr_verify_block", "vr_xnu_crypto", "verified_root_key",
                    "verified_root_policies", "verified_root_policy_count",
                    "verified_root_os_manifest", "verified_root_systemos_manifest"],
    }
    (output / "inputs.json").write_text(json.dumps(report, indent=2) + "\n")
    print(archive)


if __name__ == "__main__":
    main()
