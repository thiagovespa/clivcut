#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Exports the RIFE v4.26 frame interpolation network (Practical-RIFE, MIT license,
https://github.com/hzwer/Practical-RIFE) to ONNX for clivcut's smooth slow motion.

The official weights are downloaded from the link in Practical-RIFE's README and
checked by SHA-256. The network code ships inside that archive (train_log/IFNet_HDv3.py);
only its `warp` helper lives elsewhere in the upstream repository, so an equivalent is
defined here.

Model interface (opset 17, dynamic height and width, both multiples of 128):
    inputs:  img0, img1  float32 [1, 3, H, W], RGB in 0..1
             timestep    float32 [1, 1, 1, 1], position of the new frame (0 < t < 1)
    output:  frame       float32 [1, 3, H, W]

Requirements: torch (CPU is enough), onnx, onnxruntime, numpy.
Usage: python3 tools/rife/export_onnx.py OUTPUT.onnx [--weights RIFE_v4.26.zip]
"""

import argparse
import hashlib
import io
import sys
import types
import urllib.request
import zipfile

import numpy as np
import torch
import torch.nn.functional as F

WEIGHTS_URL = "https://drive.usercontent.google.com/download?id=1gViYvvQrtETBgU1w8axZSsr7YUuw31uy&export=download&confirm=t"
WEIGHTS_SHA256 = "c2452dd2b244947d4be580156bbead60d6b72af5736860f7d6b3f99648c9c4cc"
# Scales of the five flow blocks for full-resolution inference (Practical-RIFE: 16/scale ... 1/scale).
SCALE_LIST = [16, 8, 4, 2, 1]


def warp(img, flow):
    """Backward warp of `img` by `flow` (pixels), border padding, like Practical-RIFE's warplayer."""
    n, _, h, w = flow.shape
    xs = torch.arange(w, dtype=flow.dtype, device=flow.device) * (2.0 / (w - 1)) - 1.0
    ys = torch.arange(h, dtype=flow.dtype, device=flow.device) * (2.0 / (h - 1)) - 1.0
    grid_x = xs.view(1, 1, 1, w).expand(n, 1, h, w)
    grid_y = ys.view(1, 1, h, 1).expand(n, 1, h, w)
    fx = flow[:, 0:1] / ((img.shape[3] - 1.0) / 2.0)
    fy = flow[:, 1:2] / ((img.shape[2] - 1.0) / 2.0)
    grid = torch.cat([grid_x + fx, grid_y + fy], 1).permute(0, 2, 3, 1)
    return F.grid_sample(img, grid, mode="bilinear", padding_mode="border", align_corners=True)


def load_weights(path):
    data = open(path, "rb").read() if path else urllib.request.urlopen(WEIGHTS_URL).read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != WEIGHTS_SHA256:
        sys.exit(f"weights SHA-256 mismatch: {digest}")
    archive = zipfile.ZipFile(io.BytesIO(data))
    source = archive.read("train_log/IFNet_HDv3.py").decode()
    state = torch.load(io.BytesIO(archive.read("train_log/flownet.pkl")), map_location="cpu", weights_only=True)
    return source, {k.removeprefix("module."): v for k, v in state.items()}


def build_network(source, state):
    # IFNet_HDv3.py imports warp from model.warplayer: provide it as a module.
    warplayer = types.ModuleType("model.warplayer")
    warplayer.warp = warp
    sys.modules["model"] = types.ModuleType("model")
    sys.modules["model.warplayer"] = warplayer
    namespace = {}
    exec(compile(source, "IFNet_HDv3.py", "exec"), namespace)
    net = namespace["IFNet"]()
    net.load_state_dict(state, strict=False)
    return net.eval()


class Interpolator(torch.nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, img0, img1, timestep):
        _, _, merged = self.net(torch.cat((img0, img1), 1), timestep, SCALE_LIST)
        return merged[-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("--weights", help="RIFE_v4.26.zip already downloaded")
    args = parser.parse_args()

    torch.manual_seed(0)
    model = Interpolator(build_network(*load_weights(args.weights)))
    img0 = torch.rand(1, 3, 256, 384)
    img1 = torch.roll(img0, shifts=(3, 5), dims=(2, 3))
    t = torch.full((1, 1, 1, 1), 0.5)
    with torch.no_grad():
        expected = model(img0, img1, t).numpy()
        torch.onnx.export(model, (img0, img1, t), args.output, opset_version=17, dynamo=False,
                          input_names=["img0", "img1", "timestep"], output_names=["frame"],
                          dynamic_axes={"img0": {2: "height", 3: "width"}, "img1": {2: "height", 3: "width"},
                                        "frame": {2: "height", 3: "width"}})

    import onnxruntime as ort
    session = ort.InferenceSession(args.output, providers=["CPUExecutionProvider"])
    got = session.run(None, {"img0": img0.numpy(), "img1": img1.numpy(), "timestep": t.numpy()})[0]
    diff = float(np.abs(got - expected).max())
    print(f"{args.output}: max difference to PyTorch {diff:.2e}")
    if diff > 1e-3:
        sys.exit("ONNX output does not match PyTorch")
    print("sha256", hashlib.sha256(open(args.output, "rb").read()).hexdigest())


if __name__ == "__main__":
    main()
