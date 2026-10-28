# Scene Framings for GI Comparison

> **Credit Claude Code:** this document is AI written. It is an evaluation aid that records what
> geometry each test map contains and proposes fixed camera framings for comparing GI modes
> (Lumen vs surfel GI vs direct-light-only) from identical viewpoints; nothing in the renderer or in
> the research result depends on it. The actor inventories, transforms and bounds below were decoded
> directly out of the `.umap` files, but **every proposed camera position, rotation and focal length
> is AI-derived** - they were computed from the decoded geometry, not read from any asset, and no
> camera listed here has been looked at in the editor. Figures that were estimated rather than
> decoded are marked *(estimated)* in place and collected in "Decoding confidence" at the end -
> chiefly the Sponza arcade positions (Sponza is one merged mesh, so only its overall box is
> recoverable), the free-space checks for the Conference Room and Living Room cameras, and the
> Conference Room sun intensity.
>
> Prompts used:
>
>> also using other agents for all existing scenes and the geometry place cine cams in several angles so i can test my system
>
>> For the scripts and everything find a place to credit also in the code

---

## How to read this

* All coordinates are Unreal world space, centimetres. Rotation is `pitch / yaw / roll` in degrees.
* Focal lengths assume the stock `CineCameraActor` filmback, **23.76 x 13.365 mm**. This is not a
  guess: every camera already present in these maps serialises `FieldOfView = 37.4974`, which is
  exactly 35 mm on a 23.76 mm sensor. Angle of view for the lengths used below:

  | f (mm) | 14 | 16 | 18 | 20 | 24 | 28 | 35 |
  |---|---|---|---|---|---|---|---|
  | Horizontal FOV | 80.6 | 73.2 | 66.8 | 61.4 | 52.7 | 46.0 | 37.5 |
  | Vertical FOV | 51.0 | 45.3 | 40.7 | 37.0 | 31.1 | 26.9 | 21.6 |

* "Sun direction" is the direction photons travel, i.e. `FRotator::Vector()`. A surface is directly
  lit when its normal has a positive dot product with the *negated* sun direction.
* **Companion script.** `Tools/PlaceTestCameras.py` already implements the spawning side but ships
  with `CAMERA_DEFS` set to `None` for every map, so it falls back to a generic bounds-relative set
  (`Cam_Front`, `Cam_Corner`, `Cam_Side`, `Cam_High`, `Cam_Interior`). The tables in this document
  are the per-map, geometry-aware replacements for those entries. Note also that that script lists
  `/Game/TestMap`, which cannot be loaded - see section 6.
* `DrawCamerasSection()` in `DevGuiSubsystem.cpp` builds the radio list from `Camera->GetName()`,
  **not** `GetActorLabel()`. Renaming a camera in the World Outliner only changes the label, so the
  names below have to be set as the actor's object name, or that call switched to `GetActorLabel()`.
  Otherwise the list reads `CineCameraActor_0`, `CineCameraActor_1`, ...

---

## 1. DynamicSponza

### Inventory

| Actor | Label | Class | Location | Rotation (P/Y/R) | Scale |
|---|---|---|---|---|---|
| `DirectionalLight_0` | DirectionalLight | DirectionalLight (Movable) | (-70, 0, 400) | -66 / 3 / 121 | 1 |
| `PlayerStart_0` | PlayerStart | PlayerStart | (0, 0, 92) | 0 / 0 / 0 | 1 |
| `StaticMeshActor_0` | SM_SkySphere | StaticMeshActor | (0, 0, 0) | 0 / 0 / 0 | 400 |
| `Sponza_scene_0` | Sponza | Actor (scene root) | (0, 0, 10) | 0 / 0 / 0 | 1 |
| `Sponza_node_0` | Sponza | StaticMeshActor | (0, 0, 0) | 0 / 0 / 0 | 0.008 |
| `StaticMeshActor_1` | Cone | StaticMeshActor | (840, 0, 120) | 0 / 0 / 0 | 2 |
| `CineCameraActor_0` | CineCameraActor | CineCameraActor | (-1020, -60, 150) | -1.4 / -0.8 / 0 | 1 |
| `LevelSequenceActor_0` | LevelSequenceActor | LevelSequenceActor | (0, 0, 0) | 0 / 0 / 0 | 1 |

