// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/UiHooks.h"
#include "Zdxsv/Ggpo.h"
#include "Zdxsv/Lobby.h"

#include "Host.h"
#include "IconsPromptFont.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "VMManager.h"

#include "common/Console.h"

#include "fmt/format.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace Zdxsv
{
	namespace
	{
		// zdxsv network status (Zdxsv::GgpoOsdLines) during a GGPO battle: left edge, below the game's top HUD.
		void DrawGgpoOverlay(float scale, float margin, float spacing)
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
			// fixed width, whatever the lines hold: a longer line (a long name, a huge Wait) is cut and ends in "..."
			const auto text_w = [&](const char* s) { return font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), -1.0f, s).x; };
			const float width = std::max(text_w(" Ping 000ms (R)  P -00"), text_w("Roll 00  Wait 00000"));
			const float dots_w = text_w("...");

			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const float x = margin;
			float y = std::floor(ImGuiManager::GetWindowHeight() * 0.22f);
			dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width + pad * 2.0f, y + line_height * lines.size() + pad * 2.0f), IM_COL32(0, 0, 0, 128));
			y += pad;
			for (const Zdxsv::GgpoOsdLine& l : lines)
			{
				std::string text = l.text;
				if (text_w(text.c_str()) > width)
				{
					// drop whole UTF-8 characters from the end until the rest and "..." fit
					while (!text.empty() && text_w(text.c_str()) + dots_w > width)
					{
						// continuation bytes (10xxxxxx), then the lead or ASCII byte
						u8 c;
						do
						{
							c = static_cast<u8>(text.back());
							text.pop_back();
						} while (!text.empty() && (c & 0xC0) == 0x80);
					}
					text += "...";
				}
				dl->AddText(font, font_size, ImVec2(x + pad, y), l.color, text.c_str());
				y += line_height;
			}
		}

		// zdxsv replay key display (ZDXSV_REPLAY_KEY_DISPLAY=1 / hotkey): the shown position's last input changes, newest on top,
		// each with the frames it was held (capped at 99). Bits: the B word (ZdPadAB).
		static constexpr struct { u16 bit; const char* icon; } buttons[] = {
			{0x0200, ICON_PF_BUTTON_SQUARE}, {0x0100, ICON_PF_BUTTON_TRIANGLE}, {0x0040, ICON_PF_BUTTON_CROSS},
			{0x0020, ICON_PF_BUTTON_CIRCLE}, {0x0080, ICON_PF_LEFT_SHOULDER_L1}, {0x0010, ICON_PF_RIGHT_SHOULDER_R1},
			{0x0008, ICON_PF_LEFT_TRIGGER_L2}, {0x0004, ICON_PF_RIGHT_TRIGGER_R2}, {0x0002, ICON_PF_LEFT_ANALOG_CLICK},
			{0x4000, ICON_PF_SELECT_SHARE}, {0x8000, ICON_PF_START}};
		const char* dir(u16 b)
		{
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
		}

		// B bits as button icons ("-" = none)
		std::string BIcons(u16 b)
		{
			std::string s;
			if (const char* d = dir(b))
				s += d;
			for (const auto& e : buttons)
				if (b & e.bit)
					s += s.empty() ? e.icon : fmt::format(" {}", e.icon);
			return s.empty() ? "-" : s;
		}

		void DrawReplayKeys(float scale, float margin)
		{
			std::vector<std::pair<u16, int>> runs;
			if (!Zdxsv::g_ggpo_enabled || FullscreenUI::HasActiveWindow() || !Zdxsv::ReplayKeys(runs) || runs.empty())
				return;

			ImFont* const font = ImGuiManager::GetStandardFont();
			const float font_size = ImGuiManager::GetFontSizeStandard();
			const float line_height = ImGuiFullscreen::GetLineHeight({font, font_size});
			const float pad = std::ceil(4.0f * scale);
			const float count_w = font->CalcTextSizeA(font_size, FLT_MAX, -1.0f, "99 ").x;
			const float icon_w = font->CalcTextSizeA(font_size, FLT_MAX, -1.0f, ICON_PF_DPAD_LEFT_UP " ").x;
			const float width = count_w + icon_w * (1 + std::size(buttons));

			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const float x = margin;
			float y = std::floor(ImGuiManager::GetWindowHeight() * 0.40f);
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
		// drag, seek on release), point of view. Setting ZdxsvReplayBar: 0 = shown while paused and for 3 s after the
		// mouse moves over the bottom quarter, 1 = always, 2 = never; ZDXSV_REPLAY_BAR=1 / =0 overrides it (always / never).
		void DrawReplayBar(float scale, float margin)
		{
			static const char* const s_env = std::getenv("ZDXSV_REPLAY_BAR");
			static float s_idle = 0.0f;
			static ImVec2 s_prev_mouse(-1.0f, -1.0f);
			static std::string s_layout; // logged when it changes: test drivers (tests/zdxsv/pcsx2ctl.ps1 bar:) click from it
			std::string layout;
			int frame, frames, pov, target;
			u32 povs;
			if (!Zdxsv::g_ggpo_enabled || FullscreenUI::HasActiveWindow() || !Zdxsv::ReplayBarInfo(frame, frames, pov, povs, target) ||
				frames <= 0)
				return;
			const int mode = s_env ? (s_env[0] == '0' ? 2 : 1) : Host::GetIntSettingValue("DEV9/Eth", "ZdxsvReplayBar", 0);
			if (mode == 2)
				return;

			const ImGuiIO& io = ImGui::GetIO();
			const float w = ImGuiManager::GetWindowWidth(), h = ImGuiManager::GetWindowHeight();
			const ImVec2 mouse = io.MousePos;
			const bool inside = mouse.x >= 0.0f && mouse.y >= 0.0f && mouse.x <= w && mouse.y <= h;
			if (inside && s_prev_mouse.x >= 0.0f && (mouse.x != s_prev_mouse.x || mouse.y != s_prev_mouse.y) && mouse.y >= h * 0.75f)
				s_idle = 3.0f;
			s_prev_mouse = mouse;
			s_idle = std::max(0.0f, s_idle - io.DeltaTime);
			const bool paused = VMManager::GetState() == VMState::Paused;
			if (s_idle <= 0.0f && !paused && mode != 1)
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
				if (const std::string results = Zdxsv::ReplayRoundResults(); !results.empty())
				{
					const float tw = text_at(x, results.c_str(), text_col);
					layout += fmt::format(" results:{} {:.0f}-{:.0f}", results, x, x + tw);
					x += tw + pad;
				}
				// takeover: take over the own position here; while taken over, retry from its frame or back to the replay
				int to_phase = 0;
				u16 to_target, to_current;
				float to_left;
				Zdxsv::ReplayTakeoverInfo(to_phase, to_target, to_current, to_left);
				const std::string retry_label = fmt::format(ICON_FA_ROTATE_LEFT " {}", TRANSLATE_SV("Zdxsv", "Retry"));
				const std::string takeover_label = fmt::format(ICON_FA_GAMEPAD " {}", TRANSLATE_SV("Zdxsv", "Take over"));
				const std::string toreplay_label = fmt::format(ICON_FA_FILM " {}", TRANSLATE_SV("Zdxsv", "Replay"));
				if (button("##takeover", to_phase == 3 ? retry_label.c_str() : takeover_label.c_str(), to_phase == 0 || to_phase == 3))
				{
					Console.WriteLn("ZdxsvGgpo: replay bar: takeover at frame %d", frame);
					Host::RunOnCPUThread([] { Zdxsv::ReplayTakeover(); });
				}
				if (to_phase == 3 && button("##toreplay", toreplay_label.c_str(), true))
				{
					Console.WriteLn("ZdxsvGgpo: replay bar: back to the replay at frame %d", frame);
					Host::RunOnCPUThread([] { Zdxsv::ReplayTakeoverReturn(); });
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

		// replay takeover input matching: the replay's own input
		// at the takeover frame and the host pad's, then the 1 s countdown
		void DrawTakeover(float scale)
		{
			int phase;
			u16 target, current;
			float left;
			if (!Zdxsv::g_ggpo_enabled || FullscreenUI::HasActiveWindow() || !Zdxsv::ReplayTakeoverInfo(phase, target, current, left) ||
				(phase != 1 && phase != 2))
				return;
			const float w = ImGuiManager::GetWindowWidth(), h = ImGuiManager::GetWindowHeight();
			ImGui::PushFont(ImGuiManager::GetStandardFont(), ImGuiManager::GetFontSizeStandard());
			ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.35f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowBgAlpha(0.8f);
			if (ImGui::Begin("##zdxsv_takeover", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
			{
				if (phase == 1)
				{
					ImGui::TextUnformatted(TRANSLATE("Zdxsv", "Takeover: hold the replay's input"));
					ImGui::Text("%s %s", TRANSLATE("Zdxsv", "Replay:"), BIcons(target).c_str());
					ImGui::Text("%s %s", TRANSLATE("Zdxsv", "You:"), BIcons(current).c_str());
				}
				else
				{
					ImGui::TextUnformatted(TRANSLATE("Zdxsv", "Takeover starts"));
					ImGui::ProgressBar(1.0f - left, ImVec2(300.0f * scale, 0.0f), "");
				}
				if (phase == 1 && ImGui::Button(TRANSLATE("Zdxsv", "Skip matching (START)")))
				{
					Console.WriteLn("ZdxsvGgpo: takeover panel: skip");
					Host::RunOnCPUThread([] { Zdxsv::ReplayTakeoverSkip(); });
				}
				ImGui::SameLine();
				if (ImGui::Button(TRANSLATE("Zdxsv", "Cancel")))
				{
					Console.WriteLn("ZdxsvGgpo: takeover panel: cancel");
					Host::RunOnCPUThread([] { Zdxsv::ReplayTakeoverCancel(); });
				}
				ImGui::TextDisabled("%s", TRANSLATE("Zdxsv", "While taken over, START retries from the same frame."));
			}
			ImGui::End();
			ImGui::PopFont();
		}

		// Connection diagram during the lobby ping test: a dot per player (the first half of the positions on
		// top, the rest below) and five bars from each player toward each other one, fewer and redder as that
		// player's rtt to it grows; grey = no answer yet. Shown from 1.55 s into the test until 0.6 s before its end.
		// 0 up to 30 ms, 1 up to 60 ms, ... 4 over 120 ms
		int PingLevel(int ms)
		{
			return std::clamp((ms - 1) / 30, 0, 4);
		}

		ImU32 PingColor(int ms, float alpha)
		{
			static constexpr ImU32 colors[5] = {IM_COL32(87, 213, 213, 255), IM_COL32(0, 255, 149, 255), IM_COL32(255, 255, 0, 255),
				IM_COL32(255, 170, 0, 255), IM_COL32(255, 0, 0, 255)};
			ImColor c(ms <= 0 ? IM_COL32(64, 64, 64, 255) : colors[PingLevel(ms)]);
			c.Value.w *= alpha;
			return c;
		}

		int PingBars(int ms)
		{
			return 5 - PingLevel(ms);
		}

		// five widening bars from anchor in direction dir (0 = down, then clockwise by 45 degrees); the
		// first `bars` lit, one of them highlighted in turn every 100 ms
		void DrawPingBars(ImDrawList* dl, ImVec2 anchor, int ms, float alpha, float scale, int dir, int elapsed)
		{
			const float rad = 3.14159265f / 4.0f * dir;
			const float cs = std::cos(rad), sn = std::sin(rad);
			const int bars = PingBars(ms);
			for (int i = 0; i < 5; i++)
			{
				const float hx = 2.5f * (1.0f + i * 0.5f) * scale, hy = 1.75f * scale;
				const float cy = (i * 5.3f + 9.5f) * scale;
				const float xs[4] = {-hx, hx, hx, -hx}, ys[4] = {cy - hy, cy - hy, cy + hy, cy + hy};
				ImVec2 pts[4];
				for (int k = 0; k < 4; k++)
					pts[k] = ImVec2(anchor.x + xs[k] * cs - ys[k] * sn, anchor.y + ys[k] * cs + xs[k] * sn);
				ImU32 c = PingColor(i < bars ? ms : 0, alpha);
				if (i < bars && i == elapsed / 100 % 5)
					c = ImColor(1.0f, 1.0f, 1.0f, alpha);
				dl->AddConvexPolyFilled(pts, 4, c);
				dl->AddPolyline(pts, 4, IM_COL32(0, 0, 0, 128), scale, ImDrawFlags_Closed);
			}
		}

		void DrawPingDiagram()
		{
			int elapsed = 0, duration = 0, n = 0;
			uint8_t rtt[4][4];
			if (FullscreenUI::HasActiveWindow() || !Zdxsv::PingTestView(elapsed, duration, n, rtt))
				return;
			const int start = 1550, end = duration - 600;
			if (elapsed <= start || elapsed >= end || n < 2 || n > 4)
				return;
			const float alpha = std::clamp(std::min((elapsed - start) / 250.0f, (end - elapsed) / 300.0f), 0.0f, 1.0f);

			const float w = ImGuiManager::GetWindowWidth(), h = ImGuiManager::GetWindowHeight();
			const float scale = std::min(w / 640.0f, h / 480.0f);
			const auto at = [&](float x, float y) { return ImVec2(w / 2.0f + x * scale, h / 2.0f + y * scale); };
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const ImU32 shade = ImColor(0, 0, 0, static_cast<int>(128 * alpha));
			dl->AddRectFilled(at(-45, -97), at(45.25f, -51.875f), shade);
			dl->AddRectFilled(at(-45, 53.125f), at(45.25f, 98.25f), shade);

			// corners: 0 top left, 1 top right, 2 bottom left, 3 bottom right
			const ImVec2 corner[4] = {at(-36, -89), at(36, -89), at(-36, 89), at(36, 89)};
			static constexpr int dirs[4][4] = {{0, 6, 0, 7}, {2, 0, 1, 0}, {4, 5, 0, 6}, {3, 4, 2, 0}};
			const int top = (n + 1) / 2;
			int pos[4];
			for (int p = 0; p < n; p++)
				pos[p] = p < top ? p : 2 + p - top;
			for (int i = 0; i < n; i++)
			{
				int worst = 0;
				for (int j = 0; j < n; j++)
				{
					if (i == j)
						continue;
					DrawPingBars(dl, corner[pos[i]], rtt[i][j], alpha, scale, dirs[pos[i]][pos[j]], elapsed);
					worst = std::max(worst, static_cast<int>(rtt[i][j]));
				}
				dl->AddCircleFilled(corner[pos[i]], 6.5f * scale, shade, 20);
				dl->AddCircleFilled(corner[pos[i]], 5.5f * scale, PingColor(worst, alpha), 20);
			}
		}
	} // namespace

	void DrawOverlays(float scale, float margin, float spacing)
	{
		DrawPingDiagram();
		DrawGgpoOverlay(scale, margin, spacing);
		DrawReplayKeys(scale, margin);
		DrawReplayBar(scale, margin);
		DrawTakeover(scale);
	}
} // namespace Zdxsv
