// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Frame time panel: where the CPU thread's wall time of each GGPO frame went, as a stacked graph and
// per-stage averages; the other threads' time per frame on rows of their own (docs/zdxsv/rollback.md).

#include "Zdxsv/GgpoShared.h"

#include "Config.h"
#include "ImGui/ImGuiManager.h"
#include "PerformanceMetrics.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <mutex>
#include <string>
#include <utility>

namespace Zdxsv
{
	namespace
	{
		enum FrameStage
		{
			StageEmulate,
			StageRerun,
			StageState,
			StageWait,
			StageSleep,
			StageOther,
			StageCount,
		};

		constexpr const char* kStageNames[StageCount] = {"Emulate", "Rollback", "Save/Load", "Net wait", "Sleep", "Other"};
		constexpr ImU32 kStageColors[StageCount] = {IM_COL32(90, 170, 255, 255), IM_COL32(255, 90, 80, 255),
			IM_COL32(255, 200, 60, 255), IM_COL32(200, 110, 255, 255), IM_COL32(110, 110, 120, 255), IM_COL32(170, 230, 170, 255)};
		constexpr int kHistory = 120;
		constexpr int kLogEvery = 600;
		constexpr double kStaleMs = 1000.0;
		constexpr double kFrameMs = 1000.0 / 59.94;

		struct FrameSample
		{
			std::array<float, StageCount> ms{};
			float total = 0;
		};

		struct FrameOsdSums
		{
			double emu = 0, exit = 0, rerun = 0, state = 0, wait = 0, sleep = 0;
			int n = 0;
		};

		struct FrameOsdState
		{
			std::mutex mtx;
			std::array<FrameSample, kHistory> history{};
			int pos = 0, count = 0, logged = 0;
			Common::Timer::Value last = 0;
			FrameOsdSums prev;
		};
		FrameOsdState s_frame_osd;

		FrameOsdSums Sums(const SessionStats& s)
		{
			return {s.emu_ms.sum, s.exit_ms.sum, s.rerun_ms.sum, s.save_ms.sum + s.hash_ms.sum + s.load_ms.sum, s.wait_ms.sum,
				s.sleep_ms.sum, s.ours_ms.n};
		}

		void LogAverages(int frame, const FrameOsdState& st)
		{
			std::array<double, StageCount> sum{};
			double total = 0;
			for (int i = 0; i < st.count; i++)
			{
				for (int k = 0; k < StageCount; k++)
					sum[k] += st.history[i].ms[k];
				total += st.history[i].total;
			}
			const double n = std::max(st.count, 1);
			Console.WriteLn("ZdxsvGgpo: frame osd frame %d: frames %d total %.3f emulate %.3f rollback %.3f state %.3f wait %.3f sleep %.3f other %.3f",
				frame, st.count, total / n, sum[StageEmulate] / n, sum[StageRerun] / n, sum[StageState] / n, sum[StageWait] / n,
				sum[StageSleep] / n, sum[StageOther] / n);
		}
	} // namespace

	// The CPU thread's wall time since the previous GgpoOnExecuteReturned end, split by the running sums of s_stats.
	void GgpoFrameOsdSample(int frame)
	{
		const Common::Timer::Value now = s_stats.t_returned;
		const FrameOsdSums cur = Sums(s_stats);
		std::lock_guard lock(s_frame_osd.mtx);
		FrameOsdState& st = s_frame_osd;
		const FrameOsdSums prev = std::exchange(st.prev, cur);
		const Common::Timer::Value last = std::exchange(st.last, now);
		if (!last || cur.n != prev.n + 1 || Common::Timer::ConvertValueToMilliseconds(now - last) > kStaleMs)
			return;
		FrameSample f;
		f.total = static_cast<float>(Common::Timer::ConvertValueToMilliseconds(now - last));
		f.ms[StageEmulate] = static_cast<float>((cur.emu - prev.emu) + (cur.exit - prev.exit));
		f.ms[StageRerun] = static_cast<float>(cur.rerun - prev.rerun);
		f.ms[StageState] = static_cast<float>(cur.state - prev.state);
		f.ms[StageWait] = static_cast<float>(cur.wait - prev.wait);
		f.ms[StageSleep] = static_cast<float>(cur.sleep - prev.sleep);
		float known = 0;
		for (int k = 0; k < StageOther; k++)
			known += f.ms[k];
		f.ms[StageOther] = std::max(0.f, f.total - known);
		st.history[st.pos] = f;
		st.pos = (st.pos + 1) % kHistory;
		st.count = std::min(st.count + 1, kHistory);
		if (++st.logged % kLogEvery == 0)
			LogAverages(frame, st);
	}

	void GgpoFrameOsdReset()
	{
		std::lock_guard lock(s_frame_osd.mtx);
		s_frame_osd.history = {};
		s_frame_osd.pos = s_frame_osd.count = s_frame_osd.logged = 0;
		s_frame_osd.last = 0;
		s_frame_osd.prev = {};
	}

