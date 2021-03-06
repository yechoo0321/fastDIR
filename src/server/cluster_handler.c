//cluster_handler.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/ioevent_loop.h"
#include "fastcommon/json_parser.h"
#include "sf/sf_util.h"
#include "sf/sf_func.h"
#include "sf/sf_nio.h"
#include "sf/sf_global.h"
#include "common/fdir_proto.h"
#include "binlog/binlog_producer.h"
#include "binlog/binlog_pack.h"
#include "binlog/binlog_replication.h"
#include "server_global.h"
#include "server_func.h"
#include "dentry.h"
#include "server_binlog.h"
#include "cluster_relationship.h"
#include "cluster_handler.h"

int cluster_handler_init()
{
    return 0;
}

int cluster_handler_destroy()
{   
    return 0;
}

void cluster_task_finish_cleanup(struct fast_task_info *task)
{
    switch (CLUSTER_TASK_TYPE) {
        case FDIR_CLUSTER_TASK_TYPE_RELATIONSHIP:
            if (CLUSTER_PEER != NULL) {
                CLUSTER_PEER = NULL;
            }
            CLUSTER_TASK_TYPE = FDIR_CLUSTER_TASK_TYPE_NONE;
            break;
        case  FDIR_CLUSTER_TASK_TYPE_REPLICA_MASTER:
            if (CLUSTER_REPLICA != NULL) {
                binlog_replication_rebind_thread(CLUSTER_REPLICA);
                CLUSTER_REPLICA = NULL;
            }
            CLUSTER_TASK_TYPE = FDIR_CLUSTER_TASK_TYPE_NONE;
            break;
        case FDIR_CLUSTER_TASK_TYPE_REPLICA_SLAVE:
            if (CLUSTER_CONSUMER_CTX != NULL) {
                replica_consumer_thread_terminate(CLUSTER_CONSUMER_CTX);
                CLUSTER_CONSUMER_CTX = NULL;
                ((FDIRServerContext *)task->thread_data->arg)->
                    cluster.consumer_ctx = NULL;
            }
            CLUSTER_TASK_TYPE = FDIR_CLUSTER_TASK_TYPE_NONE;
            break;
        default:
            break;
    }

    __sync_add_and_fetch(&((FDIRServerTaskArg *)task->arg)->task_version, 1);
    sf_task_finish_clean_up(task);
}

static int cluster_deal_actvie_test(struct fast_task_info *task)
{
    return server_expect_body_length(task, 0);
}

static int cluster_check_config_sign(struct fast_task_info *task,
        const int server_id, const char *config_sign)
{
    if (memcmp(config_sign, CLUSTER_CONFIG_SIGN_BUF,
                CLUSTER_CONFIG_SIGN_LEN) != 0)
    {
        char peer_hex[2 * CLUSTER_CONFIG_SIGN_LEN + 1];
        char my_hex[2 * CLUSTER_CONFIG_SIGN_LEN + 1];

        bin2hex(config_sign, CLUSTER_CONFIG_SIGN_LEN, peer_hex);
        bin2hex((const char *)CLUSTER_CONFIG_SIGN_BUF,
                CLUSTER_CONFIG_SIGN_LEN, my_hex);

        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "server #%d 's cluster config md5: %s != my: %s",
                server_id, peer_hex, my_hex);
        return EFAULT;
    }

    return 0;
}

