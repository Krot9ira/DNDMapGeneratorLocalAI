D&D AI BATTLE MAP GENERATOR
===========================

Start here. Everything you need is already in this folder.


WHAT THIS IS
------------
You describe a place in one sentence - "a harbour with one big ship", "a flooded
crypt", "a tavern". The program works out the floor plan, draws it, and paints a
finished top-down battle map you can load straight into Roll20 or Foundry.


WHAT HAS TO BE RUNNING FIRST
----------------------------
The program does not paint anything itself. It drives two services on your own
machine. Nothing is uploaded anywhere and no account is needed.

  1. ComfyUI              - paints the picture.  http://127.0.0.1:8188
  2. Ollama (optional)    - writes the description the map is built around.
                            http://127.0.0.1:11434

ComfyUI needs these four model files, about 27.5 GB together:

  models/diffusion_models/ideogram4_fp8_scaled.safetensors                 8.6 GB
  models/diffusion_models/ideogram4_unconditional_fp8_scaled.safetensors   8.6 GB
  models/text_encoders/qwen3vl_8b_fp8_scaled.safetensors                   9.9 GB
  models/vae/flux2-vae.safetensors                                         0.3 GB

BOTH ideogram4 files are required. With only the first one the picture comes out
pale and vague - this is the most common installation mistake.

Ollama is optional. Without it, "Blueprint only" builds a floor plan instantly
and you can shape everything by hand in the Editor. An AI agent can also replace
Ollama entirely - see AGENTS.md.


WHAT YOUR COMPUTER NEEDS
------------------------
                    minimum          comfortable
  Graphics card     NVIDIA 8 GB      NVIDIA 12 GB or more
  System memory     32 GB            64 GB
  Free disk         35 GB            50 GB
  Windows           10 64-bit        11 64-bit

A render takes minutes, not seconds. The first one after starting ComfyUI is
slower because 27 GB of weights come off the disk.


HOW TO START
------------
  1. Start ComfyUI and wait until its page opens in a browser.
  2. Run DndBattlemapGenerator.exe
  3. Settings tab -> "Test both connections". The ComfyUI line must turn green.
  4. Create tab -> describe the scene, pick a look, set the size.
  5. Press MAKE MY BATTLE MAP and wait a few minutes.

The finished map appears in the window and in the output folder.


WHAT IS IN THIS FOLDER
----------------------
  DndBattlemapGenerator.exe   the program
  config.json                 service addresses and model filenames
  styles/                     the style library, editable inside the program
  presets/                    saved maps
  data/                       asset database and generated thumbnails
  output/                     your results land here (PNGs and .dungeondraft_map files)
  tools/                      command line, Dungeondraft assembler and AI agent API
  docs/                       the user manual and map format specifications
  AGENTS.md                   instructions for AI agents
  LICENSE, THIRD_PARTY.md     licences


THE FULL MANUAL
---------------
docs/Manual.pdf is written for somebody who has never opened ComfyUI. Every tab,
every button, and what to do when something goes wrong.


COMMAND LINE & DUNGEONDRAFT
---------------------------
Needs Python 3.10+ and Pillow (pip install pillow requests):

  python tools/pipeline.py styles
  python tools/pipeline.py auto "a harbour with one large moored ship" --style city_harbour
  python tools/pipeline.py dungeondraft output/my_map/map.json
  python tools/dungeondraft_indexer.py stats

For AI agents, see AGENTS.md.
