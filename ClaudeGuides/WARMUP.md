# Warmup — Project Architecture & Your First Compute Shader

**Start here.** This is the tutorial before [TUTORIAL.md](TUTORIAL.md).

It answers two questions:

1. **What are all these files, and why?** — the architecture of an Unreal C++ project with a shader plugin.
2. **What is the smallest compute shader that actually does something?** — one that fills a texture with a gradient, and nothing more.

No renderer hooks, no G-Buffer, no SceneViewExtension. You call a function, a texture
gets filled in, you look at it. Once that clicks, `TUTORIAL.md` takes the same four
pieces and points them at the real scene.

Written for UE **5.8.1**. Everything here is in the repo and compiles.

---

## Table of contents

- [Part 1 — How an Unreal project is laid out](#part-1--how-an-unreal-project-is-laid-out)
- [Part 2 — What a module actually is](#part-2--what-a-module-actually-is)
- [Part 3 — Why this is a plugin and not just project code](#part-3--why-this-is-a-plugin-and-not-just-project-code)
- [Part 4 — The two threads](#part-4--the-two-threads)
- [Part 5 — The warmup shader, end to end](#part-5--the-warmup-shader-end-to-end)
- [Part 6 — Running it](#part-6--running-it)
- [Part 7 — Experiments](#part-7--experiments)
- [Part 8 — What's in this plugin now](#part-8--whats-in-this-plugin-now)
- [Part 9 — Vocabulary](#part-9--vocabulary)

---

## Part 1 — How an Unreal project is laid out

Here is the project, with the folders that matter and the ones you can ignore:

```
Surfels/
├── Surfels.uproject          The project descriptor. Engine version + plugin list.
├── Config/                   .ini settings (renderer, input, packaging)
├── Content/                  Assets: levels, materials, textures. Binary .uasset files.
├── Source/                   The PROJECT's own C++
│   ├── Surfels.Target.cs         How to build the game
│   ├── SurfelsEditor.Target.cs   How to build the editor
│   └── Surfels/                  The project's game module
├── Plugins/                  Self-contained chunks of functionality
│   ├── Surfel/                   <- our plugin, where all the work happens
│   └── SceneViewExtensionUtilities/  (disabled - see note below)
│
├── Binaries/                 Compiled .dll / .pdb output
├── Intermediate/             Build scratch: generated headers, object files
├── DerivedDataCache/         Cooked/compiled asset + shader cache
└── Saved/                    Logs, autosaves, config backups
```

### The three you can delete

`Binaries`, `Intermediate`, and `DerivedDataCache` are all **regenerated**. If a build
goes strange in a way you cannot explain, deleting `Binaries` and `Intermediate` and
rebuilding is the standard first move. Never commit them to source control.

`Saved` holds your logs — `Saved/Logs/Surfels.log` is where shader compile errors and
`UE_LOG` output end up, and you will read it often.

### The `.uproject`

```json
{
	"FileVersion": 3,
	"EngineAssociation": "{25B76ABB-43F8-B3F2-D44A-2DAA54D957EE}",
	"Modules": [
		{ "Name": "Surfels", "Type": "Runtime", "LoadingPhase": "Default" }
	],
	"Plugins": [
		{ "Name": "Surfel", "Enabled": true },
		{ "Name": "SceneViewExtensionTemplate", "Enabled": false }
	]
}
```

The GUID in `EngineAssociation` points at a **source build** registered in the Windows
registry — yours resolves to `C:/GameDevelopment/Unreal/UnrealEngine-release`. A
launcher-installed engine would show a plain version string like `"5.8"` instead.

> **Note on `SceneViewExtensionTemplate`:** it is disabled deliberately. That template's
> `CustomSceneViewExtension.h` contains literal `...` placeholders
> (`static bool ShouldCompilePermutation(...) { ... }`), so it is documentation, not
> compilable code. Its `.cpp` is a genuinely useful reference to read — but the plugin
> cannot build as shipped, which is why the working code lives in `Surfel` instead.

---

## Part 2 — What a module actually is

A **module** is one compiled DLL plus the rules for building it. It is the unit Unreal
links, loads, and hot-reloads. Both the project and the plugin have one.

Three files define a module:

### `Surfel.Build.cs` — the build rules

Despite the extension, this is **C# executed by UnrealBuildTool at build time**, not
code that ships. Its job is to answer "what does this module need?"

```csharp
public class Surfel : ModuleRules
{
    public Surfel(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",         // FString, TArray, math types
                "CoreUObject",  // UObject, UCLASS, reflection
                "Engine",       // AActor, UWorld, render targets
                "RenderCore",   // FGlobalShader, RDG, shader parameter macros
                "RHI",          // Render Hardware Interface: pixel formats, texture flags
                "Renderer",     // FSceneViewExtensionBase (used by the main tutorial)
                "Projects",     // IPluginManager, for the shader path mapping
            }
            );
    }
}
```

**Public vs. Private dependencies:** if a dependency appears in your *header* files, it
is public — anyone including your headers needs it too. If it only appears in `.cpp`
files, it is private. Getting this wrong produces "cannot open include file" errors in
whatever module depends on yours.

`Linker error? Add the module here.` is the single most common fix in Unreal C++.

### `Surfel.h` / `Surfel.cpp` — the module's lifecycle

```cpp
class FSurfelModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
```

`StartupModule` runs when the DLL loads; `ShutdownModule` when it unloads. Ours does
exactly one thing, and it is the thing that makes shaders work at all:

```cpp
void FSurfelModule::StartupModule()
{
	AddShaderSourceDirectoryMapping(
		TEXT("/Surfel"),
		IPluginManager::Get().FindPlugin("Surfel")->GetBaseDir() + "/Shaders/Private");
}

IMPLEMENT_MODULE(FSurfelModule, Surfel)
```

**Virtual shader paths.** Unreal never references shaders by disk path. It uses virtual
paths starting with `/`. `/Engine/...` is built in; every plugin registers its own. This
line is what makes `"/Surfel/MyShader.usf"` resolve to a real file.

`IMPLEMENT_MODULE` is the entry point macro. Exactly one per module, in a `.cpp`.

---

## Part 3 — Why this is a plugin and not just project code

You *could* put all of this in `Source/Surfels/`. Reasons not to:

| | Project module | Plugin |
|---|---|---|
| Reusable in another project | Copy files by hand | Copy one folder |
| Can be toggled off | No | Yes, one checkbox |
| Owns its own `Shaders/` folder | Awkward | Natural |
| Controls its own load timing | Tied to the project | Per-plugin `LoadingPhase` |

That last row is not a nicety — it is a hard requirement here.

### The `LoadingPhase` trap

```json
"Modules": [
    { "Name": "Surfel", "Type": "Runtime", "LoadingPhase": "PostConfigInit" }
]
```

The shader path mapping must be registered **before** the shader compiler starts. The
default `"Default"` phase runs too late, and you get:

```
Shader file not found: /Surfel/MyShader.usf
```

This plugin was originally on `"Default"` and had to be changed. If you create a new
shader plugin and hit that error, this is why — it is not your shader, it is your
loading phase.

### Layout of the plugin

```
Plugins/Surfel/
├── Surfel.uplugin                        Descriptor: modules, loading phase
├── Shaders/Private/                      .usf files live here
│   ├── MyShader.usf                          <- the warmup shader
│   ├── FullscreenCS.usf                      (main tutorial, stage 1)
│   └── GBufferVisualizeCS.usf                (main tutorial, stage 2)
└── Source/Surfel/
    ├── Surfel.Build.cs
    ├── Public/                           Headers other modules may include
    │   ├── Surfel.h
    │   ├── SurfelWarmupLibrary.h             <- the warmup entry point
    │   └── SurfelSubsystem.h
    └── Private/                          Implementation, not visible outside
        ├── Surfel.cpp
        ├── SurfelWarmupLibrary.cpp           <- the warmup C++
        ├── SurfelSceneViewExtension.h/.cpp
        └── SurfelSubsystem.cpp
```

**Public vs. Private folders** mirror the dependency idea: `Public/` headers are on the
include path for anything depending on this module; `Private/` is yours alone. When in
doubt, put it in `Private/` — you can always promote it later.

---

## Part 4 — The two threads

This is the concept that causes the most beginner crashes, so it comes before the code.

Unreal runs your game across several threads. Two matter here:

- **Game thread** — gameplay, `Tick`, actors, Blueprint. Where "your code" normally runs.
- **Render thread** — translates the scene into GPU commands.

**The render thread runs roughly one frame behind the game thread.** They are not in
lockstep; that lag is what keeps the GPU fed.

The consequence:

> Never touch a `UObject` from render-thread code. By the time the render thread reads it,
> it may have been garbage collected or mutated mid-frame.

So data has to be **captured on the game thread and handed across**. The handoff looks
like this:

```cpp
// --- game thread ---
FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();

ENQUEUE_RENDER_COMMAND(SurfelWarmupGradient)(
    [Resource, Time](FRHICommandListImmediate& RHICmdList)
    {
        // --- render thread, some time later ---
        DrawGradient_RenderThread(RHICmdList, Resource, Time);
    });
```

`ENQUEUE_RENDER_COMMAND` pushes a lambda onto the render thread's queue. Everything the
lambda needs is captured **by value**, on the game thread, at the moment of the call.

Note what is captured: `Resource` (a render-thread-safe resource pointer) and `Time` (a
plain float). **Not** the `UTextureRenderTarget2D*` itself. That distinction is the whole
lesson.

The main tutorial sidesteps this differently — it uses console variables, which are
explicitly safe to read on the render thread via `GetValueOnRenderThread()`.

---

## Part 5 — The warmup shader, end to end

Four pieces, same as every compute pass in Unreal:

| # | Piece | File |
|---|---|---|
| 1 | Parameter struct | `SurfelWarmupLibrary.cpp` |
| 2 | Shader class | `SurfelWarmupLibrary.cpp` |
| 3 | The kernel | `MyShader.usf` |
| 4 | The dispatch | `SurfelWarmupLibrary.cpp` |

### 5.1 The kernel

`Shaders/Private/MyShader.usf`:

```hlsl
#include "/Engine/Public/Platform.ush"

RWTexture2D<float4> Output;
uint2 TextureSize;
float Time;

[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]
void MySimpleComputeShader(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (any(DispatchThreadId.xy >= TextureSize))
    {
        return;
    }

    float2 UV = (float2(DispatchThreadId.xy) + 0.5f) / float2(TextureSize);

    float3 Colour = float3(UV.x, UV.y, 0.5f + 0.5f * sin(Time));

    Output[DispatchThreadId.xy] = float4(Colour, 1.0f);
}
```

**How a compute shader thinks.** A pixel shader runs once per rasterized pixel and you
do not choose where. A compute shader has no geometry at all: you say "run N threads",
and each thread asks *which one am I?* via `SV_DispatchThreadID`, then decides what to
read and write.

**`[numthreads(8, 8, 1)]`** declares a **thread group** — a tile of 8×8 = 64 threads.
The GPU schedules whole groups. `THREADS_X/Y/Z` are `#define`s injected from C++ (see
5.2) so the two sides cannot drift apart.

**`RWTexture2D`** is a **UAV** — Unordered Access View, a writable texture. "Unordered"
because thousands of threads write in nondeterministic order. Read-only textures are
plain `Texture2D` (an SRV). A texture can only get a UAV if it was created allowing one.

**The bounds check is mandatory.** For a 100×100 texture with 8×8 groups you dispatch
13×13 groups = 104×104 threads. The 4 extra rows and columns must do nothing. Skip this
and you corrupt memory outside the texture.

**The `+ 0.5f`** puts you at the *centre* of the pixel. Texel 0 spans 0.0 → 1/width, so
its centre is 0.5/width. Half-texel offsets are a classic source of subtle blur.

### 5.2 The shader class

The C++ mirror of the HLSL, in `SurfelWarmupLibrary.cpp`:

```cpp
class FMySimpleComputeShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMySimpleComputeShader);
	SHADER_USE_PARAMETER_STRUCT(FMySimpleComputeShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Output)
		SHADER_PARAMETER(FUintVector2, TextureSize)
		SHADER_PARAMETER(float, Time)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMySimpleComputeShader, "/Surfel/MyShader.usf", "MySimpleComputeShader", SF_Compute);
```

Piece by piece:

- **`FGlobalShader`** — a shader not tied to any material or mesh. Right for anything
  screen-space or general-purpose.
- **`DECLARE_GLOBAL_SHADER`** — type info and shader-map registration boilerplate.
- **`SHADER_USE_PARAMETER_STRUCT`** — generates the constructor that binds `FParameters`.
- **`ShouldCompilePermutation`** — return false and the shader is skipped for that
  platform. Gating on SM5 keeps it off mobile.
- **`ModifyCompilationEnvironment`** — injects preprocessor defines. This is how the
  group size lives in exactly one place.
- **`IMPLEMENT_GLOBAL_SHADER`** — class → virtual path → entry point → frequency
  (`SF_Compute`). In a `.cpp`, exactly once.

> **The rule that will bite you.** Parameter names must match **exactly** between the C++
> struct and the `.usf`. `TextureSize` in C++ needs `TextureSize` in HLSL. A mismatch
> usually fails *silently* — the shader just reads zero. If a value is mysteriously 0,
> check spelling first.

### 5.3 The dispatch

```cpp
static void DrawGradient_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FTextureRenderTargetResource* RenderTargetResource,
	float Time)
{
	check(IsInRenderingThread());

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGTextureRef OutputTexture = RegisterExternalTexture(
		GraphBuilder,
		RenderTargetResource->GetRenderTargetTexture(),
		TEXT("Surfel.WarmupTarget"));

	const FIntPoint Size = OutputTexture->Desc.Extent;

	FMySimpleComputeShader::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FMySimpleComputeShader::FParameters>();

	PassParameters->Output      = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));
	PassParameters->TextureSize = FUintVector2(Size.X, Size.Y);
	PassParameters->Time        = Time;

	TShaderMapRef<FMySimpleComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(Size, FIntPoint(8, 8));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Surfel Warmup Gradient %dx%d", Size.X, Size.Y),
		ComputeShader,
		PassParameters,
		GroupCount);

	GraphBuilder.Execute();
}
```

**RDG — the Render Dependency Graph.** You do not issue GPU commands directly. You
*describe* passes with their inputs and outputs, then `Execute()` runs the lot. RDG
works out dependencies, inserts resource barriers, allocates transient memory, and
culls passes whose output nobody reads.

Two things follow, and both surprise people:

1. **`FRDGTextureRef` is a promise, not a texture.** During setup it is just a handle.
2. **Parameters must be allocated by the graph.** `GraphBuilder.AllocParameters<...>()`
   — a stack local would be destroyed long before `Execute()` runs.

**`RegisterExternalTexture`** brings a texture that already exists (our render target,
which lives across frames) into the graph. Compare with `GraphBuilder.CreateTexture()`,
which makes a *transient* texture that only exists inside this graph. The main tutorial
uses `CreateTexture` for its scratch output; here we want to write into something
persistent that you can look at afterwards.

**`GetGroupCount`** does the ceiling division: `ceil(Width/8) × ceil(Height/8)`. This
is the other half of the bounds check — it is *why* extra threads exist.

**`RDG_EVENT_NAME`** is the label you will see in RenderDoc. It compiles out in shipping
builds. Name things well; you will be reading these.

### 5.4 The entry point

```cpp
void USurfelWarmupLibrary::DrawGradientToRenderTarget(
	UObject* WorldContextObject,
	UTextureRenderTarget2D* RenderTarget,
	float Time)
{
	if (!RenderTarget) { /* warn and bail */ }

	if (!RenderTarget->bCanCreateUAV)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Surfel warmup: '%s' needs bCanCreateUAV enabled."), *RenderTarget->GetName());
		return;
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource) { return; }

	ENQUEUE_RENDER_COMMAND(SurfelWarmupGradient)(
		[Resource, Time](FRHICommandListImmediate& RHICmdList)
		{
			DrawGradient_RenderThread(RHICmdList, Resource, Time);
		});
}
```

A `UBlueprintFunctionLibrary` with a `static` `UFUNCTION(BlueprintCallable)` is the
simplest way to expose C++ to Blueprint — no actor, no component, no instance.

**The `bCanCreateUAV` check matters.** Render targets do not allow UAV access by
default. Without it, `CreateUAV` fails at graph setup. Checking here with a clear log
message turns a confusing RDG assertion into an obvious fix.

---

## Part 6 — Running it

### 6.1 Build

```powershell
& "C:\GameDevelopment\Unreal\UnrealEngine-release\Engine\Build\BatchFiles\Build.bat" `
    SurfelsEditor Win64 Development `
    -Project="C:\Users\david\Documents\Unreal Projects\Surfels\Surfels.uproject" -WaitMutex
```

Or build from Visual Studio / Rider as normal.

### 6.2 Make a render target

1. In the Content Browser: **right-click → Textures → Render Target**.
2. Name it `RT_Warmup`.
3. **Double-click it** and in the Details panel tick **Support UAV** (`bCanCreateUAV`).
   *Nothing will work without this.*
4. Optionally set Size X / Size Y — 512×512 is plenty.

### 6.3 Call it

In any Blueprint — a Level Blueprint is fine:

```
Event BeginPlay
  └─► Draw Gradient To Render Target
        Render Target : RT_Warmup
        Time          : 0.0
```

Hit Play, then double-click `RT_Warmup` in the Content Browser. You should see a
gradient: **black bottom-left, red rightward, green upward, yellow top-right**, with a
blue tint set by `Time`.

To animate it, call the node on Tick and feed it `Get Game Time in Seconds`.

### 6.4 If nothing happens

| Symptom | Cause |
|---|---|
| Log: `needs bCanCreateUAV enabled` | Step 3 above — tick Support UAV |
| `Shader file not found: /Surfel/MyShader.usf` | `LoadingPhase` not `PostConfigInit`, or path mapping missing |
| Node missing in Blueprint | Module failed to compile — check the build output |
| `could not be bound to ... FParameters` | You added a shader parameter but the module never relinked. **The editor holds the DLL open while running** — close it, rebuild, reopen. Compare the `.dll` and `.cpp` timestamps to confirm |
| Texture stays black | Confirm the node is actually firing; add a print string |
| Texture is all one colour | `TextureSize` reading 0 — check the parameter name spelling |

### 6.5 Faster shader iteration

```
r.ShaderDevelopmentMode 1
r.DumpShaderDebugInfo 1
```

Then edit the `.usf` and run `recompileshaders changed` in the console. Seconds instead
of a full restart. Worth doing before your second shader, not your tenth.

---

## Part 7 — Experiments

Change the kernel, run `recompileshaders changed`, look at the texture. Each of these
teaches one thing:

**Solid colour** — the absolute minimum, and what `MyShader.usf` originally contained:
```hlsl
Output[DispatchThreadId.xy] = float4(1, 0, 0, 1);
```

**See the thread groups** — makes the 8×8 tiling literally visible:
```hlsl
uint2 GroupId = DispatchThreadId.xy / 8;
float Checker = (GroupId.x + GroupId.y) % 2;
Output[DispatchThreadId.xy] = float4(Checker.xxx, 1);
```

**Distance field** — a circle, and your first taste of per-pixel maths:
```hlsl
float2 Centre = float2(0.5, 0.5);
float  Dist   = length(UV - Centre);
Output[DispatchThreadId.xy] = float4((Dist < 0.3).xxx, 1);
```

**Prove the bounds check matters** — comment out the early `return` and use a texture
size that is not a multiple of 8 (say 100×100). On some drivers you will see corruption;
on others a device removal. This is worth doing *once*, deliberately, so the rule sticks.

**Remove a parameter from C++ but not HLSL** — watch it silently become 0. This is the
failure mode described in 5.2, and seeing it once is worth a paragraph of warning.

---

## Part 8 — What's in this plugin now

Three shaders, in increasing order of difficulty:

| Shader | Reads | Writes | Driven by | Tutorial |
|---|---|---|---|---|
| `MyShader.usf` | nothing | a render target asset | Blueprint node | this file |
| `FullscreenCS.usf` | scene colour | scene colour | `r.Surfel.Mode 1` | [TUTORIAL.md](TUTORIAL.md) §3 |
| `GBufferVisualizeCS.usf` | the G-Buffer | scene colour | `r.Surfel.Mode 2` | [TUTORIAL.md](TUTORIAL.md) §6 |

The step from this file to the next is smaller than it looks. The four pieces are
identical. What changes:

- **Where the work is triggered** — a Blueprint call becomes a `SceneViewExtension` hook
  that fires every frame, for every view, automatically.
- **What you read** — nothing becomes scene colour, then the full G-Buffer.
- **Thread-safety approach** — `ENQUEUE_RENDER_COMMAND` becomes console variables,
  because the SceneViewExtension callback is *already* on the render thread.

Everything else — parameter struct, shader class, `numthreads`, bounds check,
`GetGroupCount`, RDG — is the same in all three.

---

## Part 9 — Vocabulary

| Term | Meaning |
|---|---|
| **Module** | One compiled DLL plus its build rules. The unit Unreal links and loads. |
| **Plugin** | A self-contained folder holding one or more modules, plus its own content and shaders. |
| **RHI** | Render Hardware Interface. Unreal's abstraction over D3D12 / Vulkan / Metal. |
| **RDG** | Render Dependency Graph. You describe passes; it schedules and barriers them. |
| **SRV** | Shader Resource View. A read-only binding. `Texture2D` in HLSL. |
| **UAV** | Unordered Access View. A writable binding. `RWTexture2D` in HLSL. |
| **Dispatch** | Launching a grid of compute thread groups. |
| **Thread group** | A tile of threads declared by `[numthreads(X,Y,Z)]`; scheduled as a unit. |
| **`.usf`** | Unreal Shader File — HLSL that gets compiled. |
| **`.ush`** | Unreal Shader Header — meant to be `#include`d, never compiled alone. |
| **Virtual shader path** | `/Surfel/MyShader.usf` — resolved via `AddShaderSourceDirectoryMapping`. |
| **Game thread** | Gameplay, Tick, Blueprint. Where normal code runs. |
| **Render thread** | Builds GPU commands. Runs about one frame behind the game thread. |
| **Global shader** | A shader independent of any material or mesh. `FGlobalShader`. |
| **Feature level** | GPU capability tier. `SM5` is the desktop deferred baseline. |

---

## Next

You now have a compute shader you wrote, dispatched, and can see the output of. The
[main tutorial](TUTORIAL.md) takes exactly these four pieces and points them at the
live scene — first tinting scene colour, then reading and visualizing the deferred
G-Buffer.
