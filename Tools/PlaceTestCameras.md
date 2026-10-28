# PlaceTestCameras

<!--
- David
Credit AI Usage: This file and Tools/PlaceTestCameras.py are AI written. I asked Claude Code to
generate an editor script that drops cine cameras at several angles in every test scene so I can
screenshot the GI visualization modes from the same viewpoints. It only spawns and configures
editor cameras and saves the levels, nothing in the surfel renderer or in the research result
depends on it.
Prompts used:
	also using other agents for all existing scenes and the geometry place cine cams in several angles so i can test my system
	For the scripts and everything find a place to credit also in the code
-->

Editor Python script that places fixed `CineCameraActor`s in every test map so the GI
visualization modes can be compared from identical viewpoints. At runtime, switch between them
from the Surfel dev GUI's **Cameras** section (`UDevGuiSubsystem::DrawCamerasSection`), which is
PIE-only.

## 1. Enable the Python plugin (one-off)

`Surfels.uproject` does not enable `PythonScriptPlugin`. Add this entry to the `"Plugins"` array:

```json
		{
			"Name": "PythonScriptPlugin",
			"Enabled": true
		}
```

So the array becomes:

```json
	"Plugins": [
		{
			"Name": "ModelingToolsEditorMode",
			"Enabled": true
		},
		{
			"Name": "Surfel",
			"Enabled": true
		},
		{
			"Name": "SceneViewExtensionTemplate",
			"Enabled": false
		},
		{
			"Name": "ImGui",
			"Enabled": true
		},
		{
			"Name": "PythonScriptPlugin",
			"Enabled": true
		}
	]
```

`EditorScriptingUtilities` is **not** needed. The script uses only `UnrealEd` / `LevelEditor`
editor subsystems and never touches `unreal.EditorLevelLibrary` or `unreal.EditorAssetLibrary`,
which live in that plugin and are deprecated in UE 5.x.

Restart the editor after the edit.

## 2. Run it

Save the level you have open first (the script opens other levels, and script-driven level loads
auto-answer the "save?" prompt rather than showing it). Then, in the editor **Output Log**:

- Switch the command box dropdown from `Cmd` to `Python`, paste the script path, press Enter; or
- Stay on `Cmd` and run:

```
py "C:/Users/david/Documents/Unreal Projects/Surfels/Tools/PlaceTestCameras.py"
```

Stop Play In Editor first; the script refuses to run during PIE.

It opens each map in `CAMERA_DEFS`, places the cameras, saves the level, and prints a per-map
summary to the log. The last map in the table stays open when it finishes.

## 3. What it does per map

1. Loads the level with `LevelEditorSubsystem.load_level`.
2. Builds the combined world AABB of the **static geometry** only. Lights, `PlayerStart`,
   cameras, `LevelSequenceActor`, volumes, info actors, decals and sky/fog/cloud actors are
   excluded, as are static meshes under `/Engine/EngineSky/` and anything whose half-extent
   exceeds 100 000 uu (the engine sky sphere). BSP `Brush` actors **are** included, minus the
   level's builder brush, because several of the test rooms are built from brushes.
3. Spawns or updates one `CineCameraActor` per definition.
4. Sets both the UObject name and the actor label, puts the camera in a `TestCameras` outliner
   folder, disables depth of field, applies a 16:9 filmback and a fixed focal length.
5. Saves the level with `LevelEditorSubsystem.save_current_level`.

### Why the object name matters

`DrawCamerasSection` labels each ImGui radio button with `Camera->GetName()` - the UObject name,
not the editor label. Setting only `set_actor_label` would leave the runtime list showing
`CineCameraActor_3`, so the script calls `Object.rename()` as well. If the rename fails (a
pending-kill object still holds the name), it logs a warning and keeps going with the
engine-assigned name.

### Camera settings for GI screenshots

