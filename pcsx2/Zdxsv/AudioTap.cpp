// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/MediaHooks.h"
#include "Zdxsv/TestOptions.h"

#include "common/FileSystem.h"
#include "common/Timer.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace Zdxsv
{
	namespace
	{
		struct AudioTapState
		{
			std::FILE* wav = nullptr;
			std::FILE* gaps = nullptr;
			u32 channels = 0;
			u32 data_bytes = 0;
			u64 frames = 0;
			Common::Timer::Value last_read = 0;
			bool failed = false;
		};

		AudioTapState s_tap;

		void AudioTapPut32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

		void AudioTapWriteHeader(u32 rate)
		{
			u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
				3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
			h[22] = static_cast<u8>(s_tap.channels);
			AudioTapPut32(h + 4, 36 + s_tap.data_bytes);
			AudioTapPut32(h + 24, rate);
			AudioTapPut32(h + 28, rate * s_tap.channels * 4);
			h[32] = static_cast<u8>(s_tap.channels * 4);
			AudioTapPut32(h + 40, s_tap.data_bytes);
			std::fseek(s_tap.wav, 0, SEEK_SET);
			std::fwrite(h, 1, sizeof(h), s_tap.wav);
			std::fseek(s_tap.wav, 0, SEEK_END);
		}
	} // namespace

	const bool g_audio_tap = TestEnv("ZDXSV_AUDIO_WAV") != nullptr;

	void AudioTapWrite(const float* samples, u32 num_frames, u32 channels, u32 rate)
	{
		if (s_tap.failed)
			return;
		if (!s_tap.wav)
		{
			const std::string path = TestEnv("ZDXSV_AUDIO_WAV");
			s_tap.wav = FileSystem::OpenCFile(path.c_str(), "wb");
			s_tap.gaps = FileSystem::OpenCFile((path + ".gaps.txt").c_str(), "w");
			s_tap.failed = !s_tap.wav || !s_tap.gaps;
			if (s_tap.failed)
				return;
			s_tap.channels = channels;
			AudioTapWriteHeader(rate);
		}
		if (channels != s_tap.channels)
			return;

		// A device read more than 50 ms after the previous one is a stream resume: the analysis splits there.
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		const double ms = Common::Timer::ConvertValueToMilliseconds(now - s_tap.last_read);
		if (s_tap.last_read == 0 || ms > 50.0)
			std::fprintf(s_tap.gaps, "%llu %.1f\n", static_cast<unsigned long long>(s_tap.frames), s_tap.last_read ? ms : 0.0);
		s_tap.last_read = now;

		std::fwrite(samples, sizeof(float) * channels, num_frames, s_tap.wav);
		s_tap.frames += num_frames;
		s_tap.data_bytes += num_frames * channels * 4;
		AudioTapWriteHeader(rate);
		std::fflush(s_tap.gaps);
	}
} // namespace Zdxsv