* **Scene bounds (Sponza mesh, sky excluded):** X `[-1536.8, 1439.9]`, Y `[-946.2, 884.3]`,
  Z `[-101.2, 1143.5]` - roughly 29.8 m long (X) x 18.3 m wide (Y) x 12.4 m tall. The long axis of
  the atrium is **X**. Floor sits at Z ~ 0.
* **Cone** world box: X `[740, 940]`, Y `[-100, 100]`, Z `[20, 220]` - a 2 m volume standing in the
  open courtyard.
* **Sun:** 6 lux, direction `(0.406, 0.021, -0.914)` - 66 deg above the horizon, almost straight down
  and very slightly toward +X. The courtyard floor is strongly lit; everything under the galleries is
  in full shade. There is no SkyLight, so shaded surfaces receive *only* bounce.
* The `LevelSequenceActor` points at `DynamicSponzaSequence`, which contains a MovieScene with a
  playback range and editor state but **no object bindings and no transform tracks** - the Cone is
  not actually animated by it in the current file, and the Cone's component does not serialise a
  `Mobility` override (so it is Static). Worth checking before relying on it as the dynamic subject.

### Cameras

| Name | Position | Rotation | f | Rationale |
|---|---|---|---|---|
| `CineCam_Overview` | (-1020, -30, 300) | -10 / 0 / 0 | 14 | Establishing shot straight down the atrium from the author's existing camera spot, wider and raised. Frame top stays under the roofline for the whole depth, so no screen area is wasted on sky. |
| `CineCam_Arcade` | (-150, -30, 190) | -6 / 90 / 0 | 24 | Looks sideways out of the sunlit courtyard into the +Y colonnade. With the sun near vertical nothing in this frame is directly lit, so the whole image is bounce. This is the Lumen-vs-surfel shot. |
| `CineCam_SunPool` | (200, -30, 120) | -20 / 60 / 0 | 18 | Diagonal, low, catching the sunlit courtyard floor *and* the shaded arcade base in one frame. Direct and indirect side by side at a shared exposure. |
| `CineCam_Cone` | (480, -20, 185) | -7 / 3 / 0 | 24 | Frames the Cone (centre 840, 0, 120) at ~3.6 m with floor and colonnade context, so bounce off and around the moving object is visible. |
| `CineCam_EndWall` | (900, -30, 190) | -4 / 0 / 0 | 20 | The +X end colonnade, overhung and shaded, seen from just past the Cone. Deep indirect fed by a sunlit floor. |

All five sit on the courtyard centreline band (Y between -60 and 0), which is the only part of the
interior the file *proves* is open: the author's own camera is at Y = -60, the PlayerStart at Y = 0
and the Cone at Y = 0. *(estimated)* The Y extent of the arcades themselves is **not** recoverable -
Sponza is a single merged static mesh (`Content/Sponza/Sponza/StaticMeshes/Sponza_mesh_0.uasset`), so
only its overall box exists in the asset. Cameras are therefore never placed laterally inside an
arcade; they look into them from known-open ground. The yaw/pitch values that aim into the colonnades
are reasoned from the overall box, not measured.

---

## 2. CornellBox

### Inventory

