import json, os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

GAME = os.path.expanduser("~/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo")
CLI = os.path.expanduser("~/.local/share/spaxer/tools/s2v/Source2Viewer-CLI")
OUT = os.path.expanduser("~/.config/spaxer/preview")
MODEL = "agents/models/tm_phoenix/tm_phoenix.vmdl_c"
POSE = "animation/anims/ui_anims/main_menu/t/t_main_menu_rifle02_idle"
WIDTH, HEIGHT = 420, 840
SKELETON = [("head_0", "neck_0"), ("neck_0", "spine_2"), ("spine_2", "pelvis"),
            ("neck_0", "arm_upper_L"), ("arm_upper_L", "arm_lower_L"), ("arm_lower_L", "hand_L"),
            ("neck_0", "arm_upper_R"), ("arm_upper_R", "arm_lower_R"), ("arm_lower_R", "hand_R"),
            ("pelvis", "leg_upper_L"), ("leg_upper_L", "leg_lower_L"), ("leg_lower_L", "ankle_L"),
            ("pelvis", "leg_upper_R"), ("leg_upper_R", "leg_lower_R"), ("leg_lower_R", "ankle_R")]


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
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc["type"]]
        item = np.dtype(dtype).itemsize * width
        stride = view.get("byteStride", 0) or item
        offset = self.bin_offset + view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        raw = np.frombuffer(self.data, np.uint8, stride * (acc["count"] - 1) + item, offset)
        rows = np.lib.stride_tricks.as_strided(raw, (acc["count"], item), (stride, 1)).copy()
        return rows.view(dtype).reshape(-1, width)


