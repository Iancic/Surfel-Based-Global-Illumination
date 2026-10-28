# -*- coding: utf-8 -*-
# - David
# Credit AI Usage: This whole file is AI written. I asked Claude Code to generate an editor script
# that drops cine cameras at several angles in every test scene so I can screenshot the GI
# visualization modes from the same viewpoints. It only spawns and configures editor cameras and
# saves the levels, nothing in the surfel renderer or in the research result depends on it.
# Prompts used:
	# also using other agents for all existing scenes and the geometry place cine cams in several angles so i can test my system
	# For the scripts and everything find a place to credit also in the code
"""
PlaceTestCameras.py - place fixed comparison cameras in the Surfel GI test maps.

WHAT IT DOES
    For every map listed in CAMERA_DEFS below:
      1. Opens the level (LevelEditorSubsystem.load_level).
      2. Computes the combined world bounds of the *static geometry* only
         (lights, PlayerStart, sky sphere, cameras, LevelSequenceActor, volumes
         and info actors are excluded - see _is_static_geometry).
      3. Spawns / updates one CineCameraActor per camera definition, either at an
         absolute world position or at a position expressed relative to those
         bounds.
      4. Sets BOTH the UObject name and the actor label to the definition's
         "name", because UDevGuiSubsystem::DrawCamerasSection labels its ImGui
         radio buttons with ACameraActor::GetName() (the object name), not the
         editor label. Without the rename the runtime list shows useless entries
         like "CineCameraActor_3".
      5. Configures the cine camera for still GI comparison shots: depth of field
         off, 16:9 filmback, fixed focal length, exposure left to the scene.
      6. Saves the level.
    Re-running is safe: a camera whose object name already matches a definition is
    moved/reconfigured in place instead of being duplicated. Pre-existing cameras
    that are NOT named in CAMERA_DEFS (e.g. a map's own CineCameraActor_0) are
    never touched or deleted.

REQUIRED PROJECT CHANGE (do this once, by hand - this script does not edit it)
    Surfels.uproject currently enables only ModelingToolsEditorMode, Surfel and
    ImGui. Add PythonScriptPlugin to the "Plugins" array:

        {
            "Name": "PythonScriptPlugin",
            "Enabled": true
        }

    EditorScriptingUtilities is NOT required: this script deliberately uses only
    UnrealEd / LevelEditor subsystems, never unreal.EditorLevelLibrary or
    unreal.EditorAssetLibrary (both of which live in that plugin and are
    deprecated in UE 5.x anyway).

HOW TO RUN
    Restart the editor after enabling the plugin, then in the editor Output Log
    command box switch the mode dropdown from "Cmd" to "Python" and enter the
    full path, or stay on "Cmd" and type:

        py "C:/Users/david/Documents/Unreal Projects/Surfels/Tools/PlaceTestCameras.py"

    Save your current level first: opening another level from script runs with
    GIsRunningUnattendedScript set, so save prompts are auto-answered rather than
    shown.

UE 5.8 API NOTES (verified against the installed engine source, not guessed)
    Engine/Source/Editor/LevelEditor/Public/LevelEditorSubsystem.h
        bool LoadLevel(const FString& AssetPath)            -> load_level(asset_path)
        bool SaveCurrentLevel()                             -> save_current_level()
        bool IsInPlayInEditor() const                       -> is_in_play_in_editor()
        (SaveCurrentLevel calls FEditorFileUtils::SaveLevel, so it writes the
        package even if the dirty flag was not set.)
    Engine/Source/Editor/UnrealEd/Public/Subsystems/EditorActorSubsystem.h
        AActor* SpawnActorFromClass(TSubclassOf<AActor>, FVector Location,
                                    FRotator Rotation = ZeroRotator,
                                    bool bTransient = false)
            -> spawn_actor_from_class(actor_class, location, rotation, transient)
        TArray<AActor*> GetAllLevelActors()                 -> get_all_level_actors()
    Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h
        GetActorBounds(bool bOnlyCollidingComponents, FVector& Origin,
                       FVector& BoxExtent, bool bIncludeFromChildActors=false)
            -> get_actor_bounds(only_colliding_components, include_from_child_actors)
               -> (origin, box_extent)
        SetActorLabel(const FString&, bool bMarkDirty=true) -> set_actor_label(...)
        SetFolderPath(const FName&)                         -> set_folder_path(...)
        K2_GetComponentsByClass (ScriptName GetComponentsByClass)
            -> get_components_by_class(component_class)
    Engine/Source/Runtime/CinematicCamera/Public/CineCameraActor.h
        UCineCameraComponent* GetCineCameraComponent() const
            -> get_cine_camera_component()
    Engine/Source/Runtime/CinematicCamera/Public/CineCameraSettings.h
        ECameraFocusMethod::Disable          -> unreal.CameraFocusMethod.DISABLE
        ECameraExposureMethod::DoNotOverride -> unreal.CameraExposureMethod.DO_NOT_OVERRIDE
        (CineCameraComponent.cpp UpdateCameraLens: FocusMethod == Disable forces
        DepthOfFieldFocalDistance = 0, which is what actually kills DOF even if a
        PostProcessVolume in the level enables it. RecalcDerivedData derives both
        AspectRatio and FieldOfView from Filmback, and clamps CurrentFocalLength
        to the lens min/max - hence the explicit LensSettings below.)
    Engine/Plugins/Experimental/PythonScriptPlugin/.../PyWrapperObject.cpp
        rename(self, name="None", outer=None) -> bool
        modify(self, always_mark_dirty=True)   -> bool
    Engine/Plugins/Experimental/PythonScriptPlugin/.../PyEditor.cpp
        unreal.get_editor_subsystem(subsystem)

    THINGS I COULD NOT FULLY VERIFY (handled defensively in the code):
    * K2_SetActorLocation / K2_SetActorLocationAndRotation / K2_SetActorTransform
      all take an FHitResult& SweepHitResult out-parameter, so their Python
      return value may be a plain bool or a (bool, HitResult) tuple depending on
      how the glue folds out-params. The return values are simply ignored here,
      so either shape is fine.
    * Object.rename() can fail if a *pending-kill* object already owns the target
      name in the same outer. _rename_actor() logs a warning and carries on with
      the engine-assigned name in that case rather than aborting.
    * ABrush::BrushType is a UPROPERTY but the pythonized enum entry spelling
      (unreal.BrushType.BRUSH_DEFAULT) is inferred, not confirmed; the builder
      brush test compares the enum's numeric .value and falls back to a name
      test, inside a try/except.
    * ProceduralMeshComponent / DynamicMeshComponent only exist when their
      plugins are loaded, so all component and actor classes are resolved with
      getattr(unreal, ...) and silently skipped when absent.
"""

