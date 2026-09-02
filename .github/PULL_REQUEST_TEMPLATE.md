<!-- What was wrong, why it was wrong, and what this does about it. -->

## What this changes

## Why

<!-- The checks CI runs. If one of them cannot pass yet, say which and why
     rather than deleting the line. -->
## Checks

- [ ] `python tools/check_scenes.py`
- [ ] `python tools/check_layouts.py`
- [ ] `python tools/check_captions.py`
- [ ] `python tools/check_dungeondraft.py`
- [ ] `python tools/check_caption_parity.py`

<!-- tools/architect.py and app/include/map_architect.h are one geometry engine
     written twice, and so are tools/ideogram_prompt.py and
     app/include/ideogram_caption.h. A change to one that does not reach the
     other makes the app and the agents disagree. -->
## Both ports

- [ ] This touches nothing that exists in both Python and C++
- [ ] Or: both are changed, and here is what I compared them on:

## If it changes what a map looks like

<!-- Attach the before and after, or say which scene to run to see it. The 13
     scenes in tools/scenes are what changes get judged against. -->
