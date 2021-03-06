//binlog_replay.h

#ifndef _BINLOG_REPLAY_H_
#define _BINLOG_REPLAY_H_

#include <pthread.h>
#include "binlog_types.h"

typedef void (*binlog_replay_notify_func)(const int result,
        struct fdir_binlog_record *record, void *args);

typedef struct binlog_replay_context {
    struct {
        int size;
        FDIRBinlogRecord *records;
    } record_array;
    int64_t data_current_version;
    volatile int waiting_count;
    int last_errno;
    int64_t record_count;
    int64_t skip_count;
    int64_t warning_count;
    volatile int64_t fail_count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct {
        binlog_replay_notify_func func;
        void *args;
    } notify;
} BinlogReplayContext;

#ifdef __cplusplus
extern "C" {
#endif

int binlog_replay_init_ex(BinlogReplayContext *replay_ctx,
        binlog_replay_notify_func notify_func, void *args,
        const int batch_size);

#define binlog_replay_init(replay_ctx, batch_size) \
        binlog_replay_init_ex(replay_ctx, NULL, NULL, batch_size)

void binlog_replay_destroy(BinlogReplayContext *replay_ctx);

int binlog_replay_deal_buffer(BinlogReplayContext *replay_ctx,
         const char *buff, const int len,
         FDIRBinlogFilePosition *binlog_position);

#ifdef __cplusplus
}
#endif

#endif