	void GgpoFrameOsdDraw()
	{
		std::array<FrameSample, kHistory> history;
		int pos, count;
		{
			std::lock_guard lock(s_frame_osd.mtx);
			if (!s_frame_osd.count || Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_frame_osd.last) > kStaleMs)
				return;
			history = s_frame_osd.history;
			pos = s_frame_osd.pos;
			count = s_frame_osd.count;
		}

		std::array<double, StageCount> avg{};
		double total_avg = 0, total_max = 0;
		for (int i = 0; i < count; i++)
		{
			for (int k = 0; k < StageCount; k++)
				avg[k] += history[i].ms[k];
			total_avg += history[i].total;
			total_max = std::max<double>(total_max, history[i].total);
		}
		for (double& a : avg)
			a /= count;
		total_avg /= count;

		const float scale = ImGuiManager::GetGlobalScale();
		const float display_w = ImGuiManager::GetWindowWidth(), hud = ImGuiManager::GetWindowHeight() / 960.f;
		ImFont* const font = ImGuiManager::GetStandardFont();
		const float fs = 13.f * scale, line = fs + 2.f * scale, pad = 6.f * scale;
		const float graph_w = kHistory * 2.f * scale, graph_h = 40.f * scale;
		const float width = graph_w + pad * 2.f;
		const bool vu = THREAD_VU1;
		const int rows = 1 + (StageCount + 1) / 2 + 1;
		const float height = pad * 2.f + graph_h + pad + rows * line;
		const float left = display_w - 30.f * hud - width, top = (192.f + 21.f) * hud;

		ImDrawList* const dl = ImGui::GetBackgroundDrawList();
		dl->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + height), IM_COL32(0, 0, 0, 150), 4.f * scale);

		const float range = static_cast<float>(std::max(kFrameMs * 1.5, total_max));
		const float gx = left + pad, gy = top + pad;
		const float bar = graph_w / kHistory;
		dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + graph_w, gy + graph_h), IM_COL32(255, 255, 255, 20));
		for (int i = 0; i < count; i++)
		{
			const FrameSample& f = history[(pos - count + i + kHistory) % kHistory];
			const float x = gx + (kHistory - count + i) * bar;
			float y = gy + graph_h;
			for (int k = 0; k < StageCount; k++)
			{
				const float h = f.ms[k] / range * graph_h;
				if (h <= 0.f)
					continue;
				dl->AddRectFilled(ImVec2(x, std::max(gy, y - h)), ImVec2(x + bar, y), kStageColors[k]);
				y -= h;
			}
		}
		const float budget_y = gy + graph_h - static_cast<float>(kFrameMs / range) * graph_h;
		dl->AddLine(ImVec2(gx, budget_y), ImVec2(gx + graph_w, budget_y), IM_COL32(255, 255, 255, 140), 1.f);

		const ImU32 text = IM_COL32(235, 240, 255, 255), dim = IM_COL32(170, 175, 190, 255);
		const float col_w = graph_w * 0.5f, gap = 4.f * scale;
		const auto right = [&](float x_end, float y, ImU32 color, const std::string& value) {
			const float w = font->CalcTextSizeA(fs, FLT_MAX, 0.f, value.c_str()).x;
			dl->AddText(font, fs, ImVec2(x_end - w, y), color, value.c_str());
		};
		float y = gy + graph_h + pad;
		dl->AddText(font, fs, ImVec2(gx, y), text, "CPU thread");
		right(gx + graph_w, y, text, fmt::format("{:.2f} ms  max {:.1f}", total_avg, total_max));
		y += line;
		for (int k = 0; k < StageCount; k++)
		{
			const float x = gx + (k % 2) * col_w, ry = y + (k / 2) * line;
			dl->AddRectFilled(ImVec2(x, ry + fs * 0.25f), ImVec2(x + fs * 0.5f, ry + fs * 0.75f), kStageColors[k]);
			dl->AddText(font, fs, ImVec2(x + fs * 0.5f + gap, ry), text, kStageNames[k]);
			right(x + col_w - (k % 2 ? 0.f : gap * 2.f), ry, text, fmt::format("{:.2f}", avg[k]));
		}
		y += (StageCount + 1) / 2 * line;
		std::string others = fmt::format("GS {:.2f}", PerformanceMetrics::GetGSThreadAverageTime());
		if (vu)
			others += fmt::format("  VU {:.2f}", PerformanceMetrics::GetVUThreadAverageTime());
		others += fmt::format("  GPU {:.2f}", PerformanceMetrics::GetGPUAverageTime());
		dl->AddText(font, fs, ImVec2(gx, y), dim, "Other threads");
		right(gx + graph_w, y, text, others);
	}
} // namespace Zdxsv
