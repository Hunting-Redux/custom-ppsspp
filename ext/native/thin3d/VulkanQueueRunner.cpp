#include <map>

#include "base/timeutil.h"
#include "DataFormat.h"
#include "VulkanQueueRunner.h"
#include "VulkanRenderManager.h"

// Debug help: adb logcat -s DEBUG PPSSPPNativeActivity PPSSPP NativeGLView NativeRenderer NativeSurfaceView PowerSaveModeReceiver InputDeviceState

void VulkanQueueRunner::CreateDeviceObjects() {
	ILOG("VulkanQueueRunner::CreateDeviceObjects");
	InitBackbufferRenderPass();

	framebufferRenderPass_ = GetRenderPass(VKRRenderPassAction::CLEAR, VKRRenderPassAction::CLEAR, VKRRenderPassAction::CLEAR,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

#if 0
	// Just to check whether it makes sense to split some of these. drawidx is way bigger than the others...
	// We should probably just move to variable-size data in a raw buffer anyway...
	VkRenderData rd;
	ILOG("sizeof(pipeline): %d", (int)sizeof(rd.pipeline));
	ILOG("sizeof(draw): %d", (int)sizeof(rd.draw));
	ILOG("sizeof(drawidx): %d", (int)sizeof(rd.drawIndexed));
	ILOG("sizeof(clear): %d", (int)sizeof(rd.clear));
	ILOG("sizeof(viewport): %d", (int)sizeof(rd.viewport));
	ILOG("sizeof(scissor): %d", (int)sizeof(rd.scissor));
	ILOG("sizeof(blendColor): %d", (int)sizeof(rd.blendColor));
	ILOG("sizeof(push): %d", (int)sizeof(rd.push));
#endif
}

void VulkanQueueRunner::ResizeReadbackBuffer(VkDeviceSize requiredSize) {
	if (readbackBuffer_ && requiredSize <= readbackBufferSize_) {
		return;
	}
	if (readbackMemory_) {
		vulkan_->Delete().QueueDeleteDeviceMemory(readbackMemory_);
	}
	if (readbackBuffer_) {
		vulkan_->Delete().QueueDeleteBuffer(readbackBuffer_);
	}

	readbackBufferSize_ = requiredSize;

	VkDevice device = vulkan_->GetDevice();

	VkBufferCreateInfo buf{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	buf.size = readbackBufferSize_;
	buf.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	vkCreateBuffer(device, &buf, nullptr, &readbackBuffer_);

	VkMemoryRequirements reqs{};
	vkGetBufferMemoryRequirements(device, readbackBuffer_, &reqs);

	VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = reqs.size;

	// For speedy readbacks, we want the CPU cache to be enabled. However on most hardware we then have to
	// sacrifice coherency, which means manual flushing. But try to find such memory first! If no cached
	// memory type is available we fall back to just coherent.
	const VkFlags desiredTypes[] = {
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	};
	VkFlags successTypeReqs = 0;
	for (VkFlags typeReqs : desiredTypes) {
		if (vulkan_->MemoryTypeFromProperties(reqs.memoryTypeBits, typeReqs, &allocInfo.memoryTypeIndex)) {
			successTypeReqs = typeReqs;
			break;
		}
	}
	_assert_(successTypeReqs != 0);
	readbackBufferIsCoherent_ = (successTypeReqs & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

	VkResult res = vkAllocateMemory(device, &allocInfo, nullptr, &readbackMemory_);
	if (res != VK_SUCCESS) {
		readbackMemory_ = VK_NULL_HANDLE;
		vkDestroyBuffer(device, readbackBuffer_, nullptr);
		readbackBuffer_ = VK_NULL_HANDLE;
		return;
	}
	uint32_t offset = 0;
	vkBindBufferMemory(device, readbackBuffer_, readbackMemory_, offset);
}

void VulkanQueueRunner::DestroyDeviceObjects() {
	ILOG("VulkanQueueRunner::DestroyDeviceObjects");
	vulkan_->Delete().QueueDeleteDeviceMemory(readbackMemory_);
	vulkan_->Delete().QueueDeleteBuffer(readbackBuffer_);
	readbackBufferSize_ = 0;

	renderPasses_.Iterate([&](const RPKey &rpkey, VkRenderPass rp) {
		_assert_(rp != VK_NULL_HANDLE);
		vulkan_->Delete().QueueDeleteRenderPass(rp);
	});
	renderPasses_.Clear();

	assert(backbufferRenderPass_ != VK_NULL_HANDLE);
	vulkan_->Delete().QueueDeleteRenderPass(backbufferRenderPass_);
	backbufferRenderPass_ = VK_NULL_HANDLE;
}

void VulkanQueueRunner::InitBackbufferRenderPass() {
	VkAttachmentDescription attachments[2];
	attachments[0].format = vulkan_->GetSwapchainFormat();
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // We don't want to preserve the backbuffer between frames so we really don't care.
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // We only render once to the backbuffer per frame so we can do this here.
	attachments[0].flags = 0;

	attachments[1].format = vulkan_->GetDeviceInfo().preferredDepthStencilFormat;  // must use this same format later for the back depth buffer.
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // Don't care about storing backbuffer Z - we clear it anyway.
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_DEPTH_STENCIL
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
#else
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
#endif
	attachments[1].flags = 0;

	VkAttachmentReference color_reference{};
	color_reference.attachment = 0;
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_COLOR
	color_reference.layout = VK_IMAGE_LAYOUT_GENERAL;
#else
	color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#endif

	VkAttachmentReference depth_reference{};
	depth_reference.attachment = 1;
	depth_reference.layout = attachments[1].finalLayout;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.flags = 0;
	subpass.inputAttachmentCount = 0;
	subpass.pInputAttachments = nullptr;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	subpass.pResolveAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depth_reference;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	// For the built-in layout transitions.
	VkSubpassDependency dep{};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.srcAccessMask = 0;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rp_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rp_info.attachmentCount = 2;
	rp_info.pAttachments = attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	rp_info.dependencyCount = 1;
	rp_info.pDependencies = &dep;

	VkResult res = vkCreateRenderPass(vulkan_->GetDevice(), &rp_info, nullptr, &backbufferRenderPass_);
	_assert_(res == VK_SUCCESS);
}

VkRenderPass VulkanQueueRunner::GetRenderPass(const RPKey &key) {
	auto pass = renderPasses_.Get(key);
	if (pass) {
		return pass;
	}

	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	switch (key.colorLoadAction) {
	case VKRRenderPassAction::CLEAR:
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		break;
	case VKRRenderPassAction::KEEP:
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		break;
	case VKRRenderPassAction::DONT_CARE:
	default:
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		break;
	}
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_COLOR
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
#else
	attachments[0].initialLayout = key.prevColorLayout;
	attachments[0].finalLayout = key.finalColorLayout;
#endif
	attachments[0].flags = 0;

	attachments[1].format = vulkan_->GetDeviceInfo().preferredDepthStencilFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	switch (key.depthLoadAction) {
	case VKRRenderPassAction::CLEAR:
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		break;
	case VKRRenderPassAction::KEEP:
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		break;
	case VKRRenderPassAction::DONT_CARE:
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		break;
	}
	switch (key.stencilLoadAction) {
	case VKRRenderPassAction::CLEAR:
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		break;
	case VKRRenderPassAction::KEEP:
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		break;
	case VKRRenderPassAction::DONT_CARE:
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		break;
	}
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_DEPTH_STENCIL
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
#else
	attachments[1].initialLayout = key.prevDepthLayout;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
#endif
	attachments[1].flags = 0;

	VkAttachmentReference color_reference{};
	color_reference.attachment = 0;
	color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_reference{};
	depth_reference.attachment = 1;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.flags = 0;
	subpass.inputAttachmentCount = 0;
	subpass.pInputAttachments = nullptr;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	subpass.pResolveAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depth_reference;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	VkSubpassDependency deps[2]{};
	int numDeps = 0;
	switch (key.prevColorLayout) {
	case VK_IMAGE_LAYOUT_UNDEFINED:
		// No need to specify stage or access.
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		// Already the right color layout. Unclear that we need to do a lot here..
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
		// We came from the Mali workaround, and are transitioning back to COLOR_ATTACHMENT_OPTIMAL.
		deps[numDeps].srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		deps[numDeps].srcAccessMask |= VK_ACCESS_SHADER_READ_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		deps[numDeps].srcAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		deps[numDeps].srcAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	default:
		_dbg_assert_msg_(G3D, false, "GetRenderPass: Unexpected color layout %d", (int)key.prevColorLayout);
		break;
	}

	switch (key.prevDepthLayout) {
	case VK_IMAGE_LAYOUT_UNDEFINED:
		// No need to specify stage or access.
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		// Already the right depth layout. Unclear that we need to do a lot here..
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		deps[numDeps].srcAccessMask |= VK_ACCESS_SHADER_READ_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		deps[numDeps].srcAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		deps[numDeps].srcAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
		deps[numDeps].srcStageMask |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	default:
		_dbg_assert_msg_(G3D, false, "PerformBindRT: Unexpected depth layout %d", (int)key.prevDepthLayout);
		break;
	}

	if (deps[numDeps].srcAccessMask) {
		deps[numDeps].srcSubpass = VK_SUBPASS_EXTERNAL;
		deps[numDeps].dstSubpass = 0;
		deps[numDeps].dependencyFlags = 0;
		deps[numDeps].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		deps[numDeps].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		numDeps++;
	}

	// And the final transition.
	// Don't need to transition it if VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
	switch (key.finalColorLayout) {
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		deps[numDeps].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		deps[numDeps].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		deps[numDeps].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		deps[numDeps].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		deps[numDeps].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		deps[numDeps].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_UNDEFINED:
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		// Nothing to do.
		break;
	default:
		_dbg_assert_msg_(G3D, false, "GetRenderPass: Unexpected final color layout %d", (int)key.finalColorLayout);
		break;
	}

	if (deps[numDeps].dstAccessMask) {
		deps[numDeps].srcSubpass = 0;
		deps[numDeps].dstSubpass = VK_SUBPASS_EXTERNAL;
		deps[numDeps].dependencyFlags = 0;
		deps[numDeps].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[numDeps].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		numDeps++;
	}

	VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rp.attachmentCount = 2;
	rp.pAttachments = attachments;
	rp.subpassCount = 1;
	rp.pSubpasses = &subpass;

	if (numDeps) {
		rp.dependencyCount = numDeps;
		rp.pDependencies = deps;
	}

	VkResult res = vkCreateRenderPass(vulkan_->GetDevice(), &rp, nullptr, &pass);
	_assert_(res == VK_SUCCESS);
	_assert_(pass != VK_NULL_HANDLE);
	renderPasses_.Insert(key, pass);
	return pass;
}

void VulkanQueueRunner::RunSteps(VkCommandBuffer cmd, std::vector<VKRStep *> &steps, QueueProfileContext *profile) {
	if (profile)
		profile->cpuStartTime = real_time_now();
	// Optimizes renderpasses, then sequences them.
	// Planned optimizations: 
	//  * Create copies of render target that are rendered to multiple times and textured from in sequence, and push those render passes
	//    as early as possible in the frame (Wipeout billboards).

	for (int j = 0; j < (int)steps.size(); j++) {
		if (steps[j]->stepType == VKRStepType::RENDER &&
			steps[j]->render.framebuffer &&
			steps[j]->render.finalColorLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
			// Just leave it at color_optimal.
			steps[j]->render.finalColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
	}

	for (int j = 0; j < (int)steps.size() - 1; j++) {
		// Push down empty "Clear/Store" renderpasses, and merge them with the first "Load/Store" to the same framebuffer.
		if (steps.size() > 1 && steps[j]->stepType == VKRStepType::RENDER &&
			steps[j]->render.numDraws == 0 &&
			steps[j]->render.numReads == 0 &&
			steps[j]->render.color == VKRRenderPassAction::CLEAR &&
			steps[j]->render.stencil == VKRRenderPassAction::CLEAR &&
			steps[j]->render.depth == VKRRenderPassAction::CLEAR) {

			// Drop the clear step, and merge it into the next step that touches the same framebuffer.
			for (int i = j + 1; i < (int)steps.size(); i++) {
				if (steps[i]->stepType == VKRStepType::RENDER &&
					steps[i]->render.framebuffer == steps[j]->render.framebuffer) {
					if (steps[i]->render.color != VKRRenderPassAction::CLEAR) {
						steps[i]->render.color = VKRRenderPassAction::CLEAR;
						steps[i]->render.clearColor = steps[j]->render.clearColor;
					}
					if (steps[i]->render.depth != VKRRenderPassAction::CLEAR) {
						steps[i]->render.depth = VKRRenderPassAction::CLEAR;
						steps[i]->render.clearDepth = steps[j]->render.clearDepth;
					}
					if (steps[i]->render.stencil != VKRRenderPassAction::CLEAR) {
						steps[i]->render.stencil = VKRRenderPassAction::CLEAR;
						steps[i]->render.clearStencil = steps[j]->render.clearStencil;
					}
					// Cheaply skip the first step.
					steps[j]->stepType = VKRStepType::RENDER_SKIP;
					break;
				} else if (steps[i]->stepType == VKRStepType::COPY &&
					steps[i]->copy.src == steps[j]->render.framebuffer) {
					// Can't eliminate the clear if a game copies from it before it's
					// rendered to. However this should be rare.
					// TODO: This should never happen when we check numReads now.
					break;
				}
			}
		}
	}

	// Queue hacks.
	if (hacksEnabled_) {
		if (hacksEnabled_ & QUEUE_HACK_MGS2_ACID) {
			// Massive speedup.
			ApplyMGSHack(steps);
		}
		if (hacksEnabled_ & QUEUE_HACK_SONIC) {
			ApplySonicHack(steps);
		}
		if (hacksEnabled_ & QUEUE_HACK_RENDERPASS_MERGE) {
			ApplyRenderPassMerge(steps);
		}
	}

	for (size_t i = 0; i < steps.size(); i++) {
		const VKRStep &step = *steps[i];
		switch (step.stepType) {
		case VKRStepType::RENDER:
			PerformRenderPass(step, cmd);
			break;
		case VKRStepType::COPY:
			PerformCopy(step, cmd);
			break;
		case VKRStepType::BLIT:
			PerformBlit(step, cmd);
			break;
		case VKRStepType::READBACK:
			PerformReadback(step, cmd);
			break;
		case VKRStepType::READBACK_IMAGE:
			PerformReadbackImage(step, cmd);
			break;
		case VKRStepType::RENDER_SKIP:
			break;
		}

		if (profile && profile->timestampDescriptions.size() + 1 < MAX_TIMESTAMP_QUERIES) {
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile->queryPool, (uint32_t)profile->timestampDescriptions.size());
			profile->timestampDescriptions.push_back(StepToString(step));
		}
	}

	// Deleting all in one go should be easier on the instruction cache than deleting
	// them as we go - and easier to debug because we can look backwards in the frame.
	for (size_t i = 0; i < steps.size(); i++) {
		delete steps[i];
	}

	if (profile)
		profile->cpuEndTime = real_time_now();
}

void VulkanQueueRunner::ApplyMGSHack(std::vector<VKRStep *> &steps) {
	// Really need a sane way to express transforms of steps.

	// We want to turn a sequence of copy,render(1),copy,render(1),copy,render(1) to copy,copy,copy,render(n).

	for (int i = 0; i < (int)steps.size() - 3; i++) {
		int last = -1;
		if (!(steps[i]->stepType == VKRStepType::COPY &&
			steps[i + 1]->stepType == VKRStepType::RENDER &&
			steps[i + 2]->stepType == VKRStepType::COPY &&
			steps[i + 1]->render.numDraws == 1 &&
			steps[i]->copy.dst == steps[i + 2]->copy.dst))
			continue;
		// Looks promising! Let's start by finding the last one.
		for (int j = i; j < (int)steps.size(); j++) {
			switch (steps[j]->stepType) {
			case VKRStepType::RENDER:
				if (steps[j]->render.numDraws > 1)
					last = j - 1;
				// should really also check descriptor sets...
				if (steps[j]->commands.size()) {
					VkRenderData &cmd = steps[j]->commands.back();
					if (cmd.cmd == VKRRenderCommand::DRAW_INDEXED && cmd.draw.count != 6)
						last = j - 1;
				}
				break;
			case VKRStepType::COPY:
				if (steps[j]->copy.dst != steps[i]->copy.dst)
					last = j - 1;
				break;
			default:
				break;
			}
			if (last != -1)
				break;
		}

		if (last != -1) {
			// We've got a sequence from i to last that needs reordering.
			// First, let's sort it, keeping the same length.
			std::vector<VKRStep *> copies;
			std::vector<VKRStep *> renders;
			copies.reserve((last - i) / 2);
			renders.reserve((last - i) / 2);
			for (int n = i; n <= last; n++) {
				if (steps[n]->stepType == VKRStepType::COPY)
					copies.push_back(steps[n]);
				else if (steps[n]->stepType == VKRStepType::RENDER)
					renders.push_back(steps[n]);
			}
			// Write the copies back. TODO: Combine them too.
			for (int j = 0; j < (int)copies.size(); j++) {
				steps[i + j] = copies[j];
			}
			// Write the renders back (so they will be deleted properly).
			for (int j = 0; j < (int)renders.size(); j++) {
				steps[i + j + copies.size()] = renders[j];
			}
			assert(steps[i + copies.size()]->stepType == VKRStepType::RENDER);
			// Combine the renders.
			for (int j = 1; j < (int)renders.size(); j++) {
				for (int k = 0; k < (int)renders[j]->commands.size(); k++) {
					steps[i + copies.size()]->commands.push_back(renders[j]->commands[k]);
				}
				steps[i + copies.size() + j]->stepType = VKRStepType::RENDER_SKIP;
			}
			// We're done.
			break;
		}
	}

	// There's also a post processing effect using depals that's just brutal in some parts
	// of the game.
	for (int i = 0; i < (int)steps.size() - 3; i++) {
		int last = -1;
		if (!(steps[i]->stepType == VKRStepType::RENDER &&
			steps[i + 1]->stepType == VKRStepType::RENDER &&
			steps[i + 2]->stepType == VKRStepType::RENDER &&
			steps[i]->render.numDraws == 1 &&
			steps[i + 1]->render.numDraws == 1 &&
			steps[i + 2]->render.numDraws == 1 &&
			steps[i]->render.color == VKRRenderPassAction::DONT_CARE &&
			steps[i + 1]->render.color == VKRRenderPassAction::KEEP &&
			steps[i + 2]->render.color == VKRRenderPassAction::DONT_CARE))
			continue;
		VKRFramebuffer *depalFramebuffer = steps[i]->render.framebuffer;
		VKRFramebuffer *targetFramebuffer = steps[i + 1]->render.framebuffer;
		// OK, found the start of a post-process sequence. Let's scan until we find the end.
		for (int j = i; j < steps.size() - 3; j++) {
			if (((j - i) & 1) == 0) {
				// This should be a depal draw.
				if (steps[j]->render.numDraws != 1)
					break;
				if (steps[j]->render.color != VKRRenderPassAction::DONT_CARE)
					break;
				if (steps[j]->render.framebuffer != depalFramebuffer)
					break;
				last = j;
			} else {
				// This should be a target draw.
				if (steps[j]->render.numDraws != 1)
					break;
				if (steps[j]->render.color != VKRRenderPassAction::KEEP)
					break;
				if (steps[j]->render.framebuffer != targetFramebuffer)
					break;
				last = j;
			}
		}

		if (last == -1)
			continue;

		// Combine the depal renders.
		for (int j = i + 2; j <= last + 1; j += 2) {
			for (int k = 0; k < (int)steps[j]->commands.size(); k++) {
				switch (steps[j]->commands[k].cmd) {
				case VKRRenderCommand::DRAW:
				case VKRRenderCommand::DRAW_INDEXED:
					steps[i]->commands.push_back(steps[j]->commands[k]);
					break;
				}
			}
			steps[j]->stepType = VKRStepType::RENDER_SKIP;
		}

		// Combine the target renders.
		for (int j = i + 3; j <= last; j += 2) {
			for (int k = 0; k < (int)steps[j]->commands.size(); k++) {
				switch (steps[j]->commands[k].cmd) {
				case VKRRenderCommand::DRAW:
				case VKRRenderCommand::DRAW_INDEXED:
					steps[i + 1]->commands.push_back(steps[j]->commands[k]);
					break;
				}
			}
			steps[j]->stepType = VKRStepType::RENDER_SKIP;
		}

		// We're done - we only expect one of these sequences per frame.
		break;
	}
}

void VulkanQueueRunner::ApplySonicHack(std::vector<VKRStep *> &steps) {
	// We want to turn a sequence of render(3),render(1),render(6),render(1),render(6),render(1),render(3) to
	// render(1), render(1), render(1), render(6), render(6), render(6)

	for (int i = 0; i < (int)steps.size() - 4; i++) {
		int last = -1;
		if (!(steps[i]->stepType == VKRStepType::RENDER &&
			steps[i + 1]->stepType == VKRStepType::RENDER &&
			steps[i + 2]->stepType == VKRStepType::RENDER &&
			steps[i + 3]->stepType == VKRStepType::RENDER &&
			steps[i]->render.numDraws == 3 &&
			steps[i + 1]->render.numDraws == 1 &&
			steps[i + 2]->render.numDraws == 6 &&
			steps[i + 3]->render.numDraws == 1 &&
			steps[i]->render.framebuffer == steps[i + 2]->render.framebuffer &&
			steps[i + 1]->render.framebuffer == steps[i + 3]->render.framebuffer))
			continue;
		// Looks promising! Let's start by finding the last one.
		for (int j = i; j < (int)steps.size(); j++) {
			switch (steps[j]->stepType) {
			case VKRStepType::RENDER:
				if ((j - i) & 1) {
					if (steps[j]->render.framebuffer != steps[i + 1]->render.framebuffer)
						last = j - 1;
					if (steps[j]->render.numDraws != 1)
						last = j - 1;
				} else {
					if (steps[j]->render.framebuffer != steps[i]->render.framebuffer)
						last = j - 1;
					if (steps[j]->render.numDraws != 3 && steps[j]->render.numDraws != 6)
						last = j - 1;
				}
				break;
			default:
				break;
			}
			if (last != -1)
				break;
		}

		if (last != -1) {
			// We've got a sequence from i to last that needs reordering.
			// First, let's sort it, keeping the same length.
			std::vector<VKRStep *> type1;
			std::vector<VKRStep *> type2;
			type1.reserve((last - i) / 2);
			type2.reserve((last - i) / 2);
			for (int n = i; n <= last; n++) {
				if (steps[n]->render.framebuffer == steps[i]->render.framebuffer)
					type1.push_back(steps[n]);
				else
					type2.push_back(steps[n]);
			}

			// Write the renders back in order. Same amount, so deletion will work fine.
			for (int j = 0; j < (int)type1.size(); j++) {
				steps[i + j] = type1[j];
			}
			for (int j = 0; j < (int)type2.size(); j++) {
				steps[i + j + type1.size()] = type2[j];
			}

			// Combine the renders.
			for (int j = 1; j < (int)type1.size(); j++) {
				for (int k = 0; k < (int)type1[j]->commands.size(); k++) {
					steps[i]->commands.push_back(type1[j]->commands[k]);
				}
				steps[i + j]->stepType = VKRStepType::RENDER_SKIP;
			}
			for (int j = 1; j < (int)type2.size(); j++) {
				for (int k = 0; k < (int)type2[j]->commands.size(); k++) {
					steps[i + type1.size()]->commands.push_back(type2[j]->commands[k]);
				}
				steps[i + j + type1.size()]->stepType = VKRStepType::RENDER_SKIP;
			}
			// We're done.
			break;
		}
	}
}

std::string VulkanQueueRunner::StepToString(const VKRStep &step) const {
	char buffer[256];
	switch (step.stepType) {
	case VKRStepType::RENDER:
	{
		int w = step.render.framebuffer ? step.render.framebuffer->width : vulkan_->GetBackbufferWidth();
		int h = step.render.framebuffer ? step.render.framebuffer->height : vulkan_->GetBackbufferHeight();
		snprintf(buffer, sizeof(buffer), "RenderPass (draws: %d, %dx%d, fb: %p, )", step.render.numDraws, w, h, step.render.framebuffer);
		break;
	}
	case VKRStepType::COPY:
		snprintf(buffer, sizeof(buffer), "Copy (%dx%d)", step.copy.srcRect.extent.width, step.copy.srcRect.extent.height);
		break;
	case VKRStepType::BLIT:
		snprintf(buffer, sizeof(buffer), "Blit (%dx%d->%dx%d)", step.blit.srcRect.extent.width, step.blit.srcRect.extent.height, step.blit.dstRect.extent.width, step.blit.dstRect.extent.height);
		break;
	case VKRStepType::READBACK:
		snprintf(buffer, sizeof(buffer), "Readback (%dx%d, fb: %p)", step.readback.srcRect.extent.width, step.readback.srcRect.extent.height, step.readback.src);
		break;
	case VKRStepType::READBACK_IMAGE:
		snprintf(buffer, sizeof(buffer), "ReadbackImage (%dx%d)", step.readback_image.srcRect.extent.width, step.readback_image.srcRect.extent.height);
		break;
	case VKRStepType::RENDER_SKIP:
		snprintf(buffer, sizeof(buffer), "(SKIPPED RenderPass)");
		break;
	default:
		buffer[0] = 0;
		break;
	}
	return std::string(buffer);
}

// Ideally, this should be cheap enough to be applied to all games. At least on mobile, it's pretty
// much a guaranteed neutral or win in terms of GPU power. However, dependency calculation really
// must be perfect!
void VulkanQueueRunner::ApplyRenderPassMerge(std::vector<VKRStep *> &steps) {
	// First let's count how many times each framebuffer is rendered to.
	// If it's more than one, let's do our best to merge them. This can help God of War quite a bit.
	std::map<VKRFramebuffer *, int> counts;
	for (int i = 0; i < (int)steps.size(); i++) {
		if (steps[i]->stepType == VKRStepType::RENDER) {
			counts[steps[i]->render.framebuffer]++;
		}
	}

	// Now, let's go through the steps. If we find one that is rendered to more than once,
	// we'll scan forward and slurp up any rendering that can be merged across.
	for (int i = 0; i < (int)steps.size(); i++) {
		if (steps[i]->stepType == VKRStepType::RENDER && counts[steps[i]->render.framebuffer] > 1) {
			auto fb = steps[i]->render.framebuffer;
			TinySet<VKRFramebuffer *, 8> touchedFramebuffers;  // must be the same fast-size as the dependencies TinySet for annoying reasons.
			for (int j = i + 1; j < (int)steps.size(); j++) {
				// If any other passes are reading from this framebuffer as-is, we cancel the scan.
				switch (steps[j]->stepType) {
				case VKRStepType::RENDER:
					if (steps[j]->dependencies.contains(fb)) {
						goto done_fb;
					}
					// Prevent Unknown's example case from https://github.com/hrydgard/ppsspp/pull/12242
					if (steps[j]->dependencies.contains(touchedFramebuffers)) {
						goto done_fb;
					}
					if (steps[j]->render.framebuffer == fb) {
						// ok. Now, if it's a render, slurp up all the commands
						// and kill the step.
						// Also slurp up any pretransitions.
						steps[i]->preTransitions.insert(steps[i]->preTransitions.end(), steps[j]->preTransitions.begin(), steps[j]->preTransitions.end());
						steps[i]->commands.insert(steps[i]->commands.end(), steps[j]->commands.begin(), steps[j]->commands.end());
						steps[j]->stepType = VKRStepType::RENDER_SKIP;
					}
					// Remember the framebuffer this wrote to. We can't merge with later passes that depend on these.
					if (steps[j]->render.framebuffer != fb) {
						touchedFramebuffers.insert(steps[j]->render.framebuffer);
					}
					break;
				case VKRStepType::COPY:
					if (steps[j]->copy.src == fb || steps[j]->copy.dst == fb) {
						goto done_fb;
					}
					touchedFramebuffers.insert(steps[j]->copy.dst);
					break;
				case VKRStepType::BLIT:
					if (steps[j]->blit.src == fb || steps[j]->blit.dst == fb) {
						goto done_fb;
					}
					touchedFramebuffers.insert(steps[j]->blit.dst);
					break;
				case VKRStepType::READBACK:
					// Not sure this has much effect, when executed READBACK is always the last step
					// since we stall the GPU and wait immediately after.
					if (steps[j]->readback.src == fb) {
						goto done_fb;
					}
					break;
				}
			}
			done_fb:
				;
		}
	}
}

void VulkanQueueRunner::LogSteps(const std::vector<VKRStep *> &steps) {
	ILOG("=======================================");
	for (size_t i = 0; i < steps.size(); i++) {
		const VKRStep &step = *steps[i];
		ILOG("%s", StepToString(step).c_str());
		switch (step.stepType) {
		case VKRStepType::RENDER:
			LogRenderPass(step);
			break;
		case VKRStepType::COPY:
			LogCopy(step);
			break;
		case VKRStepType::BLIT:
			LogBlit(step);
			break;
		case VKRStepType::READBACK:
			LogReadback(step);
			break;
		case VKRStepType::READBACK_IMAGE:
			LogReadbackImage(step);
			break;
		case VKRStepType::RENDER_SKIP:
			ILOG("(skipped render pass)");
			break;
		}
	}
}

void VulkanQueueRunner::LogRenderPass(const VKRStep &pass) {
	int fb = (int)(intptr_t)(pass.render.framebuffer ? pass.render.framebuffer->framebuf : 0);
	ILOG("RenderPass Begin(%x)", fb);
	for (auto &cmd : pass.commands) {
		switch (cmd.cmd) {
		case VKRRenderCommand::REMOVED:
			ILOG("  (Removed)");
			break;

		case VKRRenderCommand::BIND_PIPELINE:
			ILOG("  BindPipeline(%x)", (int)(intptr_t)cmd.pipeline.pipeline);
			break;
		case VKRRenderCommand::BLEND:
			ILOG("  BlendColor(%08x)", cmd.blendColor.color);
			break;
		case VKRRenderCommand::CLEAR:
			ILOG("  Clear");
			break;
		case VKRRenderCommand::DRAW:
			ILOG("  Draw(%d)", cmd.draw.count);
			break;
		case VKRRenderCommand::DRAW_INDEXED:
			ILOG("  DrawIndexed(%d)", cmd.drawIndexed.count);
			break;
		case VKRRenderCommand::SCISSOR:
			ILOG("  Scissor(%d, %d, %d, %d)", (int)cmd.scissor.scissor.offset.x, (int)cmd.scissor.scissor.offset.y, (int)cmd.scissor.scissor.extent.width, (int)cmd.scissor.scissor.extent.height);
			break;
		case VKRRenderCommand::STENCIL:
			ILOG("  Stencil(ref=%d, compare=%d, write=%d)", cmd.stencil.stencilRef, cmd.stencil.stencilCompareMask, cmd.stencil.stencilWriteMask);
			break;
		case VKRRenderCommand::VIEWPORT:
			ILOG("  Viewport(%f, %f, %f, %f, %f, %f)", cmd.viewport.vp.x, cmd.viewport.vp.y, cmd.viewport.vp.width, cmd.viewport.vp.height, cmd.viewport.vp.minDepth, cmd.viewport.vp.maxDepth);
			break;
		case VKRRenderCommand::PUSH_CONSTANTS:
			ILOG("  PushConstants(%d)", cmd.push.size);
			break;

		case VKRRenderCommand::NUM_RENDER_COMMANDS:
			break;
		}
	}
	ILOG("RenderPass End(%x)", fb);
}

void VulkanQueueRunner::LogCopy(const VKRStep &step) {
	ILOG("%s", StepToString(step).c_str());
}

void VulkanQueueRunner::LogBlit(const VKRStep &step) {
	ILOG("%s", StepToString(step).c_str());
}

void VulkanQueueRunner::LogReadback(const VKRStep &step) {
	ILOG("%s", StepToString(step).c_str());
}

void VulkanQueueRunner::LogReadbackImage(const VKRStep &step) {
	ILOG("%s", StepToString(step).c_str());
}

void VulkanQueueRunner::PerformRenderPass(const VKRStep &step, VkCommandBuffer cmd) {
	// TODO: If there are multiple, we can transition them together.
	for (const auto &iter : step.preTransitions) {
		if (iter.fb->color.layout != iter.targetLayout) {
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = iter.fb->color.layout;
			barrier.subresourceRange.layerCount = 1;
			barrier.subresourceRange.levelCount = 1;
			barrier.image = iter.fb->color.image;
			VkPipelineStageFlags srcStage{};
			VkPipelineStageFlags dstStage{};
			switch (barrier.oldLayout) {
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				break;
			case VK_IMAGE_LAYOUT_UNDEFINED:
				barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			default:
				_assert_msg_(G3D, false, "PerformRenderPass: Unexpected oldLayout: %d", (int)barrier.oldLayout);
				break;
			}
			barrier.newLayout = iter.targetLayout;
			switch (barrier.newLayout) {
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				break;
			default:
				_assert_msg_(G3D, false, "PerformRenderPass: Unexpected newLayout: %d", (int)barrier.newLayout);
				break;
			}
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			iter.fb->color.layout = barrier.newLayout;
		}
	}

	// Don't execute empty renderpasses that keep the contents.
	if (step.commands.empty() && step.render.color == VKRRenderPassAction::KEEP && step.render.depth == VKRRenderPassAction::KEEP && step.render.stencil == VKRRenderPassAction::KEEP) {
		// Nothing to do.
		return;
	}

	if (step.render.framebuffer && step.render.framebuffer->color.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.subresourceRange.layerCount = 1;
		barrier.subresourceRange.levelCount = 1;
		barrier.image = step.render.framebuffer->color.image;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	// This is supposed to bind a vulkan render pass to the command buffer.
	PerformBindFramebufferAsRenderTarget(step, cmd);

	int curWidth = step.render.framebuffer ? step.render.framebuffer->width : vulkan_->GetBackbufferWidth();
	int curHeight = step.render.framebuffer ? step.render.framebuffer->height : vulkan_->GetBackbufferHeight();

	VKRFramebuffer *fb = step.render.framebuffer;

	VkPipeline lastPipeline = VK_NULL_HANDLE;

	auto &commands = step.commands;

	// We can do a little bit of state tracking here to eliminate some calls into the driver.
	// The stencil ones are very commonly mostly redundant so let's eliminate them where possible.
	int lastStencilWriteMask = -1;
	int lastStencilCompareMask = -1;
	int lastStencilReference = -1;

	for (const auto &c : commands) {
		switch (c.cmd) {
		case VKRRenderCommand::REMOVED:
			break;

		case VKRRenderCommand::BIND_PIPELINE:
			if (c.pipeline.pipeline != lastPipeline) {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline.pipeline);
				lastPipeline = c.pipeline.pipeline;
				// Reset dynamic state so it gets refreshed with the new pipeline.
				lastStencilWriteMask = -1;
				lastStencilCompareMask = -1;
				lastStencilReference = -1;
			}
			break;

		case VKRRenderCommand::VIEWPORT:
			if (fb != nullptr) {
				vkCmdSetViewport(cmd, 0, 1, &c.viewport.vp);
			} else {
				const VkViewport &vp = c.viewport.vp;
				DisplayRect<float> rc{ vp.x, vp.y, vp.width, vp.height };
				RotateRectToDisplay(rc, (float)vulkan_->GetBackbufferWidth(), (float)vulkan_->GetBackbufferHeight());
				VkViewport final_vp;
				final_vp.x = rc.x;
				final_vp.y = rc.y;
				final_vp.width = rc.w;
				final_vp.height = rc.h;
				final_vp.maxDepth = vp.maxDepth;
				final_vp.minDepth = vp.minDepth;
				vkCmdSetViewport(cmd, 0, 1, &final_vp);
			}
			break;

		case VKRRenderCommand::SCISSOR:
		{
			if (fb != nullptr) {
				vkCmdSetScissor(cmd, 0, 1, &c.scissor.scissor);
			} else {
				// Rendering to backbuffer. Might need to rotate.
				const VkRect2D &rc = c.scissor.scissor;
				DisplayRect<int> rotated_rc{ rc.offset.x, rc.offset.y, (int)rc.extent.width, (int)rc.extent.height };
				RotateRectToDisplay(rotated_rc, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
				_dbg_assert_(G3D, rotated_rc.x >= 0);
				_dbg_assert_(G3D, rotated_rc.y >= 0);
				VkRect2D finalRect = VkRect2D{ { rotated_rc.x, rotated_rc.y }, { (uint32_t)rotated_rc.w, (uint32_t)rotated_rc.h} };
				vkCmdSetScissor(cmd, 0, 1, &finalRect);
			}
			break;
		}

		case VKRRenderCommand::BLEND:
		{
			float bc[4];
			Uint8x4ToFloat4(bc, c.blendColor.color);
			vkCmdSetBlendConstants(cmd, bc);
			break;
		}

		case VKRRenderCommand::PUSH_CONSTANTS:
			vkCmdPushConstants(cmd, c.push.pipelineLayout, c.push.stages, c.push.offset, c.push.size, c.push.data);
			break;

		case VKRRenderCommand::STENCIL:
			if (lastStencilWriteMask != c.stencil.stencilWriteMask) {
				lastStencilWriteMask = (int)c.stencil.stencilWriteMask;
				vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FRONT_AND_BACK, c.stencil.stencilWriteMask);
			}
			if (lastStencilCompareMask != c.stencil.stencilCompareMask) {
				lastStencilCompareMask = c.stencil.stencilCompareMask;
				vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FRONT_AND_BACK, c.stencil.stencilCompareMask);
			}
			if (lastStencilReference != c.stencil.stencilRef) {
				lastStencilReference = c.stencil.stencilRef;
				vkCmdSetStencilReference(cmd, VK_STENCIL_FRONT_AND_BACK, c.stencil.stencilRef);
			}
			break;

		case VKRRenderCommand::DRAW_INDEXED:
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.drawIndexed.pipelineLayout, 0, 1, &c.drawIndexed.ds, c.drawIndexed.numUboOffsets, c.drawIndexed.uboOffsets);
			vkCmdBindIndexBuffer(cmd, c.drawIndexed.ibuffer, c.drawIndexed.ioffset, c.drawIndexed.indexType);
			vkCmdBindVertexBuffers(cmd, 0, 1, &c.drawIndexed.vbuffer, &c.drawIndexed.voffset);
			vkCmdDrawIndexed(cmd, c.drawIndexed.count, c.drawIndexed.instances, 0, 0, 0);
			break;

		case VKRRenderCommand::DRAW:
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.draw.pipelineLayout, 0, 1, &c.draw.ds, c.draw.numUboOffsets, c.draw.uboOffsets);
			if (c.draw.vbuffer) {
				vkCmdBindVertexBuffers(cmd, 0, 1, &c.draw.vbuffer, &c.draw.voffset);
			}
			vkCmdDraw(cmd, c.draw.count, 1, 0, 0);
			break;

		case VKRRenderCommand::CLEAR:
		{
			// If we get here, we failed to merge a clear into a render pass load op. This is bad for perf.
			int numAttachments = 0;
			VkClearRect rc{};
			rc.baseArrayLayer = 0;
			rc.layerCount = 1;
			rc.rect.extent.width = (uint32_t)curWidth;
			rc.rect.extent.height = (uint32_t)curHeight;
			VkClearAttachment attachments[2];
			if (c.clear.clearMask & VK_IMAGE_ASPECT_COLOR_BIT) {
				VkClearAttachment &attachment = attachments[numAttachments++];
				attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				attachment.colorAttachment = 0;
				Uint8x4ToFloat4(attachment.clearValue.color.float32, c.clear.clearColor);
			}
			if (c.clear.clearMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
				VkClearAttachment &attachment = attachments[numAttachments++];
				attachment.aspectMask = 0;
				if (c.clear.clearMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
					attachment.clearValue.depthStencil.depth = c.clear.clearZ;
					attachment.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
				}
				if (c.clear.clearMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
					attachment.clearValue.depthStencil.stencil = (uint32_t)c.clear.clearStencil;
					attachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
				}
			}
			if (numAttachments) {
				vkCmdClearAttachments(cmd, numAttachments, attachments, 1, &rc);
			}
			break;
		}
		default:
			ELOG("Unimpl queue command");
			;
		}
	}
	vkCmdEndRenderPass(cmd);

	// The renderpass handles the layout transition.
	if (fb) {
		fb->color.layout = step.render.finalColorLayout;
	}
}

