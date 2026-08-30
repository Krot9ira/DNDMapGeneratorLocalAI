# Development

## Editable install

We recommend installing into an isolated environment — the dependencies include several GB of CUDA-built wheels.

```bash
python -m venv .venv && source .venv/bin/activate
```

For development, install the package in editable mode so changes to the source
tree are picked up without reinstalling:

```bash
pip install -e .
```

or with [`uv`](https://docs.astral.sh/uv/):

```bash
uv venv && source .venv/bin/activate
```

```bash
uv pip install -e .
```

## Pre-commit hooks

This repo uses [pre-commit](https://pre-commit.com/) to run lint, format, and
type checks (`ruff`, `mypy`, etc.) before each commit.

Install once per clone:

```bash
pip install pre-commit
pre-commit install
```

`pre-commit install` registers a git hook in `.git/hooks/pre-commit`, so it
requires the directory to be a git repo. The hooks now run automatically on
`git commit` against staged files.

To run the hooks manually against every file in the repo (useful right after
the first install, or in CI):

```bash
pre-commit run --all-files
```

The first run downloads each hook's environment (ruff, mypy, etc.) into
`~/.cache/pre-commit/` and may take a minute. Subsequent runs are fast.

To bump pinned hook versions in `.pre-commit-config.yaml`:

```bash
pre-commit autoupdate
```