import math
import os

import unreal


# ---------------------------------------------------------------------------
# Camera definition table
# ---------------------------------------------------------------------------
# Schema for one camera definition (a plain dict):
#
#   "name"            (required) str. Used as BOTH the UObject name and the
#                     actor label, and is the key used for idempotent updates.
#                     This is the string that shows up in the ImGui camera list.
#
#   Placement - give exactly one of:
#   "location"        (x, y, z) absolute world position in Unreal units.
#   "rel_location"    (rx, ry, rz) bounds-relative position. Resolved as
#                         world = bounds_center + (r * bounds_extent)
#                     where bounds_extent is the HALF size of the static
#                     geometry bounds. So (0, 0, 0) is the middle of the scene
#                     and (0, -2, 0.35) is two half-depths out along -Y, a bit up.
#
#   "location_offset" (optional) (dx, dy, dz) absolute offset in Unreal units,
#                     added after the placement above is resolved. Handy for
#                     nudging a bounds-relative camera.
#
#   Aim - give at most one of:
#   "rotation"        (pitch, yaw, roll) in degrees, absolute.
#   "look_at"         (x, y, z) absolute world point to aim at.
#   "rel_look_at"     (rx, ry, rz) bounds-relative point to aim at, resolved the
#                     same way as "rel_location".
#   (If none is given the camera aims at the bounds center.)
#
#   Lens - give at most one of:
#   "focal_length"    mm. Defaults to DEFAULT_FOCAL_LENGTH_MM.
#   "fov"             horizontal field of view in degrees; converted to a focal
#                     length for the 16:9 filmback below.
#
#   "folder"          (optional) World Outliner folder. Defaults to CAMERA_FOLDER.
#
# A map whose value is None uses DEFAULT_CAMERA_SET. Drop a scene-specific list
# in place of the None to take over that map - mixing absolute and
# bounds-relative entries in the same list is fine, e.g.
#
#   "/Game/DynamicSponza": [
#       {"name": "Cam_Sponza_Atrium", "location": (-1200.0, 0.0, 250.0),
#        "rotation": (-5.0, 0.0, 0.0), "focal_length": 21.0},
#       {"name": "Cam_Sponza_Corner", "rel_location": (-1.8, -1.8, 0.55),
#        "rel_look_at": (0.0, 0.0, 0.0), "fov": 75.0},
#   ],

