# Pipeline: How All the Components Work Together

This document explains the end-to-end Ideogram 4 inference pipeline
conceptually. For the architecture spec and code pointers, see
[model_architecture.md](model_architecture.md).

## Overview

Ideogram 4 is a **flow-matching text-to-image model** built on a
**single-stream DiT** (Diffusion Transformer). The pipeline has four main
components:

```
 ┌─────────────┐   ┌──────────────────────┐   ┌──────────────┐   ┌───────────┐
 │  Qwen3-VL   │   │  Ideogram4           │   │  KL VAE      │   │           │
 │  Text       ├──►│  Transformer (DiT)   ├──►│  VAE         ├──►│  Image    │
 │  Encoder    │   │  + Euler Sampler     │   │  Decoder     │   │           │
 └─────────────┘   └──────────────────────┘   └──────────────┘   └───────────┘
     frozen              trainable                 frozen
```

## 1. Text Encoder — Qwen3-VL-8B-Instruct

The text encoder is a frozen [Qwen3-VL-8B-Instruct](https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct)
vision-language model, used in text-only mode (no vision inputs).

**What it does:**
- Tokenizes the prompt using the Qwen3 chat template.
- Runs a forward pass through the 36-layer transformer.
- **Extracts hidden states** from 13 specific layers: 0, 3, 6, 9, 12, 15, 18, 21,
  24, 27, 30, 33, 35.
- Concatenates these hidden states along the feature dimension, producing a
  multi-scale text representation.

**Why multi-layer extraction?** Different layers capture different levels of
abstraction — early layers encode surface-level token information, while later
layers encode deeper semantic meaning. Concatenating them gives the DiT access
to the full spectrum.

**Output:** A tensor of shape `(batch, num_text_tokens, hidden_dim * 13)`.

## 2. DiT Backbone — Ideogram4Transformer

The core generative model is a 34-layer single-stream Diffusion Transformer.

### Sequence layout

Text tokens and image latent tokens are concatenated into one sequence and
processed through the same self-attention layers.

```
Sequence layout (per sample):

  ┌───────────────────┬────────────────────────┐
  │  text tokens      │  image latent tokens   │
  │  (up to 2048)     │  (grid_h × grid_w)     │
  └───────────────────┴────────────────────────┘
           ▲                    ▲
     Qwen3-VL features    noisy latents z_t
```

### Key components per block

- **Self-attention** with QK-RMSNorm and 3D Multimodal RoPE (MRoPE). The
  positional encoding is 3-dimensional: for text tokens it uses a 1D position
  broadcast to 3 axes; for image tokens it uses (temporal, height, width)
  coordinates. This lets text and image tokens coexist in a unified positional
  space.
- **SwiGLU MLP** — the feed-forward layer uses a gated linear unit with SiLU
  activation.
- **Adaptive Layer Norm (AdaLN)** — the timestep `t` is embedded as a scalar
  and generates per-block scale and gate parameters. This conditions every layer
  on the current noise level.

### Flow matching

The model is trained with a **flow-matching** objective. Instead of predicting
noise (as in DDPM), the model predicts a **velocity field** `v(z_t, t)` that
defines the ODE:

```
dz/dt = v(z_t, t)
```

At inference time, we start from pure Gaussian noise `z_1` and integrate
backward to `z_0` (the clean image) using the Euler method:

```
z_{t-dt} = z_t + v(z_t, t) * dt
```

### Noise schedule

The timestep distribution follows a **logit-normal schedule** parameterized by
`(mu, sigma)`. The mean `mu` controls how much time the sampler spends at
different noise levels — higher `mu` shifts more steps toward higher noise
(important for high-resolution images). The schedule auto-adjusts for
resolution:

```
mu_adjusted = mu_base + 0.5 * log(num_pixels / base_pixels)
```

where `base_pixels = 512 * 512`.

## 3. Classifier-Free Guidance (CFG)

At each sampling step, two forward passes are run through the DiT:

1. **Conditional (positive):** full text features + noisy image latents.
2. **Unconditional (negative):** zeroed text features + noisy image latents
   (image-only tokens, asymmetric CFG).

The guided velocity is a weighted combination:

```
v_guided = gw * v_conditional + (1 - gw) * v_unconditional
```

where `gw` is the per-step guidance weight. With
`gw > 1`, the model amplifies the text-conditional signal and suppresses the
unconditional prediction, producing images that follow the prompt more
faithfully.

**Asymmetric CFG:** The unconditional branch only processes image tokens (no
text padding), making it computationally cheaper than a full-sequence negative
pass.

**Per-step schedules:** The guidance weight can vary across steps. The
`V4_QUALITY_48` preset, for example, uses `gw=7` for the first 45 steps and
`gw=3` for the final 3 "polish" steps near `t=0`.


## 4. VAE Decoder — KL Autoencoder

The denoised latent `z_0` is decoded to pixel space using a frozen KL
autoencoder.

**What it does:**
- **Unpatching:** The DiT works with 2×2 patches of latent pixels. The decoder
  input is reshaped from `(batch, grid_h * grid_w, channels * 4)` to
  `(batch, channels, grid_h * 2, grid_w * 2)`.
- **Denormalization:** Per-channel shift and scale are applied to undo the
  latent normalization used during training.
- **Decoding:** The VAE decoder maps latents to RGB pixels.
- **Clipping:** Output is clamped to [-1, 1] and rescaled to [0, 255] uint8.

**Compression factor:** The autoencoder provides 8× spatial compression on each
axis, and the 2×2 patching in the DiT adds another 2×. So a 1024×1024 image
is represented as a 64×64 grid of latent tokens, each with 128 channels
(32 base channels × 2² patch).

## Putting it all together

```python
# Pseudocode for one generation call:

# 1. Encode text
text_features = qwen3_vl.encode(prompt)  # (B, L_text, D)

# 2. Initialize noise
z = torch.randn(B, grid_h * grid_w, 128)  # pure noise at t=1

# 3. Euler integration from t=1 to t=0
for step in reversed(range(num_steps)):
    t = schedule(step)
    s = schedule(step - 1)

    # Conditional pass (text + image)
    v_cond = dit(text_features, z, t)

    # Unconditional pass (image only, zeroed text)
    v_uncond = dit(zeros, z, t)

    # CFG combination
    v = gw[step] * v_cond + (1 - gw[step]) * v_uncond

    # Euler step
    z = z + v * (s - t)

# 4. Decode to pixels
image = vae.decode(z)
```
