// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Config.h"
#include "Counters.h"
#include "GS/GS.h"
#include "GS/GSShaderCompileIndicator.h"
#include "GS/GSCapture.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"
#ifdef _WIN32
#include "GS/Renderers/DX12/GSDevice12.h"
#endif
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiAnimated.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ImGuiOverlays.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "Recording/InputRecording.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "Zdxsv/Ggpo.h"

#include "common/BitUtils.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/chrono.h"
#include "fmt/format.h"
#include "imgui.h"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <tuple>

InputRecordingUI::InputRecordingData g_InputRecordingData;

// Start timers at 0 so we immediately get lines to cache.
static constexpr double ONE_BILLION = 1000000000;
static constexpr double UPDATE_INTERVAL = 0.1 * ONE_BILLION;
static constexpr double UPDATE_INTERVAL_CPU_INFO = 5.0 * ONE_BILLION;
Common::Timer s_last_update_timer = Common::Timer(0.0);
Common::Timer s_last_update_timer_cpu_info = Common::Timer(0.0);

ImU32 s_speed_line_color;
SmallString s_speed_line;
SmallString s_gs_stats_line;
SmallString s_gs_memory_stats_line;
SmallString s_gs_frame_times_line;
SmallString s_resolution_line;
SmallString s_hardware_info_cpu_line;
SmallString s_hardware_info_gpu_line;
SmallString s_cpu_usage_ee_line;
SmallString s_cpu_usage_gs_line;
SmallString s_cpu_usage_vu_line;
std::vector<SmallString> s_software_thread_lines;
SmallString s_capture_line;
SmallString s_gpu_usage_line;
SmallString s_gpu_debug_info_line;
SmallString s_gpu_stats_line;
SmallString s_speed_icon;

constexpr ImU32 white_color = IM_COL32(255, 255, 255, 255);

// OSD positioning funcs
ImVec2 CalculateOSDPosition(OsdOverlayPos position, float margin, const ImVec2& text_size, float window_width, float window_height)
{
	switch (position)
	{
		case OsdOverlayPos::TopLeft:
			return ImVec2(margin, margin);
		case OsdOverlayPos::TopCenter:
			return ImVec2((window_width - text_size.x) * 0.5f, margin);
		case OsdOverlayPos::TopRight:
			return ImVec2(window_width - margin - text_size.x, margin);
		case OsdOverlayPos::CenterLeft:
			return ImVec2(margin, (window_height - text_size.y) * 0.5f);
		case OsdOverlayPos::Center:
			return ImVec2((window_width - text_size.x) * 0.5f, (window_height - text_size.y) * 0.5f);
		case OsdOverlayPos::CenterRight:
			return ImVec2(window_width - margin - text_size.x, (window_height - text_size.y) * 0.5f);
		case OsdOverlayPos::BottomLeft:
			return ImVec2(margin, window_height - margin - text_size.y);
		case OsdOverlayPos::BottomCenter:
			return ImVec2((window_width - text_size.x) * 0.5f, window_height - margin - text_size.y);
		case OsdOverlayPos::BottomRight:
			return ImVec2(window_width - margin - text_size.x, window_height - margin - text_size.y);
		case OsdOverlayPos::None:
		default:
			return ImVec2(0.0f, 0.0f);
	}
}

ImVec2 CalculatePerformanceOverlayTextPosition(OsdOverlayPos position, float margin, const ImVec2& text_size, float window_width, float position_y)
{
	const float abs_margin = std::abs(margin);

	// Get the X position based on horizontal alignment
	float x_pos;
	switch (position)
	{
		case OsdOverlayPos::TopLeft:
		case OsdOverlayPos::CenterLeft:
		case OsdOverlayPos::BottomLeft:
			x_pos = abs_margin; // Left alignment
			break;

		case OsdOverlayPos::TopCenter:
		case OsdOverlayPos::Center:
		case OsdOverlayPos::BottomCenter:
			x_pos = (window_width - text_size.x) * 0.5f; // Center alignment
			break;

		case OsdOverlayPos::TopRight:
		case OsdOverlayPos::CenterRight:
		case OsdOverlayPos::BottomRight:
		default:
			x_pos = window_width - text_size.x - abs_margin; // Right alignment
			break;
	}

	return ImVec2(x_pos, position_y);
}

bool ShouldUseLeftAlignment(OsdOverlayPos position)
{
	return (position == OsdOverlayPos::TopLeft || position == OsdOverlayPos::CenterLeft || position == OsdOverlayPos::BottomLeft);
}

namespace ImGuiManager
{
	static void FormatProcessorStat(SmallStringBase& text, double usage, double time);
	static void DrawPerformanceOverlay(float& position_y, float scale, float margin, float spacing);
	static void DrawShaderCompileIndicator(float scale, float margin, float spacing);
	static void DrawSettingsOverlay(float scale, float margin, float spacing);
	static void DrawInputsOverlay(float scale, float margin, float spacing);
	static void DrawInputRecordingOverlay(float& position_y, float scale, float margin, float spacing);
	static void DrawVideoCaptureOverlay(float& position_y, float scale, float margin, float spacing);
	static void DrawTextureReplacementsOverlay(float& position_y, float scale, float margin, float spacing);
	static void DrawIndicatorsOverlay(float& position_y, float scale, float margin, float spacing);
	static void DrawZdxsvGgpoOverlay(float scale, float margin, float spacing);
	static void DrawZdxsvReplayBar(float scale, float margin);
	static void DrawZdxsvReplayKeys(float scale, float margin);
} // namespace ImGuiManager

static std::tuple<float, float> GetMinMax(std::span<const float> values)
{
	GSVector4 vmin(GSVector4::load<false>(values.data()));
	GSVector4 vmax(vmin);

	const u32 count = static_cast<u32>(values.size());
	const u32 aligned_count = Common::AlignDownPow2(count, 4);
	u32 i = 4;
	for (; i < aligned_count; i += 4)
	{
		const GSVector4 v(GSVector4::load<false>(&values[i]));
		vmin = vmin.min(v);
		vmax = vmax.max(v);
	}

	float min = std::min(vmin.x, std::min(vmin.y, std::min(vmin.z, vmin.w)));
	float max = std::max(vmax.x, std::max(vmax.y, std::max(vmax.z, vmax.w)));
	for (; i < count; i++)
	{
		min = std::min(min, values[i]);
		max = std::max(max, values[i]);
	}

	return std::tie(min, max);
}

__ri void ImGuiManager::FormatProcessorStat(SmallStringBase& text, double usage, double time)
{
	// Some values, such as GPU (and even CPU to some extent) can be out of phase with the wall clock,
	// which the processor time is divided by to get a utilization percentage. Let's clamp it at 100%,
	// so that people don't get confused, and remove the decimal places when it's there while we're at it.
	if (usage >= 99.95)
		text.append_format("100% ({:.2f}ms)", time);
	else
		text.append_format("{:.1f}% ({:.2f}ms)", usage, time);
}

