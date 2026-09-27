import glob, json, os, re, struct, subprocess, sys, tempfile
import numpy as np

GAME = os.path.expanduser("~/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo")
CLI = os.path.expanduser("~/.local/share/spaxer/tools/s2v/Source2Viewer-CLI")
OUT = os.path.expanduser("~/.config/spaxer/maps")
SEE_THROUGH = ("playerclip", "grenadeclip", "passbullets", "sky", "glass", "chainlink", "npcclip")


def node_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], np.float64).reshape(4, 4).T
    m = np.eye(4)
    if "scale" in node:
        m = np.diag([*node["scale"], 1.0]) @ m
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        r = np.eye(4)
        r[:3, :3] = [
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
        ]
        m = r @ m
    if "translation" in node:
        t = np.eye(4)
        t[:3, 3] = node["translation"]
        m = t @ m
    return m


class Glb:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        json_len = struct.unpack("<I", self.data[12:16])[0]
        self.json = json.loads(self.data[20:20 + json_len])
        self.bin_offset = 20 + json_len + 8

    def accessor(self, index):
        acc = self.json["accessors"][index]
        view = self.json["bufferViews"][acc["bufferView"]]
        dtype = {5126: np.float32, 5123: np.uint16, 5121: np.uint8, 5125: np.uint32}[acc["componentType"]]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        item = np.dtype(dtype).itemsize * width
        stride = view.get("byteStride", 0) or item
        offset = self.bin_offset + view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        raw = np.frombuffer(self.data, np.uint8, stride * (acc["count"] - 1) + item, offset)
        rows = np.lib.stride_tricks.as_strided(raw, (acc["count"], item), (stride, 1)).copy()
        return rows.view(dtype).reshape(-1, width)


def solid_triangles(glb):
    nodes = glb.json.get("nodes", [])
    triangles, materials, names = [], [], []

    def material_index(name):
        if name not in names:
            names.append(name)
        return names.index(name)

    def visit(index, parent):
        node = nodes[index]
        world = parent @ node_matrix(node)
        name = node.get("name", "").lower()
        if "mesh" in node and not any(tag in name for tag in SEE_THROUGH):
            surface = str(node.get("extras", {}).get("SurfaceProperty") or "default").lower()
            for primitive in glb.json["meshes"][node["mesh"]]["primitives"]:
                if primitive.get("mode", 4) != 4 or "indices" not in primitive:
                    continue
                local = glb.accessor(primitive["attributes"]["POSITION"]).astype(np.float64)
                gltf = local @ world[:3, :3].T + world[:3, 3]
                source = np.stack([gltf[:, 2], gltf[:, 0], gltf[:, 1]], 1) / 0.0254
                indices = glb.accessor(primitive["indices"]).ravel().astype(np.int64)
                batch = source[indices].reshape(-1, 9)
                triangles.append(batch)
                materials.append(np.full(len(batch), material_index(surface), np.uint8))
        for child in node.get("children", []):
            visit(child, world)

    for scene in glb.json.get("scenes", []):
        for root in scene.get("nodes", []):
            visit(root, np.eye(4))
    if not triangles:
        return np.zeros((0, 9), np.float32), np.zeros(0, np.uint8), names
    return np.concatenate(triangles).astype(np.float32), np.concatenate(materials), names


def export_sun(map_name, vpk, workdir):
    subprocess.run([CLI, "-i", vpk, "-f", f"maps/{map_name}/entities/default_ents.vents_c", "-o", workdir, "-d"],
                   capture_output=True)
    found = glob.glob(os.path.join(workdir, "**", "default_ents.vents"), recursive=True)
    if not found:
        return
    text = open(found[0], errors="replace").read()
    for block in text.split("classname")[1:]:
        if not block.lstrip().startswith('"light_environment"'):
            continue
        before = text[:text.index(block)]
        window = before[-3000:] + block[:3000]
        angles = [m for m in re.finditer(r"angles\s+\[\s*([-\d.]+),\s*([-\d.]+),", window)]
        if not angles:
            continue
        nearest = min(angles, key=lambda m: abs(m.start() - len(before[-3000:])))
        with open(os.path.join(OUT, map_name + ".sun"), "w") as f:
            f.write(f"{float(nearest.group(1))} {float(nearest.group(2))}\n")
        return


def convert(map_name, sun_only=False):
    vpk = os.path.join(GAME, "maps", map_name + ".vpk")
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as workdir:
        export_sun(map_name, vpk, workdir)
    if sun_only:
        return
    with tempfile.TemporaryDirectory() as workdir:
        subprocess.run([CLI, "-i", vpk, "-f", f"maps/{map_name}/world_physics.vmdl_c", "-o", workdir,
                        "-d", "--gltf_export_format", "glb"], capture_output=True)
        found = glob.glob(os.path.join(workdir, "**", "*_physics.glb"), recursive=True)
        if not found:
            print(f"{map_name}: no physics")
            return
        triangles, materials, names = solid_triangles(Glb(found[0]))
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, map_name + ".smap"), "wb") as f:
        f.write(b"SMAP" + struct.pack("<II", 2, len(names)))
        for name in names:
            encoded = name.encode()[:255]
            f.write(struct.pack("<B", len(encoded)) + encoded)
        f.write(struct.pack("<I", len(triangles)))
        f.write(triangles.tobytes())
        f.write(materials.tobytes())
    print(f"{map_name}: {len(triangles)} triangles")


def installed_maps():
    names = []
    for path in sorted(glob.glob(os.path.join(GAME, "maps", "*.vpk"))):
        name = os.path.basename(path)[:-4]
        if name.endswith("_vanity") or name.endswith("_dir") or name == "graphics_settings":
            continue
        names.append(name)
    return names


if __name__ == "__main__":
    args = sys.argv[1:]
    sun_only = "--sun-only" in args
    names = [a for a in args if not a.startswith("--")]
    for map_name in names or installed_maps():
        convert(map_name, sun_only)