| Actor | Label | Material | Location | Rotation (P/Y/R) | Scale | World box |
|---|---|---|---|---|---|---|
| `DirectionalLight_0` | DirectionalLight | - | (0, 0, 400) | -49.52 / -10.31 / 112.36 | 1 | - |
| `StaticMeshActor_0` | SM_SkySphere | M_SimpleSkyDome | (0, 0, 0) | 0 / 0 / 0 | 400 | - |
| `StaticMeshActor_1` | Cube7 | **White** | (-60, -80, 125) | 0 / 0 / 0 | 1 | X[-110,-10] Y[-130,-30] Z[75,175] |
| `StaticMeshActor_2` | Cube8 | **White** | (150, 0, 330) | 0 / 0 / 0 | (0.5, 5, 5.625) | X[125,175] Y[-250,250] Z[48.8,611.2] |
| `StaticMeshActor_3` | Cube9 | **Green** | (-30, 225, 325) | 0 / -90 / 0 | (0.5, 5, 5) | X[-280,220] Y[200,250] Z[75,575] |
| `StaticMeshActor_4` | Cube10 | **Red** | (-30, -225, 325) | 0 / -90 / 0 | (0.5, 5, 5) | X[-280,220] Y[-250,-200] Z[75,575] |
| `StaticMeshActor_5` | Cube11 | **White** | (-30, 0, 50) | 90 / 180 / -180 | (0.5, 5, 5) | X[-280,220] Y[-250,250] Z[25,75] |
| `StaticMeshActor_6` | Cube12 | **White** | (-30, 0, 600) | 90 / 180 / -180 | (0.5, 5, 5) | X[-280,220] Y[-250,250] Z[575,625] |
| `StaticMeshActor_7` | Cube13 | **White** | (-20, 90, 200) | 0 / 30 / 0 | (1, 1, 2.5) | X[-88.3,48.3] Y[21.7,158.3] Z[75,325] |
| `CineCameraActor_0` | CineCameraActor | - | (-2180, -20, 310) | 0 / 0 / 0 | 1 | - |

* **Interior cavity:** X `[-280, 125]`, Y `[-200, 200]`, Z `[75, 575]`, i.e. a 4.05 x 4.0 x 5.0 m box.
  **The box is open on the -X face** - there is no front wall; the back wall is Cube8 at X = 125.
* **Colour layout:** green wall on **+Y**, red wall on **-Y**, floor / ceiling / back / both boxes
  white. Material assignment was read from each component's `OverrideMaterials` import reference and
  cross-checked against that component's `RelativeLocation`, so this mapping is decoded, not assumed.
* **Sun:** 1.892 lux, direction `(0.639, -0.116, -0.761)` - enters through the open -X face and
  travels in and down. Per-surface direct lighting works out as:
  * floor - strongly lit (cos 0.761); back wall - lit (cos 0.639)
  * **red wall - grazing light only (cos 0.116)**
  * **green wall - not directly lit at all**
  * **ceiling - not directly lit at all**

  So the ceiling and the green wall are *pure* indirect surfaces, and almost all colour bleed in the
  room is driven by the sunlit white floor. That makes this map an unusually clean GI test.
* The existing camera is 19 m back from the opening on a 35 mm lens, so the box occupies only about a
  third of the frame width. The framings below are much closer.

### Cameras

| Name | Position | Rotation | f | Rationale |
|---|---|---|---|---|
| `CineCam_Overview` | (-820, 0, 325) | 0 / 0 / 0 | 20 | Canonical Cornell view on the box axis. Back wall, both coloured walls, ceiling and the sunlit floor pool all in frame; only the nearest floor/ceiling edges crop. |
| `CineCam_ColourBleed` | (-265, 0, 200) | -10 / 0 / 0 | 16 | Just inside the opening, low and wide: both boxes centre-frame with the red wall filling the right edge and the green the left. The white box flanks are where red/green bleed reads most clearly. |
| `CineCam_Ceiling` | (-255, 0, 120) | 35 / 0 / 0 | 18 | Aimed up at the ceiling and upper walls - surfaces that receive **zero** direct light. Any brightness here is entirely bounce, so this is the hardest frame for an approximate GI method. |
| `CineCam_Corner` | (-250, -120, 300) | 29 / 40 / 0 | 24 | The green-wall / back-wall / ceiling corner. Puts one directly lit surface against two indirect-only ones in a single exposure, which is the easiest way to spot a GI method losing energy. |