void VulkanQueueRunner::PerformBindFramebufferAsRenderTarget(const VKRStep &step, VkCommandBuffer cmd) {
	VkRenderPass renderPass;
	int numClearVals = 0;
	VkClearValue clearVal[2]{};
	VkFramebuffer framebuf;
	int w;
	int h;
	if (step.render.framebuffer) {
		_dbg_assert_(G3D, step.render.finalColorLayout != VK_IMAGE_LAYOUT_UNDEFINED);

		VKRFramebuffer *fb = step.render.framebuffer;
		framebuf = fb->framebuf;
		w = fb->width;
		h = fb->height;

		// Mali driver on S8 (Android O) and S9 mishandles renderpasses that do just a clear
		// and then no draw calls. Memory transaction elimination gets mis-flagged or something.
		// To avoid this, we transition to GENERAL and back in this case (ARM-approved workaround).
		// See pull request #10723.
		bool maliBugWorkaround = step.render.numDraws == 0 &&
			step.render.color == VKRRenderPassAction::CLEAR &&
			vulkan_->GetPhysicalDeviceProperties().properties.driverVersion == 0xaa9c4b29;
		if (maliBugWorkaround) {
			TransitionImageLayout2(cmd, step.render.framebuffer->color.image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
				fb->color.layout, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
			fb->color.layout = VK_IMAGE_LAYOUT_GENERAL;
		}

		renderPass = GetRenderPass(
			step.render.color, step.render.depth, step.render.stencil,
			fb->color.layout, fb->depth.layout, step.render.finalColorLayout);

		// We now do any layout pretransitions as part of the render pass.
		fb->color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		fb->depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		if (step.render.color == VKRRenderPassAction::CLEAR) {
			Uint8x4ToFloat4(clearVal[0].color.float32, step.render.clearColor);
			numClearVals = 1;
		}
		if (step.render.depth == VKRRenderPassAction::CLEAR || step.render.stencil == VKRRenderPassAction::CLEAR) {
			clearVal[1].depthStencil.depth = step.render.clearDepth;
			clearVal[1].depthStencil.stencil = step.render.clearStencil;
			numClearVals = 2;
		}
	} else {
		framebuf = backbuffer_;
		w = vulkan_->GetBackbufferWidth();
		h = vulkan_->GetBackbufferHeight();
		renderPass = GetBackbufferRenderPass();
		Uint8x4ToFloat4(clearVal[0].color.float32, step.render.clearColor);
		numClearVals = 2;  // We don't bother with a depth buffer here.
		clearVal[1].depthStencil.depth = 0.0f;
		clearVal[1].depthStencil.stencil = 0;
	}

	VkRenderPassBeginInfo rp_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	rp_begin.renderPass = renderPass;
	rp_begin.framebuffer = framebuf;
	rp_begin.renderArea.offset.x = 0;
	rp_begin.renderArea.offset.y = 0;
	rp_begin.renderArea.extent.width = w;
	rp_begin.renderArea.extent.height = h;
	rp_begin.clearValueCount = numClearVals;
	rp_begin.pClearValues = numClearVals ? clearVal : nullptr;
	vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanQueueRunner::PerformCopy(const VKRStep &step, VkCommandBuffer cmd) {
	VKRFramebuffer *src = step.copy.src;
	VKRFramebuffer *dst = step.copy.dst;

	VkImageCopy copy{};
	copy.srcOffset.x = step.copy.srcRect.offset.x;
	copy.srcOffset.y = step.copy.srcRect.offset.y;
	copy.srcOffset.z = 0;
	copy.srcSubresource.mipLevel = 0;
	copy.srcSubresource.layerCount = 1;
	copy.dstOffset.x = step.copy.dstPos.x;
	copy.dstOffset.y = step.copy.dstPos.y;
	copy.dstOffset.z = 0;
	copy.dstSubresource.mipLevel = 0;
	copy.dstSubresource.layerCount = 1;
	copy.extent.width = step.copy.srcRect.extent.width;
	copy.extent.height = step.copy.srcRect.extent.height;
	copy.extent.depth = 1;

	VkImageMemoryBarrier srcBarriers[2]{};
	VkImageMemoryBarrier dstBarriers[2]{};
	int srcCount = 0;
	int dstCount = 0;

	VkPipelineStageFlags srcStage = 0;
	VkPipelineStageFlags dstStage = 0;
	// First source barriers.
	if (step.copy.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		if (src->color.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			SetupTransitionToTransferSrc(src->color, srcBarriers[srcCount++], srcStage, VK_IMAGE_ASPECT_COLOR_BIT);
		}
		if (dst->color.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			SetupTransitionToTransferDst(dst->color, dstBarriers[dstCount++], dstStage, VK_IMAGE_ASPECT_COLOR_BIT);
		}
	}

	// We can't copy only depth or only stencil unfortunately.
	if (step.copy.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		if (src->depth.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			SetupTransitionToTransferSrc(src->depth, srcBarriers[srcCount++], srcStage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		}
		if (dst->depth.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			SetupTransitionToTransferDst(dst->depth, dstBarriers[dstCount++], dstStage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		}
	}

	if (srcCount) {
		vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, srcCount, srcBarriers);
	}
	if (dstCount) {
		vkCmdPipelineBarrier(cmd, dstStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, dstCount, dstBarriers);
	}

	if (step.copy.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vkCmdCopyImage(cmd, src->color.image, src->color.layout, dst->color.image, dst->color.layout, 1, &copy);
	}
	if (step.copy.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		copy.srcSubresource.aspectMask = 0;
		copy.dstSubresource.aspectMask = 0;
		if (step.copy.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
			copy.srcSubresource.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
			copy.dstSubresource.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		if (step.copy.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
			copy.srcSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			copy.dstSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
		vkCmdCopyImage(cmd, src->depth.image, src->depth.layout, dst->depth.image, dst->depth.layout, 1, &copy);
	}
}

void VulkanQueueRunner::PerformBlit(const VKRStep &step, VkCommandBuffer cmd) {
	VkImageMemoryBarrier srcBarriers[2]{};
	VkImageMemoryBarrier dstBarriers[2]{};

	VKRFramebuffer *src = step.blit.src;
	VKRFramebuffer *dst = step.blit.dst;

	// If any validation needs to be performed here, it should probably have been done
	// already when the blit was queued. So don't validate here.
	VkImageBlit blit{};
	blit.srcOffsets[0].x = step.blit.srcRect.offset.x;
	blit.srcOffsets[0].y = step.blit.srcRect.offset.y;
	blit.srcOffsets[0].z = 0;
	blit.srcOffsets[1].x = step.blit.srcRect.offset.x + step.blit.srcRect.extent.width;
	blit.srcOffsets[1].y = step.blit.srcRect.offset.y + step.blit.srcRect.extent.height;
	blit.srcOffsets[1].z = 1;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.layerCount = 1;
	blit.dstOffsets[0].x = step.blit.dstRect.offset.x;
	blit.dstOffsets[0].y = step.blit.dstRect.offset.y;
	blit.dstOffsets[0].z = 0;
	blit.dstOffsets[1].x = step.blit.dstRect.offset.x + step.blit.dstRect.extent.width;
	blit.dstOffsets[1].y = step.blit.dstRect.offset.y + step.blit.dstRect.extent.height;
	blit.dstOffsets[1].z = 1;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.layerCount = 1;

	VkPipelineStageFlags srcStage = 0;
	VkPipelineStageFlags dstStage = 0;

	int srcCount = 0;
	int dstCount = 0;

	// First source barriers.
	if (step.blit.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		if (src->color.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			SetupTransitionToTransferSrc(src->color, srcBarriers[srcCount++], srcStage, VK_IMAGE_ASPECT_COLOR_BIT);
		}
		if (dst->color.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			SetupTransitionToTransferDst(dst->color, dstBarriers[dstCount++], dstStage, VK_IMAGE_ASPECT_COLOR_BIT);
		}
	}

	// We can't copy only depth or only stencil unfortunately.
	if (step.blit.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		if (src->depth.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			SetupTransitionToTransferSrc(src->depth, srcBarriers[srcCount++], srcStage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		}
		if (dst->depth.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			SetupTransitionToTransferDst(dst->depth, dstBarriers[dstCount++], dstStage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		}
	}

	if (srcCount) {
		vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, srcCount, srcBarriers);
	}
	if (dstCount) {
		vkCmdPipelineBarrier(cmd, dstStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, dstCount, dstBarriers);
	}

	if (step.blit.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vkCmdBlitImage(cmd, src->color.image, src->color.layout, dst->color.image, dst->color.layout, 1, &blit, step.blit.filter);
	}

	// TODO: Need to check if the depth format is blittable.
	// Actually, we should probably almost always use copies rather than blits for depth buffers.
	if (step.blit.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		blit.srcSubresource.aspectMask = 0;
		blit.dstSubresource.aspectMask = 0;
		if (step.blit.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
			blit.srcSubresource.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
			blit.dstSubresource.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		if (step.blit.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
			blit.srcSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			blit.dstSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
		vkCmdBlitImage(cmd, src->depth.image, src->depth.layout, dst->depth.image, dst->depth.layout, 1, &blit, step.blit.filter);
	}
}

void VulkanQueueRunner::SetupTransitionToTransferSrc(VKRImage &img, VkImageMemoryBarrier &barrier, VkPipelineStageFlags &stage, VkImageAspectFlags aspect) {
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = img.layout;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.levelCount = 1;
	barrier.image = img.image;
	barrier.srcAccessMask = 0;
	switch (img.layout) {
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		stage |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	default:
		_dbg_assert_msg_(G3D, false, "Transition from this layout to transfer src not supported (%d)", (int)img.layout);
		break;
	}
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	if (img.format == VK_FORMAT_D16_UNORM_S8_UINT || img.format == VK_FORMAT_D24_UNORM_S8_UINT || img.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
		// Barrier must specify both for combined depth/stencil buffers.
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	} else {
		barrier.subresourceRange.aspectMask = aspect;
	}
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	img.layout = barrier.newLayout;

	// NOTE: Must do this AFTER updating img.layout to avoid behaviour differences.
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_COLOR
	if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
		if (barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || barrier.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (barrier.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
#endif
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_DEPTH_STENCIL
	if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
		if (barrier.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || barrier.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (barrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
#endif
}

void VulkanQueueRunner::SetupTransitionToTransferDst(VKRImage &img, VkImageMemoryBarrier &barrier, VkPipelineStageFlags &stage, VkImageAspectFlags aspect) {
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = img.layout;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.levelCount = 1;
	barrier.image = img.image;
	barrier.srcAccessMask = 0;
	switch (img.layout) {
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		stage |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	default:
		_dbg_assert_msg_(G3D, false, "Transition from this layout to transfer dst not supported (%d)", (int)img.layout);
		break;
	}
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	if (img.format == VK_FORMAT_D16_UNORM_S8_UINT || img.format == VK_FORMAT_D24_UNORM_S8_UINT || img.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
		// Barrier must specify both for combined depth/stencil buffers.
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	} else {
		barrier.subresourceRange.aspectMask = aspect;
	}
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	img.layout = barrier.newLayout;

	// NOTE: Must do this AFTER updating img.layout to avoid behaviour differences.
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_COLOR
	if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
		if (barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || barrier.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (barrier.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
#endif
#ifdef VULKAN_USE_GENERAL_LAYOUT_FOR_DEPTH_STENCIL
	if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
		if (barrier.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || barrier.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (barrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
#endif
}

void VulkanQueueRunner::PerformReadback(const VKRStep &step, VkCommandBuffer cmd) {
	ResizeReadbackBuffer(sizeof(uint32_t) * step.readback.srcRect.extent.width * step.readback.srcRect.extent.height);

	VkBufferImageCopy region{};
	region.imageOffset = { step.readback.srcRect.offset.x, step.readback.srcRect.offset.y, 0 };
	region.imageExtent = { step.readback.srcRect.extent.width, step.readback.srcRect.extent.height, 1 };
	region.imageSubresource.aspectMask = step.readback.aspectMask;
	region.imageSubresource.layerCount = 1;
	region.bufferOffset = 0;
	region.bufferRowLength = step.readback.srcRect.extent.width;
	region.bufferImageHeight = step.readback.srcRect.extent.height;

	VkImage image;
	VkImageLayout copyLayout;
	// Special case for backbuffer readbacks.
	if (step.readback.src == nullptr) {
		// We only take screenshots after the main render pass (anything else would be stupid) so we need to transition out of PRESENT,
		// and then back into it.
		TransitionImageLayout2(cmd, backbufferImage_, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, VK_ACCESS_TRANSFER_READ_BIT);
		copyLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		image = backbufferImage_;
	} else {
		VKRImage *srcImage;
		if (step.readback.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
			srcImage = &step.readback.src->color;
		} else if (step.readback.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
			srcImage = &step.readback.src->depth;
		} else {
			_dbg_assert_msg_(G3D, false, "No image aspect to readback?");
			return;
		}

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		VkPipelineStageFlags stage = 0;
		if (srcImage->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			SetupTransitionToTransferSrc(*srcImage, barrier, stage, step.readback.aspectMask);
			vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		image = srcImage->image;
		copyLayout = srcImage->layout;
	}

	vkCmdCopyImageToBuffer(cmd, image, copyLayout, readbackBuffer_, 1, &region);

	// NOTE: Can't read the buffer using the CPU here - need to sync first.

	// If we copied from the backbuffer, transition it back.
	if (step.readback.src == nullptr) {
		// We only take screenshots after the main render pass (anything else would be stupid) so we need to transition out of PRESENT,
		// and then back into it.
		TransitionImageLayout2(cmd, backbufferImage_, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_ACCESS_TRANSFER_READ_BIT, 0);
		copyLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	}
}

void VulkanQueueRunner::PerformReadbackImage(const VKRStep &step, VkCommandBuffer cmd) {
	// TODO: Clean this up - just reusing `SetupTransitionToTransferSrc`.
	VKRImage srcImage;
	srcImage.image = step.readback_image.image;
	srcImage.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkPipelineStageFlags stage = 0;
	SetupTransitionToTransferSrc(srcImage, barrier, stage, VK_IMAGE_ASPECT_COLOR_BIT);
	vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	ResizeReadbackBuffer(sizeof(uint32_t) * step.readback_image.srcRect.extent.width * step.readback_image.srcRect.extent.height);

	VkBufferImageCopy region{};
	region.imageOffset = { step.readback_image.srcRect.offset.x, step.readback_image.srcRect.offset.y, 0 };
	region.imageExtent = { step.readback_image.srcRect.extent.width, step.readback_image.srcRect.extent.height, 1 };
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = step.readback_image.mipLevel;
	region.bufferOffset = 0;
	region.bufferRowLength = step.readback_image.srcRect.extent.width;
	region.bufferImageHeight = step.readback_image.srcRect.extent.height;
	vkCmdCopyImageToBuffer(cmd, step.readback_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_, 1, &region);

	// Now transfer it back to a texture.
	TransitionImageLayout2(cmd, step.readback_image.image, 0, 1,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);

	// NOTE: Can't read the buffer using the CPU here - need to sync first.
	// Doing that will also act like a heavyweight barrier ensuring that device writes are visible on the host.
}

void VulkanQueueRunner::CopyReadbackBuffer(int width, int height, Draw::DataFormat srcFormat, Draw::DataFormat destFormat, int pixelStride, uint8_t *pixels) {
	if (!readbackMemory_)
		return;  // Something has gone really wrong.

	// Read back to the requested address in ram from buffer.
	void *mappedData;
	const size_t srcPixelSize = DataFormatSizeInBytes(srcFormat);

	VkResult res = vkMapMemory(vulkan_->GetDevice(), readbackMemory_, 0, width * height * srcPixelSize, 0, &mappedData);
	if (!readbackBufferIsCoherent_) {
		VkMappedMemoryRange range{};
		range.memory = readbackMemory_;
		range.offset = 0;
		range.size = width * height * srcPixelSize;
		vkInvalidateMappedMemoryRanges(vulkan_->GetDevice(), 1, &range);
	}

	if (res != VK_SUCCESS) {
		ELOG("CopyReadbackBuffer: vkMapMemory failed! result=%d", (int)res);
		return;
	}

	// TODO: Perform these conversions in a compute shader on the GPU.
	if (srcFormat == Draw::DataFormat::R8G8B8A8_UNORM) {
		ConvertFromRGBA8888(pixels, (const uint8_t *)mappedData, pixelStride, width, width, height, destFormat);
	} else if (srcFormat == Draw::DataFormat::B8G8R8A8_UNORM) {
		ConvertFromBGRA8888(pixels, (const uint8_t *)mappedData, pixelStride, width, width, height, destFormat);
	} else if (srcFormat == destFormat) {
		uint8_t *dst = pixels;
		const uint8_t *src = (const uint8_t *)mappedData;
		for (int y = 0; y < height; ++y) {
			memcpy(dst, src, width * srcPixelSize);
			src += width * srcPixelSize;
			dst += pixelStride * srcPixelSize;
		}
	} else if (destFormat == Draw::DataFormat::D32F) {
		ConvertToD32F(pixels, (const uint8_t *)mappedData, pixelStride, width, width, height, srcFormat);
	} else {
		// TODO: Maybe a depth conversion or something?
		ELOG("CopyReadbackBuffer: Unknown format");
		assert(false);
	}
	vkUnmapMemory(vulkan_->GetDevice(), readbackMemory_);
}