__ri void ImGuiManager::DrawPerformanceOverlay(float& position_y, float scale, float margin, float spacing)
{
	const float shadow_offset = std::ceil(scale);

	ImFont* const osd_font = ImGuiManager::GetOSDFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float line_height = ImGuiFullscreen::GetLineHeight({ osd_font, font_size });

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImVec2 text_size;

	// Adjust initial Y position based on vertical alignment
	switch (GSConfig.OsdPerformancePos)
	{
		case OsdOverlayPos::CenterLeft:
		case OsdOverlayPos::Center:
		case OsdOverlayPos::CenterRight:

			position_y = (GetWindowHeight() - (line_height * 8.0f)) * 0.5f;
			break;

		case OsdOverlayPos::BottomLeft:
		case OsdOverlayPos::BottomCenter:
		case OsdOverlayPos::BottomRight:

			position_y = GetWindowHeight() - margin - (line_height * 15.0f + spacing * 14.0f);
			break;

		case OsdOverlayPos::TopLeft:
		case OsdOverlayPos::TopCenter:
		case OsdOverlayPos::TopRight:
		default:
			// Top alignment keeps the passed position_y
			break;
	}

#define DRAW_LINE(font, size, text, color) \
	do \
	{ \
		text_size = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr); \
		const ImVec2 text_pos = CalculatePerformanceOverlayTextPosition(GSConfig.OsdPerformancePos, margin, text_size, GetWindowWidth(), position_y); \
		const bool __bold_osd = GSConfig.OsdBoldText; \
		dl->AddText(font, size, ImVec2(text_pos.x + shadow_offset, text_pos.y + shadow_offset), IM_COL32(0, 0, 0, 100), (text)); \
		dl->AddText(font, size, text_pos, color, (text)); \
		const auto __is_all_digits = [](const char* __begin, const char* __end) -> bool \
		{ \
			for (const char* __c = __begin; __c < __end; __c++) \
			{ \
				if (!std::isdigit(static_cast<unsigned char>(*__c))) \
					return false; \
			} \
			return true; \
		}; \
		const auto __is_all_alpha = [](const char* __begin, const char* __end) -> bool \
		{ \
			for (const char* __c = __begin; __c < __end; __c++) \
			{ \
				if (!std::isalpha(static_cast<unsigned char>(*__c))) \
					return false; \
			} \
			return true; \
		}; \
		for (const char* __p = (text); __p && *__p;) \
		{ \
			const char* __sep = strstr(__p, " | "); \
			const char* __seg_end = __sep ? __sep : __p + strlen(__p); \
			const char* __label_begin = nullptr; \
			const char* __label_end = nullptr; \
			const unsigned char __first = static_cast<unsigned char>(*__p); \
			const bool __starts_numeric = (std::isdigit(__first) || __first == '+' || __first == '-' || __first == '.'); \
			for (const char* __c = __p; __c < __seg_end; __c++) \
			{ \
				if (*__c == ':') \
				{ \
					__label_begin = __p; \
					__label_end = __c + 1; \
					break; \
				} \
			} \
			if (!__label_begin) \
			{ \
				if (!__starts_numeric) \
				{ \
					__label_begin = __p; \
					__label_end = __seg_end; \
				} \
				else \
				{ \
					const char* __first_space = __p; \
					while (__first_space < __seg_end && *__first_space != ' ') \
						__first_space++; \
					if (__first_space < __seg_end) \
					{ \
						const char* __x = __p; \
						while (__x < __first_space && *__x != 'x' && *__x != 'X') \
							__x++; \
						const bool __has_resolution_prefix = \
							(__x > __p && (__x + 1) < __first_space && \
							__is_all_digits(__p, __x) && __is_all_digits(__x + 1, __first_space)); \
						if (__has_resolution_prefix && (__first_space + 1) < __seg_end) \
						{ \
							__label_begin = __first_space + 1; \
							__label_end = __seg_end; \
							const char* __trim_end = __label_end; \
							while (__trim_end > __label_begin && std::isspace(static_cast<unsigned char>(*(__trim_end - 1)))) \
								__trim_end--; \
							if (__trim_end > __label_begin && *(__trim_end - 1) == ')') \
							{ \
								const char* __open = __trim_end - 1; \
								while (__open > __label_begin && *__open != '(') \
									__open--; \
								if (__open > __label_begin && *__open == '(' && *(__open - 1) == ' ') \
									__label_end = __open - 1; \
							} \
						} \
					} \
					if (!__label_begin) \
					{ \
						const char* __space = __seg_end; \
						while (__space > __p && *(__space - 1) != ' ') \
							__space--; \
						if (__space > __p && __space < __seg_end && __is_all_alpha(__space, __seg_end)) \
						{ \
							__label_begin = __space; \
							__label_end = __seg_end; \
						} \
					} \
				} \
			} \
			if (__label_begin && __label_end && __label_begin < __label_end) \
			{ \
				const float __x0 = font->CalcTextSizeA(size, FLT_MAX, -1.0f, (text), __label_begin).x; \
				const ImVec2 __pos(text_pos.x + __x0, text_pos.y); \
				if (__bold_osd) \
				{ \
					dl->AddText(font, size, __pos, color, __label_begin, __label_end); \
					dl->AddText(font, size, ImVec2(__pos.x + 0.6f, __pos.y), color, __label_begin, __label_end); \
				} \
			} \
			__p = __sep ? __sep + 3 : nullptr; \
		} \
		position_y += text_size.y + spacing; \
	} while (0)

	if (VMManager::GetState() != VMState::Paused)
	{
		if (s_last_update_timer.GetTimeNanoseconds() >= UPDATE_INTERVAL)
		{
			s_last_update_timer.Reset();
			const float speed = PerformanceMetrics::GetSpeed();

			s_speed_line.clear();
			if (GSConfig.OsdShowFPS)
			{
				switch (PerformanceMetrics::GetInternalFPSMethod())
				{
					case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
						s_speed_line.append_format("FPS: {:.2f} [P]", PerformanceMetrics::GetInternalFPS());
						break;

					case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
						s_speed_line.append_format("FPS: {:.2f} [B]", PerformanceMetrics::GetInternalFPS());
						break;

					case PerformanceMetrics::InternalFPSMethod::None:
					default:
						s_speed_line.append("FPS: N/A");
						break;
				}
			}

			if (GSConfig.OsdShowVPS)
				s_speed_line.append_format("{}VPS: {:.2f}", s_speed_line.empty() ? "" : " | ", PerformanceMetrics::GetFPS());

			if (GSConfig.OsdShowSpeed)
			{
				s_speed_line.append_format("{}Speed: {}%", s_speed_line.empty() ? "" : " | ", static_cast<u32>(std::round(speed)));

				const float target_speed = VMManager::GetTargetSpeed();
				if (target_speed == 0.0f)
					s_speed_line.append(" (T: Max)");
				else
					s_speed_line.append_format(" (T: {:.0f}%)", target_speed * 100.0f);
			}

			if (GSConfig.OsdShowVersion)
				s_speed_line.append_format("{}PCSX2 {}", s_speed_line.empty() ? "" : " | ", BuildVersion::GitRev);

			if (!s_speed_line.empty())
			{
				if (speed < 95.0f)
					s_speed_line_color = IM_COL32(255, 100, 100, 255); // red
				else if (speed > 105.0f)
					s_speed_line_color = IM_COL32(100, 255, 100, 255); // green
				else
					s_speed_line_color = white_color;

				DRAW_LINE(osd_font, font_size, s_speed_line.c_str(), s_speed_line_color);
			}

			if (GSConfig.OsdShowGSStats)
			{
				GSgetStats(s_gs_stats_line);
				GSgetMemoryStats(s_gs_memory_stats_line);
				s_gs_frame_times_line.format("{} QF | Min: {:.2f}ms | Avg: {:.2f}ms | Max: {:.2f}ms",
					MTGS::GetCurrentVsyncQueueSize() - 1, // subtract one for the current frame
					PerformanceMetrics::GetMinimumFrameTime(),
					PerformanceMetrics::GetAverageFrameTime(),
					PerformanceMetrics::GetMaximumFrameTime());

				if (!s_gs_stats_line.empty())
					DRAW_LINE(osd_font, font_size, s_gs_stats_line.c_str(), white_color);
				if (!s_gs_memory_stats_line.empty())
					DRAW_LINE(osd_font, font_size, s_gs_memory_stats_line.c_str(), white_color);
				DRAW_LINE(osd_font, font_size, s_gs_frame_times_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowResolution)
			{
				int iwidth, iheight;
				GSgetInternalResolution(&iwidth, &iheight);

				s_resolution_line.format("{}x{} {} {}", iwidth, iheight, ReportVideoMode(), ReportInterlaceMode());
				DRAW_LINE(osd_font, font_size, s_resolution_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowHardwareInfo)
			{
				// GPU can change on the fly with settings, but CPU change of any kind is a rare edge case.
				if (s_last_update_timer_cpu_info.GetTimeNanoseconds() >= UPDATE_INTERVAL_CPU_INFO)
				{
					s_last_update_timer_cpu_info.Reset();

					// CPU
					const CPUInfo& info = GetCPUInfo();
					const bool has_small = info.num_small_cores > 0;
					const bool has_smt = info.num_threads != info.num_big_cores + info.num_small_cores;
					s_hardware_info_cpu_line.format("CPU: {}", !info.name.empty() ? info.name : "Unknown");
					if (has_smt && has_small)
						s_hardware_info_cpu_line.append_format(" ({}P/{}E/{}T)", info.num_big_cores, info.num_small_cores, info.num_threads);
					else if (has_small)
						s_hardware_info_cpu_line.append_format(" ({}P/{}E)", info.num_big_cores, info.num_small_cores);
					else
						s_hardware_info_cpu_line.append_format(" ({}C/{}T)", info.num_big_cores, info.num_threads);
				}

				DRAW_LINE(osd_font, font_size, s_hardware_info_cpu_line.c_str(), white_color);

				// GPU
				const char* gpu_suffix = "";

				if (GSConfig.Renderer != GSRendererType::SW)
				{
					if (GSConfig.UseDebugDevice && GSConfig.HWROV)
						gpu_suffix = " (Debug & ROV)";
					else if (GSConfig.UseDebugDevice)
						gpu_suffix = " (Debug)";
					else if (GSConfig.HWROV)
						gpu_suffix = " (ROV)";
				}

				s_hardware_info_gpu_line.format(
					"GPU: {}{}",
					g_gs_device->GetName(),
					gpu_suffix);

				DRAW_LINE(osd_font, font_size, s_hardware_info_gpu_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowCPU)
			{
				if (EmuConfig.Speedhacks.EECycleRate != 0 || EmuConfig.Speedhacks.EECycleSkip != 0)
					s_cpu_usage_ee_line.format("EE[{}/{}]: ", EmuConfig.Speedhacks.EECycleRate, EmuConfig.Speedhacks.EECycleSkip);
				else
					s_cpu_usage_ee_line.assign("EE: ");
				FormatProcessorStat(s_cpu_usage_ee_line, PerformanceMetrics::GetCPUThreadUsage(), PerformanceMetrics::GetCPUThreadAverageTime());
				DRAW_LINE(osd_font, font_size, s_cpu_usage_ee_line.c_str(), white_color);

				s_cpu_usage_gs_line.assign("GS: ");
				FormatProcessorStat(s_cpu_usage_gs_line, PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetGSThreadAverageTime());
				DRAW_LINE(osd_font, font_size, s_cpu_usage_gs_line.c_str(), white_color);

				if (THREAD_VU1)
				{
					s_cpu_usage_vu_line.assign("VU: ");
					FormatProcessorStat(s_cpu_usage_vu_line, PerformanceMetrics::GetVUThreadUsage(), PerformanceMetrics::GetVUThreadAverageTime());
					DRAW_LINE(osd_font, font_size, s_cpu_usage_vu_line.c_str(), white_color);
				}

				const u32 gs_sw_threads = PerformanceMetrics::GetGSSWThreadCount();
				for (u32 thread = 0; thread < gs_sw_threads; thread++)
				{
					if (thread < s_software_thread_lines.size())
						s_software_thread_lines[thread].format("SW-{}: ", thread);
					else
						s_software_thread_lines.push_back(SmallString("SW-{}: ", thread));
					FormatProcessorStat(s_software_thread_lines[thread], PerformanceMetrics::GetGSSWThreadUsage(thread), PerformanceMetrics::GetGSSWThreadAverageTime(thread));
					DRAW_LINE(osd_font, font_size, s_software_thread_lines[thread].c_str(), white_color);
				}

				if (GSCapture::IsCapturing())
				{
					s_capture_line.assign("CAP: ");
					FormatProcessorStat(s_capture_line, PerformanceMetrics::GetCaptureThreadUsage(), PerformanceMetrics::GetCaptureThreadAverageTime());
					DRAW_LINE(osd_font, font_size, s_capture_line.c_str(), white_color);
				}
			}

			if (GSConfig.OsdShowGPU)
			{
				s_gpu_usage_line.assign("GPU: ");
				FormatProcessorStat(s_gpu_usage_line, PerformanceMetrics::GetGPUUsage(), PerformanceMetrics::GetGPUAverageTime());
				DRAW_LINE(osd_font, font_size, s_gpu_usage_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowGPUDebug)
			{
#ifdef _WIN32
				if (g_gs_device->GetRenderAPI() == RenderAPI::D3D12)
				{
					GSDevice12* dev12 = static_cast<GSDevice12*>(g_gs_device.get());

					s_gpu_debug_info_line.format("D3D12 Descriptor Heaps | SRV/UAV: {}/{} | RTV: {}/{} | DSV {}/{}",
						dev12->GetDescriptorHeapManager().GetAllocatedDescriptors(), dev12->GetDescriptorHeapManager().GetNumDescriptors(),
						dev12->GetRTVHeapManager().GetAllocatedDescriptors(), dev12->GetRTVHeapManager().GetNumDescriptors(),
						dev12->GetDSVHeapManager().GetAllocatedDescriptors(), dev12->GetDSVHeapManager().GetNumDescriptors());
					DRAW_LINE(osd_font, font_size, s_gpu_debug_info_line.c_str(), white_color);
				}
#endif
			}

			if (GSConfig.OsdShowGPUStats)
			{
				const auto FormatUnits = [](double val) {
					if (val >= 1e9)
						return fmt::format("{:.5}B", val / 1e9);
					if (val >= 1e6)
						return fmt::format("{:.5}M", val / 1e6);
					if (val >= 1e3)
						return fmt::format("{:.5}K", val / 1e3);
					return fmt::format("{:.5}", val);
				};

				s_gpu_stats_line.format("VSI: {} | PSI: {}",
					FormatUnits(PerformanceMetrics::GetGPUAverageVSInvocations()),
					FormatUnits(PerformanceMetrics::GetGPUAveragePSInvocations()));
				DRAW_LINE(osd_font, font_size, s_gpu_stats_line.c_str(), white_color);
			}
		}
		// No refresh yet. Display cached lines.
		else
		{
			if (GSConfig.OsdShowFPS || GSConfig.OsdShowVPS || GSConfig.OsdShowSpeed || GSConfig.OsdShowVersion)
				DRAW_LINE(osd_font, font_size, s_speed_line.c_str(), s_speed_line_color);

			if (GSConfig.OsdShowGSStats)
			{
				if (!s_gs_stats_line.empty())
					DRAW_LINE(osd_font, font_size, s_gs_stats_line.c_str(), white_color);
				if (!s_gs_memory_stats_line.empty())
					DRAW_LINE(osd_font, font_size, s_gs_memory_stats_line.c_str(), white_color);
				DRAW_LINE(osd_font, font_size, s_gs_frame_times_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowResolution)
				DRAW_LINE(osd_font, font_size, s_resolution_line.c_str(), white_color);

			if (GSConfig.OsdShowHardwareInfo)
			{
				DRAW_LINE(osd_font, font_size, s_hardware_info_cpu_line.c_str(), white_color);
				DRAW_LINE(osd_font, font_size, s_hardware_info_gpu_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowCPU)
			{
				DRAW_LINE(osd_font, font_size, s_cpu_usage_ee_line.c_str(), white_color);
				DRAW_LINE(osd_font, font_size, s_cpu_usage_gs_line.c_str(), white_color);
				if (THREAD_VU1)
					DRAW_LINE(osd_font, font_size, s_cpu_usage_vu_line.c_str(), white_color);

				const u32 thread_count = std::min(
					PerformanceMetrics::GetGSSWThreadCount(),
					static_cast<u32>(s_software_thread_lines.size()));
				for (u32 thread = 0; thread < thread_count; thread++)
					DRAW_LINE(osd_font, font_size, s_software_thread_lines[thread].c_str(), white_color);

				if (GSCapture::IsCapturing())
					DRAW_LINE(osd_font, font_size, s_capture_line.c_str(), white_color);
			}

			if (GSConfig.OsdShowGPU)
				DRAW_LINE(osd_font, font_size, s_gpu_usage_line.c_str(), white_color);

			if (GSConfig.OsdShowGPUDebug)
			{
#ifdef _WIN32
				if (g_gs_device->GetRenderAPI() == RenderAPI::D3D12)
					DRAW_LINE(osd_font, font_size, s_gpu_debug_info_line.c_str(), white_color);
#endif
			}

			if (GSConfig.OsdShowGPUStats)
			{
				DRAW_LINE(osd_font, font_size, s_gpu_stats_line.c_str(), white_color);
			}
		}

		// Check every OSD frame because this is an animation.
		if (GSConfig.OsdShowFrameTimes)
		{
			const auto& history = PerformanceMetrics::GetFrameTimeHistory();
			const u32 sample_count = PerformanceMetrics::NUM_FRAME_TIME_SAMPLES;
			const u32 hist_pos = PerformanceMetrics::GetFrameTimeHistoryPos();
			static constexpr u32 SCALE_WINDOW = 60u;
			static constexpr float DEFAULT_FT_MIN = 0.0f;
			static constexpr float DEFAULT_FT_MAX = 20.0f;
			static constexpr float DEFAULT_VPS_MIN = 0.0f;
			static constexpr float DEFAULT_VPS_MAX = 100.0f;
			static constexpr float SCALE_SMOOTHING = 0.25f;

			const auto compute_window_extents = [&](const auto& values) {
				float lo = 1.0e9f;
				float hi = 0.0f;
				for (u32 k = 0; k < SCALE_WINDOW && k < sample_count; k++)
				{
					const float v = values[(hist_pos + sample_count - SCALE_WINDOW + k) % sample_count];
					if (v > 0.0f)
					{
						lo = std::min(lo, v);
						hi = std::max(hi, v);
					}
				}
				return std::pair(lo, hi);
			};

			auto [initial_lo, initial_hi] = compute_window_extents(history);
			auto [min_val, max_val] = [&]() {
				float lo = initial_lo;
				float hi = initial_hi;
				if (hi < lo)
					return std::pair(DEFAULT_FT_MIN, DEFAULT_FT_MAX);
				if ((hi - lo) < 4.0f)
				{
					lo = lo - std::fmod(lo, 1.0f);
					hi = hi - std::fmod(hi, 1.0f) + 1.0f;
					lo = std::max(lo - 2.0f, 0.0f);
					hi += 2.0f;
				}
				return std::pair(lo, std::max(hi, lo + 1.0f));
			}();

			PerformanceMetrics::FrameTimeHistory ft_history = history;

			float last_ft = 0.0f;
			for (u32 k = 0; k < sample_count; k++)
			{
				const u32 idx = (hist_pos + sample_count - 1 - k) % sample_count;
				const float ft = ft_history[idx];
				if (ft > 0.0f)
				{
					last_ft = ft;
					break;
				}
			}

			for (u32 i = 0; i < sample_count; i++)
			{
				const u32 idx = (hist_pos + i) % sample_count;
				float& ft = ft_history[idx];
				if (ft > 0.0f)
				{
					last_ft = ft;
					continue;
				}

				ft = last_ft;
			}

			std::array<float, PerformanceMetrics::NUM_FRAME_TIME_SAMPLES> vps_history;
			for (u32 i = 0; i < sample_count; i++)
				vps_history[i] = (ft_history[i] >= 0.01f) ? std::min(10000.0f, 1000.0f / ft_history[i]) : 0.0f;

			auto [vps_initial_lo, vps_initial_hi] = compute_window_extents(vps_history);
			auto [vps_scale_min, vps_scale_max] = [&]() {
				float lo = vps_initial_lo;
				float hi = vps_initial_hi;
				if (hi < lo)
					return std::pair(DEFAULT_VPS_MIN, DEFAULT_VPS_MAX);
				if (hi - lo < 10.0f)
				{
					lo = std::floor(lo / 50.0f) * 50.0f;
					hi = std::ceil(hi / 50.0f) * 50.0f;
					lo = std::max(0.0f, lo - 5.0f);
					hi = std::max(hi + 5.0f, lo + 10.0f);
				}
				return std::pair(lo, hi);
			}();

			static float s_ft_min = DEFAULT_FT_MIN;
			static float s_ft_max = DEFAULT_FT_MAX;
			static float s_vps_min = DEFAULT_VPS_MIN;
			static float s_vps_max = DEFAULT_VPS_MAX;
			s_ft_min += (min_val - s_ft_min) * SCALE_SMOOTHING;
			s_ft_max += (max_val - s_ft_max) * SCALE_SMOOTHING;
			s_vps_min += (vps_scale_min - s_vps_min) * SCALE_SMOOTHING;
			s_vps_max += (vps_scale_max - s_vps_max) * SCALE_SMOOTHING;
			min_val = s_ft_min;
			max_val = std::max(s_ft_max, min_val + 1.0f);
			float min_vps = s_vps_min;
			float max_vps = std::max(s_vps_max, min_vps + 10.0f);

			SmallString label_buf;
			label_buf.format("{:.1f}", max_val);
			const float y_label_w = osd_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label_buf.c_str(), label_buf.c_str() + label_buf.length()).x + 4.0f * scale;
			label_buf.format("{:.0f}", max_vps);
			const float right_label_w = osd_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label_buf.c_str(), label_buf.c_str() + label_buf.length()).x + 4.0f * scale;

			const float pad = 4.0f * scale;
			const float row_gap = 2.0f * scale;
			const float legend_h = (font_size * 2.0f) + row_gap + pad;
			const ImVec2 graph_size(200.0f * scale, 60.0f * scale);
			const ImVec2 total_size(y_label_w + graph_size.x + right_label_w + 2.0f * pad, graph_size.y + legend_h + 2.0f * pad);

			ImGui::SetNextWindowSize(total_size);
			ImGui::SetNextWindowPos(CalculatePerformanceOverlayTextPosition(GSConfig.OsdPerformancePos, margin, total_size, GetWindowWidth(), position_y));
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::PushFont(osd_font, font_size);
			if (ImGui::Begin("##frame_times", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs))
			{
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const ImVec2 wpos(ImGui::GetWindowPos());
				const ImVec2 plot_tl(wpos.x + pad + y_label_w, wpos.y + pad);
				const ImVec2 plot_br(plot_tl.x + graph_size.x, plot_tl.y + graph_size.y);

				dl->AddRectFilled(plot_tl, plot_br, IM_COL32(0, 0, 0, 60));

				const int num_ticks = std::max(1, std::min(8, static_cast<int>(graph_size.y / (font_size * 1.1f))));
				const float left_label_x = wpos.x + pad + y_label_w;
				const float right_label_x = plot_br.x + 2.0f * scale;

				const ImU32 ft_col = IM_COL32(100, 200, 255, 230);
				const ImU32 vps_col = IM_COL32(100, 255, 100, 230);

				auto draw_grid_and_labels = [&](int ticks) {
					SmallString s;
					for (int i = 0; i <= ticks; i++)
					{
						const float frac = static_cast<float>(i) / ticks;
						const float grid_y = plot_br.y - frac * graph_size.y;
						const float ly = grid_y - font_size * 0.5f;

						dl->AddLine(ImVec2(plot_tl.x, grid_y), ImVec2(plot_br.x, grid_y), IM_COL32(255, 255, 255, 40), 1.0f);

						s.format("{:.1f}", min_val + (max_val - min_val) * frac);
						const float left_text_w = osd_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, s.c_str(), s.c_str() + s.length()).x;
						const float lx = left_label_x - left_text_w - 2.0f * scale;
						dl->AddText(osd_font, font_size, ImVec2(lx + shadow_offset, ly + shadow_offset), IM_COL32(0, 0, 0, 100), s.c_str(), s.c_str() + s.length());
						dl->AddText(osd_font, font_size, ImVec2(lx, ly), ft_col, s.c_str(), s.c_str() + s.length());

						s.format("{:.0f}", min_vps + (max_vps - min_vps) * frac);
						dl->AddText(osd_font, font_size, ImVec2(right_label_x + shadow_offset, ly + shadow_offset), IM_COL32(0, 0, 0, 100), s.c_str(), s.c_str() + s.length());
						dl->AddText(osd_font, font_size, ImVec2(right_label_x, ly), vps_col, s.c_str(), s.c_str() + s.length());
					}
				};

				draw_grid_and_labels(num_ticks);

				const auto col32_to_vec4 = [](ImU32 col) -> ImVec4 {
					return ImVec4(
						static_cast<float>((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
						static_cast<float>((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
						static_cast<float>((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
						static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
				};

				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

				ImGui::SetCursorScreenPos(plot_tl);
				ImGui::PushStyleColor(ImGuiCol_PlotLines, col32_to_vec4(ft_col));
				ImGui::PlotLines("##frame_time_plot", ft_history.data(), static_cast<int>(sample_count), static_cast<int>(hist_pos),
					nullptr, min_val, max_val, graph_size);
				ImGui::PopStyleColor();

				ImGui::SetCursorScreenPos(plot_tl);
				ImGui::PushStyleColor(ImGuiCol_PlotLines, col32_to_vec4(vps_col));
				ImGui::PlotLines("##vps_plot", vps_history.data(), static_cast<int>(sample_count), static_cast<int>(hist_pos),
					nullptr, min_vps, max_vps, graph_size);
				ImGui::PopStyleColor();

				ImGui::PopStyleVar();
				ImGui::PopStyleColor(2);

				const float legend_y = plot_br.y + pad * 0.5f;
				const float legend_square_size = font_size * 0.65f;
				const float legend_gap = 4.0f * scale;
				SmallString frame_part, vps_part;
				frame_part.format("Frame: {:.2f} ms", PerformanceMetrics::GetAverageFrameTime());
				vps_part.format("V-Blank: {:.2f}", PerformanceMetrics::GetFPS());
				const float fw = osd_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, frame_part.c_str(), nullptr).x;
				const float vw = osd_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, vps_part.c_str(), nullptr).x;
				const float max_text_w = std::max(fw, vw);
				const float row_width = legend_square_size + legend_gap + max_text_w;
				float base_x = wpos.x + (total_size.x - row_width) * 0.5f;
				auto draw_legend_entry = [&](ImU32 col, const char* text, float text_w, float y) {
					float lx = base_x;
					dl->AddRectFilled(ImVec2(lx, y + (font_size - legend_square_size) * 0.5f),
						ImVec2(lx + legend_square_size, y + (font_size + legend_square_size) * 0.5f), col);
					lx += legend_square_size + legend_gap;
					dl->AddText(osd_font, font_size, ImVec2(lx + shadow_offset, y + shadow_offset), IM_COL32(0, 0, 0, 100), text, nullptr);
					dl->AddText(osd_font, font_size, ImVec2(lx, y), white_color, text, nullptr);
				};
				draw_legend_entry(IM_COL32(100, 200, 255, 230), frame_part.c_str(), fw, legend_y);
				draw_legend_entry(IM_COL32(100, 255, 100, 230), vps_part.c_str(), vw, legend_y + font_size + row_gap);
			}
			ImGui::End();
			ImGui::PopFont();
			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor(1);
		}
	}

#undef DRAW_LINE
}

__ri void ImGuiManager::DrawShaderCompileIndicator(float scale, float margin, float spacing)
{
	static bool s_indicator_was_visible = false;
	static double s_indicator_fade_in_start = 0.0;

	if (!GSConfig.OsdShowGPU || !GSShaderCompileIndicator::IsVisible())
	{
		s_indicator_was_visible = false;
		return;
	}

	const double imgui_time = ImGui::GetTime();
	if (!s_indicator_was_visible)
		s_indicator_fade_in_start = imgui_time;
	s_indicator_was_visible = true;

	constexpr double fade_in_seconds = 0.12;
	const float fade_in_alpha = static_cast<float>(
		std::min(1.0, (imgui_time - s_indicator_fade_in_start) / fade_in_seconds));
	const float fade_out_alpha = GSShaderCompileIndicator::GetFadeAlpha();
	const float alpha = std::clamp(fade_in_alpha * fade_out_alpha, 0.0f, 1.0f);
	const ImU32 text_col = IM_COL32(255, 255, 255, static_cast<int>(std::lround(255.0f * alpha)));
	const ImU32 shadow_col = IM_COL32(0, 0, 0, static_cast<int>(std::lround(100.0f * alpha)));
	const ImU32 spinner_track_col = IM_COL32(255, 255, 255, static_cast<int>(std::lround(45.0f * alpha)));

	static constexpr const char* COMPILED_ONE =
		TRANSLATE_NOOP("ImGuiOverlays", "Compiled {0} shader in {1}ms");
	static constexpr const char* COMPILED_MANY =
		TRANSLATE_NOOP("ImGuiOverlays", "Compiled {0} shaders in {1}ms");

	const u32 count = GSShaderCompileIndicator::GetCount();
	const u32 time_ms = GSShaderCompileIndicator::GetTimeMs();
	const std::string label = (count == 1) ?
	                              fmt::format(TRANSLATE_FS("ImGuiOverlays", COMPILED_ONE), count, time_ms) :
	                              fmt::format(TRANSLATE_FS("ImGuiOverlays", COMPILED_MANY), count, time_ms);

	ImFont* const font = ImGuiManager::GetOSDFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float baseline_y =
		GetWindowHeight() - margin - (GSConfig.OsdShowSettings ? font_size : 0.0f);
	const float radius = std::ceil(10.0f * scale);
	const float cx = GetWindowWidth() - margin - radius;
	const float cy = baseline_y - spacing - radius;
	const ImVec2 center(cx, cy);

	const ImVec2 text_size = font->CalcTextSizeA(
		font_size, std::numeric_limits<float>::max(), -1.0f, label.c_str(), label.c_str() + label.length(), nullptr);
	const float shadow_offset = std::ceil(scale);
	const float text_gap = std::ceil(6.0f * scale);
	const float text_x = cx - radius - text_gap - text_size.x;
	const float text_y = cy - font_size * 0.5f;

	ImDrawList* const dl = ImGui::GetBackgroundDrawList();
	const float a0 = static_cast<float>(ImGui::GetTime()) * 10.0f;
	const float a1 = a0 + IM_PI * 1.12f;
	const float thickness = std::ceil(2.5f * scale);

	dl->AddText(font, font_size, ImVec2(text_x + shadow_offset, text_y + shadow_offset), shadow_col,
		label.c_str(), label.c_str() + label.length());
	dl->AddText(font, font_size, ImVec2(text_x, text_y), text_col, label.c_str(), label.c_str() + label.length());
	if (GSConfig.OsdBoldText)
	{
		dl->AddText(font, font_size, ImVec2(text_x + 0.6f, text_y), text_col, label.c_str(),
			label.c_str() + label.length());
	}

	dl->PathClear();
	dl->PathArcTo(center, radius, 0.0f, 2.0f * IM_PI, 32);
	dl->PathStroke(spinner_track_col, std::max(1.0f, thickness * 0.65f), false);

	dl->PathClear();
	dl->PathArcTo(center, radius, a0, a1, 24);
	dl->PathStroke(text_col, thickness, false);
}

__ri void ImGuiManager::DrawSettingsOverlay(float scale, float margin, float spacing)
{
	if (!GSConfig.OsdShowSettings ||
		FullscreenUI::HasActiveWindow())
		return;

	std::string text;
	text.reserve(512);

#define APPEND(...) \
	do \
	{ \
		fmt::format_to(std::back_inserter(text), __VA_ARGS__); \
	} while (0)

	if (Patch::GetAllActivePatchesCount() > 0 && EmuConfig.GS.OsdshowPatches)
		APPEND("DB={} P={} C={} | ",
			Patch::GetActiveGameDBPatchesCount(),
			Patch::GetActivePatchesCount(),
			Patch::GetActiveCheatsCount());

	if (EmuConfig.Speedhacks.EECycleRate != 0)
		APPEND("CR={} ", EmuConfig.Speedhacks.EECycleRate);
	if (EmuConfig.Speedhacks.EECycleSkip != 0)
		APPEND("CS={} ", EmuConfig.Speedhacks.EECycleSkip);
	if (EmuConfig.Speedhacks.fastCDVD)
		APPEND("FCDVD ");
	if (EmuConfig.Speedhacks.vu1Instant)
		APPEND("IVU ");
	if (EmuConfig.Speedhacks.vuThread)
		APPEND("MTVU ");
	if (EmuConfig.GS.VsyncEnable)
		APPEND("VSYNC ");

	APPEND("EER={} EEC={} VUR={} VUC={} VQS={} ", static_cast<unsigned>(EmuConfig.Cpu.FPUFPCR.GetRoundMode()),
		EmuConfig.Cpu.Recompiler.GetEEClampMode(), static_cast<unsigned>(EmuConfig.Cpu.VU0FPCR.GetRoundMode()),
		EmuConfig.Cpu.Recompiler.GetVUClampMode(), EmuConfig.GS.VsyncQueueSize);

	if (GSIsHardwareRenderer())
	{
		if ((GSConfig.UpscaleMultiplier - std::floor(GSConfig.UpscaleMultiplier)) > 0.01)
			APPEND("IR={:.2f} ", static_cast<float>(GSConfig.UpscaleMultiplier));
		else
			APPEND("IR={} ", static_cast<unsigned>(GSConfig.UpscaleMultiplier));

		APPEND("BL={} TPL={} ", static_cast<unsigned>(GSConfig.AccurateBlendingUnit), static_cast<unsigned>(GSConfig.TexturePreloading));
		if (GSConfig.GPUPaletteConversion)
			APPEND("PLTX ");

		if (GSConfig.HWDownloadMode != GSHardwareDownloadMode::Enabled)
			APPEND("HWDM={} ", static_cast<unsigned>(GSConfig.HWDownloadMode));

		if (GSConfig.HWMipmap)
			APPEND("MM ");

		if (GSConfig.HWAccurateAlphaTest)
			APPEND("AAT ");

		if (GSConfig.HWAA1)
			APPEND("AA1 ");

		if (GSConfig.HWROV)
			APPEND("ROV ");

		if (GSConfig.HWROVBarriersVK)
			APPEND("RBVK ");

		// deliberately test global and print local here for auto values
		if (EmuConfig.GS.TextureFiltering != BiFiltering::PS2)
			APPEND("BF={} ", static_cast<unsigned>(GSConfig.TextureFiltering));
		if (EmuConfig.GS.TriFilter != TriFiltering::Automatic)
			APPEND("TF={} ", static_cast<unsigned>(GSConfig.TriFilter));
		if (GSConfig.MaxAnisotropy > 1)
			APPEND("AF={} ", EmuConfig.GS.MaxAnisotropy);
		if (GSConfig.Dithering != 2)
			APPEND("DI={} ", GSConfig.Dithering);
		if (GSConfig.UserHacks_HalfPixelOffset != GSHalfPixelOffset::Off)
			APPEND("HPO={} ", static_cast<u32>(GSConfig.UserHacks_HalfPixelOffset));
		if (GSConfig.UserHacks_RoundSprite > 0)
			APPEND("RS={} ", GSConfig.UserHacks_RoundSprite);
		if (GSConfig.UserHacks_NativeScaling > GSNativeScaling::Off)
			APPEND("NS={} ", static_cast<unsigned>(GSConfig.UserHacks_NativeScaling));
		if (GSConfig.UserHacks_TCOffsetX != 0 || GSConfig.UserHacks_TCOffsetY != 0)
			APPEND("TCO={}/{} ", GSConfig.UserHacks_TCOffsetX, GSConfig.UserHacks_TCOffsetY);
		if (GSConfig.UserHacks_CPUSpriteRenderBW != 0)
			APPEND("CSBW={}/{} ", GSConfig.UserHacks_CPUSpriteRenderBW, GSConfig.UserHacks_CPUSpriteRenderLevel);
		if (GSConfig.UserHacks_CPUCLUTRender != 0)
			APPEND("CCLUT={} ", GSConfig.UserHacks_CPUCLUTRender);
		if (GSConfig.UserHacks_GPUTargetCLUTMode != GSGPUTargetCLUTMode::Disabled)
			APPEND("GCLUT={} ", static_cast<unsigned>(GSConfig.UserHacks_GPUTargetCLUTMode));
		if (GSConfig.SkipDrawStart != 0 || GSConfig.SkipDrawEnd != 0)
			APPEND("SD={}/{} ", GSConfig.SkipDrawStart, GSConfig.SkipDrawEnd);
		if (GSConfig.UserHacks_TextureInsideRt != GSTextureInRtMode::Disabled)
			APPEND("TexRT={} ", static_cast<unsigned>(GSConfig.UserHacks_TextureInsideRt));
		if (GSConfig.UserHacks_Limit24BitDepth != GSLimit24BitDepth::Disabled)
			APPEND("LDR={} ", static_cast<unsigned>(GSConfig.UserHacks_Limit24BitDepth));
		if (GSConfig.UserHacks_BilinearHack != GSBilinearDirtyMode::Automatic)
			APPEND("BLU={} ", static_cast<unsigned>(GSConfig.UserHacks_BilinearHack));
		if (GSConfig.UserHacks_ForceEvenSpritePosition)
			APPEND("FESP ");
		if (GSConfig.UserHacks_NativePaletteDraw)
			APPEND("NPD ");
		if (GSConfig.UserHacks_MergePPSprite)
			APPEND("MS ");
		if (GSConfig.UserHacks_AlignSpriteX)
			APPEND("AS ");
		if (GSConfig.UserHacks_AutoFlush != GSHWAutoFlushLevel::Disabled)
			APPEND("ATFL={} ", static_cast<unsigned>(GSConfig.UserHacks_AutoFlush));
		if (GSConfig.UserHacks_CPUFBConversion)
			APPEND("FBC ");
		if (GSConfig.UserHacks_ReadTCOnClose)
			APPEND("RTOC ");
		if (GSConfig.UserHacks_DisableDepthSupport)
			APPEND("DDC ");
		if (GSConfig.UserHacks_DisablePartialInvalidation)
			APPEND("DPIV ");
		if (GSConfig.UserHacks_DisableSafeFeatures)
			APPEND("DSF ");
		if (GSConfig.UserHacks_DisableRenderFixes)
			APPEND("DRF ");
		if (GSConfig.PreloadFrameWithGSData)
			APPEND("PLFD ");
		if (GSConfig.UserHacks_EstimateTextureRegion)
			APPEND("ETR ");
		if (GSConfig.UserHacks_DrawBuffering)
			APPEND("DRWB ");
		if (GSConfig.HWSpinGPUForReadbacks)
			APPEND("RBSG ");
		if (GSConfig.HWSpinCPUForReadbacks)
			APPEND("RBSC ");
	}

#undef APPEND

	if (text.empty())
		return;
	else if (text.back() == ' ')
		text.pop_back();

	const float shadow_offset = std::ceil(scale);
	ImFont* const font = ImGuiManager::GetOSDFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float position_y = GetWindowHeight() - margin - font_size;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImVec2 text_size =
		font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, text.c_str(), text.c_str() + text.length(), nullptr);
	const ImVec2 text_pos(GetWindowWidth() - margin - text_size.x, position_y);
	const bool bold_osd = GSConfig.OsdBoldText;
	dl->AddText(font, font_size,
		ImVec2(text_pos.x + shadow_offset, text_pos.y + shadow_offset), IM_COL32(0, 0, 0, 100),
		text.c_str(), text.c_str() + text.length());
	dl->AddText(font, font_size, text_pos, white_color,
		text.c_str(), text.c_str() + text.length());
	if (bold_osd)
	{
		dl->AddText(font, font_size, ImVec2(text_pos.x + 0.6f, text_pos.y), white_color,
			text.c_str(), text.c_str() + text.length());
	}
}

__ri void ImGuiManager::DrawInputsOverlay(float scale, float margin, float spacing)
{
	// Technically this is racing the CPU thread.. but it doesn't really matter, at worst, the inputs get displayed onscreen late.
	if (!GSConfig.OsdShowInputs ||
		FullscreenUI::HasActiveWindow())
		return;

	const float shadow_offset = std::ceil(scale);
	ImFont* const font = ImGuiManager::GetStandardFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float line_height = ImGuiFullscreen::GetLineHeight({ font, font_size });

	static constexpr u32 text_color = IM_COL32(0xff, 0xff, 0xff, 255);
	static constexpr u32 shadow_color = IM_COL32(0x00, 0x00, 0x00, 100);

	const ImVec2& display_size = ImGui::GetIO().DisplaySize;
	ImDrawList* dl = ImGui::GetBackgroundDrawList();

	u32 num_ports = 0;

	for (u32 slot = 0; slot < Pad::NUM_CONTROLLER_PORTS; slot++)
	{
		if (Pad::HasConnectedPad(slot))
			num_ports++;
	}

	for (u32 port = 0; port < USB::NUM_PORTS; port++)
	{
		if (EmuConfig.USB.Ports[port].DeviceType >= 0)
			num_ports++;
	}

	float current_x = ImFloor(margin);
	float current_y = ImFloor(display_size.y - margin - ((static_cast<float>(num_ports) * (line_height + spacing)) - spacing));
	const ImVec4 clip_rect(current_x, current_y, display_size.x - margin, display_size.y);

	SmallString text;

	for (u32 slot = 0; slot < Pad::NUM_CONTROLLER_PORTS; slot++)
	{
		const PadBase* const pad = Pad::GetPad(slot);
		const Pad::ControllerType ctype = pad->GetType();
		if (ctype == Pad::ControllerType::NotConnected)
			continue;

		const Pad::ControllerInfo& cinfo = pad->GetInfo();
		text.format("{} {} • {} |", ICON_FA_GAMEPAD, slot + 1u, cinfo.icon_name ? cinfo.icon_name : ICON_FA_TRIANGLE_EXCLAMATION);

		for (u32 bind = 0; bind < static_cast<u32>(cinfo.bindings.size()); bind++)
		{
			const InputBindingInfo& bi = cinfo.bindings[bind];
			switch (bi.bind_type)
			{
				case InputBindingInfo::Type::Axis:
				case InputBindingInfo::Type::HalfAxis:
				{
					// axes are only shown if not resting/past deadzone. values are normalized.
					const float value = pad->GetEffectiveInput(bind);
					const float abs_value = std::abs(value);
					if (abs_value >= (254.0f / 255.0f))
						text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
					else if (abs_value >= (1.0f / 255.0f))
						text.append_format(" {}: {:.2f}", bi.icon_name ? bi.icon_name : bi.name, value);
				}
				break;

				case InputBindingInfo::Type::Button:
				{
					// buttons display the value from 0 through 255.
					const float value = pad->GetEffectiveInput(bind);
					if (value >= 254.0f)
						text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
					else if (value > 0.0f)
						text.append_format(" {}: {:.0f}", bi.icon_name ? bi.icon_name : bi.name, value);
				}
				break;

				case InputBindingInfo::Type::Motor:
				case InputBindingInfo::Type::Macro:
				case InputBindingInfo::Type::Unknown:
				default:
					break;
			}
		}

		dl->AddText(font, font_size, ImVec2(current_x + shadow_offset, current_y + shadow_offset), shadow_color,
			text.c_str(), text.c_str() + text.length(), 0.0f, &clip_rect);
		dl->AddText(font, font_size, ImVec2(current_x, current_y), text_color,
			text.c_str(), text.c_str() + text.length(), 0.0f, &clip_rect);

		current_y += line_height + spacing;
	}

	for (u32 port = 0; port < USB::NUM_PORTS; port++)
	{
		if (EmuConfig.USB.Ports[port].DeviceType < 0)
			continue;

		const std::span<const InputBindingInfo> bindings(USB::GetDeviceBindings(port));

		const char* icon = USB::GetDeviceIconName(port);
		text.format("{} {} • {} | ", ICON_PF_USB, port + 1u, icon ? icon : ICON_FA_TRIANGLE_EXCLAMATION);

		for (const InputBindingInfo& bi : bindings)
		{
			switch (bi.bind_type)
			{
				case InputBindingInfo::Type::Axis:
				case InputBindingInfo::Type::HalfAxis:
				{
					// axes are only shown if not resting/past deadzone. values are normalized.
					const float value = static_cast<float>(USB::GetDeviceBindValue(port, bi.bind_index));
					if (value >= (254.0f / 255.0f))
						text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
					else if (value > (1.0f / 255.0f))
						text.append_format(" {}: {:.2f}", bi.icon_name ? bi.icon_name : bi.name, value);
				}
				break;

				case InputBindingInfo::Type::Button:
				{
					// buttons display the value from 0 through 255. values are normalized, so denormalize them.
					const float value = static_cast<float>(USB::GetDeviceBindValue(port, bi.bind_index)) * 255.0f;
					if (value >= 254.0f)
						text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
					else if (value > 0.0f)
						text.append_format(" {}: {:.0f}", bi.icon_name ? bi.icon_name : bi.name, value);
				}
				break;

				case InputBindingInfo::Type::Motor:
				case InputBindingInfo::Type::Macro:
				case InputBindingInfo::Type::Unknown:
				default:
					break;
			}
		}

		dl->AddText(font, font_size, ImVec2(current_x + shadow_offset, current_y + shadow_offset), shadow_color,
			text.c_str(), text.c_str() + text.length(), 0.0f, &clip_rect);
		dl->AddText(font, font_size, ImVec2(current_x, current_y), text_color,
			text.c_str(), text.c_str() + text.length(), 0.0f, &clip_rect);

		current_y += line_height + spacing;
	}
}

__ri void ImGuiManager::DrawInputRecordingOverlay(float& position_y, float scale, float margin, float spacing)
{
	if (!GSConfig.OsdShowInputRec ||
		!g_InputRecording.isActive() ||
		FullscreenUI::HasActiveWindow())
		return;

	const float shadow_offset = std::ceil(scale);

	ImFont* const osd_font = ImGuiManager::GetOSDFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	std::string text;
	ImVec2 text_size;

	text.reserve(128);
#define DRAW_LINE(font, size, text, color) \
	do \
	{ \
		text_size = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr); \
		dl->AddText(font, size, \
			ImVec2(GetWindowWidth() - margin - text_size.x + shadow_offset, position_y + shadow_offset), \
			IM_COL32(0, 0, 0, 100), (text)); \
		dl->AddText(font, size, ImVec2(GetWindowWidth() - margin - text_size.x, position_y), color, (text)); \
		position_y += text_size.y + spacing; \
	} while (0)

	// Status Indicators
	if (g_InputRecordingData.is_recording)
	{
		DRAW_LINE(osd_font, font_size, TinyString::from_format(TRANSLATE_FS("ImGuiOverlays", "{} Recording Input"), ICON_PF_CIRCLE).c_str(), IM_COL32(255, 0, 0, 255));
	}
	else
	{
		DRAW_LINE(osd_font, font_size, TinyString::from_format(TRANSLATE_FS("ImGuiOverlays", "{} Replaying"), ICON_FA_PLAY).c_str(), IM_COL32(97, 240, 84, 255));
	}

	// Input Recording Metadata
	DRAW_LINE(osd_font, font_size, g_InputRecordingData.recording_active_message.c_str(), IM_COL32(117, 255, 241, 255));
	DRAW_LINE(osd_font, font_size, g_InputRecordingData.frame_data_message.c_str(), IM_COL32(117, 255, 241, 255));
	DRAW_LINE(osd_font, font_size, g_InputRecordingData.undo_count_message.c_str(), IM_COL32(117, 255, 241, 255));

#undef DRAW_LINE
}

__ri void ImGuiManager::DrawVideoCaptureOverlay(float& position_y, float scale, float margin, float spacing)
{
	if (!GSConfig.OsdShowVideoCapture ||
		!GSCapture::IsCapturing() ||
		FullscreenUI::HasActiveWindow())
		return;

	const float shadow_offset = std::ceil(scale);
	ImFont* const osd_font = ImGuiManager::GetOSDFont();
	float font_size = ImGuiManager::GetFontSizeStandard();
	ImDrawList* dl = ImGui::GetBackgroundDrawList();

	static constexpr const char* ICON = ICON_PF_CIRCLE;
	const TinyString text_msg = TinyString::from_format(" {}", GSCapture::GetElapsedTime());
	const ImVec2 icon_size = osd_font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(),
		-1.0f, ICON, nullptr, nullptr);
	const ImVec2 text_size = osd_font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(),
		-1.0f, text_msg.c_str(), text_msg.end_ptr(), nullptr);

	// Shadow
	dl->AddText(osd_font, font_size,
		ImVec2(GetWindowWidth() - margin - text_size.x - icon_size.x + shadow_offset, position_y + shadow_offset),
		IM_COL32(0, 0, 0, 100), ICON);
	dl->AddText(osd_font, font_size,
		ImVec2(GetWindowWidth() - margin - text_size.x + shadow_offset, position_y + shadow_offset),
		IM_COL32(0, 0, 0, 100), text_msg.c_str(), text_msg.end_ptr());

	// Text
	dl->AddText(osd_font, font_size,
		ImVec2(GetWindowWidth() - margin - text_size.x - icon_size.x, position_y), IM_COL32(255, 0, 0, 255), ICON);
	dl->AddText(osd_font, font_size,
		ImVec2(GetWindowWidth() - margin - text_size.x, position_y), white_color, text_msg.c_str(),
		text_msg.end_ptr());

	position_y += std::max(icon_size.y, text_size.y) + spacing;
}

__ri void ImGuiManager::DrawTextureReplacementsOverlay(float& position_y, float scale, float margin, float spacing)
{
	if (!GSConfig.OsdShowTextureReplacements ||
		FullscreenUI::HasActiveWindow())
		return;

	const bool dumping_active = GSConfig.DumpReplaceableTextures;
	const bool replacement_active = GSConfig.LoadTextureReplacements;

	if (!dumping_active && !replacement_active)
		return;

	const float shadow_offset = std::ceil(scale);
	ImFont* const osd_font = ImGuiManager::GetOSDFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	ImDrawList* dl = ImGui::GetBackgroundDrawList();

	SmallString texture_line;
	if (replacement_active)
	{
		const u32 loaded_count = GSTextureReplacements::GetLoadedTextureCount();
		texture_line.format("{} Replaced: {}", ICON_FA_IMAGES, loaded_count);
	}
	if (dumping_active)
	{
		if (!texture_line.empty())
			texture_line.append(" | ");
		const u32 dumped_count = GSTextureReplacements::GetDumpedTextureCount();
		texture_line.append_format("{} Dumped: {}", ICON_FA_DOWNLOAD, dumped_count);
	}

	ImVec2 text_size = osd_font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, texture_line.c_str(), nullptr, nullptr);
	const ImVec2 text_pos(GetWindowWidth() - margin - text_size.x, position_y);

	dl->AddText(osd_font, font_size, ImVec2(text_pos.x + shadow_offset, text_pos.y + shadow_offset), IM_COL32(0, 0, 0, 100), texture_line.c_str());
	dl->AddText(osd_font, font_size, text_pos, white_color, texture_line.c_str());

	position_y += text_size.y + spacing;
}

__ri void ImGuiManager::DrawIndicatorsOverlay(float& position_y, float scale, float margin, float spacing)
{
	if (!GSConfig.OsdShowIndicators ||
		FullscreenUI::HasActiveWindow())
		return;

	const float shadow_offset = std::ceil(scale);

	ImFont* const osd_font = ImGuiManager::GetOSDFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	std::string text;
	ImVec2 text_size;

	text.reserve(64);
	#define DRAW_LINE(font, size, text, color) \
		do \
		{ \
			text_size = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr); \
			dl->AddText(font, size, \
				ImVec2(GetWindowWidth() - margin - text_size.x + shadow_offset, position_y + shadow_offset), \
				IM_COL32(0, 0, 0, 100), (text)); \
			dl->AddText(font, size, ImVec2(GetWindowWidth() - margin - text_size.x, position_y), color, (text)); \
			position_y += text_size.y + spacing; \
		} while (0)

		if (VMManager::GetState() != VMState::Paused)
		{
			// Draw Speed indicator
			const float target_speed = VMManager::GetTargetSpeed();
			const bool is_normal_speed = (target_speed == EmuConfig.EmulationSpeed.NominalScalar ||
										  VMManager::IsTargetSpeedAdjustedToHost());
			if (!is_normal_speed)
			{
				if (target_speed == EmuConfig.EmulationSpeed.SlomoScalar) // Slow-Motion
					s_speed_icon = ICON_PF_SLOW_MOTION;
				else if (target_speed == EmuConfig.EmulationSpeed.TurboScalar) // Turbo
					s_speed_icon = ICON_FA_FORWARD_FAST;
				else // Unlimited
					s_speed_icon = ICON_FA_FORWARD;

				DRAW_LINE(osd_font, font_size, s_speed_icon, white_color);
			}
		}
		else
		{
			// Draw Pause indicator
			const TinyString pause_msg = TinyString::from_format(TRANSLATE_FS("ImGuiOverlays", "{} Paused"), ICON_FA_PAUSE);
			DRAW_LINE(osd_font, font_size, pause_msg, white_color);
		}
		#undef DRAW_LINE
}

namespace SaveStateSelectorUI
{
	namespace
	{
		struct ListEntry
		{
			std::string title;
			std::string summary;
			std::string filename;
			std::unique_ptr<GSTexture> preview_texture;
		};
	} // namespace

	static void InitializePlaceholderListEntry(ListEntry* li, std::string path, s32 slot);
	static void InitializeListEntry(const std::string& serial, u32 crc, ListEntry* li, s32 slot);

	static void RefreshHotkeyLegend();
	static void Draw();
	static void ShowSlotOSDMessage();
	static std::string GetSaveStateTimestampSummary(const std::time_t& modification_time);
	bool IsOpen();

	static constexpr const char* SAVED_AGO_DAYS_TIME_DATE =
		TRANSLATE_NOOP("ImGuiOverlays", "Saved {0} days ago at {1:%H:%M} on {1:%a} {1:%Y/%m/%d}");
	static constexpr const char* SAVED_FUTURE_TIME_DATE =
		TRANSLATE_NOOP("ImGuiOverlays", "Saved in the future at {0:%H:%M} on {0:%a} {0:%Y/%m/%d}");
	static constexpr const char* SAVED_AGO_HOURS_MINUTES =
		TRANSLATE_NOOP("ImGuiOverlays", "Saved {0} hours, {1} minutes ago at {2:%H:%M}");
	static constexpr const char* SAVED_AGO_MINUTES = TRANSLATE_NOOP("ImGuiOverlays", "Saved {0} minutes ago at {1:%H:%M}");
	static constexpr const char* SAVED_AGO_SECONDS = TRANSLATE_NOOP("ImGuiOverlays", "Saved {} seconds ago");
	static constexpr const char* SAVED_AGO_NOW = TRANSLATE_NOOP("ImGuiOverlays", "Saved just now");
	static constexpr std::time_t ONE_HOUR = 60 * 60; // 3600
	static constexpr std::time_t TWENTY_FOUR_HOURS = ONE_HOUR * 24; // 86400

	static std::shared_ptr<GSTexture> s_placeholder_texture;
	static std::string s_load_legend;
	static std::string s_save_legend;
	static std::string s_prev_legend;
	static std::string s_next_legend;
	static std::string s_close_legend;

	static std::array<ListEntry, VMManager::NUM_SAVE_STATE_SLOTS> s_slots;
	static std::atomic_int32_t s_current_slot{0};

	static float s_open_time = 0.0f;
	static float s_close_time = 0.0f;

	static ImAnimatedFloat s_scroll_animated;
	static ImAnimatedFloat s_background_animated;

	static bool s_open = false;
} // namespace SaveStateSelectorUI

void SaveStateSelectorUI::Open(float open_time /* = DEFAULT_OPEN_TIME */)
{
	const std::string serial = VMManager::GetDiscSerial();
	if (serial.empty())
	{
		Host::AddIconOSDMessage("SaveStateSelectorUIUnavailable", ICON_PF_MEMORY_CARD,
			TRANSLATE_SV("ImGuiOverlays", "Save state selector is unavailable without a valid game serial."));
		return;
	}

	s_open_time = 0.0f;
	s_close_time = open_time;

	if (s_open)
		return;


	if (!s_placeholder_texture)
		s_placeholder_texture = ImGuiFullscreen::LoadTexture("fullscreenui/no-save.png");

	s_scroll_animated.Reset(0.0f);
	s_background_animated.Reset(0.0f);
	s_open = true;
	RefreshList(serial, VMManager::GetDiscCRC());
	RefreshHotkeyLegend();
}

bool SaveStateSelectorUI::IsOpen()
{
	return s_open;
}

void SaveStateSelectorUI::Close()
{
	s_open = false;
	s_load_legend = {};
	s_save_legend = {};
	s_prev_legend = {};
	s_next_legend = {};
	s_close_legend = {};
}

void SaveStateSelectorUI::RefreshList(const std::string& serial, u32 crc)
{
	for (ListEntry& entry : s_slots)
	{
		if (entry.preview_texture)
			g_gs_device->Recycle(entry.preview_texture.release());
	}

	for (u32 i = 0; i < VMManager::NUM_SAVE_STATE_SLOTS; i++)
		InitializeListEntry(serial, crc, &s_slots[i], static_cast<s32>(i + 1));
}

void SaveStateSelectorUI::Clear()
{
	// called on CPU thread at shutdown, textures should already be deleted, unless running
	// big picture UI, in which case we have to delete them here...
	for (ListEntry& li : s_slots)
	{
		if (li.preview_texture)
		{
			MTGS::RunOnGSThread([tex = li.preview_texture.release()]() {
				g_gs_device->Recycle(tex);
			});
		}

		li = {};
	}

	s_current_slot.store(0, std::memory_order_release);
}

void SaveStateSelectorUI::DestroyTextures()
{
	Close();

	for (ListEntry& entry : s_slots)
	{
		if (entry.preview_texture)
			g_gs_device->Recycle(entry.preview_texture.release());
	}

	s_placeholder_texture.reset();
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
	auto format_legend_entry = [](SmallString binding, std::string_view caption) {
		InputManager::PrettifyInputBinding(binding);
		if (binding.empty())
			binding.append(TRANSLATE_STR("ImGuiOverlays", "Empty"));
		return fmt::format("{} - {}", binding, caption);
	};

	s_load_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "LoadStateFromSlot"),
		TRANSLATE_STR("ImGuiOverlays", "Load"));
	s_save_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SaveStateToSlot"),
		TRANSLATE_STR("ImGuiOverlays", "Save"));
	s_prev_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "PreviousSaveStateSlot"),
		TRANSLATE_STR("ImGuiOverlays", "Select Previous"));
	s_next_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "NextSaveStateSlot"),
		TRANSLATE_STR("ImGuiOverlays", "Select Next"));
	s_close_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "OpenPauseMenu"),
		TRANSLATE_STR("ImGuiOverlays", "Close Menu"));
}

