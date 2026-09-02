# Contributing

Bug reports, scenes that came out wrong, styles, and code are all welcome. This
page is the short version of how the project is put together and what has to be
true before a change lands.

---

## Building it

You need **CMake 3.20+**, **Visual Studio 2022** (or the Build Tools) with the
Desktop C++ workload, and **Python 3.10+**.

```bash
git clone https://github.com/Krot9ira/DNDMapGeneratorLocalAI.git
cd DNDMapGeneratorLocalAI
pip install -r requirements.txt
cmake -B build -S . -A x64
cmake --build build --config Release
```

That produces `bin/Release/DndBattlemapGenerator.exe` and assembles the
shippable folder in `dist/DndBattlemapGenerator` as a post-build step.
`.\build.ps1` does the same and adds the checks that are easy to forget;
`.\build.ps1 -Zip` packs a release beside it.

Neither ComfyUI nor Ollama is needed to build, to run the checks, to draw a
layout by hand, or to export one to Dungeondraft. They are needed to paint a
map and to have a scene planned for you.

---

## Before you open a pull request

Run these. CI runs the same five, so a failure here is a failure there.

```bash
python tools/check_scenes.py          # all 13 scenes plan and validate
python tools/check_layouts.py         # every layout, across sizes and seeds
python tools/check_captions.py        # no style says anything side-on
python tools/check_dungeondraft.py    # every scene assembles as .dungeondraft_map
python tools/check_caption_parity.py  # the app and the tools agree (needs the build)
```

A check that reports a number is more use than one that reports a colour, so
they print what they found. Read the numbers, not just the exit code.

---

## The two things this project will bite you for

**1. There are two ports of the same logic, and they must move together.**
`tools/architect.py` and `app/include/map_architect.h` are one geometry engine
written twice; so are `tools/ideogram_prompt.py` and
`app/include/ideogram_caption.h`. Change one and you must change the other, or
a map made in the app comes out different from the same map made by an agent.
`check_caption_parity.py` catches the caption half and is not optional. The
architect half has no automatic check yet — if you touch it, say so in the pull
request and describe what you compared.

**2. Every map is strictly top-down, and drift off that is a critical defect,
not a nitpick.** A wall drawn as a face covers squares a token has to stand on.
`check_captions.py` lints for side-on wording; if you add wording, keep it out
of its way rather than adding an exception.

---

## House style

- **Comments explain why, not what.** If a line encodes a decision, say what the
  alternative was and why it lost. A comment restating the code is noise.
- **A named constant with a comment beats a magic number.**
- **Every tool runs alone from the command line** and prints something a human
  can read.
- **Match the surrounding code** — its naming, its comment density, its idiom.
- **Commit messages end with the prose.** No `Co-Authored-By`, no AI attribution
  trailers, no generated-by footers.

Write the commit message for whoever has to understand the change in a year:
what was wrong, why it was wrong, and what it does now.

---

## What never gets committed

`.gitignore` has the full list; these are the ones people trip over:

- `data/` — the Dungeondraft asset index and its thumbnails. Hundreds of
  megabytes, built on your machine from the packs you own, and a description of
  your installation rather than of the project.
- `output/`, `dist/`, `build/`, `bin/` — results and build products.
- `config.local.json` — your own service addresses and paths. `config.json` is
  the shared default; copy it and edit the copy.

Asset packs are never redistributed, and a pack author's
`allow_3rd_party_mapping_software_to_read` flag is carried into every map file
as the author set it. Do not add anything that works around it.

---

## Reporting a map that came out wrong

This is the most useful kind of report, and it needs three things:

1. **`map.json`** from the output folder — that is the plan, and it is what
   makes the problem reproducible.
2. **The picture**, or the `.dungeondraft_map` and a screenshot of it open.
3. **What you expected instead.** "The walls do not meet at the corners" is
   actionable; "it looks wrong" is not.

`caption.json` and `*.report.json` from the same folder help too: the first is
exactly what the renderer was told, the second is what the Dungeondraft export
placed and which packs it needed.

---

## Where things are

| | |
|---|---|
| `tools/architect.py` | all geometry: rooms, corridors, walls, doors, props, bleed margin |
| `tools/ideogram_prompt.py` | the layout as a structured JSON caption with bounding boxes |
| `tools/dungeondraft_assembler.py` | the layout as a native `.dungeondraft_map` |
| `tools/agent_api.py` | the API an AI agent calls — see `AGENTS.md` |
| `app/src/core`, `services`, `ui` | window and device, background jobs, one file per tab |
| `app/include/*.h` | the C++ ports of the architect and the caption builder |
| `styles/_base.json` | the shared caption contract every style inherits |
| `docs/dungeondraft_map_format.md` | the map format, verified against real files |
| `docs/REPORT.md` | what was built, what was decided, and what is still open |

`AGENTS.md` is the instruction sheet for AI agents driving the tools, and it is
worth reading even if you are not one — it documents the parts of the caption
contract that a render test paid for.
