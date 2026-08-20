#!/usr/bin/env python3
"""
ComfyUI graph builder.

Only one path remains: Ideogram 4, driven by a structured JSON caption.

Ideogram 4 is trained exclusively on structured JSON captions, and its bounding
boxes are a trained spatial interface rather than decorative text. Since the
architect already knows the exact rectangle of every room, water body, ship and
prop, the layout is handed over as coordinates.

The ControlNet path this replaced had an unavoidable trade-off: held tightly it
forced the render into a flat recoloured height map, held loosely the layout
drifted. Bounding boxes give an exact layout with no constraint on the painting.
"""
import random

# Ideogram's own sampling presets, taken from the reference workflow.
PRESETS = {
    "Quality": {"steps": 48, "mu": 0.0, "std": 1.5},
    "Default": {"steps": 20, "mu": 0.0, "std": 1.75},
    "Turbo": {"steps": 12, "mu": 0.0, "std": 2.0},
}


def _seed(seed):
    if seed is None or seed < 0:
        return random.randint(1, 2 ** 63 - 1)
    return int(seed)


def _snap(value, fallback):
    """Both dimensions must be a multiple of 16 and at least 256."""
    value = int(value or fallback)
    return max(256, ((value + 15) // 16) * 16)


def build_ideogram4(cfg, caption_json, seed=None, width=None, height=None):
    """Ideogram 4.0 text-to-image, layout carried by the caption's bounding boxes.

    Two UNets - a conditional and an unconditional one - are combined by
    DualModelGuider to give true classifier-free guidance, and Ideogram runs a
    lower CFG over the last stretch of the schedule.
    """
    c = cfg.get("ideogram", {}) or {}
    seed = _seed(seed if seed is not None else c.get("seed", -1))
    preset = PRESETS.get(c.get("preset", "Quality"), PRESETS["Quality"])
    steps = int(c.get("steps") or preset["steps"])
    width = _snap(width, 1536)
    height = _snap(height, 1152)

    return {
        "unet": {
            "inputs": {"unet_name": c.get("unet", "ideogram4_fp8_scaled.safetensors"),
                       "weight_dtype": "default"},
            "class_type": "UNETLoader",
        },
        "unet_uncond": {
            "inputs": {"unet_name": c.get("unet_unconditional",
                                          "ideogram4_unconditional_fp8_scaled.safetensors"),
                       "weight_dtype": "default"},
            "class_type": "UNETLoader",
        },
        "clip": {
            "inputs": {"clip_name": c.get("clip", "qwen3vl_8b_fp8_scaled.safetensors"),
                       "type": "ideogram4", "device": "default"},
            "class_type": "CLIPLoader",
        },
        "vae": {
            "inputs": {"vae_name": c.get("vae", "flux2-vae.safetensors")},
            "class_type": "VAELoader",
        },
        "pos": {
            "inputs": {"text": caption_json, "clip": ["clip", 0]},
            "class_type": "CLIPTextEncode",
        },
        "neg": {
            "inputs": {"conditioning": ["pos", 0]},
            "class_type": "ConditioningZeroOut",
        },
        "cfg_override": {
            "inputs": {"cfg": float(c.get("cfg_late", 3.0)),
                       "start_percent": float(c.get("cfg_late_start", 0.7)),
                       "end_percent": 1.0,
                       "model": ["unet", 0]},
            "class_type": "CFGOverride",
        },
        "guider": {
            "inputs": {"cfg": float(c.get("cfg", 7.0)),
                       "model": ["cfg_override", 0],
                       "positive": ["pos", 0],
                       "model_negative": ["unet_uncond", 0],
                       "negative": ["neg", 0]},
            "class_type": "DualModelGuider",
        },
        "sigmas": {
            "inputs": {"steps": steps, "width": width, "height": height,
                       "mu": float(c.get("mu", preset["mu"])),
                       "std": float(c.get("std", preset["std"]))},
            "class_type": "Ideogram4Scheduler",
        },
        "sampler_sel": {
            "inputs": {"sampler_name": c.get("sampler", "euler")},
            "class_type": "KSamplerSelect",
        },
        "noise": {"inputs": {"noise_seed": seed}, "class_type": "RandomNoise"},
        "latent": {
            "inputs": {"width": width, "height": height, "batch_size": 1},
            "class_type": "EmptyFlux2LatentImage",
        },
        "sampler": {
            "inputs": {"noise": ["noise", 0], "guider": ["guider", 0],
                       "sampler": ["sampler_sel", 0], "sigmas": ["sigmas", 0],
                       "latent_image": ["latent", 0]},
            "class_type": "SamplerCustomAdvanced",
        },
        "decode": {
            "inputs": {"samples": ["sampler", 0], "vae": ["vae", 0]},
            "class_type": "VAEDecode",
        },
        "save": {
            "inputs": {"filename_prefix": "battlemap", "images": ["decode", 0]},
            "class_type": "SaveImage",
        },
    }