void SaveStateSelectorUI::SelectNextSlot(bool open_selector)
{
	const s32 current_slot = s_current_slot.load(std::memory_order_acquire);
	s_current_slot.store((current_slot == (VMManager::NUM_SAVE_STATE_SLOTS - 1)) ? 0 : (current_slot + 1), std::memory_order_release);

	if (open_selector)
	{
		MTGS::RunOnGSThread([]() {
			if (!s_open)
				Open();

			s_open_time = 0.0f;
		});
	}
	else
	{
		ShowSlotOSDMessage();
	}
}

void SaveStateSelectorUI::SelectPreviousSlot(bool open_selector)
{
	const s32 current_slot = s_current_slot.load(std::memory_order_acquire);
	s_current_slot.store((current_slot == 0) ? (VMManager::NUM_SAVE_STATE_SLOTS - 1) : (current_slot - 1), std::memory_order_release);

	if (open_selector)
	{
		MTGS::RunOnGSThread([]() {
			if (!s_open)
				Open();

			s_open_time = 0.0f;
		});
	}
	else
	{
		ShowSlotOSDMessage();
	}
}

void SaveStateSelectorUI::InitializeListEntry(const std::string& serial, u32 crc, ListEntry* li, s32 slot)
{
	std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(path.c_str(), &sd))
	{
		InitializePlaceholderListEntry(li, std::move(path), slot);
		return;
	}

	li->title = fmt::format(TRANSLATE_FS("ImGuiOverlays", "Save Slot {0}"), slot);
	li->summary = GetSaveStateTimestampSummary(sd.ModificationTime);
	li->filename = Path::GetFileName(path);

	u32 screenshot_width, screenshot_height;
	std::vector<u32> screenshot_pixels;
	if (SaveState_ReadScreenshot(path, &screenshot_width, &screenshot_height, &screenshot_pixels))
	{
		li->preview_texture =
			std::unique_ptr<GSTexture>(g_gs_device->CreateTexture(screenshot_width, screenshot_height, 1, GSTexture::Format::Color));
		if (!li->preview_texture || !li->preview_texture->Update(GSVector4i(0, 0, screenshot_width, screenshot_height),
										screenshot_pixels.data(), sizeof(u32) * screenshot_width))
		{
			Console.Error("Failed to upload save state image to GPU");
			if (li->preview_texture)
				g_gs_device->Recycle(li->preview_texture.release());
		}
	}
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, std::string path, s32 slot)
{
	li->title = fmt::format(TRANSLATE_FS("ImGuiOverlays", "Save Slot {0}"), slot);
	li->summary = TRANSLATE_STR("ImGuiOverlays", "No save present in this slot");
	li->filename = Path::GetFileName(path);
}

