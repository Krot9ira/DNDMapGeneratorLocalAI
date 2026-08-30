# Model Architecture

```
prompt ─► Qwen3-VL-8B-Instruct (extract hidden states from layers (0,3,…,33,35) → concat)
            │   
            ▼
    ┌─────────────────────────────────────────────────┐
    │    Ideogram4Transformer                         │
    │  • 34 × Ideogram4TransformerBlock               │
    │      – Ideogram4Attention (QK-RMSNorm, MRoPE)   │
    │      – Ideogram4MLP (SwiGLU)                    │
    │      – adaln scale/gate from t-embedding        │
    │  • Ideogram4FinalLayer                          │
    └─────────────────────────────────────────────────┘
            │
            │ velocity prediction
            │
            ▼
    Euler flow-matching sampler with asymmetric CFG
            │
            │ denoised image latents
            │
            ▼
       VAE decode
            │
            ▼
        PIL.Image
```

The transformer is a single-stream DiT: text tokens (Qwen3-VL hidden states from
the activation layers) and image latent tokens are concatenated into one
sequence, modulated per-block by an AdaLN computed from the flow-matching
timestep embedding. Attention uses QK-RMSNorm and 3D MRoPE so that text and
image tokens share a unified positional space.

Model spec:

| field             | value         |
|-------------------|---------------|
| `emb_dim`         | 4608          |
| `num_layers`      | 34            |
| `num_heads`       | 18            |
| `intermediate`    | 12288         |
| `adanln_dim`      | 512           |
| `rope_theta`      | 5_000_000     |
| `mrope_section`   | (24, 20, 20)  |
| latent channels   | 32 × 2² = 128 |
| max text tokens   | 2048          |
| sampler           | Euler flow-matching, logit-normal schedule, asymmetric CFG |
