#include "gdxsv_synctest.h"

GdxsvSyncTestBackend::~GdxsvSyncTestBackend()
{
}

GGPOErrorCode GdxsvSyncTestBackend::IncrementFrame(void)
{
    _sync.IncrementFrame();
    _current_input.erase();

    Log("End of frame(%d)...\n", _sync.GetFrameCount());
    EndLog();

    if (_rollingback) {
        return GGPO_OK;
    }

	if (0 < _check_distance) {
		int frame = _sync.GetFrameCount();
		// Hold onto the current frame in our queue of saved states.  We'll need
		// the checksum later to verify that our replay of the same frame got the
		// same results.
		SavedInfo info;
		info.frame = frame;
		info.input = _last_input;
		info.cbuf = _sync.GetLastSavedFrame().cbuf;
		info.buf = (char *)malloc(info.cbuf);
		memcpy(info.buf, _sync.GetLastSavedFrame().buf, info.cbuf);
		info.checksum = _sync.GetLastSavedFrame().checksum;
		_saved_frames.push(info);
	}

    return GGPO_OK;
}

GGPOErrorCode GdxsvSyncTestBackend::BeginRollback(int *rollback_frame)
{
    Log("BeginRollback");
	*rollback_frame = 0;
    int frame = _sync.GetFrameCount();
	if (0 < _check_distance && frame - _last_verified == _check_distance) {
		// We've gone far enough ahead and should now start replaying frames.
		// Load the last verified frame and set the rollback flag to true.
		_sync.LoadFrame(_last_verified);
		_rollingback = true;
		*rollback_frame = _check_distance;
		begin_rollback_frame = frame;
	}
    return GGPO_OK;
}

GGPOErrorCode GdxsvSyncTestBackend::StepRollback() {
	if (!_rollingback) {
		RaiseSyncError("Not in rollback");
	}

	_callbacks.advance_frame(0);

	// Verify that the checksumn of this frame is the same as the one in our
	// list.
    SavedInfo info;
	info = _saved_frames.front();
	_saved_frames.pop();

	if (info.frame != _sync.GetFrameCount()) {
		RaiseSyncError("Frame number %d does not match saved frame number %d", info.frame, _sync.GetFrameCount());
	}
	int checksum = _sync.GetLastSavedFrame().checksum;
	if (info.checksum != checksum) {
		LogSaveStates(info);
		RaiseSyncError("Checksum for frame %d does not match saved (%d != %d)", _sync.GetFrameCount(), checksum, info.checksum);
	}
	printf("Checksum %08d for frame %d matches.\n", checksum, info.frame);
	free(info.buf);
	return GGPO_OK;
}

GGPOErrorCode GdxsvSyncTestBackend::EndRollback()
{
	if (!_rollingback) {
		RaiseSyncError("Not in rollback");
	}

    Log("EndRollback");
	int frame = _sync.GetFrameCount();
	if (frame != begin_rollback_frame) {
		RaiseSyncError("Frame number %d does not match begin rollback frame number %d", frame, begin_rollback_frame);
	}
	_last_verified = begin_rollback_frame;
	_rollingback = false;
    return GGPO_OK;
}