---

## 3. Dragon

### Inventory

| Actor | Label | Material | Location | Rotation (P/Y/R) | Scale | World box |
|---|---|---|---|---|---|---|
| `DirectionalLight_0` | DirectionalLight | - | (0, 0, 400) | -49.52 / -10.31 / 112.36 | 1 | **Intensity 0 - off** |
| `DirectionalLight_2` | DirectionalLight2 | - | (700, 310, 400) | -49.52 / -128 / 112.36 | 1 | Intensity 2 lux - the active sun |
| `StaticMeshActor_0` | SM_SkySphere | M_SimpleSkyDome | (0, 0, 0) | 0 / 0 / 0 | 400 | - |
| `Floor_0` | Floor | MI_ProcGrid | (0, 0, 109.5) | 0 / 0 / 0 | 8 | X[-4000,4000] Y[-4000,4000] Z[-290.5,109.5] |
| `DragonAttenuation_node_0` | Dragon | **RedMaterial** | (290, -50, 106.94) | 0 / 0 / 90 | 0.25 | X[113.8,466.2] Y[-128.8,28.8] Z[106.9,355.4] |
| `StaticMeshActor_1` | Cube | BasicShapeMaterial (grey) | (90, -150, 230) | 0 / -30 / 0 | (1.25, 0.5, 1.25) | X[23.4,156.6] Y[-202.9,-97.1] Z[167.5,292.5] |
| `StaticMeshActor_2` | Cube2 | BasicShapeMaterial (grey) | (470, -90, 280) | 0 / 50 / 0 | (2.8125, 0.5, 2.8125) | X[360.5,579.5] Y[-213.8,33.8] Z[139.4,420.6] |
| `CineCameraActor_0` | CineCameraActor | - | (304, 1185, 267) | 0 / -90 / 0 | 1 | - |
| `CineCameraActor_1` | CineCameraActor2 | - | (304, -1595, 267) | 0 / 90 / 0 | 1 | - |

* **Subject bounds (floor excluded):** X `[23.4, 579.5]`, Y `[-213.8, 33.8]`, Z `[106.9, 420.6]`.
  The floor's top surface is at **Z = 109.5** and the dragon's feet are at Z = 106.94, which is how
  the bounds decoder was validated.
* **The dragon is red and both panels are neutral grey**, so this map is effectively a one-sided
  Cornell box. The strongest colour-bleed signal in the project is red light from the dragon landing
  on Cube2.
* **Sun:** only `DirectionalLight2` is on, 2 lux, direction `(-0.400, -0.512, -0.761)`. Lit faces
  point toward +X and +Y; faces toward -X and -Y are shadowed.
* Cube2's slab normal is `(-0.766, 0.643, 0)`. Its dot product with the negated sun direction is
  **+0.02**, i.e. the dragon-facing side of Cube2 sits at exact grazing incidence and receives
  essentially no direct light. Whatever colour appears on it is bounce off the red dragon.
* Both directional lights and the floor are Movable.

### Cameras

| Name | Position | Rotation | f | Rationale |
|---|---|---|---|---|
| `CineCam_Overview` | (300, 720, 290) | -6 / -90 / 0 | 24 | Establishing shot of the whole group from the lit side. Frames the dragon and both panels with floor context; tighter than the existing camera, which sits 12 m back. |
| `CineCam_RedBleed` | (100, 400, 260) | -2 / -60 / 0 | 24 | Red dragon in the foreground, grey Cube2 behind it. Cube2's near face is the zero-direct-light surface computed above, so it reads as a pure red-bounce target. The colour-bleeding shot for this map. |
| `CineCam_Shadow` | (300, -820, 230) | -3 / 90 / 0 | 28 | The fully backlit side: dragon, both panels and the floor shadow, none of it directly lit. Indirect-dominated. |
| `CineCam_Closeup` | (170, 230, 250) | -4 / -67 / 0 | 35 | Tight on the dragon's upper body at ~3 m. High-resolution read of bounce detail and contact darkening against the floor. |

