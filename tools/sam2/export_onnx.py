#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Exports SAM 2.1 Hiera-Tiny (Meta, Apache-2.0, https://github.com/facebookresearch/sam2) to
ONNX for clivcut's interactive cutout: click an object on one frame and follow it through
the clip. The video tracking state (memory bank, object pointers) is kept by the caller;
these graphs are the networks between those steps.

Requires the sam2 package at commit SAM2_COMMIT (pip install --no-deps -e <checkout>,
with SAM2_BUILD_CUDA=0), torch (CPU), onnx, onnxruntime and numpy. The checkpoint is
downloaded from Meta's link in the SAM 2 README and checked by SHA-256.

Graphs (batch 1, image 1024x1024, features 64x64):
  sam2_encoder.onnx
      image [1,3,1024,1024] (RGB resized to 1024x1024, ImageNet mean/std)
      -> vision_feat [1,256,64,64], vision_feat_nomem (+ no-memory embedding, for the
         prompted frame), vision_pos [1,256,64,64], high_res_0 [1,32,256,256],
         high_res_1 [1,64,128,128]
  sam2_heads.onnx (prompt encoder + mask decoder, with SAM 2's mask selection)
      pix_feat [1,256,64,64], high_res_0, high_res_1,
      point_coords [1,N,2] (pixels of the 1024x1024 image), point_labels [1,N] int32
      (1 positive, 0 negative, -1 none), multimask [1] (1 on the prompted frame)
      -> mask_logits [1,1,256,256], obj_ptr [1,256], object_score [1,1]
  sam2_memory_encoder.onnx
      pix_feat (vision_feat, without memory), mask_logits [1,1,256,256], object_score,
      from_points [1] (1 on the prompted frame: binarized mask)
      -> maskmem_feat [1,64,64,64], maskmem_pos [1,64,64,64]
  sam2_memory_attention.onnx
      vision_feat, vision_pos, memory_feats [M,64,64,64], memory_pos [M,64,64,64],
      memory_tpos [M] int64 (index into the 7 temporal encodings: 6 for the prompted
      frame, 6 - k for the frame k steps back), obj_ptrs [P,256], obj_ptr_tpos [P]
      (signed frame distance), obj_ptr_tpos_max [1] (min(frames, 16) - 1)
      -> pix_feat [1,256,64,64]

Usage: python3 tools/sam2/export_onnx.py OUTPUT_DIR [--checkpoint sam2.1_hiera_tiny.pt]
"""

import argparse
import hashlib
import os
import sys
import urllib.request

import numpy as np
import torch
import torch.nn.functional as F

SAM2_COMMIT = "2b90b9f5ceec907a1c18123530e92e794ad901a4"
CKPT_URL = "https://dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_tiny.pt"
CKPT_SHA256 = "7402e0d864fa82708a20fbd15bc84245c2f26dff0eb43a4b5b93452deb34be69"
CONFIG = "configs/sam2.1/sam2.1_hiera_t.yaml"
OPSET = 17


def rope_tables(dim, end_x, end_y, theta=10000.0):
    """cos/sin of SAM 2's axial rotary encoding, [L, dim/2] each (instead of complex numbers)."""
    from sam2.modeling.position_encoding import init_t_xy

    freqs = 1.0 / (theta ** (torch.arange(0, dim, 4)[: (dim // 4)].float() / dim))
    t_x, t_y = init_t_xy(end_x, end_y)
    angles = torch.cat([torch.outer(t_x, freqs), torch.outer(t_y, freqs)], dim=-1)
    return torch.cos(angles), torch.sin(angles)


def rotate(x, cos, sin):
    """Rotary encoding on [B, heads, L, D] with tables [L', D/2]; L is a multiple of L'."""
    pairs = x.float().reshape(*x.shape[:-1], -1, 2)
    reps = x.shape[-2] // cos.shape[0]
    cos = cos.repeat(reps, 1)
    sin = sin.repeat(reps, 1)
    a, b = pairs[..., 0], pairs[..., 1]
    return torch.stack([a * cos - b * sin, a * sin + b * cos], dim=-1).flatten(3).type_as(x)


def rope_forward(self, q, k, v, num_k_exclude_rope=0):
    """RoPEAttention.forward with real arithmetic (ONNX has no complex numbers)."""
    q = self._separate_heads(self.q_proj(q), self.num_heads)
    k = self._separate_heads(self.k_proj(k), self.num_heads)
    v = self._separate_heads(self.v_proj(v), self.num_heads)
    cos, sin = self.rope_cos, self.rope_sin
    q = rotate(q, cos, sin)
    num_k_rope = k.shape[-2] - num_k_exclude_rope
    k = torch.cat([rotate(k[:, :, :num_k_rope], cos, sin), k[:, :, num_k_rope:]], dim=2)
    out = F.scaled_dot_product_attention(q, k, v)
    return self.out_proj(self._recombine_heads(out))


def load_model(checkpoint):
    from sam2.build_sam import build_sam2_video_predictor
    from sam2.modeling.sam.transformer import RoPEAttention

    data = open(checkpoint, "rb").read() if checkpoint else urllib.request.urlopen(CKPT_URL).read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != CKPT_SHA256:
        sys.exit(f"checkpoint SHA-256 mismatch: {digest}")
    if not checkpoint:
        checkpoint = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sam2.1_hiera_tiny.pt")
        open(checkpoint, "wb").write(data)
    model = build_sam2_video_predictor(CONFIG, checkpoint, device="cpu").eval()
    for m in model.modules():
        if isinstance(m, RoPEAttention):
            head_dim = m.internal_dim // m.num_heads
            m.rope_cos, m.rope_sin = rope_tables(head_dim, 64, 64)
    RoPEAttention.forward = rope_forward
    return model


class Encoder(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, image):
        _, feats, pos, _ = self.m._prepare_backbone_features(self.m.forward_image(image))
        hr0 = feats[0].permute(1, 2, 0).reshape(1, 32, 256, 256)
        hr1 = feats[1].permute(1, 2, 0).reshape(1, 64, 128, 128)
        vf = feats[2].permute(1, 2, 0).reshape(1, 256, 64, 64)
        vp = pos[2].permute(1, 2, 0).reshape(1, 256, 64, 64)
        nomem = vf + self.m.no_mem_embed.reshape(1, 256, 1, 1)
        return vf, nomem, vp, hr0, hr1


class Heads(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, pix_feat, high_res_0, high_res_1, point_coords, point_labels, multimask):
        points = {"point_coords": point_coords, "point_labels": point_labels}
        multi = self.m._forward_sam_heads(pix_feat, points, None, [high_res_0, high_res_1], multimask_output=True)
        single = self.m._forward_sam_heads(pix_feat, points, None, [high_res_0, high_res_1], multimask_output=False)
        use_multi = multimask.reshape(1, 1) > 0.5
        mask = torch.where(use_multi.reshape(1, 1, 1, 1), multi[3], single[3])
        obj_ptr = torch.where(use_multi, multi[5], single[5])
        return mask, obj_ptr, single[6]


class MemoryEncoder(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, pix_feat, mask_logits, object_score, from_points):
        high = F.interpolate(mask_logits, size=(1024, 1024), mode="bilinear", align_corners=False)
        mask = torch.where(from_points.reshape(1, 1, 1, 1) > 0.5, (high > 0).float(), torch.sigmoid(high))
        mask = mask * self.m.sigmoid_scale_for_mem_enc + self.m.sigmoid_bias_for_mem_enc
        out = self.m.memory_encoder(pix_feat, mask, skip_mask_sigmoid=True)
        feat = out["vision_features"]
        absent = 1.0 - (object_score > 0).float()
        feat = feat + absent.reshape(1, 1, 1, 1) * self.m.no_obj_embed_spatial.reshape(1, -1, 1, 1)
        return feat, out["vision_pos_enc"][-1]


class MemoryAttention(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, vision_feat, vision_pos, memory_feats, memory_pos, memory_tpos, obj_ptrs, obj_ptr_tpos,
                obj_ptr_tpos_max):
        from sam2.modeling.sam2_utils import get_1d_sine_pe

        mem_dim = self.m.mem_dim
        curr = vision_feat.flatten(2).permute(2, 0, 1)
        curr_pos = vision_pos.flatten(2).permute(2, 0, 1)
        feats = memory_feats.flatten(2).permute(0, 2, 1).reshape(-1, 1, mem_dim)
        tpos = self.m.maskmem_tpos_enc[memory_tpos].reshape(-1, 1, mem_dim)
        pos = (memory_pos.flatten(2).permute(0, 2, 1) + tpos).reshape(-1, 1, mem_dim)
        # Object pointers: each 256-d pointer becomes 4 tokens of 64, with a temporal encoding.
        ptr_pe = self.m.obj_ptr_tpos_proj(get_1d_sine_pe(obj_ptr_tpos / obj_ptr_tpos_max, dim=self.m.hidden_dim))
        split = self.m.hidden_dim // mem_dim
        ptr_tokens = obj_ptrs.reshape(-1, 1, mem_dim)
        ptr_pos = ptr_pe.repeat_interleave(split, dim=0).unsqueeze(1)
        out = self.m.memory_attention(curr=[curr], curr_pos=[curr_pos], memory=torch.cat([feats, ptr_tokens]),
                                      memory_pos=torch.cat([pos, ptr_pos]),
                                      num_obj_ptr_tokens=obj_ptrs.shape[0] * split)
        return out.permute(1, 2, 0).reshape(1, self.m.hidden_dim, 64, 64)


def export(module, args, path, inputs, outputs, dynamic=None):
    # Separate copies for the reference, the trace and ONNX Runtime: SAM 2 modifies some inputs in place.
    copy = lambda: tuple(a.clone() for a in args)
    feed = {n: a.numpy() for n, a in zip(inputs, copy())}
    with torch.no_grad():
        expected = module(*copy())
        torch.onnx.export(module, copy(), path, opset_version=OPSET, dynamo=False, input_names=inputs,
                          output_names=outputs, dynamic_axes=dynamic or {})
    import onnxruntime as ort

    session = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    used = {i.name for i in session.get_inputs()}
    got = session.run(None, {n: a for n, a in feed.items() if n in used})
    expected = expected if isinstance(expected, tuple) else (expected,)
    diff = max(float(np.abs(g - e.numpy()).max()) for g, e in zip(got, expected))
    print(f"{os.path.basename(path)}: max difference to PyTorch {diff:.2e}")
    return diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir")
    parser.add_argument("--checkpoint", help="sam2.1_hiera_tiny.pt already downloaded")
    args = parser.parse_args()
    os.makedirs(args.output_dir, exist_ok=True)
    out = lambda name: os.path.join(args.output_dir, name)

    # Each graph from a freshly loaded model: tracing leaves SAM 2's positional-encoding caches
    # holding traced values, which would skew the PyTorch reference of the next export.
    fresh = lambda: load_model(args.checkpoint)
    torch.manual_seed(0)
    image = torch.randn(1, 3, 1024, 1024)
    coords = torch.tensor([[[500.0, 400.0], [620.0, 380.0]]])
    labels = torch.tensor([[1, 0]], dtype=torch.int32)
    with torch.no_grad():
        model = fresh()
        vf, nomem, vp, hr0, hr1 = Encoder(model)(image)
        mask, ptr, score = Heads(model)(nomem, hr0, hr1, coords, labels, torch.ones(1))
        mem_feat, mem_pos = MemoryEncoder(model)(vf, mask, score, torch.ones(1))
    m, p = 3, 2
    diffs = [
        export(Encoder(fresh()), (image,), out("sam2_encoder.onnx"), ["image"],
               ["vision_feat", "vision_feat_nomem", "vision_pos", "high_res_0", "high_res_1"]),
        export(Heads(fresh()), (nomem, hr0, hr1, coords, labels, torch.ones(1)), out("sam2_heads.onnx"),
               ["pix_feat", "high_res_0", "high_res_1", "point_coords", "point_labels", "multimask"],
               ["mask_logits", "obj_ptr", "object_score"],
               {"point_coords": {1: "points"}, "point_labels": {1: "points"}}),
        export(MemoryEncoder(fresh()), (vf, mask, score, torch.ones(1)), out("sam2_memory_encoder.onnx"),
               ["pix_feat", "mask_logits", "object_score", "from_points"], ["maskmem_feat", "maskmem_pos"]),
        export(MemoryAttention(fresh()),
               (vf, vp, mem_feat.repeat(m, 1, 1, 1), mem_pos.repeat(m, 1, 1, 1), torch.tensor([6, 5, 4]),
                ptr.repeat(p, 1), torch.tensor([1.0, 2.0]), torch.tensor([15.0])),
               out("sam2_memory_attention.onnx"),
               ["vision_feat", "vision_pos", "memory_feats", "memory_pos", "memory_tpos", "obj_ptrs", "obj_ptr_tpos",
                "obj_ptr_tpos_max"], ["pix_feat"],
               {"memory_feats": {0: "memories"}, "memory_pos": {0: "memories"}, "memory_tpos": {0: "memories"},
                "obj_ptrs": {0: "pointers"}, "obj_ptr_tpos": {0: "pointers"}}),
    ]
    if max(diffs) > 1e-2:
        sys.exit("ONNX output does not match PyTorch")
    for name in sorted(os.listdir(args.output_dir)):
        if name.endswith(".onnx"):
            print("sha256", name, hashlib.sha256(open(out(name), "rb").read()).hexdigest())


if __name__ == "__main__":
    main()
