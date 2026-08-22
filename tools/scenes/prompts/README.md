# The regression suite

Each file in `tools/scenes/` is a scene: a plan built from a written description,
with everything the description places pinned by hand. The descriptions the
scenes were built from are kept beside them here, verbatim, so a change to the
architect or the caption builder can be checked against the thing it is supposed
to serve rather than against a paraphrase of it.

These are deliberately awkward. Between them they cover a walled interior of one
room, a walled interior of six, an open-air site with no envelope at all, a
natural cave, a river gorge that is two separate banks, water at four depths, a
hole in the floor, terrain the scene places itself, fire and fog laid over the
map, and several descriptions that insist on the absence of something the
renderer likes to add on its own.

## Running it

Before any GPU time — every scene built, every plan checked:

```bash
python tools/check_scenes.py
```

It answers the questions a person would: is everything reachable, is there a way
in, does each thing the description names have somewhere to be, is the caption
still carrying it. A plan that is wrong cannot be rendered right, and a render
costs ten minutes to find that out.

Then, with ComfyUI running, one scene:

```bash
python tools/render_scene.py tools/scenes/tavern.json
```

or the whole suite, which takes a couple of hours:

```bash
python tools/render_scene.py --all
```

`--plan-only` stops after the plan and writes the preview and the caption, which
is enough to see most problems.

## After a large change

Run `check_scenes.py` first and fix anything it reports. Then render the suite
and look at every picture against its description in this folder. What matters
is not that a picture is pretty: it is whether the thing the description put in
the north-west corner is in the north-west corner, whether the room that is one
room came back as one room, and whether anything appears that nobody asked for.

Fix what you find in the architect or the caption builder, never in the scene
file — a scene tuned until it renders is a scene that has stopped testing
anything.
