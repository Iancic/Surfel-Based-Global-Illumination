// - David
// Credit AI Usage: This file was written by Claude Code because I ran out of time to dig through the 5.8 GPU profiler
// API myself. It is only profiling support (timings for stat gpu / Insights and the ImGui GPU Timings block) and
// nothing in the surfel rendering depends on it.
// Prompt used:
	// Can you add the timing markers in my C++ scene view extension and maybe help me find a nice way to filter the timings when using stat gpu or so.
	// Maybe an in-app readout too, but I'm not sure how much code it will touch.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "GPUProfiler.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include <atomic>

// GPU timing stats for the surfel passes.
//
// Used through RDG_EVENT_SCOPE_STAT(GraphBuilder, SurfelScatter, "Surfel Scatter Pass"), the macro looks for a
// variable called GPUStat_<StatName>. Each scope then shows up in `stat gpu`, Unreal Insights and the CSV profiler
// under the name given below, and OnTimingResults also hands the time back to us for the ImGui panel.
// All the names start with "Surfel" so they are easy to filter on.
#if HAS_GPU_STATS

struct FSurfelGPUStat : UE::RHI::GPUProfiler::FGPUStat
{
	// Smoothed so the ImGui number is readable, the raw per frame value is still what stat gpu / Insights show
	std::atomic<float> SmoothedMs{ 0.0f };

	FSurfelGPUStat(const TCHAR* InStatName, const TCHAR* InDisplayName)
		: FGPUStat(InStatName, InDisplayName, nullptr)
	{}

	virtual EOnTimingResultsAction OnTimingResults(UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double IdleMs, double WaitMs) override
	{
		// Called from the RHI timing thread, once per queue. Everything here runs on the graphics queue.
		if (BusyMs > 0.0)
		{
			const float Previous = SmoothedMs.load(std::memory_order_relaxed);
			SmoothedMs.store(Previous <= 0.0f ? (float)BusyMs : FMath::Lerp(Previous, (float)BusyMs, 0.1f), std::memory_order_relaxed);
		}
		return EOnTimingResultsAction::Keep;
	}
};

// Inline so every .cpp that includes this header shares one instance of each stat
inline FSurfelGPUStat GPUStat_SurfelTotal    (TEXT("SurfelTotal"),     TEXT("Surfel Total"));
inline FSurfelGPUStat GPUStat_SurfelScatter  (TEXT("SurfelScatter"),   TEXT("Surfel Scatter"));
inline FSurfelGPUStat GPUStat_SurfelGather   (TEXT("SurfelGather"),    TEXT("Surfel Gather"));
inline FSurfelGPUStat GPUStat_SurfelSpawn    (TEXT("SurfelSpawn"),     TEXT("Surfel Spawn"));
inline FSurfelGPUStat GPUStat_SurfelIrradiance(TEXT("SurfelIrradiance"), TEXT("Surfel Irradiance"));
inline FSurfelGPUStat GPUStat_SurfelTextureIrradiance(TEXT("SurfelTextureIrradiance"), TEXT("Surfel Texture Irradiance"));
inline FSurfelGPUStat GPUStat_SurfelComposite(TEXT("SurfelComposite"), TEXT("Surfel Composite"));
// Debug view only, not part of the GI cost so it is not inside SurfelTotal
inline FSurfelGPUStat GPUStat_SurfelVisualize(TEXT("SurfelVisualize"), TEXT("Surfel Visualize"));

// Display order for the ImGui panel (Total first, the rest are its children)
inline FSurfelGPUStat* const GSurfelGPUStats[] =
{
	&GPUStat_SurfelTotal,
	&GPUStat_SurfelScatter,
	&GPUStat_SurfelGather,
	&GPUStat_SurfelSpawn,
	&GPUStat_SurfelIrradiance,
	&GPUStat_SurfelTextureIrradiance,
	&GPUStat_SurfelComposite,
	&GPUStat_SurfelVisualize,
};

#endif // HAS_GPU_STATS