static int cluster_deal_get_server_status(struct fast_task_info *task)
{
    int result;
    int server_id;
    FDIRProtoGetServerStatusReq *req;
    FDIRProtoGetServerStatusResp *resp;

    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoGetServerStatusReq))) != 0)
    {
        return result;
    }

    req = (FDIRProtoGetServerStatusReq *)REQUEST.body;
    server_id = buff2int(req->server_id);
    if ((result=cluster_check_config_sign(task, server_id,
                    req->config_sign)) != 0)
    {
        return result;
    }

    resp = (FDIRProtoGetServerStatusResp *)REQUEST.body;

    resp->is_master = MYSELF_IS_MASTER;
    resp->status = CLUSTER_MYSELF_PTR->status;
    int2buff(CLUSTER_MY_SERVER_ID, resp->server_id);
    long2buff(DATA_CURRENT_VERSION, resp->data_version);

    RESPONSE.header.body_len = sizeof(FDIRProtoGetServerStatusResp);
    RESPONSE.header.cmd = FDIR_CLUSTER_PROTO_GET_SERVER_STATUS_RESP;
    TASK_ARG->context.response_done = true;
    return 0;
}

static int cluster_deal_join_master(struct fast_task_info *task)
{
    int result;
    int cluster_id;
    int server_id;
    FDIRProtoJoinMasterReq *req;
    FDIRClusterServerInfo *peer;

    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoJoinMasterReq))) != 0)
    {
        return result;
    }

    req = (FDIRProtoJoinMasterReq *)REQUEST.body;
    cluster_id = buff2int(req->cluster_id);
    server_id = buff2int(req->server_id);
    if (cluster_id != CLUSTER_ID) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "peer cluster id: %d != mine: %d",
                cluster_id, CLUSTER_ID);
        return EINVAL;
    }

    peer = fdir_get_server_by_id(server_id);
    if (peer == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "peer server id: %d not exist", server_id);
        return ENOENT;
    }

    if ((result=cluster_check_config_sign(task, server_id,
                    req->config_sign)) != 0)
    {
        return result;
    }

    if (CLUSTER_MYSELF_PTR != CLUSTER_MASTER_PTR) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "i am not master");
        return EINVAL;
    }

    if (CLUSTER_PEER != NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "peer server id: %d already joined", server_id);
        return EEXIST;
    }

    memcpy(peer->key, req->key, FDIR_REPLICA_KEY_SIZE);
    CLUSTER_TASK_TYPE = FDIR_CLUSTER_TASK_TYPE_RELATIONSHIP;
    CLUSTER_PEER = peer;
    return 0;
}

static int cluster_deal_ping_master(struct fast_task_info *task)
{
    int result;
    FDIRProtoPingMasterRespHeader *resp_header;
    FDIRProtoPingMasterRespBodyPart *body_part;
    FDIRClusterServerInfo *cs;
    FDIRClusterServerInfo *end;

    if ((result=server_expect_body_length(task, 0)) != 0) {
        return result;
    }

    if (CLUSTER_PEER == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "please join first");
        return EINVAL;
    }

    if (CLUSTER_MYSELF_PTR != CLUSTER_MASTER_PTR) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "i am not master");
        return EINVAL;
    }

    resp_header = (FDIRProtoPingMasterRespHeader *)REQUEST.body;
    body_part = (FDIRProtoPingMasterRespBodyPart *)(REQUEST.body +
            sizeof(FDIRProtoPingMasterRespHeader));
    long2buff(CURRENT_INODE_SN, resp_header->inode_sn);
    if (CLUSTER_PEER->last_change_version < CLUSTER_SERVER_ARRAY.change_version) {
        CLUSTER_PEER->last_change_version = CLUSTER_SERVER_ARRAY.change_version;
        int2buff(CLUSTER_SERVER_ARRAY.count, resp_header->server_count);

        end = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
        for (cs=CLUSTER_SERVER_ARRAY.servers; cs<end; cs++, body_part++) {
            int2buff(cs->server->id, body_part->server_id);
            body_part->status = cs->status;
        }
    } else {
        int2buff(0, resp_header->server_count);
    }

    TASK_ARG->context.response_done = true;
    RESPONSE.header.cmd = FDIR_CLUSTER_PROTO_PING_MASTER_RESP;
    RESPONSE.header.body_len = (char *)body_part - REQUEST.body;
    return 0;
}