void SaveStateSelectorUI::Draw()
{
	static constexpr float SCROLL_ANIMATION_TIME = 0.25f;
	static constexpr float BG_ANIMATION_TIME = 0.15f;

	const auto& io = ImGui::GetIO();
	const float scale = ImGuiManager::GetGlobalScale();
	const float width = (600.0f * scale);
	const float height = (430.0f * scale);

	const float padding_and_rounding = 10.0f * scale;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, padding_and_rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding_and_rounding, padding_and_rounding));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.15f, 0.17f, 0.8f));
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
		ImVec2(0.5f, 0.5f));

	if (ImGui::Begin("##save_state_selector", nullptr,
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoScrollbar))
	{
		// Leave room for the legend.
		const float legend_margin = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
		const float padding = 10.0f * scale;

		ImGui::BeginChild("##item_list", ImVec2(0, -legend_margin), false,
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoBackground);
		{
			const s32 current_slot = s_current_slot.load(std::memory_order_acquire);
			const ImVec2 image_size = ImVec2(128.0f * scale, (128.0f / (4.0f / 3.0f)) * scale);
			const float item_width = std::floor(width - (padding_and_rounding * 2.0f) - ImGui::GetStyle().ScrollbarSize);
			const float item_height = std::floor(image_size.y + padding * 2.0f);
			const float text_indent = image_size.x + padding + padding;

			for (size_t i = 0; i < s_slots.size(); i++)
			{
				const ListEntry& entry = s_slots[i];
				const float y_start = item_height * static_cast<float>(i);

				if (i == static_cast<size_t>(current_slot))
				{
					ImGui::SetCursorPosY(y_start);

					const ImVec2 p_start(ImGui::GetCursorScreenPos());
					const ImVec2 p_end(p_start.x + item_width, p_start.y + item_height);
					const ImRect item_rect(p_start, p_end);
					const ImRect& window_rect = ImGui::GetCurrentWindow()->ClipRect;
					if (!window_rect.Contains(item_rect))
					{
						float scroll_target = ImGui::GetScrollY();
						if (item_rect.Min.y < window_rect.Min.y)
							scroll_target = (ImGui::GetScrollY() - (window_rect.Min.y - item_rect.Min.y));
						else if (item_rect.Max.y > window_rect.Max.y)
							scroll_target = (ImGui::GetScrollY() + (item_rect.Max.y - window_rect.Max.y));

						if (scroll_target != s_scroll_animated.GetEndValue())
							s_scroll_animated.Start(ImGui::GetScrollY(), scroll_target, SCROLL_ANIMATION_TIME);
					}

					if (s_scroll_animated.IsActive())
						ImGui::SetScrollY(s_scroll_animated.UpdateAndGetValue());

					if (s_background_animated.GetEndValue() != p_start.y)
						s_background_animated.Start(s_background_animated.UpdateAndGetValue(), p_start.y, BG_ANIMATION_TIME);

					ImVec2 highlight_pos;
					if (s_background_animated.IsActive())
						highlight_pos = ImVec2(p_start.x, s_background_animated.UpdateAndGetValue());
					else
						highlight_pos = p_start;

					ImGui::GetWindowDrawList()->AddRectFilled(highlight_pos,
						ImVec2(highlight_pos.x + item_width, highlight_pos.y + item_height),
						ImColor(0.22f, 0.30f, 0.34f, 0.9f), padding_and_rounding);
				}

				if (GSTexture* preview_texture = entry.preview_texture ? entry.preview_texture.get() : s_placeholder_texture.get())
				{
					ImGui::SetCursorPosY(y_start + padding);
					ImGui::SetCursorPosX(padding);
					ImGui::Image(reinterpret_cast<ImTextureID>(preview_texture->GetNativeHandle()), image_size);
				}

				ImGui::SetCursorPosY(y_start + padding);

				ImGui::Indent(text_indent);

				ImGui::TextUnformatted(entry.title.c_str(), entry.title.c_str() + entry.title.length());
				ImGui::TextUnformatted(entry.summary.c_str(), entry.summary.c_str() + entry.summary.length());
				ImGui::PushFont(ImGuiManager::GetFixedFont(), ImGuiManager::GetFontSizeStandard());
				ImGui::TextUnformatted(entry.filename.c_str(), entry.filename.c_str() + entry.filename.length());
				ImGui::PopFont();

				ImGui::Unindent(text_indent);
				ImGui::SetCursorPosY(y_start);
				ImGui::ItemSize(ImVec2(item_width, item_height));
			}
		}
		ImGui::EndChild();

		ImGui::BeginChild("##legend", ImVec2(0, 0), false,
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
		{
			ImGui::SetCursorPosX(padding);
			if (ImGui::BeginTable("table", 2))
			{
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(s_load_legend.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(s_prev_legend.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(s_save_legend.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(s_next_legend.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(s_close_legend.c_str());

				ImGui::EndTable();
			}
		}
		ImGui::EndChild();
	}
	ImGui::End();

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();

	// auto-close
	s_open_time += io.DeltaTime;
	if (s_open_time >= s_close_time)
		Close();
}

s32 SaveStateSelectorUI::GetCurrentSlot()
{
	return s_current_slot.load(std::memory_order_acquire) + 1;
}

void SaveStateSelectorUI::LoadCurrentSlot()
{
	Host::RunOnCPUThread([slot = GetCurrentSlot()]() {
		Error error;
		if (!VMManager::LoadStateFromSlot(slot, false, &error))
			FullscreenUI::ReportStateLoadError(error.GetDescription(), slot, false);
	});
	Close();
}

void SaveStateSelectorUI::LoadCurrentBackupSlot()
{
	Host::RunOnCPUThread([slot = GetCurrentSlot()]() {
		Error error;
		if (!VMManager::LoadStateFromSlot(slot, true, &error))
			FullscreenUI::ReportStateLoadError(error.GetDescription(), slot, true);
	});
	Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
	Host::RunOnCPUThread([slot = GetCurrentSlot()]() {
		VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) {
			FullscreenUI::ReportStateSaveError(error, slot);
		});
	});
	Close();
}

void SaveStateSelectorUI::ShowSlotOSDMessage()
{
	const s32 slot = GetCurrentSlot();
	const u32 crc = VMManager::GetDiscCRC();
	const std::string serial = VMManager::GetDiscSerial();
	const std::string filename = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
	FILESYSTEM_STAT_DATA sd;
	std::string timestamp_summary;

	if (!filename.empty() && FileSystem::StatFile(filename.c_str(), &sd))
		timestamp_summary = GetSaveStateTimestampSummary(sd.ModificationTime);
	else
		timestamp_summary = TRANSLATE_STR("ImGuiOverlays", "no save yet");

	Host::AddIconOSDMessage("ShowSlotOSDMessage", ICON_FA_MAGNIFYING_GLASS,
		fmt::format(TRANSLATE_FS("Hotkeys", "Save slot {0} selected ({1})."), slot, timestamp_summary),
		Host::OSD_QUICK_DURATION);
}

// zdxsv network status (Zdxsv::GgpoOsdLines) during a GGPO battle: left edge, below the game's top HUD.
__ri void ImGuiManager::DrawZdxsvGgpoOverlay(float scale, float margin, float spacing)
{
	if (!Zdxsv::g_ggpo_enabled || FullscreenUI::HasActiveWindow())
		return;
	const std::vector<Zdxsv::GgpoOsdLine> lines = Zdxsv::GgpoOsdLines();
	if (lines.empty())
		return;

	ImFont* const font = ImGuiManager::GetStandardFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float line_height = ImGuiFullscreen::GetLineHeight({font, font_size});
	const float pad = std::ceil(4.0f * scale);
	float width = 0.0f;
	for (const Zdxsv::GgpoOsdLine& l : lines)
		width = std::max(width, font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, l.text.c_str()).x);

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const float x = margin;
	float y = std::floor(GetWindowHeight() * 0.22f);
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width + pad * 2.0f, y + line_height * lines.size() + pad * 2.0f), IM_COL32(0, 0, 0, 128));
	y += pad;
	for (const Zdxsv::GgpoOsdLine& l : lines)
	{
		dl->AddText(font, font_size, ImVec2(x + pad, y), l.color, l.text.c_str());
		y += line_height;
	}
}