// Lumen GPU time, for the ImGui panel in Lumen GI mode.
//
// Lumen's GPU stats are file-static inside the Renderer module, so they can't be read directly like ours. The GPU
// profiler still sends every stat's time to the stats system, so this listens there and sums the top-level Lumen
// scopes (the same names `stat gpu` shows). The stats system only collects while something asks for it, which is
// why Start/Stop bump the collection count, so only call Start while the number is actually on screen.
#if STATS

#include "Stats/StatsData.h"
#include "Engine/Engine.h"

namespace SurfelLumenGPUTime
{
	inline std::atomic<float> SmoothedMs{ 0.0f };
	inline FDelegateHandle NewFrameHandle;
	inline bool bPreviousDisableAILogging = false;

	// Busy stats are named STAT_<Queue>_<StatName> by the GPU profiler, Idle/Wait get a _0/_1 suffix on top
	inline const TArray<FName>& GetLumenStatNames()
	{
		static const TArray<FName> Names = []
		{
			const TCHAR* const Queues[] = { TEXT("GPU0_Graphics0"), TEXT("GPU0_Compute0") };
			const TCHAR* const Stats[] =
			{
				TEXT("LumenSceneUpdate"),
				TEXT("LumenSceneLighting"),
				TEXT("LumenScreenProbeGather"),
				TEXT("LumenIrradianceFieldGather"),
				TEXT("LumenReflections"),
			};

			TArray<FName> Result;
			for (const TCHAR* Queue : Queues)
			{
				for (const TCHAR* Stat : Stats)
				{
					Result.Add(FName(*FString::Printf(TEXT("STAT_%s_%s"), Queue, Stat)));
				}
			}
			return Result;
		}();
		return Names;
	}

	// Runs on the stats thread once per finished frame
	inline void OnNewStatsFrame(int64 Frame)
	{
		const FStatsThreadState& State = FStatsThreadState::GetLocalState();
		if (!State.IsFrameValid(Frame))
		{
			return;
		}

		const TArray<FName>& LumenNames = GetLumenStatNames();
		double TotalMs = 0.0;
		bool bFound = false;

		for (const FStatPacket* Packet : State.GetStatPacketArray(Frame).Packets)
		{
			// GPU timings all come in on the end of pipe packet
			if (Packet->ThreadType != EThreadType::EndOfPipe)
			{
				continue;
			}

			for (const FStatMessage& Message : Packet->StatMessages)
			{
				if (Message.NameAndInfo.GetField<EStatOperation>() == EStatOperation::Set
					&& Message.NameAndInfo.GetField<EStatDataType>() == EStatDataType::ST_double
					&& LumenNames.Contains(Message.NameAndInfo.GetShortName()))
				{
					TotalMs += Message.GetValue_double();
					bFound = true;
				}
			}
		}

		if (bFound)
		{
			const float Previous = SmoothedMs.load(std::memory_order_relaxed);
			SmoothedMs.store(Previous <= 0.0f ? (float)TotalMs : FMath::Lerp(Previous, (float)TotalMs, 0.1f), std::memory_order_relaxed);
		}
	}

	inline void Start()
	{
		if (NewFrameHandle.IsValid())
		{
			return;
		}
		SmoothedMs.store(0.0f, std::memory_order_relaxed);

		// With stats collecting the engine draws "PROFILING WITH AI LOGGING ON!" in the viewport. There's no AI
		// in these scenes, so turn AI logging off while reading (also keeps it out of the timings) and restore it in Stop.
		if (GEngine)
		{
			bPreviousDisableAILogging = GEngine->bDisableAILogging;
			GEngine->bDisableAILogging = true;
		}

		StatsPrimaryEnableAdd();
		// Thread safe delegate, so binding from the game thread is fine
		NewFrameHandle = FStatsThreadState::GetLocalState().NewFrameDelegate.AddStatic(&OnNewStatsFrame);
	}

	inline void Stop()
	{
		if (!NewFrameHandle.IsValid())
		{
			return;
		}
		FStatsThreadState::GetLocalState().NewFrameDelegate.Remove(NewFrameHandle);
		NewFrameHandle.Reset();
		StatsPrimaryEnableSubtract();

		if (GEngine)
		{
			GEngine->bDisableAILogging = bPreviousDisableAILogging;
		}
	}
}

#endif // STATS
