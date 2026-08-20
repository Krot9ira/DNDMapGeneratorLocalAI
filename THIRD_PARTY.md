# Third-party components

This project is MIT licensed (see [`LICENSE`](LICENSE)). It also uses the work of other
people. Everything listed here is under a permissive licence compatible with MIT, and each
requires that its own copyright notice travel with any copy — so this file ships inside the
release folder as well as living in the repository.

## Bundled in this repository and compiled into the executable

| Component | Version | Author | Licence | Where |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93 (WIP) | Omar Cornut | MIT | `app/vendor/imgui/` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | Niels Lohmann | MIT | `app/vendor/nlohmann/` |
| [stb_image](https://github.com/nothings/stb) | 2.30 | Sean Barrett | MIT / public domain | `app/vendor/stb/` |
| [stb_image_write](https://github.com/nothings/stb) | 1.16 | Sean Barrett | MIT / public domain | `app/vendor/stb/` |

Dear ImGui keeps its own licence text at `app/vendor/imgui/LICENSE.txt`. The stb headers
carry their dual MIT / public-domain notice in the file footer, and `json.hpp` carries the
MIT notice in its header comment. Those notices are left untouched.

The vendored copy of Dear ImGui is trimmed to the files this build compiles — the core
sources, the Win32 and DirectX 11 backends, and the licence. Nothing was modified.

## Required at runtime, not bundled

Installed and updated by the user; this project only talks to them over HTTP.

| Component | Purpose | Licence |
|---|---|---|
| [ComfyUI](https://github.com/comfyanonymous/ComfyUI) | runs the image model | GPL-3.0 |
| Ideogram 4.0 weights | paints the map | the model's own licence, as obtained by the user |
| [Ollama](https://github.com/ollama/ollama) | plans the scene (optional) | MIT |
| [Pillow](https://github.com/python-pillow/Pillow) | draws the plan preview in the Python tools | MIT-CPL (HPND) |
| Python 3.10+ | runs the command line tools | PSF |

No model weights, no ComfyUI code and no Ollama code are copied into this repository or into
the release folder. The app is a client: it sends a prompt and reads back an image.

## Fonts

The app loads **Segoe UI** from the Windows system font directory at startup and falls back
to the built-in Dear ImGui font when it is missing. No font file is redistributed.

## Using this project

If you build on this — code, styles, the caption format, the architect — keep the copyright
notice from [`LICENSE`](LICENSE) and credit **Krot9ira**
(<https://github.com/Krot9ira>). That is the whole of the obligation; commercial use, forks
and closed-source products are all fine.