---

## 4. ConferenceRoom

### Inventory

37 actors: one `Actor` scene root (`Scene`, at origin), one `DirectionalLight`, and 35
`StaticMeshActor` nodes named `ConferenceRoom_node_0 .. _35`. **Every mesh node has the identical
transform** - location (0, 0, 0), rotation 0 / 0 / 90, uniform scale 0.01 - so the layout lives
entirely inside the merged mesh bounds. There are **no cameras and no PlayerStart** in this map, and
no SkySphere.

* `DirectionalLight_0` at (-326, 30, 160), rotation -17 / 133 / 145, scale 2.5. Direction
  `(-0.652, 0.699, -0.292)`, i.e. only 17 deg above the horizon - a raking, long-shadow sun.
  **Intensity is not serialised in the map**, so the light runs at the engine default
  *(estimated: 10 lux for `UDirectionalLightComponent` in UE5 - verify in editor)*.
* **Scene bounds:** X `[-983.5, 1685.8]`, Y `[-1055.7, 668.5]`, Z `[-2.8, 650.3]` - about
  26.7 x 17.2 x 6.5 m. Floor plane at **Z = -1.6**, ceiling plane at **Z = 650.3**.

Named parts worth framing (world boxes):

| Node | Mesh | World box | Reading |
|---|---|---|---|
| `_3` | mesh4 | Z = -1.6 flat, X[-947,1653] Y[-1016,668] | Floor |
| `_4` | mesh5 | Z = 650.3 flat | Ceiling |
| `_2` | mesh2 | Y = 612.6 flat, X[-434,1421] Z[-1.6,503.8] | Glazed facade on the +Y wall |
| `_6` | mesh7 | Y[592,612.6] Z[-1.6,523.6] | Window frames / mullions |
| `_13`,`_15`,`_16`,`_17` | mesh15,17,18,19 | Y ~ 577-612, Z[466,587] | Blinds / valance above the glazing |
| `_24` | mesh28 | X[-530,1618] Y[478,566] Z[-1.6,90.5] | Long low credenza / sill under the windows |
| `_25` | mesh29 | X[75,954] Y[-285,9] Z[168,179.2] | **Conference table top** |
| `_26` | mesh30 | X[113,917] Y[-211,-65] Z[-2.8,169.3] | Table base |
| `_27` | mesh31 | X[-73,1069] Y[-398,119] Z[87.7,192.2] | Chairs |
| `_9` | mesh10 | Y = -1055.7 flat, Z[205.7,649.6] | Far (-Y) wall, upper |
| `_31` | Plane001 | Y = -1014.6 flat, Z[13.9,209.3] | Far (-Y) wall, lower band |
| `_1` | mesh1 | X[-947,-471] Y[-522,-49] Z[-1.6,650.3] | Full-height solid volume in the -X/-Y corner (service core) - **keep cameras out of this box** |

**Important caveat on this map.** The sun's azimuth (yaw 133) sends light *toward* +Y, meaning it
arrives from the -Y side and lands on the inner face of the glazed +Y facade. But the -Y side is
closed by `mesh10` (Z 206-650) plus `Plane001` (Z 14-209), which between them cover the full wall
height. Either one of those is glazed or masked, or very little direct sun actually reaches the
interior. *(estimated - this could not be settled from the file.)* If the room turns out to be almost
entirely indirectly lit that is not a problem for this task - it makes every framing below a GI
framing - but it should be confirmed in the editor before any numbers are quoted in the report.

### Cameras