def quat_matrix(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def trs(translation, rotation, scale):
    m = np.eye(4)
    m[:3, :3] = quat_matrix(rotation) * np.asarray(scale)
    m[:3, 3] = translation
    return m


def posed_globals(glb):
    nodes = glb.json["nodes"]
    pose = {i: [np.array(n.get("translation", [0, 0, 0]), float), np.array(n.get("rotation", [0, 0, 0, 1]), float),
                np.array(n.get("scale", [1, 1, 1]), float)] for i, n in enumerate(nodes)}
    for animation in glb.json.get("animations", []):
        for channel in animation["channels"]:
            sampler = animation["samplers"][channel["sampler"]]
            values = glb.accessor(sampler["output"])
            slot = {"translation": 0, "rotation": 1, "scale": 2}.get(channel["target"]["path"])
            if slot is not None:
                pose[channel["target"]["node"]][slot] = values[len(values) // 2].astype(float)
    world = {}

    def visit(index, parent):
        world[index] = parent @ trs(*pose[index])
        for child in nodes[index].get("children", []):
            visit(child, world[index])

    for root in glb.json["scenes"][0]["nodes"]:
        visit(root, np.eye(4))
    return world


def load_texture(glb, directory, material_index):
    material = glb.json["materials"][material_index]
    texture = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
    if texture is None:
        return None
    image = glb.json["images"][glb.json["textures"][texture["index"]]["source"]]
    return np.asarray(Image.open(os.path.join(directory, image["uri"])).convert("RGB"), np.float32) / 255.0


def skinned_meshes(glb, directory, world):
    for index, node in enumerate(glb.json["nodes"]):
        name = node.get("name", "")
        if "mesh" not in node or "thirdperson" not in name:
            continue
        skin = glb.json["skins"][node["skin"]]
        inverse = glb.accessor(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
        joints = np.stack([world[j] @ inverse[k] for k, j in enumerate(skin["joints"])])
        for primitive in glb.json["meshes"][node["mesh"]]["primitives"]:
            attrs = primitive["attributes"]
            positions = glb.accessor(attrs["POSITION"]).astype(float)
            normals = glb.accessor(attrs["NORMAL"]).astype(float)
            uvs = glb.accessor(attrs["TEXCOORD_0"]).astype(float)
            joint_ids = glb.accessor(attrs["JOINTS_0"]).astype(int)
            weights = glb.accessor(attrs["WEIGHTS_0"]).astype(float)
            if weights.max() > 1.5:
                weights /= 255.0
            blend = np.einsum("vk,vkij->vij", weights, joints[joint_ids])
            homogeneous = np.concatenate([positions, np.ones((len(positions), 1))], 1)
            skinned = np.einsum("vij,vj->vi", blend, homogeneous)[:, :3]
            turned = np.einsum("vij,vj->vi", blend[:, :3, :3], normals)
            turned /= np.maximum(np.linalg.norm(turned, axis=1, keepdims=True), 1e-6)
            indices = glb.accessor(primitive["indices"]).ravel().astype(int).reshape(-1, 3)
            yield skinned, turned, uvs, indices, load_texture(glb, directory, primitive["material"])


def render(meshes, joints_world):
    all_points = np.concatenate([m[0] for m in meshes])
    front = np.array([0.0, 0.0, 1.0])
    right = np.array([1.0, 0.0, 0.0])
    up = np.array([0.0, 1.0, 0.0])
    lo = np.array([all_points @ right, all_points @ up]).min(1)
    hi = np.array([all_points @ right, all_points @ up]).max(1)
    margin = 0.04
    span = max((hi[0] - lo[0]) / WIDTH, (hi[1] - lo[1]) / HEIGHT) * (1 + margin * 2)
    center = (lo + hi) / 2

    def to_screen(p):
        return np.stack([(p @ right - center[0]) / span + WIDTH / 2, HEIGHT / 2 - (p @ up - center[1]) / span, p @ front], -1)

    color = np.zeros((HEIGHT, WIDTH, 3), np.float32)
    shade = np.zeros((HEIGHT, WIDTH), np.float32)
    depth = np.full((HEIGHT, WIDTH), -np.inf, np.float32)
    light = np.array([0.35, 0.6, 0.75])
    light /= np.linalg.norm(light)
    for points, normals, uvs, triangles, texture in meshes:
        screen = to_screen(points)
        lit = np.clip(normals @ light, 0, 1) * 0.75 + 0.25 + np.clip(1 - np.abs(normals @ front), 0, 1) ** 3 * 0.25
        for tri in triangles:
            a, b, c = screen[tri]
            x0, x1 = int(max(min(a[0], b[0], c[0]), 0)), int(min(max(a[0], b[0], c[0]), WIDTH - 1)) + 1
            y0, y1 = int(max(min(a[1], b[1], c[1]), 0)), int(min(max(a[1], b[1], c[1]), HEIGHT - 1)) + 1
            if x1 <= x0 or y1 <= y0:
                continue
            area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if abs(area) < 1e-9:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            w0 = ((b[0] - xs) * (c[1] - ys) - (b[1] - ys) * (c[0] - xs)) / area
            w1 = ((c[0] - xs) * (a[1] - ys) - (c[1] - ys) * (a[0] - xs)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            z = w0 * a[2] + w1 * b[2] + w2 * c[2]
            region = depth[y0:y1, x0:x1]
            closer = inside & (z > region)
            if not closer.any():
                continue
            region[closer] = z[closer]
            light_value = (w0 * lit[tri[0]] + w1 * lit[tri[1]] + w2 * lit[tri[2]])[closer]
            shade[y0:y1, x0:x1][closer] = light_value
            if texture is not None:
                uv = w0[..., None] * uvs[tri[0]] + w1[..., None] * uvs[tri[1]] + w2[..., None] * uvs[tri[2]]
                tx = (np.mod(uv[..., 0], 1.0) * (texture.shape[1] - 1)).astype(int)
                ty = (np.mod(uv[..., 1], 1.0) * (texture.shape[0] - 1)).astype(int)
                albedo = texture[ty, tx][closer]
            else:
                albedo = np.full((closer.sum(), 3), 0.5, np.float32)
            color[y0:y1, x0:x1][closer] = albedo * light_value[:, None]
    alpha = np.isfinite(depth).astype(np.float32)
    rgba = np.dstack([np.clip(color, 0, 1), alpha])
    shade_rgba = np.dstack([np.clip(shade, 0, 1)] * 3 + [alpha])
    screen_joints = {name: to_screen(point[None])[0][:2] for name, point in joints_world.items()}
    ys, xs = np.nonzero(alpha)
    bounds = (xs.min(), ys.min(), xs.max(), ys.max())
    return rgba, shade_rgba, screen_joints, bounds


def main():
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as workdir:
        glb_path = os.path.join(workdir, "agent.glb")
        subprocess.run([CLI, "-i", os.path.join(GAME, "pak01_dir.vpk"), "-f", MODEL, "-o", glb_path, "--gltf_export_format",
                        "glb", "-d", "--gltf_export_animations", "--gltf_animation_list", POSE, "--gltf_export_materials",
                        "--game", os.path.join(GAME, "gameinfo.gi")], capture_output=True)
        glb = Glb(glb_path)
        world = posed_globals(glb)
        meshes = list(skinned_meshes(glb, workdir, world))
        names = {n.get("name"): i for i, n in enumerate(glb.json["nodes"])}
        joints_world = {name: world[names[name]][:3, 3] for pair in SKELETON for name in pair if name in names}
        rgba, shade, joints, bounds = render(meshes, joints_world)
    Image.fromarray((rgba * 255).astype(np.uint8), "RGBA").save(os.path.join(OUT, "agent.png"))
    Image.fromarray((shade * 255).astype(np.uint8), "RGBA").save(os.path.join(OUT, "agent_shade.png"))
    with open(os.path.join(OUT, "agent.txt"), "w") as f:
        f.write(f"size {WIDTH} {HEIGHT}\n")
        f.write("bounds %d %d %d %d\n" % tuple(int(v) for v in bounds))
        for a, b in SKELETON:
            if a in joints and b in joints:
                f.write("bone %.1f %.1f %.1f %.1f\n" % (*joints[a], *joints[b]))
        if "head_0" in joints:
            f.write("head %.1f %.1f\n" % tuple(joints["head_0"]))
    print("preview written to", OUT)


if __name__ == "__main__":
    main()