| Setting | Value | Why |
| --- | --- | --- |
| `focus_settings.focus_method` | `CameraFocusMethod.DISABLE` | Forces `DepthOfFieldFocalDistance = 0`, which also overrides DOF coming from a `PostProcessVolume`. DOF would ruin GI comparison shots. |
| `filmback` | 36.0 x 20.25 mm | Exactly 16:9. `RecalcDerivedData` derives both `AspectRatio` and `FieldOfView` from the filmback. |
| `lens_settings` min/max focal | 4 - 1000 mm | `RecalcDerivedData` clamps `CurrentFocalLength` to the lens range, so a wide 18 mm setting would otherwise be silently clamped. |
| `exposure_method` | `DO_NOT_OVERRIDE` | Leaves exposure to the scene's post process so every GI mode is compared under identical exposure. |
| `constrain_aspect_ratio` | `True` | Letterboxes to 16:9 regardless of window size, keeping framing identical between screenshots. Set `CONSTRAIN_ASPECT_RATIO = False` at the top of the script if the bars are a nuisance. |

## 4. Idempotency and pre-existing cameras

Re-running is safe. A camera whose **object name** matches a definition is moved and
reconfigured in place; nothing is duplicated. Cameras not named in the table - such as
`CineCameraActor_0` in `DynamicSponza`, and the cine cameras already present in `CornellBox` and
`Dragon` - are never moved or deleted, only listed in the summary as "left alone".

If a non-camera actor already owns one of the names, the script warns and skips that definition
rather than clobbering it.

## 5. Camera definition schema

`CAMERA_DEFS` at the top of the script maps a map package path to a list of definitions, or to
`None` meaning "use `DEFAULT_CAMERA_SET`". Each definition is a plain dict:

| Key | Required | Meaning |
| --- | --- | --- |
| `name` | yes | UObject name, actor label, and the idempotency key. This is the string the ImGui camera list shows. |
| `location` | one of these two | `(x, y, z)` absolute world position, Unreal units. |
| `rel_location` | one of these two | `(rx, ry, rz)` bounds-relative: `world = bounds_center + r * bounds_extent`, where `bounds_extent` is the **half** size. `(0, 0, 0)` is the middle of the scene; `(0, -2.2, 0.35)` is 2.2 half-depths out along -Y and slightly up. |
| `location_offset` | no | `(dx, dy, dz)` absolute nudge in Unreal units, applied after the placement above. |
| `rotation` | at most one aim key | `(pitch, yaw, roll)` degrees, absolute. |
| `look_at` | at most one aim key | `(x, y, z)` absolute world point to aim at. |
| `rel_look_at` | at most one aim key | `(rx, ry, rz)` bounds-relative aim point, resolved like `rel_location`. |
| `focal_length` | at most one lens key | Focal length in mm. Defaults to `DEFAULT_FOCAL_LENGTH_MM` (24). |
| `fov` | at most one lens key | Horizontal FOV in degrees, converted to a focal length for the 16:9 filmback. |
| `folder` | no | World Outliner folder. Defaults to `TestCameras`. |

With no aim key the camera looks at the bounds center. Absolute and bounds-relative entries can
be mixed freely in the same list, which is the point of the schema: the generic bounds-relative
defaults work today, and hand-tuned absolute framings can be dropped in per scene later.

```python
CAMERA_DEFS = {
    "/Game/DynamicSponza": [
        {"name": "Cam_Sponza_Atrium", "location": (-1200.0, 0.0, 250.0),
         "rotation": (-5.0, 0.0, 0.0), "focal_length": 21.0},
        {"name": "Cam_Sponza_Corner", "rel_location": (-1.8, -1.8, 0.55),
         "rel_look_at": (0.0, 0.0, 0.0), "fov": 75.0},
    ],
    "/Game/CornellBox": None,   # use DEFAULT_CAMERA_SET
}
```

### Default set

Applied to every map whose entry is `None`: `Cam_Front` (straight on from -Y),
`Cam_Corner` (45 degrees, best single angle for two-wall bounce), `Cam_Side` (elevation from +X),
`Cam_High` (three-quarter looking down, shows floor bounce and contact AO) and `Cam_Interior`
(inside the scene, wide, aimed into the opposite corner - the angle that exposes light leaking
through walls).

## 6. Maps covered

`/Game/DynamicSponza`, `/Game/CornellBox`, `/Game/Dragon`, `/Game/ConferenceRoom`,
`/Game/LivingRoom` (the five in `GDevScenes`) and `/Game/TestMap`. A map that fails to load is
reported as `NOT LOADED` and skipped, so entries can be left in the table for maps that do not
exist yet.