DEFAULT_CAMERA_SET = [
    {
        # Straight-on establishing shot from -Y, slightly above center.
        "name": "Cam_Front",
        "rel_location": (0.0, -2.2, 0.35),
        "rel_look_at": (0.0, 0.0, 0.0),
        "focal_length": 24.0,
    },
    {
        # 45 degree corner view - the most useful single angle for comparing
        # indirect bounce on two walls at once.
        "name": "Cam_Corner",
        "rel_location": (-1.8, -1.8, 0.55),
        "rel_look_at": (0.0, 0.0, 0.0),
        "focal_length": 28.0,
    },
    {
        # Side elevation from +X.
        "name": "Cam_Side",
        "rel_location": (2.2, 0.0, 0.3),
        "rel_look_at": (0.0, 0.0, 0.0),
        "focal_length": 24.0,
    },
    {
        # High three-quarter looking down - shows floor bounce / AO contact.
        "name": "Cam_High",
        "rel_location": (0.0, -1.4, 1.8),
        "rel_look_at": (0.0, 0.0, -0.2),
        "focal_length": 20.0,
    },
    {
        # Inside the scene, wide, aimed into the opposite corner. This is the
        # angle that exposes light leaking through walls.
        "name": "Cam_Interior",
        "rel_location": (0.55, 0.55, 0.1),
        "rel_look_at": (-0.9, -0.9, -0.25),
        "focal_length": 18.0,
    },
]

# Same order as GDevScenes in DevGuiSubsystem.cpp.
#
# /Game/TestMap is deliberately absent: it is not a level. It is a 1219-byte
# ObjectRedirector pointing at /Game/Demo, and Content/Demo.umap does not exist
# (only orphaned Content/__ExternalActors__/Demo folders remain). Loading it
# fails. It is also not in GDevScenes, so it has no runtime button either.
#
# Credit Claude Code: every coordinate, rotation and focal length below is AI
# derived. They were computed from geometry decoded out of the .umap files, not
# measured in the editor, and no frame below has been looked at. The per-scene
# reasoning, the sun-direction analysis each framing is based on, and the list
# of figures that were estimated rather than decoded are in
# Tools/scene-framings.md - read its "Decoding confidence" section before
# trusting any single number. Expect to nudge these in the editor.
CAMERA_DEFS = {
    # Sponza as it is set up now: Sponza_mesh_0 at actor scale 0.01, location
    # (0, 0, 40). World = (0, 0, 40) + 0.01 * mesh-local, so the building spans
    # x -1921..1800, y -1183..1105, z 40..1469 (mesh-local z 0 is the floor, the
    # -12644 minimum is an outlier) and the open nave centreline is y ~ -35.
    # These are the original DynamicSponza framings (scale 0.008, floor at z=10)
    # moved onto the new scale: x/y * 1.25 and z = 40 + (z - 10) * 1.25.
    # The dragon (centre ~(1007, 0, 164), box ~456 x 392 x 296) took the old
    # cone's place. The mesh's two lion heads sit on the end walls at y ~ -35,
    # z ~ 120..305: west one x -1427..-1352 (faces +X), east one x 1224..1299
    # (faces -X, hidden behind the dragon), found by the lion-textured material
    # section's vertices, so CineCam_Lion aims at the unobstructed west one.
    "/Game/Sponza": [
        {"name": "CineCam_Overview", "location": (-1275.0, -37.5, 402.5),
         "rotation": (-10.0, 0.0, 0.0), "focal_length": 14.0},
        {"name": "CineCam_Arcade", "location": (-187.5, -37.5, 265.0),
         "rotation": (-6.0, 90.0, 0.0), "focal_length": 24.0},
        {"name": "CineCam_SunPool", "location": (250.0, -37.5, 177.5),
         "rotation": (-20.0, 60.0, 0.0), "focal_length": 18.0},
        {"name": "CineCam_Dragon", "location": (330.0, -37.5, 250.0),
         "rotation": (-7.0, 3.0, 0.0), "focal_length": 24.0},
        {"name": "CineCam_EndWall", "location": (1350.0, -37.5, 265.0),
         "rotation": (-4.0, 0.0, 0.0), "focal_length": 20.0},
        # From the middle of the nave straight down the long axis at the west
        # lion head (centre ~(-1352, -35, 212)), ~1.3 km of atrium in between.
        {"name": "CineCam_Lion", "location": (-60.0, -35.0, 180.0),
         "look_at": (-1352.0, -35.0, 212.0), "focal_length": 35.0},
    ],
    # Box is open on -X, green wall on +Y, red on -Y. The ceiling and the green
    # wall take zero direct light, so all bleed comes off the sunlit floor.
    "/Game/CornellBox": [
        {"name": "CineCam_Overview", "location": (-820.0, 0.0, 325.0),
         "rotation": (0.0, 0.0, 0.0), "focal_length": 20.0},
        {"name": "CineCam_ColourBleed", "location": (-265.0, 0.0, 200.0),
         "rotation": (-10.0, 0.0, 0.0), "focal_length": 16.0},
        {"name": "CineCam_Ceiling", "location": (-255.0, 0.0, 120.0),
         "rotation": (35.0, 0.0, 0.0), "focal_length": 18.0},
        {"name": "CineCam_Corner", "location": (-250.0, -120.0, 300.0),
         "rotation": (29.0, 40.0, 0.0), "focal_length": 24.0},
    ],
    # DirectionalLight_0 here is Intensity 0; DirectionalLight2 is the real sun.
    # The dragon is RedMaterial against neutral grey panels, so this is the best
    # colour-bleed subject in the project.
    "/Game/Dragon": [
        {"name": "CineCam_Overview", "location": (300.0, 720.0, 290.0),
         "rotation": (-6.0, -90.0, 0.0), "focal_length": 24.0},
        {"name": "CineCam_RedBleed", "location": (100.0, 400.0, 260.0),
         "rotation": (-2.0, -60.0, 0.0), "focal_length": 24.0},
        {"name": "CineCam_Shadow", "location": (300.0, -820.0, 230.0),
         "rotation": (-3.0, 90.0, 0.0), "focal_length": 28.0},
        {"name": "CineCam_Closeup", "location": (170.0, 230.0, 250.0),
         "rotation": (-4.0, -67.0, 0.0), "focal_length": 35.0},
    ],
    # Free-space checks for this map are AABB-only against a sparse import, so
    # these are the likeliest of the set to need nudging.
    "/Game/ConferenceRoom": [
        {"name": "CineCam_Overview", "location": (-400.0, -400.0, 300.0),
         "rotation": (-7.0, 16.0, 0.0), "focal_length": 18.0},
        {"name": "CineCam_Window", "location": (500.0, -600.0, 210.0),
         "rotation": (-2.0, 80.0, 0.0), "focal_length": 24.0},
        {"name": "CineCam_Table", "location": (-250.0, -450.0, 250.0),
         "rotation": (-5.0, 22.0, 0.0), "focal_length": 24.0},
        {"name": "CineCam_Corner", "location": (900.0, -300.0, 210.0),
         "rotation": (-4.0, -44.0, 0.0), "focal_length": 24.0},
    ],
    # Window position is inferred from frame-like mesh shapes, not decoded.
    "/Game/LivingRoom": [
        {"name": "CineCam_Overview", "location": (28.0, 120.0, 200.0),
         "rotation": (-5.0, 90.0, 0.0), "focal_length": 16.0},
        {"name": "CineCam_Window", "location": (28.0, 600.0, 170.0),
         "rotation": (-2.0, -90.0, 0.0), "focal_length": 18.0},
        {"name": "CineCam_Corner", "location": (-150.0, 650.0, 120.0),
         "rotation": (22.0, 20.0, 0.0), "focal_length": 18.0},
        {"name": "CineCam_Lamps", "location": (28.0, 400.0, 150.0),
         "rotation": (18.0, -90.0, 0.0), "focal_length": 20.0},
    ],
}


