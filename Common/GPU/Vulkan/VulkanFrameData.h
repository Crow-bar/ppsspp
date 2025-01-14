#pragma once

#include <cstdint>

#include <mutex>
#include <condition_variable>

#include "Common/GPU/Vulkan/VulkanContext.h"

struct VKRStep;

enum class VKRRunType {
	END,
	SYNC,
};

struct QueueProfileContext {
	VkQueryPool queryPool;
	std::vector<std::string> timestampDescriptions;
	std::string profileSummary;
	double cpuStartTime;
	double cpuEndTime;
};

// Per-frame data, round-robin so we can overlap submission with execution of the previous frame.
struct FrameData {
	std::mutex push_mutex;
	std::condition_variable push_condVar;

	std::mutex pull_mutex;
	std::condition_variable pull_condVar;

	bool readyForFence = true;
	bool readyForRun = false;
	bool skipSwap = false;
	VKRRunType type = VKRRunType::END;

	VkFence fence;
	VkFence readbackFence;  // Strictly speaking we might only need one of these.
	bool readbackFenceUsed = false;

	// These are on different threads so need separate pools.
	VkCommandPool cmdPoolInit;  // Written to from main thread
	VkCommandPool cmdPoolMain;  // Written to from render thread, which also submits

	VkCommandBuffer initCmd;
	VkCommandBuffer mainCmd;
	VkCommandBuffer presentCmd;

	bool hasInitCommands = false;
	bool hasPresentCommands = false;
	bool hasAcquired = false;

	std::vector<VKRStep *> steps;

	// Swapchain.
	bool hasBegun = false;
	uint32_t curSwapchainImage = -1;
	VkSemaphore acquireSemaphore;  // Not owned, shared between all FrameData.

	// Profiling.
	QueueProfileContext profile;
	bool profilingEnabled_;

	void AcquireNextImage(VulkanContext *vulkan);
};