static int cluster_deal_next_master(struct fast_task_info *task)
{
    int result;
    int master_id;
    FDIRClusterServerInfo *master;

    if ((result=server_expect_body_length(task, 4)) != 0) {
        return result;
    }

    if (CLUSTER_MYSELF_PTR == CLUSTER_MASTER_PTR) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "i am already master");
        return EEXIST;
    }

    master_id = buff2int(REQUEST.body);
    master = fdir_get_server_by_id(master_id);
    if (master == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "master server id: %d not exist", master_id);
        return ENOENT;
    }

    if (REQUEST.header.cmd == FDIR_CLUSTER_PROTO_PRE_SET_NEXT_MASTER) {
        return cluster_relationship_pre_set_master(master);
    } else {
        return cluster_relationship_commit_master(master);
    }
}

static int cluster_deal_push_binlog_req(struct fast_task_info *task)
{
    int result;
    int binlog_length;
    uint64_t last_data_version;
    FDIRProtoPushBinlogReqBodyHeader *body_header;

    if ((result=server_check_min_body_length(task,
                    sizeof(FDIRProtoPushBinlogReqBodyHeader) +
                    BINLOG_RECORD_MIN_SIZE)) != 0)
    {
        return result;
    }

    if (CLUSTER_TASK_TYPE != FDIR_CLUSTER_TASK_TYPE_REPLICA_SLAVE) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid task type: %d != %d", CLUSTER_TASK_TYPE,
                FDIR_CLUSTER_TASK_TYPE_REPLICA_SLAVE);
        return -EINVAL;
    }
    if (CLUSTER_CONSUMER_CTX == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "please join first");
        return -EINVAL;
    }

    body_header = (FDIRProtoPushBinlogReqBodyHeader *)(task->data +
            sizeof(FDIRProtoHeader));
    binlog_length = buff2int(body_header->binlog_length);
    last_data_version = buff2long(body_header->last_data_version);
    if (sizeof(FDIRProtoPushBinlogReqBodyHeader) + binlog_length !=
            REQUEST.header.body_len)
    {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "body length: %d != expect: %d", REQUEST.header.body_len,
                (int)(sizeof(FDIRProtoPushBinlogReqBodyHeader) +
                    binlog_length));
        return -EINVAL;
    }

    //logInfo("push_binlog body length: %d", REQUEST.header.body_len);
    return deal_replica_push_request(CLUSTER_CONSUMER_CTX, (char *)
            (body_header + 1), binlog_length, last_data_version);
}

static inline int cluster_check_replication_task(struct fast_task_info *task)
{
    if (CLUSTER_TASK_TYPE != FDIR_CLUSTER_TASK_TYPE_REPLICA_MASTER) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid task type: %d != %d", CLUSTER_TASK_TYPE,
                FDIR_CLUSTER_TASK_TYPE_REPLICA_MASTER);
        return EINVAL;
    }

    if (CLUSTER_REPLICA == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "cluster replication ptr is null");
        return EINVAL;
    }
    return 0;
}