# ---------------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------------

# 16:9 sensor. UCineCameraComponent::RecalcDerivedData derives both AspectRatio
# and FieldOfView from the filmback, so setting the sensor is what actually
# makes the framing 16:9.
#
# This is the stock CineCameraActor filmback, and it is deliberately NOT the
# 36.0 x 20.25 "digital film" sensor: every CineCameraActor already placed in
# these maps serialises FieldOfView = 37.4974, which is exactly 35 mm on a
# 23.76 mm sensor. The focal lengths in CAMERA_DEFS below were computed against
# this sensor, and keeping it means new cameras frame identically to the
# author's existing ones. Changing it silently rescales every framing: a 14 mm
# shot is 80.6 degrees here and 104.2 degrees on a 36 mm sensor.
SENSOR_WIDTH_MM = 23.76
SENSOR_HEIGHT_MM = 13.365          # 23.76 / (16/9)

DEFAULT_FOCAL_LENGTH_MM = 24.0

# RecalcDerivedData clamps CurrentFocalLength to [MinFocalLength, MaxFocalLength],
# so the lens has to be opened up or short focal lengths get silently clamped.
LENS_MIN_FOCAL_MM = 4.0
LENS_MAX_FOCAL_MM = 1000.0
LENS_APERTURE = 2.8                # cosmetic only; DOF is disabled below

# True letterboxes the view to 16:9 regardless of window size, which keeps
# framing identical between screenshots. Set False if the black bars annoy you.
CONSTRAIN_ASPECT_RATIO = True

CAMERA_FOLDER = "TestCameras"

