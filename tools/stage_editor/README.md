# Stage Forge (work in progress)

The stage editor for ShadowRecomp. It runs in your web browser. It isn't finished, so expect bugs and changes. It's not in the release download yet.

**You need**
- Python 3.12 with NumPy and Pillow
- Your own disc unpacked next to the project folder (`..\Shadow`, see the main README)
- A built game (`play\` in the project folder) for playtesting

**Start it**

```
python tools/stage_editor/server.py --ui2
```

It opens the editor at http://127.0.0.1:49012. Your projects are saved in `stage_projects\`. The original disc and your installed game are never changed: builds and playtests use their own copies.

**Credits and licences**
- The file readers and writers here were written for this project, using the format research of [ShadowSET](https://github.com/ShadowTheHedgehogHacking/ShadowSET), [HeroesPowerPlant](https://github.com/igorseabra4/HeroesPowerPlant) and [RenderWareFile](https://github.com/igorseabra4/RenderWareFile).
- `catalog.json` (the object list) is based on ShadowSET and is under GPL-3.0: see `catalog.LICENSE.txt`.
- `vendor/` holds [three.js](https://threejs.org) (MIT, `vendor/LICENSE`).
- Everything else here is under the project's MIT licence.