static int cluster_deal_push_binlog_resp(struct fast_task_info *task)
{
    int result;
    int count;
    int expect_body_len;
    int64_t data_version;
    short err_no;
    FDIRProtoPushBinlogRespBodyHeader *body_header;
    FDIRProtoPushBinlogRespBodyPart *body_part;
    FDIRProtoPushBinlogRespBodyPart *bp_end;

    if ((result=cluster_check_replication_task(task)) != 0) {
        return result;
    }

    if ((result=server_check_min_body_length(task,
                    sizeof(FDIRProtoPushBinlogRespBodyHeader) +
                    sizeof(FDIRProtoPushBinlogRespBodyPart))) != 0)
    {
        return result;
    }

    body_header = (FDIRProtoPushBinlogRespBodyHeader *)REQUEST.body;
    count = buff2int(body_header->count);

    expect_body_len = sizeof(FDIRProtoPushBinlogRespBodyHeader) +
        sizeof(FDIRProtoPushBinlogRespBodyPart) * count;
    if (REQUEST.header.body_len != expect_body_len) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "body length: %d != expected: %d, results count: %d",
                REQUEST.header.body_len, expect_body_len, count);
        return EINVAL;
    }

    body_part = (FDIRProtoPushBinlogRespBodyPart *)(REQUEST.body +
            sizeof(FDIRProtoPushBinlogRespBodyHeader));
    bp_end = body_part + count;
    for (; body_part<bp_end; body_part++) {
        data_version = buff2long(body_part->data_version);
        err_no = buff2short(body_part->err_no);
        if (err_no != 0) {
            result = err_no;
            RESPONSE.error.length = sprintf(
                    RESPONSE.error.message,
                    "replica fail, data_version: %"PRId64
                    ", result: %d", data_version, err_no);
            break;
        }

        //logInfo("push_binlog_resp data_version: %"PRId64", errno: %d", data_version, err_no);

        if ((result=binlog_replications_check_response_data_version(
                        CLUSTER_REPLICA, data_version)) != 0)
        {
            RESPONSE.error.length = sprintf(
                    RESPONSE.error.message,
                    "push_result_ring_remove fail, "
                    "data_version: %"PRId64", result: %d",
                    data_version, result);
            break;
        }
    }

    return result;
}

static int cluster_deal_join_slave_req(struct fast_task_info *task)
{
    int result;
    int cluster_id;
    int server_id;
    int buffer_size;
    FDIRServerContext *server_ctx;
    FDIRBinlogFilePosition bf_position;
    FDIRProtoJoinSlaveReq *req;
    FDIRClusterServerInfo *peer;
    FDIRClusterServerInfo *master;
    FDIRClusterServerInfo *next_master;
    FDIRProtoJoinSlaveResp *resp;

    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoJoinSlaveReq))) != 0)
    {
        return result;
    }

    req = (FDIRProtoJoinSlaveReq *)REQUEST.body;
    cluster_id = buff2int(req->cluster_id);
    server_id = buff2int(req->server_id);
    buffer_size = buff2int(req->buffer_size);
    if (cluster_id != CLUSTER_ID) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "peer cluster id: %d != mine: %d",
                cluster_id, CLUSTER_ID);
        return EINVAL;
    }
    if (buffer_size != task->size) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "peer task buffer size: %d != mine: %d",
                buffer_size, task->size);
        return EINVAL;
    }

    peer = fdir_get_server_by_id(server_id);
    if (peer == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "peer server id: %d not exist", server_id);
        return ENOENT;
    }

    next_master = g_next_master;
    if (next_master != NULL) {
        if (next_master != peer) {
            RESPONSE.error.length = sprintf(
                    RESPONSE.error.message,
                    "master selection in progress, the candidate "
                    "master id: %d", next_master->server->id);
            return FDIR_STATUS_MASTER_INCONSISTENT;
        }
        return EBUSY;
    }

    master = CLUSTER_MASTER_PTR;
    if (peer != master) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "master NOT consistent, peer server id: %d, "
                "local master id: %d", server_id, master != NULL ?
                master->server->id : 0);
        return master != NULL ? FDIR_STATUS_MASTER_INCONSISTENT : EFAULT;
    }

    if (memcmp(req->key, REPLICA_KEY_BUFF, FDIR_REPLICA_KEY_SIZE) != 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "check key fail");
        return EPERM;
    }

    if (CLUSTER_CONSUMER_CTX != NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "master server id: %d already joined", server_id);
        return EEXIST;
    }

    server_ctx = (FDIRServerContext *)task->thread_data->arg;
    if (server_ctx->cluster.consumer_ctx != NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "replica consumer thread already exist");
        return EEXIST;
    }

    CLUSTER_TASK_TYPE = FDIR_CLUSTER_TASK_TYPE_REPLICA_SLAVE;
    CLUSTER_CONSUMER_CTX = replica_consumer_thread_init(task,
         BINLOG_BUFFER_INIT_SIZE, &result);
    if (CLUSTER_CONSUMER_CTX == NULL) {
        return result;
    }
    server_ctx->cluster.consumer_ctx = CLUSTER_CONSUMER_CTX;

    binlog_get_current_write_position(&bf_position);

    resp = (FDIRProtoJoinSlaveResp *)REQUEST.body;
    long2buff(DATA_CURRENT_VERSION, resp->last_data_version);
    int2buff(bf_position.index, resp->binlog_pos_hint.index);
    long2buff(bf_position.offset, resp->binlog_pos_hint.offset);

    TASK_ARG->context.response_done = true;
    RESPONSE.header.cmd = FDIR_REPLICA_PROTO_JOIN_SLAVE_RESP;
    RESPONSE.header.body_len = sizeof(FDIRProtoJoinSlaveResp);

    return 0;
}