# Anything with a half-extent bigger than this is treated as sky/backdrop and
# left out of the scene bounds (the engine sky sphere is ~400000 uu).
SKY_EXTENT_LIMIT = 100000.0

# Used when a map has no usable static geometry at all.
FALLBACK_EXTENT = unreal.Vector(x=500.0, y=500.0, z=250.0)

# Actor classes that never count as scene geometry. Resolved via getattr so a
# missing class (unloaded plugin) is just skipped.
SKIP_ACTOR_CLASS_NAMES = (
    "Light",                 # ALight: directional / point / spot / rect
    "LightmassPortal",
    "PlayerStart",
    "CameraActor",           # covers ACineCameraActor
    "SceneCapture",
    "SceneCapture2D",
    "SceneCaptureCube",
    "SkyAtmosphere",
    "VolumetricCloud",
    "ExponentialHeightFog",
    "AtmosphericFog",
    "LevelSequenceActor",
    "DecalActor",
    "Volume",                # post process, lightmass importance, nav bounds...
    "Info",                  # AWorldSettings, ASkyLight, AReflectionCapture...
    "ReflectionCapture",
    "Note",
    "TargetPoint",
    "AmbientSound",
)

# An actor only contributes to the bounds if it owns one of these.
GEOMETRY_COMPONENT_CLASS_NAMES = (
    "StaticMeshComponent",   # also covers InstancedStaticMeshComponent
    "SkeletalMeshComponent",
    "BrushComponent",        # BSP - the test rooms are built from brushes
    "ProceduralMeshComponent",
    "DynamicMeshComponent",
)

# Case-insensitive substrings that mark an actor as backdrop rather than geometry.
SKIP_NAME_SUBSTRINGS = ("sky", "atmosphere", "fog", "cloud", "backdrop")

# Static meshes under these paths are backdrops, not scene geometry.
SKY_MESH_PATH_FRAGMENTS = ("/engine/enginesky/", "/engine/editormeshes/")


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def _uclass(class_name):
    """unreal.<class_name> or None when the owning module is not loaded."""
    return getattr(unreal, class_name, None)


def _resolved_classes(class_names):
    return tuple(c for c in (_uclass(n) for n in class_names) if c is not None)


SKIP_ACTOR_CLASSES = _resolved_classes(SKIP_ACTOR_CLASS_NAMES)
GEOMETRY_COMPONENT_CLASSES = _resolved_classes(GEOMETRY_COMPONENT_CLASS_NAMES)


def _vec(triple):
    return unreal.Vector(x=float(triple[0]), y=float(triple[1]), z=float(triple[2]))


def _focal_length_from_fov(fov_degrees):
    """Inverse of UCineCameraComponent's 2*atan(SensorWidth / (2*FocalLength))."""
    fov_degrees = max(1.0, min(170.0, float(fov_degrees)))
    return SENSOR_WIDTH_MM / (2.0 * math.tan(math.radians(fov_degrees) * 0.5))


def _look_at_rotator(from_location, to_location):
    """Pitch/yaw that aims +X of from_location at to_location. Roll stays 0."""
    dx = to_location.x - from_location.x
    dy = to_location.y - from_location.y
    dz = to_location.z - from_location.z
    horizontal = math.sqrt(dx * dx + dy * dy)
    yaw = math.degrees(math.atan2(dy, dx))
    if horizontal > 1e-4:
        pitch = math.degrees(math.atan2(dz, horizontal))
    else:
        pitch = 90.0 if dz > 0.0 else -90.0
    return unreal.Rotator(roll=0.0, pitch=pitch, yaw=yaw)


# ---------------------------------------------------------------------------
# Scene bounds
# ---------------------------------------------------------------------------

def _is_builder_brush(actor):
    """The level's builder brush is an ABrush with BrushType == Brush_Default."""
    brush_class = _uclass("Brush")
    if brush_class is None or not isinstance(actor, brush_class):
        return False
    try:
        brush_type = actor.get_editor_property("brush_type")
    except Exception:
        return actor.get_name().lower() == "brush"
    try:
        # Brush_Default == 0 -> builder brush.
        return int(brush_type.value) == 0
    except Exception:
        return "DEFAULT" in str(brush_type).upper()


def _has_backdrop_mesh(actor):
    """True when every static mesh on the actor is an engine sky/editor mesh."""
    smc_class = _uclass("StaticMeshComponent")
    if smc_class is None:
        return False
    try:
        components = actor.get_components_by_class(smc_class)
    except Exception:
        return False
    mesh_paths = []
    for component in components:
        try:
            mesh = component.get_editor_property("static_mesh")
        except Exception:
            mesh = None
        if mesh is not None:
            mesh_paths.append(mesh.get_path_name().lower())
    if not mesh_paths:
        return False
    return all(
        any(fragment in path for fragment in SKY_MESH_PATH_FRAGMENTS)
        for path in mesh_paths
    )