| Name | Position | Rotation | f | Rationale |
|---|---|---|---|---|
| `CineCam_Overview` | (-400, -400, 300) | -7 / 16 / 0 | 18 | Establishing shot down the long axis with the glazed wall on the left and the table group centre-frame. Positioned clear of the `mesh1` service core. |
| `CineCam_Window` | (500, -600, 210) | -2 / 80 / 0 | 24 | Square onto the glazed facade - the only wall whose inner face is oriented toward the sun. The bright reference frame to compare the others against. |
| `CineCam_Table` | (-250, -450, 250) | -5 / 22 / 0 | 24 | The table and chairs. The table top is a large horizontal bounce surface and the underside plus the chair seats are indirect-only, so near-field occlusion shows up clearly here. |
| `CineCam_Corner` | (900, -300, 210) | -4 / -44 / 0 | 24 | The +X/-Y corner. The -Y wall's inner face points away from the sun (dot -0.699), so nothing in this frame is directly lit - the indirect-dominated shot for this map. |

*(estimated)* Free-space checks for these four positions were done against the per-node world AABBs
only. Several nodes here are large sparse meshes whose AABB spans most of the room (`mesh8`,
`mesh12`, `mesh21`), so an AABB overlap does not prove an obstruction and a clear AABB does not prove
open space. Each camera was kept where possible in the Z band between the furniture layer (tops
around Z = 194) and the ceiling structure (bottoms around Z = 227); nudge any that end up inside
geometry.

---

## 5. LivingRoom

### Inventory

151 actors: one `Actor` scene root, one `DirectionalLight`, and 149 `StaticMeshActor` nodes
(`LivingRoom_node_0 .. _150`). As in the Conference Room, **every mesh node shares the same
transform** - location (0, 0, 0), rotation 0 / 0 / 90, scale 1 - so individual placement is baked
into the meshes. **No cameras, no PlayerStart, no SkySphere.**

* `DirectionalLight_0` at (-62, -138, 160), rotation -23 / 98 / 19, scale 2.5, **Intensity 5.778**.
  Direction `(-0.128, 0.912, -0.391)` - 23 deg above the horizon, travelling down the room's long
  axis from the -Y end toward +Y.
* **Scene bounds:** X `[-258.2, 314.8]`, Y `[0, 817.4]`, Z `[0, 317.1]`. Room shell
  (`Mesh__145_`): X `[-258, 315]`, Y `[40, 817]`, Z `[2.8, 317]` - about 5.4 x 7.8 x 3.1 m. Floor
  plane at **Z = 2.9**, ceiling plane at **Z = 316.9**, cornice Z 300-317, skirting Z 2.4-19.5.

Structural parts:

| Node | Mesh | World box | Reading |
|---|---|---|---|
| `_118` | Mesh__145_ | X[-258,315] Y[40,817] Z[2.8,317] | Room shell |
| `_114` | Mesh__141_ | Z = 2.9 flat | Floor |
| `_113` | Mesh__140_ | Z = 316.9 flat | Ceiling |
| `_117` | Mesh__144_ | Z[300,317] | Cornice |
| `_150` | WhitePaint | X[-244,300] Y[37,423] Z[2.9,317] | Painted wall surfaces, -Y half of the room |
| `_58`,`_86`,`_100`,`_101`,`_103` | - | Y ~ 46-52, Z[211,279] | Window / frame elements on the **-Y wall** *(estimated reading)* |
| `_2`,`_131`,`_148` / `_97`-`_99` / `_125`,`_126`,`_128` | - | X ~ 141 / -98 / 21, Y ~ 74, Z[202,280] | Three pendant lamps in a row - **geometry only, there are no light actors for them** |
| `_44` | Mesh__53_ | X[4.8,72] Y[256,323] Z[212,270] | Further ceiling fixture |
| `_19`-`_96` cluster | - | X[-243,-218] Y[365,421] Z[66,118] | Shelving / furniture against the -X wall |

