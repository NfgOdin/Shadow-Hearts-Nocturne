#!/usr/bin/env python3
"""Quick zero-dependency .glb (glTF 2.0) Rig & Texture Inspector for Meshy.ai / Blender models.

Usage:
  python scripts/inspect_glb.py path/to/koudelka.glb
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


def inspect_glb(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        print(f"ERROR: {path} is not a valid binary .glb file.")
        return 1
    version, total_len = struct.unpack_from("<II", data, 4)
    chunk_len, chunk_type = struct.unpack_from("<II", data, 12)
    if chunk_type != 0x4E4F534A:  # 'JSON'
        print("ERROR: Missing glTF JSON chunk.")
        return 1
    gltf = json.loads(data[20 : 20 + chunk_len].decode("utf-8"))

    meshes = gltf.get("meshes") or []
    accessors = gltf.get("accessors") or []
    nodes = gltf.get("nodes") or []
    skins = gltf.get("skins") or []
    images = gltf.get("images") or []
    anims = gltf.get("animations") or []

    total_verts = 0
    total_tris = 0
    has_joints = False
    has_weights = False
    for m in meshes:
        for p in m.get("primitives") or []:
            attrs = p.get("attributes") or {}
            if "POSITION" in attrs:
                total_verts += int(accessors[attrs["POSITION"]].get("count", 0))
            if "JOINTS_0" in attrs:
                has_joints = True
            if "WEIGHTS_0" in attrs:
                has_weights = True
            if "indices" in p:
                total_tris += int(accessors[p["indices"]].get("count", 0)) // 3
            elif "POSITION" in attrs:
                total_tris += int(accessors[attrs["POSITION"]].get("count", 0)) // 3

    print(f"=== GLB Inspector: {path.name} (glTF v{version}, {total_len / 1e6:.2f} MB) ===")
    print(f"  Geometry : {total_tris:,} triangles | {total_verts:,} vertices | {len(meshes)} mesh(es)")
    print(f"  Textures : {len(images)} embedded image(s) " +
          ", ".join(f"[{i}: {img.get('mimeType', '?')}]" for i, img in enumerate(images)))
    print(f"  Anims    : {len(anims)} embedded animation clip(s)")
    if skins and has_joints and has_weights:
        j_indices = skins[0].get("joints") or []
        names = [nodes[j].get("name", f"joint_{j}") for j in j_indices if j < len(nodes)]
        print(f"  Rigging  : RIGGED OK! ({len(j_indices)} joints, JOINTS_0 + WEIGHTS_0 present)")
        print(f"  Bones    : {', '.join(names[:24])}" + (" ..." if len(names) > 24 else ""))
    else:
        print("  Rigging  : UNRIGGED (no skin/weights — will render in root-attached rigid mode until rigged)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scripts/inspect_glb.py <path-to-model.glb>")
        raise SystemExit(2)
    raise SystemExit(inspect_glb(Path(sys.argv[1])))