def _has_geometry_component(actor):
    for component_class in GEOMETRY_COMPONENT_CLASSES:
        try:
            if actor.get_components_by_class(component_class):
                return True
        except Exception:
            continue
    return False


def _is_static_geometry(actor):
    if actor is None:
        return False
    if SKIP_ACTOR_CLASSES and isinstance(actor, SKIP_ACTOR_CLASSES):
        return False
    if _is_builder_brush(actor):
        return False

    haystack = " ".join((
        actor.get_name(),
        actor.get_actor_label(),
        actor.get_class().get_name(),
    )).lower()
    if any(fragment in haystack for fragment in SKIP_NAME_SUBSTRINGS):
        return False

    if not _has_geometry_component(actor):
        return False
    if _has_backdrop_mesh(actor):
        return False
    return True


def compute_scene_bounds(actors):
    """Combined world AABB of the static geometry. Returns (center, extent, count)."""
    mins = [None, None, None]
    maxs = [None, None, None]
    counted = 0

    for actor in actors:
        if not _is_static_geometry(actor):
            continue

        origin, box_extent = actor.get_actor_bounds(False, False)
        largest = max(box_extent.x, box_extent.y, box_extent.z)

        if largest <= 0.0:
            continue
        if largest > SKY_EXTENT_LIMIT:
            unreal.log_warning(
                "  [bounds] skipping oversized actor '%s' (half-extent %.0f uu) "
                "- treated as backdrop" % (actor.get_name(), largest))
            continue

        lo = (origin.x - box_extent.x, origin.y - box_extent.y, origin.z - box_extent.z)
        hi = (origin.x + box_extent.x, origin.y + box_extent.y, origin.z + box_extent.z)
        for axis in range(3):
            mins[axis] = lo[axis] if mins[axis] is None else min(mins[axis], lo[axis])
            maxs[axis] = hi[axis] if maxs[axis] is None else max(maxs[axis], hi[axis])
        counted += 1

    if counted == 0:
        return unreal.Vector(x=0.0, y=0.0, z=0.0), FALLBACK_EXTENT, 0

    center = unreal.Vector(
        x=(mins[0] + maxs[0]) * 0.5,
        y=(mins[1] + maxs[1]) * 0.5,
        z=(mins[2] + maxs[2]) * 0.5,
    )
    extent = unreal.Vector(
        x=max((maxs[0] - mins[0]) * 0.5, 1.0),
        y=max((maxs[1] - mins[1]) * 0.5, 1.0),
        z=max((maxs[2] - mins[2]) * 0.5, 1.0),
    )
    return center, extent, counted


# ---------------------------------------------------------------------------
# Definition -> transform
# ---------------------------------------------------------------------------

def _relative_point(triple, center, extent):
    return unreal.Vector(
        x=center.x + float(triple[0]) * extent.x,
        y=center.y + float(triple[1]) * extent.y,
        z=center.z + float(triple[2]) * extent.z,
    )


def resolve_transform(definition, center, extent):
    """Returns (location, rotation) for one camera definition."""
    if "location" in definition:
        location = _vec(definition["location"])
    elif "rel_location" in definition:
        location = _relative_point(definition["rel_location"], center, extent)
    else:
        raise ValueError("camera '%s' needs either 'location' or 'rel_location'"
                         % definition.get("name", "<unnamed>"))

    offset = definition.get("location_offset")
    if offset:
        location = unreal.Vector(
            x=location.x + float(offset[0]),
            y=location.y + float(offset[1]),
            z=location.z + float(offset[2]),
        )

    if "rotation" in definition:
        pitch, yaw, roll = definition["rotation"]
        rotation = unreal.Rotator(roll=float(roll), pitch=float(pitch), yaw=float(yaw))
    else:
        if "look_at" in definition:
            target = _vec(definition["look_at"])
        elif "rel_look_at" in definition:
            target = _relative_point(definition["rel_look_at"], center, extent)
        else:
            target = center
        rotation = _look_at_rotator(location, target)

    return location, rotation


# ---------------------------------------------------------------------------
# Camera creation / configuration
# ---------------------------------------------------------------------------

def _rename_actor(actor, desired_name):
    """Set the UObject name (what DrawCamerasSection shows) and the label."""
    renamed = True
    if actor.get_name() != desired_name:
        try:
            renamed = bool(actor.rename(desired_name))
        except Exception as exc:
            renamed = False
            unreal.log_warning("  rename('%s') raised: %s" % (desired_name, exc))
        if not renamed:
            unreal.log_warning(
                "  could not rename to '%s' (name probably still held by a "
                "pending-kill object); the ImGui list will show '%s' instead"
                % (desired_name, actor.get_name()))
    actor.set_actor_label(desired_name, True)
    return renamed


