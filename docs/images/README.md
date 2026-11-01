# Demo images

The portraits are AI-generated with Google Gemini (Nano Banana Pro) and show no real
people; no copyright is claimed for them. Only these images belong in this folder;
test photos and camera recordings never enter the repository (`.gitignore`).

The processed images are the unedited output of the built plugin in headless OBS:
preset High, High Quality, Apple Acceleration, analysis at 1280 px, on the source
scaled to 3840 px wide. Original and processed images share the same scaling and
JPEG settings. To regenerate them after a look change, build the bundle and run:

```sh
uv run beauti-readme-images streamer-1.jpg streamer-2.jpg --names streamer-1 streamer-2
```

The source images are kept outside the repository. `--preset` and `--quality`
select another look or quality; `uv run beauti-readme-images --help` lists the
options.
