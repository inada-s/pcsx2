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

		constexpr ImVec4 TEXT(0.92f, 0.92f, 0.92f, 1.f);
		constexpr ImVec4 TEXT_DIM(0.92f, 0.92f, 0.92f, 0.8f);
		constexpr ImVec4 TEXT_DISABLED(0.5f, 0.5f, 0.5f, 1.f);

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

		// GGPO resends unacked inputs, so loss alone is minor; loss that hurts also grows the send queue
		GgpoOsdHealth LossHealth(int loss) { return loss <= 0 ? GgpoOsdHealth::Excellent : loss <= 2 ? GgpoOsdHealth::Good : GgpoOsdHealth::Medium; }
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

			if (static_cast<int>(observed) > static_cast<int>(t.displayed))
			{
				if (t.pending_since == 0.0)
				{
					t.pending = observed;
					t.pending_since = now;
				}
				else
					t.pending = Better(t.pending, observed);
				const bool poor_only_from_pacing = pacing == GgpoOsdHealth::Poor && queue != GgpoOsdHealth::Poor && latency != GgpoOsdHealth::Poor;
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

		// The columns of the mouse-over view; the ping ends at the panel's right edge.
		struct GgpoOsdColumns
		{
			float id_x, name_x, pilot_x, width;
		};

		// One panel draw: a pen over the draw list, rows advance y by the line height and the item spacing.
		struct GgpoOsdPen
		{
			ImDrawList* dl;
			ImFont* regular;
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

			void Stat(float tx, const char* label, int value, const ImVec4& c) const
			{
				Text(tx, TEXT, label);
				Text(tx + TextW(label), c, std::to_string(value).c_str());
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

			// The standard font takes U+FF21-FF3A (fullwidth A-Z) from the icon font, which draws them as keycaps:
			// each is drawn as its ASCII letter in a fullwidth cell. Returns the width; draws only with a color.
			float NameText(ImFont* f, float size, float tx, float ty, const ImVec4* c, const std::string& s) const
			{
				const float cell = TextW(f, size, "\xEF\xBD\x8D"); // U+FF4D, fullwidth m
				float pos = 0.f;
				size_t from = 0;
				const auto run = [&](size_t to) {
					if (to <= from)
						return;
					const std::string part = s.substr(from, to - from);
					if (c)
						Text(f, size, tx + pos, ty, *c, part.c_str());
					pos += TextW(f, size, part.c_str());
				};
				for (size_t i = 0; i + 2 < s.size(); i++)
				{
					const u8 b2 = static_cast<u8>(s[i + 2]);
					if (static_cast<u8>(s[i]) != 0xEF || static_cast<u8>(s[i + 1]) != 0xBC || b2 < 0xA1 || b2 > 0xBA)
						continue;
					run(i);
					const char letter[2] = {static_cast<char>('A' + (b2 - 0xA1)), 0};
					if (c)
						Text(f, size, tx + pos + (cell - TextW(f, size, letter)) * 0.5f, ty, *c, letter);
					pos += cell;
					i += 2;
					from = i + 1;
				}
				run(s.size());
				return pos;
			}

			// Phone-style bars ending at r: color = ping, lit bars = the lag health. Returns the left edge.
			float SignalBars(float r, float row_y, float row_h, float size, const GgpoOsdPeer& p, GgpoOsdHealth h) const
			{
				constexpr int bars = 4;
				const float gap = BarGap(size);
				const float bar_w = BarWidth(size);
				const float l = r - BarsWidth(size);
				const float bottom = row_y + (row_h + size) * 0.5f;
				const int lit = !p.connected ? 0 : h == GgpoOsdHealth::Poor ? 1 : h == GgpoOsdHealth::Fair ? 2 : h == GgpoOsdHealth::Medium ? 3 : 4;
				ImVec4 on = PingColor(p.connected ? std::max(p.ping, 1) : 999);
				on.x *= 0.75f;
				on.y *= 0.75f;
				on.z *= 0.75f;
				ImVec4 off = TEXT_DISABLED;
				off.w *= 0.5f;
				for (int i = 0; i < bars; i++)
				{
					const float bl = l + i * (bar_w + gap);
					const float top = bottom - size * 1.1f * (i + 1) / bars;
					dl->AddRectFilled(ImVec2(bl, top), ImVec2(bl + bar_w, bottom), Col(i < lit ? on : off), bar_w * 0.25f);
				}
				return l;
			}

			static float BarGap(float size) { return std::max(1.f, size * 0.12f); }
			static float BarWidth(float size) { return std::max(2.f, size * 0.26f); }
			static float BarsWidth(float size) { return 4.f * BarWidth(size) + 3.f * BarGap(size); }
			static std::string PositionLabel(const GgpoOsdPeer& p) { return std::to_string(p.position + 1) + "P"; }
			static std::string PingLabel(const GgpoOsdPeer& p) { return p.connected ? std::to_string(p.ping) + (p.relay ? "(R)" : "") : "--"; }

			float SmallSize() const { return fs * 0.85f; }
			float RelayGap() const { return SmallSize() * 0.25f; }

			float TailWidth(const GgpoOsdPeer& p, bool details) const
			{
				const float size = SmallSize();
				if (details)
					return TextW(regular, size, PingLabel(p).c_str());
				const float relay = p.connected && p.relay ? TextW(regular, size * 0.8f, "R") + RelayGap() : 0.f;
				return BarsWidth(size) + relay;
			}

			float Gap() const { return fs * 0.6f; }

			float StatsWidth() const { return TextW("D: 99") + TextW("P: 9") + TextW("R: 9999") + TextW("W: 9999") + Gap() * 3.f; }

			float SegmentWidth(const GgpoOsdPeer& p) const
			{
				const float small = SmallSize();
				return TextW(regular, small, PositionLabel(p).c_str()) + TextW(regular, small, p.id.c_str()) +
				       NameText(regular, fs, 0.f, 0.f, nullptr, p.name) + NameText(regular, small, 0.f, 0.f, nullptr, p.pilot) +
				       TailWidth(p, false) + Gap() * 4.f;
			}

			float InlineWidth(const std::vector<GgpoOsdPeer>& peers) const
			{
				float total = StatsWidth();
				for (const GgpoOsdPeer& p : peers)
					total += SegmentWidth(p) + Gap() * 2.f;
				return total;
			}

			// An opponent on the single row, after a separator; returns the segment's right edge.
			float Segment(float left_x, const GgpoOsdPeer& p, GgpoOsdHealth health) const
			{
				const float gap = Gap(), small = SmallSize();
				const float small_y = y + (fs - small) * 0.5f;
				const float line_x = left_x + gap;
				dl->AddLine(ImVec2(line_x, y + fs * 0.15f), ImVec2(line_x, y + fs * 0.85f), Col(TEXT_DISABLED), 1.f);
				float tx = left_x + gap * 2.f;
				const auto small_text = [&](const std::string& s) {
					Text(regular, small, tx, small_y, TEXT_DIM, s.c_str());
					tx += TextW(regular, small, s.c_str()) + gap;
				};
				small_text(PositionLabel(p));
				small_text(p.id);
				const ImVec4 name_color = p.connected ? TEXT : PingColor(999);
				tx += NameText(regular, fs, tx, y, &name_color, p.name) + gap;
				tx += NameText(regular, small, tx, small_y, &TEXT_DIM, p.pilot) + gap;
				const float right = tx + TailWidth(p, false);
				const float bars_l = SignalBars(right, y, fs, small, p, health);
				if (p.connected && p.relay)
				{
					const float relay_size = small * 0.8f;
					Text(regular, relay_size, bars_l - RelayGap() - TextW(regular, relay_size, "R"), y + (fs - relay_size) * 0.5f, TEXT_DIM, "R");
				}
				return right;
			}

			GgpoOsdColumns Measure(const std::vector<GgpoOsdPeer>& peers) const
			{
				const float small = SmallSize(), gap = Gap();
				float position_w = 0.f, id_w = 0.f, name_w = 0.f, pilot_w = 0.f, tail_w = 0.f;
				for (const GgpoOsdPeer& p : peers)
				{
					position_w = std::max(position_w, TextW(regular, small, PositionLabel(p).c_str()));
					id_w = std::max(id_w, TextW(regular, small, p.id.c_str()));
					name_w = std::max(name_w, NameText(regular, fs, 0.f, 0.f, nullptr, p.name));
					pilot_w = std::max(pilot_w, NameText(regular, small, 0.f, 0.f, nullptr, p.pilot));
					tail_w = std::max(tail_w, TailWidth(p, true));
				}
				GgpoOsdColumns c;
				c.id_x = x + position_w + gap;
				c.name_x = c.id_x + id_w + gap;
				c.pilot_x = c.name_x + name_w + gap;
				c.width = std::max(c.pilot_x + pilot_w + gap + tail_w - x, (TextW("D: 99") + gap) * 4.f);
				return c;
			}

			// A row of the mouse-over view: position, user id, name, pilot name, ping.
			void Player(const GgpoOsdPeer& p, const GgpoOsdColumns& c)
			{
				const float small = SmallSize();
				const float small_y = y + (fs - small) * 0.5f;
				const ImVec4 name_color = p.connected ? TEXT : PingColor(999);
				Text(regular, small, x, small_y, TEXT_DIM, PositionLabel(p).c_str());
				Text(regular, small, c.id_x, small_y, TEXT_DIM, p.id.c_str());
				NameText(regular, fs, c.name_x, y, &name_color, p.name);
				NameText(regular, small, c.pilot_x, small_y, &TEXT_DIM, p.pilot);
				const std::string ping = PingLabel(p);
				Text(regular, small, x + w - TextW(regular, small, ping.c_str()), small_y, PingColor(p.connected ? p.ping : 999), ping.c_str());
				Next();
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

	// The network panel during a GGPO battle: bottom left, the frame counts and every opponent on one row.
	// Hovering the mouse over it stacks the opponents with the ping, loss, send queue and frame pacing rows.
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
		const float unit = std::max(hud, 6.f / 10.f); // follows the window height, down to a 6 px font
		const float left = 3.f * hud;
		const float bottom = display_h - 3.f * hud;
		const float base = 10.f * unit, pad0 = 3.f * unit, row_gap0 = 1.f * unit;
		const int rows = details ? 1 + static_cast<int>(panel.peers.size()) * 3 : 1;
		const float natural_h = pad0 * 2.f + rows * base + (rows - 1) * row_gap0;

		ImFont* const regular = ImGuiManager::GetStandardFont();
		const auto make_pen = [&](float ps) {
			return GgpoOsdPen{ImGui::GetBackgroundDrawList(), regular, base * ps, 6.f * unit * ps, row_gap0 * ps, left + pad0 * ps, 0.f, 0.f};
		};
		// The mouse-over view is at least as wide as the row, so the mouse stays over the panel when it opens.
		const auto content_width = [&](const GgpoOsdPen& p) {
			return std::max(p.InlineWidth(panel.peers), details ? p.Measure(panel.peers).width : 0.f);
		};
		const float natural_w = content_width(make_pen(1.f)) + pad0 * 2.f;
		const float ps = std::clamp(std::min((ImGuiManager::GetWindowWidth() * 0.9f) / natural_w, (display_h * 0.5f) / natural_h), 0.1f, 1.f);
		const float pad = pad0 * ps;

		GgpoOsdPen pen = make_pen(ps);
		const GgpoOsdColumns columns = pen.Measure(panel.peers);
		pen.w = content_width(pen);
		const float width = pen.w + pad * 2.f;
		const float top = bottom - natural_h * ps;
		pen.y = top + pad;

		pen.dl->ChannelsSplit(2);
		pen.dl->ChannelsSetCurrent(1);

		const ImVec4 delay_color = panel.delay >= 13 ? ImVec4(1.f, 0.15f, 0.12f, 1.f) : panel.delay >= 10 ? ImVec4(1.f, 0.5f, 0.f, 1.f) :
		                           panel.delay >= 5                 ? ImVec4(1.f, 0.85f, 0.f, 1.f) : TEXT;
		const int predicted = std::clamp(panel.predicted_frames, 0, 6);
		const ImVec4 predicted_color = predicted == 4 ? ImVec4(1.f, 0.85f, 0.f, 1.f) : predicted == 5 ? ImVec4(1.f, 0.5f, 0.f, 1.f) :
		                               predicted == 6 ? ImVec4(1.f, 0.15f, 0.12f, 1.f) : TEXT_DIM;
		float tx = pen.x;
		const auto stat = [&](const char* label, const char* widest, int value, const ImVec4& c) {
			pen.Stat(tx, label, value, c);
			tx += pen.TextW(widest) + pen.Gap();
		};
		stat("D: ", "D: 99", panel.delay, delay_color);
		stat("P: ", "P: 9", predicted, predicted_color);
		stat("R: ", "R: 9999", panel.rollback_frames, TEXT);
		stat("W: ", "W: 9999", panel.sync_frames, TEXT);
		if (details)
		{
			pen.Next();
			for (const GgpoOsdPeer& p : panel.peers)
			{
				pen.Player(p, columns);
				pen.Diagnostics(p, s.flashes[std::clamp(p.position, 0, 3)], now);
			}
		}
		else
		{
			float segment_x = pen.x + pen.StatsWidth();
			for (const GgpoOsdPeer& p : panel.peers)
				segment_x = pen.Segment(segment_x, p, health[std::clamp(p.position, 0, 3)]);
		}

		pen.dl->ChannelsSetCurrent(0);
		pen.dl->AddRectFilled(ImVec2(left, top), ImVec2(left + width, bottom), Col(ImVec4(0.122f, 0.122f, 0.122f, 0.2f)));
		pen.dl->ChannelsMerge();
		s.bounds_min = ImVec2(left, top);
		s.bounds_max = ImVec2(left + width, bottom);
		s.bounds_valid = true;
	}
} // namespace Zdxsv