def configure_cine_camera(camera_actor, definition):
    """Still-comparison setup: no DOF, 16:9 filmback, fixed focal length."""
    component = camera_actor.get_cine_camera_component()

    filmback = unreal.CameraFilmbackSettings()
    filmback.set_editor_property("sensor_width", SENSOR_WIDTH_MM)
    filmback.set_editor_property("sensor_height", SENSOR_HEIGHT_MM)
    filmback.set_editor_property("sensor_horizontal_offset", 0.0)
    filmback.set_editor_property("sensor_vertical_offset", 0.0)
    component.set_editor_property("filmback", filmback)

    lens = unreal.CameraLensSettings()
    lens.set_editor_property("min_focal_length", LENS_MIN_FOCAL_MM)
    lens.set_editor_property("max_focal_length", LENS_MAX_FOCAL_MM)
    lens.set_editor_property("min_f_stop", LENS_APERTURE)
    lens.set_editor_property("max_f_stop", 22.0)
    component.set_editor_property("lens_settings", lens)

    # ECameraFocusMethod::Disable forces DepthOfFieldFocalDistance to 0 in
    # UpdateCameraLens, which also overrides any PostProcessVolume DOF in the
    # level. That is exactly what we want for GI screenshots.
    focus = unreal.CameraFocusSettings()
    focus.set_editor_property("focus_method", unreal.CameraFocusMethod.DISABLE)
    focus.set_editor_property("smooth_focus_changes", False)
    component.set_editor_property("focus_settings", focus)

    if "fov" in definition:
        focal_length = _focal_length_from_fov(definition["fov"])
    else:
        focal_length = float(definition.get("focal_length", DEFAULT_FOCAL_LENGTH_MM))
    component.set_editor_property("current_focal_length", focal_length)
    component.set_editor_property("current_aperture", LENS_APERTURE)

    # Leave exposure to the scene's post process so GI modes are compared under
    # identical exposure (the cine camera default is Enabled, which would drive
    # exposure from the aperture set above).
    try:
        component.set_editor_property(
            "exposure_method", unreal.CameraExposureMethod.DO_NOT_OVERRIDE)
    except Exception as exc:
        unreal.log_warning("  could not set exposure_method: %s" % exc)

    component.set_editor_property("constrain_aspect_ratio", CONSTRAIN_ASPECT_RATIO)

    return focal_length


def place_camera(actor_subsystem, existing_by_name, definition, center, extent):
    """Spawn or update one camera. Returns (action, name, focal_length)."""
    name = definition["name"]
    location, rotation = resolve_transform(definition, center, extent)

    camera = existing_by_name.get(name)
    if camera is not None and not isinstance(camera, unreal.CineCameraActor):
        unreal.log_warning(
            "  '%s' already exists but is a %s, not a CineCameraActor - skipped"
            % (name, camera.get_class().get_name()))
        return "skipped", name, 0.0

    if camera is None:
        camera = actor_subsystem.spawn_actor_from_class(
            unreal.CineCameraActor, location, rotation, False)
        if camera is None:
            unreal.log_warning("  failed to spawn CineCameraActor for '%s'" % name)
            return "failed", name, 0.0
        _rename_actor(camera, name)
        action = "created"
    else:
        camera.modify(True)
        # Return values ignored on purpose: these K2_ functions carry an
        # FHitResult out-param, so the Python glue may hand back a tuple.
        camera.set_actor_location(location, False, True)
        camera.set_actor_rotation(rotation, False)
        camera.set_actor_label(name, True)
        action = "updated"

    focal_length = configure_cine_camera(camera, definition)

    folder = definition.get("folder", CAMERA_FOLDER)
    if folder:
        try:
            camera.set_folder_path(folder)
        except Exception as exc:
            unreal.log_warning("  could not set folder '%s': %s" % (folder, exc))

    existing_by_name[name] = camera
    return action, name, focal_length


# ---------------------------------------------------------------------------
# Per-map driver
# ---------------------------------------------------------------------------