* Direct-light breakdown: the **+Y end wall** (inner normal -Y) is strongly lit (cos 0.912) and the
  **floor** is lit (cos 0.391); the **ceiling receives no direct light at all**, and every surface
  facing +Y is shadowed.
* No strongly saturated surface could be identified from the file - the 210 assets under
  `Content/LivingRoom` are named `Mesh__N_` or generic, so there is no colour-bleed shot that can be
  justified from the asset names alone. The Cornell Box and Dragon maps cover that case.

### Cameras

| Name | Position | Rotation | f | Rationale |
|---|---|---|---|---|
| `CineCam_Overview` | (28, 120, 200) | -5 / 90 / 0 | 16 | Wide establishing shot from the window end down the length of the room. Walls fill the frame edges and the lit +Y end wall anchors the far end. |
| `CineCam_Window` | (28, 600, 170) | -2 / -90 / 0 | 18 | Reverse angle onto the window wall and the sunlit floor pool - the direct-light reference frame, with the pendant lamps silhouetted. |
| `CineCam_Corner` | (-150, 650, 120) | 22 / 20 / 0 | 18 | Low, aimed into the far +X/+Y ceiling corner. The ceiling takes no direct light and the +Y-facing surfaces are shadowed, so this frame is almost entirely bounce. |
| `CineCam_Lamps` | (28, 400, 150) | 18 / -90 / 0 | 20 | Looks back toward the window and up at the three pendants and the ceiling. Small hanging geometry against an indirect-only ceiling is where surfel radius and coverage artefacts tend to show first. |

*(estimated)* Same caveat as the Conference Room: free space was checked against per-node AABBs, and
several nodes (notably `WhitePaint`) have AABBs spanning much of the room. `CineCam_Lamps` looks past
node `_44` (X 4.8-72, Y 256-323, Z 212-270) - the sight line passes under it by about 25 cm at the
stated pitch, which is tight enough to be worth checking.

---

## 6. TestMap

**This is not a level and has no geometry.** `Content/TestMap.umap` is 1219 bytes with a 12-entry
name table containing only `None`, `PackageLocalizationNamespace`, `/Game/Demo`, `/Game/TestMap`,
`/Script/CoreUObject`, `/Script/Engine`, `Class`, `Demo`, `ObjectRedirector`, `Package`, `TestMap`
and `World`. It is an `ObjectRedirector` package pointing `/Game/TestMap` at `/Game/Demo`, and there
is no `Demo.umap` anywhere under `Content/`. There is no `ActorsMetaData` blob, no actor and no
light. `Content/__ExternalActors__/TestMap/` exists but is empty.

It is also **not** in `GDevScenes` in `DevGuiSubsystem.cpp`, which lists only Sponza, Cornell Box,
Dragon, Conference Room and Living Room. No cameras are proposed. `Tools/PlaceTestCameras.py`
currently includes `/Game/TestMap` in `CAMERA_DEFS`, and that entry will fail to load - it should be
removed, and the map itself either deleted or rebuilt as a real level.

---

## Decoding confidence

**Decoded from the files and cross-validated - treat as reliable:**

* FName tables. Start offsets and entry counts found by brute force: DynamicSponza 657 / 412
  (matches the known-good reference exactly), ConferenceRoom 618 / 169, CornellBox 634 / 364,
  LivingRoom 614 / 397, Dragon 630 / 383, TestMap 371 / 12.
* Per-actor transforms, read from the base64 `ActorsMetaData` descriptor blob. The descriptor layout
  resolved to: version, GUIDs, four FStrings, actor GUID, then `FQuat` (4 doubles), `FVector`
  translation (3), `FVector` scale (3), then two bounds blocks of
  `origin(3) + quat(4) + extent(3) + int32 valid`, then the actor label and folder path.
  Quaternion to rotator uses the UE convention `pitch = asin(2(zx - wy))`,
  `yaw = atan2(2(wz + xy), 1 - 2(y^2 + z^2))`, `roll = atan2(-2(wx + yz), 1 - 2(x^2 + y^2))`.