static int cluster_deal_join_slave_resp(struct fast_task_info *task)
{
    int result;
    FDIRProtoJoinSlaveResp *req;

    if ((result=cluster_check_replication_task(task)) != 0) {
        return result;
    }

    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoJoinSlaveResp))) != 0)
    {
        return result;
    }

    req = (FDIRProtoJoinSlaveResp *)REQUEST.body;
    CLUSTER_REPLICA->slave->last_data_version = buff2long(
            req->last_data_version);
    CLUSTER_REPLICA->slave->binlog_pos_hint.index = buff2int(
            req->binlog_pos_hint.index);
    CLUSTER_REPLICA->slave->binlog_pos_hint.offset = buff2long(
            req->binlog_pos_hint.offset);
    return 0;
}

static int cluster_deal_slave_ack(struct fast_task_info *task)
{
    int result;

    if ((result=cluster_check_replication_task(task)) != 0) {
        return result;
    }

    if (REQUEST_STATUS != 0) {
        if (REQUEST.header.body_len > 0) {
            int remain_size;
            int len;

            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "message from peer %s:%u => ",
                    task->client_ip, task->port);
            remain_size = sizeof(RESPONSE.error.message) -
                RESPONSE.error.length;
            if (REQUEST.header.body_len >= remain_size) {
                len = remain_size - 1;
            } else {
                len = REQUEST.header.body_len;
            }

            memcpy(RESPONSE.error.message + RESPONSE.error.length,
                    REQUEST.body, len);
            RESPONSE.error.length += len;
            *(RESPONSE.error.message + RESPONSE.error.length) = '\0';
        }

        if (REQUEST_STATUS == FDIR_STATUS_MASTER_INCONSISTENT) {
            logWarning("file: "__FILE__", line: %d, "
                    "more than one masters occur, master brain-split maybe "
                    "happened, will trigger reselecting master", __LINE__);
            cluster_relationship_trigger_reselect_master();
            REQUEST_STATUS = EEXIST;
        }

        return REQUEST_STATUS;
    }

    if (REQUEST.header.body_len > 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "ACK body length: %d != 0",
                REQUEST.header.body_len);
        return -EINVAL;
    }

    return 0;
}

static inline void init_task_context(struct fast_task_info *task)
{
    TASK_ARG->req_start_time = get_current_time_us();
    RESPONSE.header.cmd = FDIR_PROTO_ACK;
    RESPONSE.header.body_len = 0;
    RESPONSE.header.status = 0;
    RESPONSE.error.length = 0;
    RESPONSE.error.message[0] = '\0';
    TASK_ARG->context.log_error = true;
    TASK_ARG->context.response_done = false;
    TASK_ARG->context.need_response = true;

    REQUEST.header.cmd = ((FDIRProtoHeader *)task->data)->cmd;
    REQUEST.header.body_len = task->length - sizeof(FDIRProtoHeader);
    REQUEST.header.status = buff2short(((FDIRProtoHeader *)task->data)->status);
    REQUEST.body = task->data + sizeof(FDIRProtoHeader);
}