// zdxsv replay key display (ZDXSV_REPLAY_KEY_DISPLAY=1 / hotkey): the shown position's last input changes, newest on top,
// each with the frames it was held (capped at 99), as in flycast's gdxsv_key_display. Bits: the B word (ZdPadAB).
__ri void ImGuiManager::DrawZdxsvReplayKeys(float scale, float margin)
{
	std::vector<std::pair<u16, int>> runs;
	if (!Zdxsv::g_ggpo_enabled || FullscreenUI::HasActiveWindow() || !Zdxsv::ReplayKeys(runs) || runs.empty())
		return;

	static constexpr struct { u16 bit; const char* icon; } buttons[] = {
		{0x0200, ICON_PF_BUTTON_SQUARE}, {0x0100, ICON_PF_BUTTON_TRIANGLE}, {0x0040, ICON_PF_BUTTON_CROSS},
		{0x0020, ICON_PF_BUTTON_CIRCLE}, {0x0080, ICON_PF_LEFT_SHOULDER_L1}, {0x0010, ICON_PF_RIGHT_SHOULDER_R1},
		{0x0008, ICON_PF_LEFT_TRIGGER_L2}, {0x0004, ICON_PF_RIGHT_TRIGGER_R2}, {0x0002, ICON_PF_LEFT_ANALOG_CLICK},
		{0x4000, ICON_PF_SELECT_SHARE}};
	const auto dir = [](u16 b) -> const char* {
		const bool u = b & 0x2000, d = b & 0x1000, l = b & 0x0800, r = b & 0x0400;
		if (u && l) return ICON_PF_DPAD_LEFT_UP;
		if (u && r) return ICON_PF_DPAD_UP_RIGHT;
		if (d && l) return ICON_PF_DPAD_LEFT_DOWN;
		if (d && r) return ICON_PF_DPAD_RIGHT_DOWN;
		if (u) return ICON_PF_DPAD_UP;
		if (d) return ICON_PF_DPAD_DOWN;
		if (l) return ICON_PF_DPAD_LEFT;
		if (r) return ICON_PF_DPAD_RIGHT;
		return nullptr;
	};

	ImFont* const font = ImGuiManager::GetStandardFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float line_height = ImGuiFullscreen::GetLineHeight({font, font_size});
	const float pad = std::ceil(4.0f * scale);
	const float count_w = font->CalcTextSizeA(font_size, FLT_MAX, -1.0f, "99 ").x;
	const float icon_w = font->CalcTextSizeA(font_size, FLT_MAX, -1.0f, ICON_PF_DPAD_LEFT_UP " ").x;
	const float width = count_w + icon_w * (1 + std::size(buttons));

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const float x = margin;
	float y = std::floor(GetWindowHeight() * 0.40f);
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width + pad * 2.0f, y + line_height * runs.size() + pad * 2.0f), IM_COL32(0, 0, 0, 128));
	y += pad;
	for (const auto& [b, n] : runs)
	{
		const std::string count = fmt::format("{:2}", std::min(n, 99));
		dl->AddText(font, font_size, ImVec2(x + pad, y), IM_COL32(255, 255, 255, 255), count.c_str());
		float cx = x + pad + count_w;
		if (const char* d = dir(b))
			dl->AddText(font, font_size, ImVec2(cx, y), IM_COL32(255, 255, 255, 255), d);
		cx += icon_w;
		for (const auto& e : buttons)
		{
			if (b & e.bit)
			{
				dl->AddText(font, font_size, ImVec2(cx, y), IM_COL32(255, 255, 255, 255), e.icon);
				cx += icon_w;
			}
		}
		y += line_height;
	}
}

