#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "sf/sf_global.h"
#include "../server_global.h"
#include "../data_thread.h"
#include "binlog_pack.h"
#include "binlog_replay.h"

static void data_thread_deal_done_callback(const int result,
        FDIRBinlogRecord *record)
{
    BinlogReplayContext *replay_ctx;

    replay_ctx = (BinlogReplayContext *)record->notify.args;
    if (result != 0) {
        replay_ctx->last_errno = result;
        __sync_add_and_fetch(&replay_ctx->fail_count, 1);
    }
    if (replay_ctx->notify.func != NULL) {
        replay_ctx->notify.func(result, record, replay_ctx->notify.args);
    }
    __sync_sub_and_fetch(&replay_ctx->waiting_count, 1);
}

int binlog_replay_init_ex(BinlogReplayContext *replay_ctx,
        binlog_replay_notify_func notify_func, void *args,
        const int batch_size)
{
    FDIRBinlogRecord *record;
    FDIRBinlogRecord *rend;
    int bytes;

    replay_ctx->record_count = 0;
    replay_ctx->last_errno = 0;
    replay_ctx->fail_count = 0;
    replay_ctx->waiting_count = 0;
    replay_ctx->ts.tv_sec = 0;
    replay_ctx->ts.tv_nsec = 10 * 1000;
    replay_ctx->notify.func = notify_func;
    replay_ctx->record_array.size = batch_size * DATA_THREAD_COUNT;
    bytes = sizeof(FDIRBinlogRecord) * replay_ctx->record_array.size;
    replay_ctx->record_array.records = (FDIRBinlogRecord *)malloc(bytes);
    if (replay_ctx->record_array.records == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(replay_ctx->record_array.records, 0, bytes);

    rend = replay_ctx->record_array.records + replay_ctx->record_array.size;
    for (record=replay_ctx->record_array.records; record<rend; record++) {
        record->notify.args = replay_ctx;
        record->notify.func = data_thread_deal_done_callback;
    }

    return 0;
}

void binlog_replay_destroy(BinlogReplayContext *replay_ctx)
{
    if (replay_ctx->record_array.records != NULL) {
        free(replay_ctx->record_array.records);
        replay_ctx->record_array.records = NULL;
    }
}

int binlog_replay_deal_buffer(BinlogReplayContext *replay_ctx,
         const char *buff, const int len)
{
    const char *p;
    const char *end;
    const char *rend;
    FDIRBinlogRecord *record;
    char error_info[FDIR_ERROR_INFO_SIZE];
    int result;
    int count;

    count = 0;
    p = buff;
    end = p + len;
    while (p < end) {
        record = replay_ctx->record_array.records + count;
        if ((result=binlog_unpack_record(p, end - p, record,
                        &rend, error_info, sizeof(error_info))) != 0)
        {
            logError("file: "__FILE__", line: %d, "
                    "%s", __LINE__, error_info);
            return result;
        }
        p = rend;

        replay_ctx->record_count++;
        __sync_add_and_fetch(&replay_ctx->waiting_count, 1);

        if ((result=push_to_data_thread_queue(record)) != 0) {
            return result;
        }

        if (++count == replay_ctx->record_array.size) {
            while (__sync_add_and_fetch(&replay_ctx->waiting_count, 0) != 0) {
                nanosleep(&replay_ctx->ts, NULL);
            }
            if (replay_ctx->fail_count > 0) {
                return replay_ctx->last_errno;
            }
            count = 0;
        }
    }

    logInfo("record_count: %"PRId64", waiting_count: %d", replay_ctx->record_count,
            __sync_add_and_fetch(&replay_ctx->waiting_count, 0));

    while (__sync_add_and_fetch(&replay_ctx->waiting_count, 0) != 0) {
        nanosleep(&replay_ctx->ts, NULL);
    }
    return replay_ctx->fail_count > 0 ? replay_ctx->last_errno : 0;
}
