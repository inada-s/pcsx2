#ifndef GGPO_GDXSV_SYNCTEST_H
#define GGPO_GDXSV_SYNCTEST_H

#include "synctest.h"

class GdxsvSyncTestBackend : public SyncTestBackend {
public:
    using SyncTestBackend::SyncTestBackend;
    virtual ~GdxsvSyncTestBackend(void);

	/*
    virtual GGPOErrorCode DoPoll(int timeout);
    virtual GGPOErrorCode AddPlayer(GGPOPlayer *player, GGPOPlayerHandle *handle);
    virtual GGPOErrorCode AddLocalInput(GGPOPlayerHandle player, void *values, int size);
    virtual GGPOErrorCode SyncInput(void *values, int size, int *disconnect_flags);
    virtual GGPOErrorCode Logv(char *fmt, va_list list);
*/
    virtual GGPOErrorCode IncrementFrame(void);
    virtual GGPOErrorCode GetFrame(int *current_frame, int *confirmed_frame);
	virtual GGPOErrorCode GetConfirmedInput(GGPOPlayerHandle player, int frame, void * values, int size);
    virtual GGPOErrorCode BeginRollback(int *frame);
    virtual GGPOErrorCode StepRollback();
    virtual GGPOErrorCode EndRollback();
protected:
	int begin_rollback_frame = 0;
};

#endif