* **Validation.** The Sponza `DirectionalLight_0` quaternion decodes to pitch -66 / yaw 3, giving a
  sun direction of `(0.406, 0.021, -0.914)` - matching the known reference value
  `(0.41, 0.02, -0.91)` to three decimals. The Dragon's bounds put its feet at Z = 106.94 against a
  floor whose top surface is at Z = 109.5. The Cornell cube bounds reproduce exact
  500 x 500 x 500 wall slabs, and the 68.3 figure for the 30-degree-rotated tall box is exactly
  `50 * (cos 30 + sin 30)`.
* Bounds convention, derived empirically and then confirmed on three independent cases: the stored
  **origin is pre-rotation** (in the actor's unrotated local frame) and the stored **extent is
  post-rotation** (world-axis-aligned); both are **pre-scale**, so `world_extent = extent * scale`
  and `world_centre = location + Rotation(origin * scale)`. This is a reverse-engineered reading of
  an undocumented layout rather than a specification, so it is the most likely place for a systematic
  error - though the three validations above make that unlikely.
* Material assignments (Cornell red/green walls, the red dragon), read from each
  `StaticMeshComponent`'s `OverrideMaterials` package index into the import table and confirmed by
  pairing each one with that component's own `RelativeLocation`. The import table stride is 40 bytes
  with `ObjectName` at +20.
* Light intensities, `Mobility` and `FieldOfView`, read from property tags.

**Estimated, inferred, or not recoverable - do not quote as measured:**

1. **Every camera position, rotation and focal length in this document.** They are computed from the
   decoded geometry; none has been verified in the editor.
2. **Sponza's interior layout.** Sponza is one merged static mesh, so the arcade and colonnade
   positions, the courtyard width and the gallery floor height are *not* in the asset - only the
   overall box is. The Sponza cameras are confined to the courtyard centreline the file does prove is
   open (PlayerStart, the Cone and the author's existing camera all sit on it); the yaw values aimed
   into the colonnades are reasoned, not measured.
3. **Sponza Z may be offset by 10 cm.** `Sponza_node_0` is at (0, 0, 0) and the `Sponza_scene_0` root
   it belongs to is at (0, 0, 10). Whether the descriptor transform is relative or absolute was not
   resolved, so the floor is at Z = 0 or Z = 10. Irrelevant for framing, relevant if exact numbers are
   quoted.
4. **ConferenceRoom sun intensity** is not serialised; the engine default applies, believed to be
   10 lux but not confirmed against the engine source.
5. **Whether direct sun reaches the ConferenceRoom interior at all** - see the caveat in that section.
6. **The LivingRoom window position** is inferred from the shape and placement of frame-like meshes
   near Y = 46-52, Z = 211-279. The map has no mesh or material named window, glass or curtain.
7. **Free-space checks for the ConferenceRoom and LivingRoom cameras** are AABB-only, and those
   imports contain large sparse meshes whose AABBs span most of the room, so the checks are weak in
   both directions.
8. **The Cone's motion in DynamicSponza.** `DynamicSponzaSequence` holds a MovieScene with no object
   bindings and no transform tracks, and the Cone does not serialise a Movable mobility. Its authored
   position (840, 0, 120) is the only one in the files. `CineCam_Cone` is framed loosely around it to
   allow for motion that is not in the asset.
9. **Filmback** is assumed to be the stock 23.76 x 13.365 mm, inferred from the serialised
   `FieldOfView = 37.4974` corresponding to 35 mm. No `FilmbackSettings` override is serialised in any
   map, but that assumption underlies every focal length quoted here.
10. The non-zero roll on several DirectionalLights (121, 112.36, 145 and 19 degrees) is decoded
    correctly but is leftover gizmo rotation; roll does not affect a directional light's direction.

**No `.umap` or `.uasset` file was modified.** All reads were byte-level and read-only.
