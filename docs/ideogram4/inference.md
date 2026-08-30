# Inference Reference

Detailed parameters, sampler presets, supported resolutions, and optimization
tips for Ideogram 4 inference.

## Sampler Presets

Named presets bundle a step count, per-step CFG schedule, schedule mean (`mu`),
and schedule standard deviation (`std`) into a single flag:

```bash
python run_inference.py \
  --prompt "a cat wearing a tiny top hat" \
  --sampler-preset V4_QUALITY_48 \
  --output out.png
```

| Preset | Steps | CFG schedule | `mu` | `std` |
| :----- | :---: | :----------- | :--: | :---: |
| `V4_QUALITY_48` | 48 | 45 steps @ gw=7, then 3 polish steps @ gw=3 | 0.0 | 1.5 |
| `V4_DEFAULT_20` | 20 | 18 steps @ gw=7, then 2 polish steps @ gw=3 | 0.0 | 1.75 |
| `V4_TURBO_12` | 12 | 11 steps @ gw=7, then 1 polish step @ gw=3 | 0.5 | 1.75 |

`V4_QUALITY_48` is the default. Fewer steps trade quality for speed. The full
registry lives in
[`ideogram4.sampler_configs.PRESETS`](../src/ideogram4/sampler_configs.py); add a
new entry there to define your own.

## Key Parameters

These are the keyword arguments accepted by `Ideogram4Pipeline.__call__`. The
defaults below apply when you call `pipe(...)` directly; `run_inference.py`
overrides `num_steps`, `guidance_schedule`, `mu`, and `std` from the chosen
sampler preset (see above).

| Parameter | Default | Notes |
| :-------- | :-----: | :---- |
| `height` / `width` | 1024 | Must be multiples of 16. Supported range: 256–2048. Aspect ratios up to 6:1 or 1:6. |
| `num_steps` | 48 | More steps = higher quality. The `V4_QUALITY_48` preset (48 steps) is a good speed/quality trade-off. |
| `guidance_scale` | 7.0 | Constant guidance weight used when no `guidance_schedule` is given. Higher = more prompt adherence, lower = more diversity. |
| `guidance_schedule` | `None` | Optional per-step guidance weights (loop-index order: index 0 is the final step). Overrides `guidance_scale`. |
| `mu` | 0.5 | Logit-normal schedule mean. Auto-adjusted for resolution. |
| `std` | 1.0 | Logit-normal schedule standard deviation. |
| `seed` | `None` | Set for reproducible results. |

## Supported Resolutions

Ideogram 4 natively supports any resolution where both height and width are
multiples of 16, within the range 256–2048 (aspect ratios up to 6:1 or 1:6).

| Use case | Resolution | Aspect ratio |
| :------- | :--------: | :----------: |
| Square | 1024 × 1024 | 1:1 |
| Landscape | 1536 × 1024 | 3:2 |
| Portrait | 1024 × 1536 | 2:3 |
| Widescreen | 1920 × 1088 | ~16:9 |
| Ultrawide | 2048 × 768 | ~21:9 |
| Phone wallpaper | 1024 × 1792 | ~9:16 |
| Social banner | 1600 × 400 | 4:1 |

Resolution buckets use 16-pixel increments, giving fine-grained control over
output dimensions.

