# Safety

Ideogram is committed to the **responsible development and deployment of
open-source models**. Image generation models can be powerful general-purpose
tools, and we believe the best way to release them openly is to pair the
weights with concrete safeguards throughout the pipeline, from development
through use. These safeguards include, but are not limited to, training data,
model behavior, and runtime filtering.

This document describes the mitigations applied to Ideogram 4 and the
expectations we have for anyone redistributing or serving the model.

## Pre-training mitigations

Before any training begins, we run our image dataset through filters that are
designed to remove content across multiple NSFW categories. The
filtered dataset is then used as the input distribution for pre-training.
This measure is designed to prevent the base model from reproducing the
filtered categories of content.

## Post-training mitigations

After pre-training, we apply additional post-training procedures that are
designed to further reduce the probability of the model generating NSFW
content, including for prompts that explicitly request it.

## Inference-time filters

The inference code in this repository ships with two runtime safety filters,
both powered by [Hive](https://thehive.ai/):

- **Prompt moderation** (`--hive-text-key` /
  `HIVE_TEXT_MODERATION_KEY`) — every prompt is screened by Hive's Text
  Moderation API before it reaches the model. Prompts flagged for unsafe
  categories are rejected and no image is generated.
- **Output moderation** (`--hive-visual-key` /
  `HIVE_VISUAL_MODERATION_KEY`) — every generated image is screened by
  Hive's Visual Content Moderation API. Images flagged for unsafe categories
  are rejected before being returned to the caller.

See `run_inference.py` for the exact integration points.

> **We expect anyone serving or redistributing the model to use these
> inference-time NSFW filters (or equivalent or stronger filters).**
> Running the model without prompt and output moderation configured disables
> runtime safety screening entirely and is not a supported deployment
> configuration. If you redistribute or host this model, you are responsible
> for keeping equivalent (or stronger) filters in place.

## Reporting violations and ongoing monitoring

We may monitor Ideogram 4 public deployments and outputs from time to time,
but we cannot catch everything on our own. If you observe violations of our
usage policies — content the model should not have produced, deployments
that have removed, overridden, or failed to deploy the required safety
mitigations described above, or other misuse — please email us at
**safety@ideogram.ai** with as much detail as you can share (prompts, image
hashes, URLs, screenshots, deployment context). Reports help us improve
both the model and the surrounding guardrails.