// zdxsv replay control bar (ZDXSV_REPLAY): play/pause, seek -10 s / +10 s, time and frame, a timeline (click or
// drag, seek on release), point of view. Shown while paused and for 3 s after the mouse moves over the bottom
// quarter; ZDXSV_REPLAY_BAR=1 always shows it, =0 never.
__ri void ImGuiManager::DrawZdxsvReplayBar(float scale, float margin)
{
	static const char* const s_env = std::getenv("ZDXSV_REPLAY_BAR");
	static float s_idle = 0.0f;
	static ImVec2 s_prev_mouse(-1.0f, -1.0f);
	static std::string s_layout; // logged when it changes: test drivers (zdxsv pcsx2ctl.ps1 bar:) click from it
	std::string layout;
	int frame, frames, pov, target;
	u32 povs;
	if (!Zdxsv::g_ggpo_enabled || (s_env && s_env[0] == '0') || FullscreenUI::HasActiveWindow() ||
		!Zdxsv::ReplayBarInfo(frame, frames, pov, povs, target) || frames <= 0)
		return;

	const ImGuiIO& io = ImGui::GetIO();
	const float w = GetWindowWidth(), h = GetWindowHeight();
	const ImVec2 mouse = io.MousePos;
	const bool inside = mouse.x >= 0.0f && mouse.y >= 0.0f && mouse.x <= w && mouse.y <= h;
	if (inside && s_prev_mouse.x >= 0.0f && (mouse.x != s_prev_mouse.x || mouse.y != s_prev_mouse.y) && mouse.y >= h * 0.75f)
		s_idle = 3.0f;
	s_prev_mouse = mouse;
	s_idle = std::max(0.0f, s_idle - io.DeltaTime);
	const bool paused = VMManager::GetState() == VMState::Paused;
	if (s_idle <= 0.0f && !paused && !(s_env && s_env[0] == '1'))
		return;

	ImFont* const font = ImGuiManager::GetStandardFont();
	const float font_size = ImGuiManager::GetFontSizeStandard();
	const float bar_h = std::ceil(36.0f * scale);
	const float bar_w = std::floor(w * 0.85f);
	const float x0 = std::floor((w - bar_w) * 0.5f);
	const float y0 = std::floor(h - bar_h - margin - 10.0f * scale);
	const float pad = std::ceil(8.0f * scale);
	const auto fmt_time = [](int f) { return fmt::format("{}:{:02}", f / 3600, f / 60 % 60); };
	const int shown = target >= 0 ? target : frame;
	const std::string time = fmt::format("{} / {}  {} / {}", fmt_time(shown), fmt_time(frames), shown, frames);
	const std::string pov_label = fmt::format(ICON_FA_EYE " P{}", pov + 1);
	const bool can_switch = (povs & ~(1u << pov)) != 0;

	ImGui::SetNextWindowPos(ImVec2(x0, y0));
	ImGui::SetNextWindowSize(ImVec2(bar_w, bar_h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushFont(font, font_size);
	if (ImGui::Begin("##zdxsv_replay_bar", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground))
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bar_w, y0 + bar_h), IM_COL32(0, 0, 0, 180), std::ceil(8.0f * scale));
		const ImU32 text_col = IM_COL32(255, 255, 255, 255);
		const ImU32 dim_col = IM_COL32(120, 120, 120, 200);
		float x = x0 + pad;
		const float cy = y0 + bar_h * 0.5f;
		const auto text_at = [&](float tx, const char* s, ImU32 col) {
			const ImVec2 sz = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, s);
			dl->AddText(font, font_size, ImVec2(tx, cy - sz.y * 0.5f), col, s);
			return sz.x;
		};
		// a text button; true when clicked
		const auto button = [&](const char* id, const char* label, bool enabled) {
			const float tw = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, label).x;
			ImGui::SetCursorScreenPos(ImVec2(x, y0));
			ImGui::InvisibleButton(id, ImVec2(tw + pad * 2.0f, bar_h));
			const bool hovered = ImGui::IsItemHovered();
			if (hovered || ImGui::IsItemActive())
				s_idle = 3.0f;
			if (hovered && enabled)
				dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 255, 40));
			text_at(x + pad, label, enabled ? text_col : dim_col);
			layout += fmt::format(" {} {:.0f}-{:.0f}", id + 2, x, x + tw + pad * 2.0f);
			x += tw + pad * 2.0f;
			return enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
		};

		if (button("##pause", paused ? ICON_FA_PLAY : ICON_FA_PAUSE, true))
		{
			Console.WriteLn("ZdxsvGgpo: replay bar: %s at frame %d", paused ? "play" : "pause", frame);
			Host::RunOnCPUThread([] { Zdxsv::ReplayTogglePause(); });
		}
		if (button("##back", ICON_FA_BACKWARD " 10s", true))
		{
			Console.WriteLn("ZdxsvGgpo: replay bar: seek -600 at frame %d", frame);
			Host::RunOnCPUThread([f = shown - 600] { Zdxsv::ReplaySeekTo(f); });
		}
		if (button("##forward", "10s " ICON_FA_FORWARD, true))
		{
			Console.WriteLn("ZdxsvGgpo: replay bar: seek +600 at frame %d", frame);
			Host::RunOnCPUThread([f = shown + 600] { Zdxsv::ReplaySeekTo(f); });
		}
		// round jump: R0 = before round 1 (MS select, briefing)
		const std::vector<int> rounds = Zdxsv::ReplayRoundStarts();
		const int round = static_cast<int>(std::upper_bound(rounds.begin(), rounds.end(), shown) - rounds.begin());
		if (button("##prevround", ICON_FA_BACKWARD_STEP, true))
		{
			Console.WriteLn("ZdxsvGgpo: replay bar: round -1 at frame %d", frame);
			Host::RunOnCPUThread([] { Zdxsv::ReplayJumpRound(-1); });
		}
		const std::string round_label = fmt::format("R{}", round);
		const float round_w = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, "R00").x;
		text_at(x + (round_w - font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, round_label.c_str()).x) * 0.5f,
			round_label.c_str(), text_col);
		x += round_w;
		if (button("##nextround", ICON_FA_FORWARD_STEP, true))
		{
			Console.WriteLn("ZdxsvGgpo: replay bar: round +1 at frame %d", frame);
			Host::RunOnCPUThread([] { Zdxsv::ReplayJumpRound(1); });
		}
		x += pad;
		text_at(x, time.c_str(), text_col);
		// fixed width (the longest time text + a margin), so the timeline does not move as digits change
		const std::string widest = fmt::format("{} / {}  {} / {}", fmt_time(frames), fmt_time(frames), frames, frames);
		x += font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, widest.c_str()).x + pad * 4.0f;

		// the timeline fills the space left of the point of view button
		const float pov_w = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, pov_label.c_str()).x + pad * 2.0f;
		const float track_x0 = x, track_x1 = x0 + bar_w - pad - pov_w - pad;
		if (track_x1 - track_x0 > pad * 4.0f)
		{
			const float track_h = std::ceil(4.0f * scale);
			const float frac = std::clamp(static_cast<float>(shown) / static_cast<float>(frames), 0.0f, 1.0f);
			dl->AddRectFilled(ImVec2(track_x0, cy - track_h * 0.5f), ImVec2(track_x1, cy + track_h * 0.5f), IM_COL32(255, 255, 255, 60));
			dl->AddRectFilled(ImVec2(track_x0, cy - track_h * 0.5f), ImVec2(track_x0 + (track_x1 - track_x0) * frac, cy + track_h * 0.5f),
				IM_COL32(255, 80, 80, 255));
			for (const int r : rounds) // round start marks
			{
				const float rx = track_x0 + (track_x1 - track_x0) * std::clamp(static_cast<float>(r) / static_cast<float>(frames), 0.0f, 1.0f);
				dl->AddRectFilled(ImVec2(rx - scale, cy - track_h * 2.0f), ImVec2(rx + scale, cy + track_h * 2.0f), IM_COL32(255, 220, 80, 255));
			}
			ImGui::SetCursorScreenPos(ImVec2(track_x0, y0));
			ImGui::InvisibleButton("##timeline", ImVec2(track_x1 - track_x0, bar_h));
			layout += fmt::format(" timeline {:.0f}-{:.0f}", track_x0, track_x1);
			const auto frame_at = [&](float mx) {
				const float t = std::clamp((mx - track_x0) / (track_x1 - track_x0), 0.0f, 1.0f);
				return std::min(frames - 1, static_cast<int>(std::lround(t * static_cast<float>(frames))));
			};
			const bool active = ImGui::IsItemActive();
			float knob = track_x0 + (track_x1 - track_x0) * frac;
			if (active || ImGui::IsItemHovered())
			{
				s_idle = 3.0f;
				const int f = frame_at(mouse.x);
				const float mx = std::clamp(mouse.x, track_x0, track_x1);
				const std::string tip = fmt::format("{}  {}", fmt_time(f), f);
				const ImVec2 sz = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, tip.c_str());
				const ImVec2 tp(std::clamp(mx - sz.x * 0.5f, 0.0f, w - sz.x), y0 - sz.y - pad);
				ImDrawList* fg = ImGui::GetForegroundDrawList();
				fg->AddRectFilled(ImVec2(tp.x - pad * 0.5f, tp.y - pad * 0.25f), ImVec2(tp.x + sz.x + pad * 0.5f, tp.y + sz.y + pad * 0.25f),
					IM_COL32(20, 20, 20, 235), std::ceil(4.0f * scale));
				fg->AddText(font, font_size, tp, text_col, tip.c_str());
				if (active)
					knob = mx;
			}
			dl->AddCircleFilled(ImVec2(knob, cy), std::ceil(6.0f * scale), text_col);
			if (ImGui::IsItemDeactivated())
			{
				const int f = frame_at(mouse.x);
				Console.WriteLn("ZdxsvGgpo: replay bar: seek to %d at frame %d", f, frame);
				Host::RunOnCPUThread([f] { Zdxsv::ReplaySeekTo(f); });
			}
		}

		x = x0 + bar_w - pad - pov_w;
		if (button("##pov", pov_label.c_str(), can_switch))
		{
			Console.WriteLn("ZdxsvGgpo: replay bar: point of view at frame %d", frame);
			Host::RunOnCPUThread([] { Zdxsv::ReplayNextPov(); });
		}
		layout = fmt::format("{:.0f}x{:.0f} y {:.0f}-{:.0f}{}", w, h, y0, y0 + bar_h, layout);
		if (layout != s_layout)
		{
			s_layout = layout;
			Console.WriteLn("ZdxsvGgpo: replay bar: layout %s", layout.c_str());
		}
	}
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar(2);
}

