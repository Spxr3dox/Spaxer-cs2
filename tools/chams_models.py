import json, os, re, struct, subprocess, sys, tempfile
import numpy as np

GAME = os.path.expanduser("~/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo")
CLI = os.path.expanduser("~/.local/share/spaxer/tools/s2v/Source2Viewer-CLI")
OUT = os.path.expanduser("~/.config/spaxer/models")
VPK = os.path.join(GAME, "pak01_dir.vpk")
MESH_PREFIX = "thirdperson_"


def run_cli(*args):
    return subprocess.run([CLI, "-i", VPK, *args], capture_output=True, text=True).stdout


def list_agents():
    listing = run_cli("--vpk_list", "-e", "vmdl_c", "-f", "agents/models/")
    paths = [line.split()[0] for line in listing.splitlines() if line.startswith("agents/models/")]
    return [p for p in paths if re.search(r"/(tm|ctm)_[^/]+\.vmdl_c$", p)]


def bracket_block(text, name):
    start = text.index("[", text.index(name + " = "))
    depth = 0
    for k in range(start, len(text)):
        if text[k] == "[":
            depth += 1
        elif text[k] == "]":
            depth -= 1
            if depth == 0:
                return text[start:k + 1]
    raise ValueError(name)


def numbers(block):
    return [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?(?:e-?\d+)?", block)]


def quat_matrix(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def bind_pose(data_text):
    parents = [int(v) for v in numbers(bracket_block(data_text, "m_nParent"))]
    positions = np.array(numbers(bracket_block(data_text, "m_bonePosParent"))).reshape(-1, 3)
    rotations = np.array(numbers(bracket_block(data_text, "m_boneRotParent"))).reshape(-1, 4)
    world = []
    for i, parent in enumerate(parents):
        local = np.eye(4)
        local[:3, :3] = quat_matrix(rotations[i])
        local[:3, 3] = positions[i]
        world.append(local if parent < 0 else world[parent] @ local)
    return world


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


def convert(model_path, workdir):
    stem = os.path.splitext(os.path.basename(model_path))[0]
    glb_path = os.path.join(workdir, stem + ".glb")
    run_cli("-f", model_path, "-o", glb_path, "--gltf_export_format", "glb", "-d", "--game", os.path.join(GAME, "gameinfo.gi"))
    data_text = run_cli("-f", model_path, "-b", "DATA")
    if not os.path.exists(glb_path):
        return None
    bind = bind_pose(data_text)
    inverse_bind = [np.linalg.inv(m) for m in bind]
    glb = Glb(glb_path)

    all_local, all_normals, all_joints, all_weights, all_indices = [], [], [], [], []
    vertex_base = 0
    for mesh in glb.json["meshes"]:
        if MESH_PREFIX not in mesh["name"].split(".")[-1]:
            continue
        for primitive in mesh["primitives"]:
            attributes = primitive["attributes"]
            if "JOINTS_0" not in attributes:
                continue
            raw = glb.accessor(attributes["POSITION"]).astype(np.float64) / 0.0254
            positions = np.stack([raw[:, 2], raw[:, 0], raw[:, 1]], 1)
            raw_normals = glb.accessor(attributes["NORMAL"]).astype(np.float64)
            normals = np.stack([raw_normals[:, 2], raw_normals[:, 0], raw_normals[:, 1]], 1)
            joints = glb.accessor(attributes["JOINTS_0"]).astype(np.int64)
            weights = glb.accessor(attributes["WEIGHTS_0"]).astype(np.float64)
            if weights.dtype != np.float64 or weights.max() > 1.5:
                weights = weights / 255.0
            indices = glb.accessor(primitive["indices"]).ravel().astype(np.int64)
            used = np.unique(indices)
            remap = np.full(len(positions), -1, np.int64)
            remap[used] = np.arange(len(used)) + vertex_base
            weights = weights[used]
            joints = joints[used]
            weights[weights < 0.01] = 0.0
            sums = weights.sum(1, keepdims=True)
            sums[sums == 0] = 1.0
            weights = weights / sums
            inverse = np.stack(inverse_bind)
            homogeneous = np.concatenate([positions[used], np.ones((len(used), 1))], 1)
            local = np.einsum("vkij,vj->vki", inverse[joints], homogeneous)[:, :, :3]
            local_normals = np.einsum("vkij,vj->vki", inverse[joints][:, :, :3, :3], normals[used])
            local_normals /= np.maximum(np.linalg.norm(local_normals, axis=2, keepdims=True), 1e-6)
            all_local.append(local)
            all_normals.append(local_normals)
            all_joints.append(joints)
            all_weights.append(weights)
            all_indices.append(remap[indices])
            vertex_base += len(used)
    if not all_local:
        return None
    local = np.concatenate(all_local).astype(np.float32)
    local_normals = np.concatenate(all_normals).astype(np.float32)
    joints = np.concatenate(all_joints).astype(np.uint16)
    weights = np.concatenate(all_weights).astype(np.float32)
    indices = np.concatenate(all_indices).astype(np.uint32)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, stem + ".smdl"), "wb") as out:
        out.write(struct.pack("<4sIII", b"SMDL", 2, len(local), len(indices)))
        out.write(joints.tobytes())
        out.write(weights.tobytes())
        out.write(local.tobytes())
        out.write(local_normals.tobytes())
        out.write(indices.tobytes())
    return stem, len(local), len(indices) // 3


def main():
    models = sys.argv[1:] or list_agents()
    with tempfile.TemporaryDirectory() as workdir:
        for model in models:
            result = convert(model, workdir)
            print(model, result, flush=True)


if __name__ == "__main__":
    main()
