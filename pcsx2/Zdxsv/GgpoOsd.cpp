// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Ggpo.h"
#include "Zdxsv/UiHooks.h"

#include "Config.h"
#include "common/Console.h"
#include "IconsFontAwesome.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <deque>
#include <string>

namespace Zdxsv
{
	namespace
	{
		enum class GgpoOsdHealth
		{
			Excellent,
			Good,
			Medium,
			Fair,
			Poor,
		};

		struct GgpoOsdLossEvent
		{
			double time;
			int count;
		};

		struct GgpoOsdHealthTracker
		{
			bool loss_initialized = false;
			int previous_loss_from = 0;
			int previous_loss_to = 0;
			std::deque<GgpoOsdLossEvent> loss_from_events;
			std::deque<GgpoOsdLossEvent> loss_to_events;
			int recent_loss_from = 0;
			int recent_loss_to = 0;

			bool sample_initialized = false;
			double last_sample_time = 0.0;
			std::array<int, 10> ping_samples = {};
			int ping_sample_count = 0;
			int ping_sample_index = 0;
			int ping_p20 = 0;
			int ping_median = 0;
			int ping_jitter = 0;

			GgpoOsdHealth displayed = GgpoOsdHealth::Excellent;
			GgpoOsdHealth pending = GgpoOsdHealth::Excellent;
			double pending_since = 0.0;
			double next_recovery_at = 0.0;
		};

		struct GgpoOsdLossFlash
		{
			bool initialized = false;
			int previous_from = 0, previous_to = 0;
			double from_until = 0.0, to_until = 0.0;
			int from_grade = 0, to_grade = 0;
		};

		struct GgpoOsdState
		{
			std::array<GgpoOsdHealthTracker, 4> trackers;
			std::array<std::string, 4> tracked_ids;
			std::array<GgpoOsdLossFlash, 4> flashes;
			std::array<int, 4> logged_health = {-1, -1, -1, -1};
			bool bounds_valid = false;
			ImVec2 bounds_min, bounds_max;
		};
		GgpoOsdState s_osd;

		constexpr ImVec4 TEXT(1.f, 1.f, 1.f, 1.f);
		constexpr ImVec4 TEXT_DISABLED(0.5f, 0.5f, 0.5f, 1.f);
		constexpr ImVec4 SEPARATOR(0.43f, 0.43f, 0.5f, 0.5f);

		ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