static int deal_task_done(struct fast_task_info *task)
{
    FDIRProtoHeader *proto_header;
    int r;
    int time_used;

    if (TASK_ARG->context.log_error && RESPONSE.error.length > 0) {
        logError("file: "__FILE__", line: %d, "
                "peer %s:%u, cmd: %d (%s), req body length: %d, %s",
                __LINE__, task->client_ip, task->port, REQUEST.header.cmd,
                fdir_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len, RESPONSE.error.message);
    }

    if (!TASK_ARG->context.need_response) {
        if (RESPONSE_STATUS == 0) {
            task->offset = task->length = 0;
            return sf_set_read_event(task);
        }
        return RESPONSE_STATUS > 0 ? -1 * RESPONSE_STATUS : RESPONSE_STATUS;
    }

    proto_header = (FDIRProtoHeader *)task->data;
    if (!TASK_ARG->context.response_done) {
        RESPONSE.header.body_len = RESPONSE.error.length;
        if (RESPONSE.error.length > 0) {
            memcpy(task->data + sizeof(FDIRProtoHeader),
                    RESPONSE.error.message, RESPONSE.error.length);
        }
    }

    short2buff(RESPONSE_STATUS >= 0 ? RESPONSE_STATUS : -1 * RESPONSE_STATUS,
            proto_header->status);
    proto_header->cmd = RESPONSE.header.cmd;
    int2buff(RESPONSE.header.body_len, proto_header->body_len);
    task->length = sizeof(FDIRProtoHeader) + RESPONSE.header.body_len;

    r = sf_send_add_event(task);
    time_used = (int)(get_current_time_us() - TASK_ARG->req_start_time);
    if (time_used > 50 * 1000) {
        lwarning("process a request timed used: %d us, "
                "cmd: %d (%s), req body len: %d, resp body len: %d",
                time_used, REQUEST.header.cmd,
                fdir_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len,
                RESPONSE.header.body_len);
    }

    if (REQUEST.header.cmd != FDIR_CLUSTER_PROTO_PING_MASTER_REQ) {
    logInfo("file: "__FILE__", line: %d, "
            "client ip: %s, req cmd: %d (%s), req body_len: %d, "
            "resp cmd: %d (%s), status: %d, resp body_len: %d, "
            "time used: %d us", __LINE__,
            task->client_ip, REQUEST.header.cmd,
            fdir_get_cmd_caption(REQUEST.header.cmd),
            REQUEST.header.body_len, RESPONSE.header.cmd,
            fdir_get_cmd_caption(RESPONSE.header.cmd),
            RESPONSE_STATUS, RESPONSE.header.body_len, time_used);
    }

    return r == 0 ? RESPONSE_STATUS : r;
}