def process_map(level_subsystem, actor_subsystem, map_path, definitions):
    report = {
        "map": map_path,
        "loaded": False,
        "saved": False,
        "geometry_actors": 0,
        "center": None,
        "extent": None,
        "cameras": [],
        "pre_existing_untouched": [],
    }

    unreal.log("=== %s ===" % map_path)
    if not level_subsystem.load_level(map_path):
        unreal.log_warning("  could not load level (does the package exist?) - skipped")
        return report
    report["loaded"] = True

    actors = list(actor_subsystem.get_all_level_actors())
    center, extent, counted = compute_scene_bounds(actors)
    report["geometry_actors"] = counted
    report["center"] = center
    report["extent"] = extent

    if counted == 0:
        unreal.log_warning(
            "  no static geometry found - falling back to the origin with a "
            "%.0f/%.0f/%.0f uu half-extent" % (extent.x, extent.y, extent.z))
    unreal.log(
        "  static geometry: %d actor(s)  center=(%.1f, %.1f, %.1f)  "
        "half-extent=(%.1f, %.1f, %.1f)"
        % (counted, center.x, center.y, center.z, extent.x, extent.y, extent.z))

    existing_by_name = {}
    for actor in actors:
        existing_by_name[actor.get_name()] = actor

    wanted_names = set(d["name"] for d in definitions)
    for actor in actors:
        if isinstance(actor, unreal.CameraActor) and actor.get_name() not in wanted_names:
            report["pre_existing_untouched"].append(actor.get_name())

    for definition in definitions:
        action, name, focal_length = place_camera(
            actor_subsystem, existing_by_name, definition, center, extent)
        report["cameras"].append((action, name, focal_length))
        if action in ("created", "updated"):
            camera = existing_by_name[name]
            loc = camera.get_actor_location()
            rot = camera.get_actor_rotation()
            unreal.log(
                "  %-8s %-16s loc=(%.1f, %.1f, %.1f)  rot=(p %.1f, y %.1f)  %.1fmm"
                % (action, camera.get_name(), loc.x, loc.y, loc.z,
                   rot.pitch, rot.yaw, focal_length))

    if level_subsystem.save_current_level():
        report["saved"] = True
        unreal.log("  level saved")
    else:
        unreal.log_warning("  save_current_level() returned False - level NOT saved")

    return report


def print_summary(reports):
    unreal.log("")
    unreal.log("================ PlaceTestCameras summary ================")
    total_created = 0
    total_updated = 0
    for report in reports:
        if not report["loaded"]:
            unreal.log("%-24s  NOT LOADED" % report["map"])
            continue

        created = [n for a, n, _f in report["cameras"] if a == "created"]
        updated = [n for a, n, _f in report["cameras"] if a == "updated"]
        other = [(a, n) for a, n, _f in report["cameras"]
                 if a not in ("created", "updated")]
        total_created += len(created)
        total_updated += len(updated)

        unreal.log("%-24s  geometry actors: %-4d  saved: %s"
                   % (report["map"], report["geometry_actors"],
                      "yes" if report["saved"] else "NO"))
        center = report["center"]
        extent = report["extent"]
        unreal.log("    bounds center (%.1f, %.1f, %.1f)  half-extent (%.1f, %.1f, %.1f)"
                   % (center.x, center.y, center.z, extent.x, extent.y, extent.z))
        if created:
            unreal.log("    created: %s" % ", ".join(created))
        if updated:
            unreal.log("    updated: %s" % ", ".join(updated))
        for action, name in other:
            unreal.log_warning("    %s: %s" % (action, name))
        if report["pre_existing_untouched"]:
            unreal.log("    left alone (pre-existing): %s"
                       % ", ".join(report["pre_existing_untouched"]))

    unreal.log("----------------------------------------------------------")
    unreal.log("%d camera(s) created, %d updated across %d map(s)."
               % (total_created, total_updated, len(reports)))
    unreal.log("Switch between them at runtime from the Surfel dev GUI "
               "'Cameras' section (PIE only).")
    unreal.log("==========================================================")


def main():
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if level_subsystem is None or actor_subsystem is None:
        unreal.log_error("Editor subsystems unavailable - run this from the editor, "
                         "not a cooked build or a -game commandlet.")
        return

    if level_subsystem.is_in_play_in_editor():
        unreal.log_error("Stop Play In Editor before running PlaceTestCameras.")
        return

    # Optional: SURFEL_CAM_MAPS="/Game/Sponza,/Game/Dragon" limits the run to those maps
    only = [m.strip() for m in os.environ.get("SURFEL_CAM_MAPS", "").split(",") if m.strip()]

    reports = []
    for map_path, definitions in CAMERA_DEFS.items():
        if only and map_path not in only:
            continue
        if definitions is None:
            definitions = DEFAULT_CAMERA_SET
        reports.append(
            process_map(level_subsystem, actor_subsystem, map_path, definitions))

    print_summary(reports)


# Executed as a one-shot tool (py <path>), so there is no __main__ guard: the
# Python plugin's file execution scope does not reliably name the module
# "__main__".
main()