		ImVec4 PingColor(int ms)
		{
			const auto rgb = [](int r, int g, int b) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.f); };
			if (ms <= 0) return rgb(64, 64, 64);
			if (ms <= 30) return rgb(87, 213, 213);
			if (ms <= 60) return rgb(0, 255, 149);
			if (ms <= 90) return rgb(255, 255, 0);
			if (ms <= 120) return rgb(255, 170, 0);
			return rgb(255, 0, 0);
		}

		GgpoOsdHealth Worse(GgpoOsdHealth a, GgpoOsdHealth b) { return static_cast<int>(a) >= static_cast<int>(b) ? a : b; }
		GgpoOsdHealth Better(GgpoOsdHealth a, GgpoOsdHealth b) { return static_cast<int>(a) <= static_cast<int>(b) ? a : b; }

		GgpoOsdHealth Grade(int v, int excellent, int good, int medium, int fair)
		{
			if (v <= excellent) return GgpoOsdHealth::Excellent;
			if (v <= good) return GgpoOsdHealth::Good;
			if (v <= medium) return GgpoOsdHealth::Medium;
			if (v <= fair) return GgpoOsdHealth::Fair;
			return GgpoOsdHealth::Poor;
		}

		GgpoOsdHealth LossHealth(int loss) { return Grade(loss, 0, 2, 4, 7); }
		GgpoOsdHealth QueueHealth(int excess) { return Grade(excess, 1, 2, 4, 7); }
		// a stable RTT adds input delay but never looks as bad as a lossy or stalling link
		GgpoOsdHealth StableLatencyHealth(int ping) { return ping <= 60 ? GgpoOsdHealth::Excellent : ping <= 180 ? GgpoOsdHealth::Good : GgpoOsdHealth::Medium; }
		GgpoOsdHealth JitterHealth(int jitter) { return Grade(jitter, 5, 10, 15, 20); }
		GgpoOsdHealth SpikeHealth(int spike) { return Grade(spike, 10, 15, 20, 35); }
		GgpoOsdHealth PacingHealth(int frames_behind) { return Grade(std::abs(frames_behind), 2, 4, 6, 8); }

		GgpoOsdHealth UpdateHealth(GgpoOsdHealthTracker& t, const GgpoOsdPeer& peer)
		{
			const double now = ImGui::GetTime();
			const int loss_from = peer.recv_loss;
			const int loss_to = peer.send_loss;
			if (!t.loss_initialized || loss_from < t.previous_loss_from || loss_to < t.previous_loss_to)
			{
				t = {};
				t.loss_initialized = true;
			}
			else
			{
				if (loss_from > t.previous_loss_from)
				{
					t.loss_from_events.push_back({now, loss_from - t.previous_loss_from});
					t.recent_loss_from += loss_from - t.previous_loss_from;
				}
				if (loss_to > t.previous_loss_to)
				{
					t.loss_to_events.push_back({now, loss_to - t.previous_loss_to});
					t.recent_loss_to += loss_to - t.previous_loss_to;
				}
			}
			t.previous_loss_from = loss_from;
			t.previous_loss_to = loss_to;
			const auto expire = [now](std::deque<GgpoOsdLossEvent>& events, int& recent) {
				while (!events.empty() && now - events.front().time > 2.0)
				{
					recent -= events.front().count;
					events.pop_front();
				}
			};
			expire(t.loss_from_events, t.recent_loss_from);
			expire(t.loss_to_events, t.recent_loss_to);
			const int recent_loss = std::max(t.recent_loss_from, t.recent_loss_to);

			if (!t.sample_initialized || now - t.last_sample_time >= 1.0)
			{
				t.sample_initialized = true;
				t.last_sample_time = now;
				if (peer.ping > 0)
				{
					t.ping_samples[t.ping_sample_index] = peer.ping;
					t.ping_sample_index = (t.ping_sample_index + 1) % static_cast<int>(t.ping_samples.size());
					t.ping_sample_count = std::min(t.ping_sample_count + 1, static_cast<int>(t.ping_samples.size()));
					if (t.ping_sample_count >= 4)
					{
						std::array<int, 10> s = t.ping_samples;
						std::sort(s.begin(), s.begin() + t.ping_sample_count);
						const int n = t.ping_sample_count - 1;
						t.ping_p20 = s[n / 5];
						t.ping_median = s[n / 2];
						t.ping_jitter = s[n * 9 / 10] - s[n / 10];
					}
				}
			}

			const int spike = t.ping_sample_count >= 4 ? std::max(peer.ping - t.ping_p20, 0) : 0;
			const int stable_ping = t.ping_sample_count >= 4 ? t.ping_median : peer.ping;
			const GgpoOsdHealth latency = Worse(Worse(StableLatencyHealth(stable_ping), JitterHealth(t.ping_jitter)), SpikeHealth(spike));
			constexpr double frame_ms = 1000.0 / 59.94;
			// Inputs pending on a healthy link: one per frame of the round trip, plus the input added this frame, the one
			// the peer acked last (removed only by a later ack) and the frame the peer waits to send its ack with its input.
			constexpr int queue_overhead = 3;
			const int expected_queue = static_cast<int>(std::ceil(std::max(peer.ping, 0) / frame_ms)) + queue_overhead;
			const GgpoOsdHealth loss = LossHealth(recent_loss);
			const GgpoOsdHealth queue = QueueHealth(std::max(peer.send_queue - expected_queue, 0));
			const GgpoOsdHealth pacing = PacingHealth(peer.frames_behind);
			const GgpoOsdHealth observed = Worse(Worse(loss, queue), Worse(latency, pacing));

			t.displayed = Worse(t.displayed, loss);
			if (static_cast<int>(observed) > static_cast<int>(t.displayed))
			{
				if (t.pending_since == 0.0)
				{
					t.pending = observed;
					t.pending_since = now;
				}
				else
					t.pending = Better(t.pending, observed);
				const bool poor_only_from_pacing = pacing == GgpoOsdHealth::Poor && loss != GgpoOsdHealth::Poor &&
				                                   queue != GgpoOsdHealth::Poor && latency != GgpoOsdHealth::Poor;
				const double delay = t.pending == GgpoOsdHealth::Poor && poor_only_from_pacing ? 2.0 : 1.0;
				if (now - t.pending_since >= delay)
				{
					t.displayed = t.pending;
					t.pending_since = 0.0;
				}
				t.next_recovery_at = 0.0;
			}
			else if (static_cast<int>(observed) < static_cast<int>(t.displayed))
			{
				t.pending_since = 0.0;
				if (t.next_recovery_at == 0.0)
					t.next_recovery_at = now + 3.0;
				else if (now >= t.next_recovery_at)
				{
					t.displayed = static_cast<GgpoOsdHealth>(static_cast<int>(t.displayed) - 1);
					t.next_recovery_at = now + 2.0;
				}
			}
			else
				t.next_recovery_at = 0.0;
			return t.displayed;
		}

		ImVec4 HealthColor(GgpoOsdHealth h)
		{
			if (h == GgpoOsdHealth::Medium) return ImVec4(1.f, 0.85f, 0.f, 1.f);
			if (h == GgpoOsdHealth::Fair) return ImVec4(1.f, 0.5f, 0.f, 1.f);
			if (h == GgpoOsdHealth::Poor) return ImVec4(1.f, 0.15f, 0.12f, 1.f);
			return ImVec4(1.f, 1.f, 1.f, 0.75f);
		}

		void UpdateLossFlash(GgpoOsdLossFlash& f, const GgpoOsdPeer& peer, double now)
		{
			if (!f.initialized || peer.recv_loss < f.previous_from || peer.send_loss < f.previous_to)
			{
				f = {};
				f.initialized = true;
				f.previous_from = peer.recv_loss;
				f.previous_to = peer.send_loss;
				return;
			}
			const auto update = [now](int loss, int& previous, double& until, int& grade) {
				if (loss > previous)
				{
					grade = now < until ? std::min(grade + loss - previous, 5) : std::min(loss - previous, 5);
					previous = loss;
					until = now + 0.3;
				}
				else if (now >= until)
					grade = 0;
			};
			update(peer.recv_loss, f.previous_from, f.from_until, f.from_grade);
			update(peer.send_loss, f.previous_to, f.to_until, f.to_grade);
		}

		// One panel draw: a pen over the draw list, rows advance y by the line height and the item spacing.
		struct GgpoOsdPen
		{
			ImDrawList* dl;
			ImFont* regular;
			float base; // unscaled font size of the name and heading rows
			float fs; // the panel's font size (= line height)
			float sx, sy;
			float x, w; // content left, width
			float y;

			float TextW(ImFont* f, float size, const char* s) const { return f->CalcTextSizeA(size, FLT_MAX, 0.f, s).x; }
			float TextW(const char* s) const { return TextW(regular, fs, s); }
			void Text(ImFont* f, float size, float tx, float ty, const ImVec4& c, const char* s) const { dl->AddText(f, size, ImVec2(tx, ty), Col(c), s); }
			void Text(float tx, const ImVec4& c, const char* s) const { Text(regular, fs, tx, y, c, s); }
			void Right(const ImVec4& c, const char* s) const { Text(x + w - TextW(s), c, s); }
			void Centered(const ImVec4& c, const char* s) const { Text(x + (w - TextW(s)) * 0.5f, c, s); }
			void Next() { y += fs + sy; }

			void Meter(float mx, float width, int segments, int filled, const ImVec4& fill) const
			{
				width = std::max(width, fs * 2.f);
				const float gap = std::max(1.f, fs * 0.1f);
				const float seg_w = (width - gap * (segments - 1)) / segments;
				const float seg_h = std::max(3.f, fs * 0.5f);
				const float my = y + (fs - seg_h) * 0.5f + 1.f;
				ImVec4 empty = TEXT_DISABLED;
				empty.w *= 0.4f;
				for (int i = 0; i < segments; i++)
				{
					const float l = mx + i * (seg_w + gap);
					dl->AddRectFilled(ImVec2(l, my), ImVec2(l + seg_w, my + seg_h), Col(i < filled ? fill : empty), seg_h * 0.2f);
				}
			}

			void LabelMeter(const char* label, int segments, int filled, const ImVec4& fill)
			{
				Text(x, TEXT, label);
				const float mx = x + TextW(label) + sx;
				Meter(mx, x + w - mx, segments, filled, fill);
				Next();
			}

			void PacingMeter(float px, float width, int frames_behind) const
			{
				const float track_l = px + fs * 0.2f, track_r = px + width - fs * 0.2f;
				const float center = (track_l + track_r) * 0.5f, my = y + fs * 0.5f;
				const int offset = std::clamp(frames_behind, -8, 8);
				const float marker = center - (track_r - track_l) * offset / 16.f;
				dl->AddLine(ImVec2(track_l, my), ImVec2(track_r, my), Col(TEXT_DISABLED), std::max(1.f, fs * 0.075f));
				dl->AddLine(ImVec2(center, my), ImVec2(marker, my), Col(HealthColor(PacingHealth(frames_behind))), std::max(1.f, fs * 0.85f));
				dl->AddLine(ImVec2(center, my - fs * 0.45f), ImVec2(center, my + fs * 0.45f), Col(TEXT), std::max(1.5f, fs * 0.14f));
				const char* slow = offset >= 5 ? "You" : offset <= -5 ? "Peer" : nullptr;
				if (!slow)
					return;
				const float half_space = TextW("  ") * 0.5f;
				const float subject_x = center - half_space - TextW(slow), slow_x = center + half_space;
				const ImVec4 shadow(0.2f, 0.2f, 0.2f, 0.7f);
				Text(regular, fs, subject_x + 1.f, y + 1.f, shadow, slow);
				Text(regular, fs, slow_x + 1.f, y + 1.f, shadow, "Slow");
				Text(regular, fs, subject_x, y, TEXT, slow);
				Text(regular, fs, slow_x, y, TEXT, "Slow");
			}

			// A name row, centered, shrunk to the width; its pitch stays one text row.
			void Name(const std::string& s)
			{
				ImFont* f = regular;
				const float row_h = base * 1.1f;
				float size = row_h;
				const float tw = TextW(f, size, s.c_str());
				if (tw > w && w > 0.f)
					size *= w / tw;
				Text(f, size, x + (w - TextW(f, size, s.c_str())) * 0.5f, y + (row_h - size) * 0.5f, TEXT, s.c_str());
				y += std::max(row_h, fs + sy);
			}

			void Heading(const GgpoOsdPeer& p, bool details)
			{
				const float h = 1.f + sy * 2.f;
				float size = base * 0.85f;
				const std::string label = std::to_string(p.position + 1) + "P  " + p.id;
				const float gap = size * 0.5f;
				float line_r = x + w;
				if (details)
				{
					const std::string ping = p.connected ? std::to_string(p.ping) + (p.relay ? "(R)" : "") : "--";
					const float ping_x = x + w - TextW(regular, size, ping.c_str());
					Text(regular, size, ping_x, y + (h - size) * 0.5f, PingColor(p.connected ? p.ping : 999), ping.c_str());
					line_r = ping_x - gap;
				}
				const float label_room = std::max(1.f, line_r - x - gap * 2.f);
				float label_w = TextW(regular, size, label.c_str());
				if (label_w > label_room)
				{
					size *= label_room / label_w;
					label_w = TextW(regular, size, label.c_str());
				}
				const float label_x = x + (line_r - x - label_w) * 0.5f;
				const float line_y = y + h * 0.5f;
				const float thick = std::max(1.f, std::floor(ImGuiManager::GetGlobalScale()));
				if (x < label_x - gap)
					dl->AddLine(ImVec2(x, line_y), ImVec2(label_x - gap, line_y), Col(SEPARATOR), thick);
				if (label_x + label_w + gap < line_r)
					dl->AddLine(ImVec2(label_x + label_w + gap, line_y), ImVec2(line_r, line_y), Col(SEPARATOR), thick);
				Text(regular, size, label_x, y + (h - size) * 0.5f, ImVec4(1.f, 1.f, 1.f, 0.65f), label.c_str());
				y += h + sy;
			}

			void Diagnostics(const GgpoOsdPeer& p, const GgpoOsdLossFlash& f, double now)
			{
				if (!p.connected)
				{
					Next();
					Centered(PingColor(999), "Disconnected");
					Next();
					return;
				}
				const auto flash = [](int grade) {
					const float blend = (std::clamp(grade, 1, 5) - 1.f) / 4.f;
					return ImVec4(1.f, 0.65f * (1.f - blend), 0.15f * (1.f - blend), 1.f);
				};
				const ImVec4 orange(1.f, 0.55f, 0.1f, 1.f), red(1.f, 0.2f, 0.2f, 1.f);
				const auto mix = [](const ImVec4& a, const ImVec4& b, float t) {
					return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.f);
				};
				const int q = p.send_queue;
				const ImVec4 queue_color = q >= 8 ? mix(orange, red, std::min((q - 7.f) / 3.f, 1.f)) : q >= 5 ? mix(TEXT, orange, (q - 4.f) / 3.f) : TEXT;
				float tx = x;
				const auto put = [&](const std::string& s, const ImVec4& c) {
					Text(tx, c, s.c_str());
					tx += TextW(s.c_str());
				};
				put("L: ", TEXT);
				put("\xE2\x86\x93 " + std::to_string(p.recv_loss), now < f.from_until ? flash(f.from_grade) : TEXT);
				put(" ", TEXT);
				put("\xE2\x86\x91 " + std::to_string(p.send_loss), now < f.to_until ? flash(f.to_grade) : TEXT);
				Text(x + w - TextW("Q:99"), TEXT, "Q:");
				Right(queue_color, std::to_string(q).c_str());
				Next();

				ImFont* icons = ImGuiManager::GetStandardFont();
				const float icon_size = fs * 0.8f;
				const float icon_w = TextW(icons, icon_size, ICON_FA_CLOCK);
				Text(icons, icon_size, x, y + (fs - icon_size) * 0.5f, TEXT, ICON_FA_CLOCK);
				const float meter_x = x + icon_w + sx + TextW(" ") * 0.5f;
				PacingMeter(meter_x, x + w - meter_x, p.frames_behind);
				Next();
			}
		};
	} // namespace

	// The network panel during a GGPO battle: left edge, between the game's top and bottom HUD.
	// Hovering the mouse over it swaps the ping and lag rows for loss, send queue and frame pacing.
	void GgpoOsdDraw()
	{
		GgpoOsdState& s = s_osd;
		GgpoOsdPanel panel;
		if (!g_ggpo_enabled || FullscreenUI::HasActiveWindow() || !GgpoOsd(panel))
		{
			s.bounds_valid = false;
			return;
		}
		const double now = ImGui::GetTime();
		std::array<GgpoOsdHealth, 4> health{};
		for (const GgpoOsdPeer& p : panel.peers)
		{
			if (p.position < 0 || p.position >= 4)
				continue;
			if (s.tracked_ids[p.position] != p.id)
			{
				s.trackers[p.position] = {};
				s.flashes[p.position] = {};
				s.tracked_ids[p.position] = p.id;
			}
			if (p.connected)
			{
				health[p.position] = UpdateHealth(s.trackers[p.position], p);
				if (static_cast<int>(health[p.position]) != s.logged_health[p.position])
				{
					s.logged_health[p.position] = static_cast<int>(health[p.position]);
					Console.WriteLn("ZdxsvGgpo: osd lag %dP %d", p.position + 1, s.logged_health[p.position]);
				}
				UpdateLossFlash(s.flashes[p.position], p, now);
			}
			else
				s.trackers[p.position] = {};
		}

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool details = s.bounds_valid && ImGui::IsMousePosValid(&mouse) && mouse.x >= s.bounds_min.x && mouse.x <= s.bounds_max.x &&
		                     mouse.y >= s.bounds_min.y && mouse.y <= s.bounds_max.y;

		const float display_h = ImGuiManager::GetWindowHeight();
		const float hud = display_h / 960.f;
		const float scale = ImGuiManager::GetGlobalScale();
		const float top = (192.f + 21.f) * hud;
		const float max_h = display_h - (205.f + 21.f) * hud - top;
		const float left = 30.f * hud;
		const float title_h = 20.f * hud, title_gap = 4.f * hud;
		const float base = 17.f * scale, pad0 = 8.f * scale;
		const int peers = static_cast<int>(panel.peers.size());
		const float natural_sy = std::max(4.f * scale, base * 0.7f);
		const float scalable = pad0 * 2.f + (3 + peers * 4) * (base + natural_sy) + peers * (1.f + natural_sy * 3.f);
		const float ps = std::clamp((max_h - title_h - title_gap) / scalable, 0.1f, 1.f);
		const float width = 160.f * scale * ps;
		const float pad = pad0 * ps;

		ImFont* const regular = ImGuiManager::GetStandardFont();
		GgpoOsdPen pen{ImGui::GetBackgroundDrawList(), regular, base, base * ps, 8.f * scale * ps, natural_sy * ps, left + pad, width - pad * 2.f,
			top + title_h + title_gap + pad};

		pen.dl->ChannelsSplit(2);
		pen.dl->ChannelsSetCurrent(1);
		static constexpr const char* title = "NETWORK STATS";
		pen.Text(regular, pen.fs, left + (width - pen.TextW(regular, pen.fs, title)) * 0.5f, top + std::max(0.f, (title_h - pen.fs) * 0.5f) + 1.f,
			ImVec4(0.925f, 0.941f, 1.f, 0.26f), title);

		pen.Text(pen.x, TEXT, "Delay");
		const ImVec4 delay_color = panel.delay >= 13 ? ImVec4(1.f, 0.15f, 0.12f, 1.f) : panel.delay >= 10 ? ImVec4(1.f, 0.5f, 0.f, 1.f) :
		                           panel.delay >= 5                 ? ImVec4(1.f, 0.85f, 0.f, 1.f) : TEXT;
		pen.Right(delay_color, (std::to_string(panel.delay) + "fr").c_str());
		pen.Next();
		pen.Text(pen.x, TEXT, ("Roll: " + std::to_string(panel.rollback_frames)).c_str());
		pen.Right(TEXT, ("Wait: " + std::to_string(panel.sync_frames)).c_str());
		pen.Next();
		const int predicted = std::clamp(panel.predicted_frames, 0, 6);
		const ImVec4 predicted_color = predicted == 4 ? ImVec4(1.f, 0.85f, 0.f, 1.f) : predicted == 5 ? ImVec4(1.f, 0.5f, 0.f, 1.f) :
		                               predicted == 6 ? ImVec4(1.f, 0.15f, 0.12f, 1.f) : ImVec4(1.f, 1.f, 1.f, 0.75f);
		pen.LabelMeter("P:", 6, predicted, predicted_color);

		for (const GgpoOsdPeer& p : panel.peers)
		{
			pen.Heading(p, details);
			pen.Name(p.name);
			pen.Name(p.pilot);
			if (details)
			{
				pen.Diagnostics(p, s.flashes[std::clamp(p.position, 0, 3)], now);
				continue;
			}
			pen.Text(pen.x, TEXT, "Ping");
			if (p.connected && p.relay)
			{
				const float size = base * 0.85f;
				pen.Text(regular, size, pen.x + pen.TextW("Ping") + pen.sx, pen.y + (pen.fs - size) * 0.5f, TEXT, "(Relay)");
			}
			pen.Right(PingColor(p.connected ? p.ping : 999), p.connected ? std::to_string(p.ping).c_str() : "--");
			pen.Next();
			if (p.connected)
			{
				const GgpoOsdHealth h = health[std::clamp(p.position, 0, 3)];
				pen.LabelMeter("Lag", 5, h == GgpoOsdHealth::Poor ? 5 : static_cast<int>(h), HealthColor(h));
			}
			else
			{
				pen.Centered(PingColor(999), "Disconnected");
				pen.Next();
			}
		}

		const float bottom = pen.y + pad;
		pen.dl->ChannelsSetCurrent(0);
		pen.dl->AddRectFilled(ImVec2(left, top), ImVec2(left + width, bottom), Col(ImVec4(0.122f, 0.122f, 0.122f, 0.055f)));
		pen.dl->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + title_h), Col(ImVec4(0.122f, 0.122f, 0.122f, 0.16f)));
		pen.dl->ChannelsMerge();
		s.bounds_min = ImVec2(left, top);
		s.bounds_max = ImVec2(left + width, bottom);
		s.bounds_valid = true;
	}
} // namespace Zdxsv
