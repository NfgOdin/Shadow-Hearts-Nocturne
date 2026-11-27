Drop your high-poly .glb model from Meshy.ai or Blender right here as:

  koudelka.glb

Tips:
- Export as .glb (glTF 2.0 binary) with textures embedded (Albedo + Normal supported automatically).
- If you enabled Auto-Rig in Meshy.ai (or rigged with Mixamo / Blender), Nocturne automatically
  detects your skeleton and prints bone diagnostics to stderr/mods.log on launch.
- Want to verify if your .glb exported with a valid rig even if Meshy's web preview is down?
  Run:
    tools\portable\python\python.exe scripts\inspect_glb.py path\to\koudelka.glb