void ImGuiManager::RenderOverlays()
{
	const float scale = ImGuiManager::GetGlobalScale();
	const float margin = std::ceil(GSConfig.OsdMargin * scale);
	const float spacing = std::ceil(5.0f * scale);
	float position_y = margin;

	DrawIndicatorsOverlay(position_y, scale, margin, spacing);
	DrawVideoCaptureOverlay(position_y, scale, margin, spacing);
	DrawInputRecordingOverlay(position_y, scale, margin, spacing);
	DrawTextureReplacementsOverlay(position_y, scale, margin, spacing);
	if (GSConfig.OsdPerformancePos != OsdOverlayPos::None)
		DrawPerformanceOverlay(position_y, scale, margin, spacing);
	DrawSettingsOverlay(scale, margin, spacing);
	DrawShaderCompileIndicator(scale, margin, spacing);
	DrawInputsOverlay(scale, margin, spacing);
	DrawZdxsvGgpoOverlay(scale, margin, spacing);
	DrawZdxsvReplayKeys(scale, margin);
	DrawZdxsvReplayBar(scale, margin);
	if (SaveStateSelectorUI::s_open)
		SaveStateSelectorUI::Draw();
}

std::string SaveStateSelectorUI::GetSaveStateTimestampSummary(const std::time_t& modification_time)
{

	std::tm tm_modification_local = {};
#ifdef _MSC_VER
	localtime_s(&tm_modification_local, &modification_time);
#else
	localtime_r(&modification_time, &tm_modification_local);
#endif

	const std::time_t current_time = std::time(nullptr);
	const std::time_t time_since_save = current_time - std::mktime(&tm_modification_local);

	if (time_since_save >= TWENTY_FOUR_HOURS)
	{
		return fmt::format(TRANSLATE_FS("ImGuiOverlays", SAVED_AGO_DAYS_TIME_DATE),
			time_since_save / TWENTY_FOUR_HOURS, tm_modification_local);
	}
	else if (time_since_save >= ONE_HOUR)
	{
		return fmt::format(TRANSLATE_FS("ImGuiOverlays", SAVED_AGO_HOURS_MINUTES),
			time_since_save / ONE_HOUR, (time_since_save / 60) % 60, tm_modification_local);
	}
	else if (time_since_save >= 60)
	{
		return fmt::format(TRANSLATE_FS("ImGuiOverlays", SAVED_AGO_MINUTES),
			time_since_save / 60, tm_modification_local);
	}
	else if (time_since_save >= 5)
	{
		return fmt::format(TRANSLATE_FS("ImGuiOverlays", SAVED_AGO_SECONDS),
			time_since_save);
	}
	else if (time_since_save >= 0)
	{
		return TRANSLATE_STR("ImGuiOverlays", SAVED_AGO_NOW);
	}
	else
	{
		return fmt::format(TRANSLATE_FS("ImGuiOverlays", SAVED_FUTURE_TIME_DATE),
			tm_modification_local);
	}
}