int cluster_deal_task(struct fast_task_info *task)
{
    int result;

    /*
    logInfo("file: "__FILE__", line: %d, "
            "nio_stage: %d, SF_NIO_STAGE_CONTINUE: %d", __LINE__,
            task->nio_stage, SF_NIO_STAGE_CONTINUE);
            */

    if (task->nio_stage == SF_NIO_STAGE_CONTINUE) {
        task->nio_stage = SF_NIO_STAGE_SEND;
        if (TASK_ARG->context.deal_func != NULL) {
            result = TASK_ARG->context.deal_func(task);
        } else {
            result = RESPONSE_STATUS;
            if (result == TASK_STATUS_CONTINUE) {
                logError("file: "__FILE__", line: %d, "
                        "unexpect status: %d", __LINE__, result);
                result = EBUSY;
            }
        }
    } else {
        init_task_context(task);

        switch (REQUEST.header.cmd) {
            case FDIR_PROTO_ACTIVE_TEST_REQ:
                RESPONSE.header.cmd = FDIR_PROTO_ACTIVE_TEST_RESP;
                result = cluster_deal_actvie_test(task);
                break;
            case FDIR_CLUSTER_PROTO_GET_SERVER_STATUS_REQ:
                result = cluster_deal_get_server_status(task);
                break;
            case FDIR_CLUSTER_PROTO_PRE_SET_NEXT_MASTER:
            case FDIR_CLUSTER_PROTO_COMMIT_NEXT_MASTER:
                result = cluster_deal_next_master(task);
                break;
            case FDIR_CLUSTER_PROTO_JOIN_MASTER:
                result = cluster_deal_join_master(task);
                break;
            case FDIR_CLUSTER_PROTO_PING_MASTER_REQ:
                result = cluster_deal_ping_master(task);
                break;
            case FDIR_REPLICA_PROTO_JOIN_SLAVE_REQ:
                result = cluster_deal_join_slave_req(task);
                break;
            case FDIR_REPLICA_PROTO_JOIN_SLAVE_RESP:
                result = cluster_deal_join_slave_resp(task);
                TASK_ARG->context.need_response = false;
                break;
            case FDIR_REPLICA_PROTO_PUSH_BINLOG_REQ:
                result = cluster_deal_push_binlog_req(task);
                TASK_ARG->context.need_response = false;
                break;
            case FDIR_REPLICA_PROTO_PUSH_BINLOG_RESP:
                result = cluster_deal_push_binlog_resp(task);
                TASK_ARG->context.need_response = false;
                break;
            case FDIR_PROTO_ACK:
                result = cluster_deal_slave_ack(task);
                TASK_ARG->context.need_response = false;
                break;
            default:
                RESPONSE.error.length = sprintf(
                        RESPONSE.error.message,
                        "unkown cmd: %d", REQUEST.header.cmd);
                result = -EINVAL;
                break;
        }
    }

    if (result == TASK_STATUS_CONTINUE) {
        return 0;
    } else {
        RESPONSE_STATUS = result;
        return deal_task_done(task);
    }
}


void *cluster_alloc_thread_extra_data(const int thread_index)
{
    FDIRServerContext *server_context;

    server_context = (FDIRServerContext *)malloc(sizeof(FDIRServerContext));
    if (server_context == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail, errno: %d, error info: %s",
                __LINE__, (int)sizeof(FDIRServerContext),
                errno, strerror(errno));
        return NULL;
    }

    memset(server_context, 0, sizeof(FDIRServerContext));
    return server_context;
}

int cluster_thread_loop_callback(struct nio_thread_data *thread_data)
{
    FDIRServerContext *server_ctx;
    int result;
    static int count = 0;

    server_ctx = (FDIRServerContext *)thread_data->arg;

    if (count++ % 100000 == 0) {
        /*
        logInfo("is_master: %d, consumer_ctx: %p, connected.count: %d",
                MYSELF_IS_MASTER, server_ctx->cluster.consumer_ctx,
                server_ctx->cluster.connected.count);
                */
    }

    if (CLUSTER_MYSELF_PTR == CLUSTER_MASTER_PTR) {
        return binlog_replication_process(server_ctx);
    } else {
        if (server_ctx->cluster.consumer_ctx != NULL) {
            result = deal_replica_push_task(server_ctx->cluster.consumer_ctx);
            return result == EAGAIN ? 0 : result;
        } else if (server_ctx->cluster.clean_connected_replicas) {
            logInfo("file: "__FILE__", line: %d, "
                    "cluster thread #%d, will clean %d connected "
                    "replications because i am no longer master",
                    __LINE__, SF_THREAD_INDEX(CLUSTER_SF_CTX, thread_data),
                    server_ctx->cluster.connected.count);
            server_ctx->cluster.clean_connected_replicas = false;
            clean_connected_replications(server_ctx);
        }

        return 0;
    }
}
