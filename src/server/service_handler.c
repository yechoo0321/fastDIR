//service_handler.c

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
#include "server_global.h"
#include "server_func.h"
#include "dentry.h"
#include "inode_index.h"
#include "cluster_relationship.h"
#include "service_handler.h"

static volatile int64_t next_token = 0;   //next token for dentry list
static int64_t dstat_mflags_mask = 0;

int service_handler_init()
{
    FDIRStatModifyFlags mask;

    mask.flags = 0;
    mask.mode = 1;
    mask.atime = 1;
    mask.ctime = 1;
    mask.mtime = 1;
    mask.uid  = 1;
    mask.gid  = 1;
    mask.size = 1;
    dstat_mflags_mask = mask.flags;

    next_token = ((int64_t)g_current_time) << 32;
    return 0;
}

int service_handler_destroy()
{   
    return 0;
}

void service_accep_done_callback(struct fast_task_info *task,
        const bool bInnerPort)
{
    FC_INIT_LIST_HEAD(FTASK_HEAD_PTR);
}

static inline void release_flock_task(struct fast_task_info *task,
        FLockTask *flck)
{
    fc_list_del_init(&flck->clink);
    inode_index_flock_release(flck);
}

void service_task_finish_cleanup(struct fast_task_info *task)
{
    //FDIRServerTaskArg *task_arg;
    //task_arg = (FDIRServerTaskArg *)task->arg;

    if (!fc_list_empty(FTASK_HEAD_PTR)) {
        FLockTask *flck;
        FLockTask *next;
        fc_list_for_each_entry_safe(flck, next, FTASK_HEAD_PTR, clink) {
            release_flock_task(task, flck);
        }
    }

    if (SYS_LOCK_TASK != NULL) {
        inode_index_sys_lock_release(SYS_LOCK_TASK);
        SYS_LOCK_TASK = NULL;
    }

    dentry_array_free(&DENTRY_LIST_CACHE.array);

    __sync_add_and_fetch(&((FDIRServerTaskArg *)task->arg)->task_version, 1);
    sf_task_finish_clean_up(task);
}

static int service_deal_actvie_test(struct fast_task_info *task)
{
    return server_expect_body_length(task, 0);
}

static int service_deal_service_stat(struct fast_task_info *task)
{
    int result;
    FDIRDentryCounters counters;
    FDIRProtoServiceStatResp *stat_resp;

    if ((result=server_expect_body_length(task, 0)) != 0) {
        return result;
    }

    data_thread_sum_counters(&counters);
    stat_resp = (FDIRProtoServiceStatResp *)REQUEST.body;

    stat_resp->is_master = CLUSTER_MYSELF_PTR == CLUSTER_MASTER_PTR ? 1 : 0;
    stat_resp->status = CLUSTER_MYSELF_PTR->status;
    int2buff(CLUSTER_MYSELF_PTR->server->id, stat_resp->server_id);

    int2buff(SF_G_CONN_CURRENT_COUNT, stat_resp->connection.current_count);
    int2buff(SF_G_CONN_MAX_COUNT, stat_resp->connection.max_count);
    long2buff(DATA_CURRENT_VERSION, stat_resp->dentry.current_data_version);
    long2buff(CURRENT_INODE_SN, stat_resp->dentry.current_inode_sn);
    long2buff(counters.ns, stat_resp->dentry.counters.ns);
    long2buff(counters.dir, stat_resp->dentry.counters.dir);
    long2buff(counters.file, stat_resp->dentry.counters.file);

    RESPONSE.header.body_len = sizeof(FDIRProtoServiceStatResp);
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_SERVICE_STAT_RESP;
    TASK_ARG->context.response_done = true;

    return 0;
}

static int service_deal_cluster_stat(struct fast_task_info *task)
{
    int result;
    FDIRProtoClusterStatRespBodyHeader *body_header;
    FDIRProtoClusterStatRespBodyPart *body_part;
    FDIRClusterServerInfo *cs;
    FDIRClusterServerInfo *send;

    if ((result=server_expect_body_length(task, 0)) != 0) {
        return result;
    }

    body_header = (FDIRProtoClusterStatRespBodyHeader *)REQUEST.body;
    body_part = (FDIRProtoClusterStatRespBodyPart *)(REQUEST.body +
            sizeof(FDIRProtoClusterStatRespBodyHeader));

    int2buff(CLUSTER_SERVER_ARRAY.count, body_header->count);

    send = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (cs=CLUSTER_SERVER_ARRAY.servers; cs<send; cs++, body_part++) {
        int2buff(cs->server->id, body_part->server_id);
        body_part->is_master = cs->is_master;
        body_part->status = cs->status;

        snprintf(body_part->ip_addr, sizeof(body_part->ip_addr), "%s",
                SERVICE_GROUP_ADDRESS_FIRST_IP(cs->server));
        short2buff(SERVICE_GROUP_ADDRESS_FIRST_PORT(cs->server),
                body_part->port);
    }

    RESPONSE.header.body_len = (char *)body_part - REQUEST.body;
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_CLUSTER_STAT_RESP;
    TASK_ARG->context.response_done = true;

    return 0;
}

static int service_deal_get_master(struct fast_task_info *task)
{
    int result;
    FDIRProtoGetServerResp *resp;
    FDIRClusterServerInfo *master;
    const FCAddressInfo *addr;

    if ((result=server_expect_body_length(task, 0)) != 0) {
        return result;
    }

    master = CLUSTER_MASTER_PTR;
    if (master == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "the master NOT exist");
        return ENOENT;
    }

    resp = (FDIRProtoGetServerResp *)REQUEST.body;
    addr = fc_server_get_address_by_peer(&SERVICE_GROUP_ADDRESS_ARRAY(
                master->server), task->client_ip);

    int2buff(master->server->id, resp->server_id);
    snprintf(resp->ip_addr, sizeof(resp->ip_addr), "%s",
            addr->conn.ip_addr);
    short2buff(addr->conn.port, resp->port);

    RESPONSE.header.body_len = sizeof(FDIRProtoGetServerResp);
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_GET_MASTER_RESP;
    TASK_ARG->context.response_done = true;

    return 0;
}

static int service_deal_get_slaves(struct fast_task_info *task)
{
    int result;
    FDIRProtoGetSlavesRespBodyHeader *body_header;
    FDIRProtoGetSlavesRespBodyPart *part_start;
    FDIRProtoGetSlavesRespBodyPart *body_part;
    FDIRClusterServerInfo *cs;
    FDIRClusterServerInfo *send;
    const FCAddressInfo *addr;

    if ((result=server_expect_body_length(task, 0)) != 0) {
        return result;
    }

    body_header = (FDIRProtoGetSlavesRespBodyHeader *)REQUEST.body;
    part_start = body_part = (FDIRProtoGetSlavesRespBodyPart *)(
            REQUEST.body + sizeof(FDIRProtoGetSlavesRespBodyHeader));

    send = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (cs=CLUSTER_SERVER_ARRAY.servers; cs<send; cs++) {
        if (cs->is_master) {
            continue;
        }

        int2buff(cs->server->id, body_part->server_id);
        body_part->status = cs->status;

        addr = fc_server_get_address_by_peer(&SERVICE_GROUP_ADDRESS_ARRAY(
                cs->server), task->client_ip);
        snprintf(body_part->ip_addr, sizeof(body_part->ip_addr),
                "%s", addr->conn.ip_addr);
        short2buff(addr->conn.port, body_part->port);

        body_part++;
    }
    int2buff(body_part - part_start, body_header->count);

    RESPONSE.header.body_len = (char *)body_part - REQUEST.body;
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_GET_SLAVES_RESP;
    TASK_ARG->context.response_done = true;

    return 0;
}

static FDIRClusterServerInfo *get_readable_server()
{
    int index;
    int old_index;
    int acc_index;
    FDIRClusterServerInfo *cs;
    FDIRClusterServerInfo *send;

    index = rand() % CLUSTER_SERVER_ARRAY.count;
    if (CLUSTER_SERVER_ARRAY.servers[index].status ==
            FDIR_SERVER_STATUS_ACTIVE)
    {
        return CLUSTER_SERVER_ARRAY.servers + index;
    }

    acc_index = 0;
    send = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    do {
        old_index = acc_index;
        for (cs=CLUSTER_SERVER_ARRAY.servers; cs<send; cs++) {
            if (cs->status == FDIR_SERVER_STATUS_ACTIVE) {
                if (acc_index++ == index) {
                    return cs;
                }
            }
        }
    } while (acc_index - old_index > 0);

    return NULL;
}

static int service_deal_get_readable_server(struct fast_task_info *task)
{
    FDIRClusterServerInfo *cs;
    FDIRProtoGetServerResp *resp;
    const FCAddressInfo *addr;

    if ((cs=get_readable_server()) == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "no active server");
        return ENOENT;
    }

    resp = (FDIRProtoGetServerResp *)REQUEST.body;
    addr = fc_server_get_address_by_peer(&SERVICE_GROUP_ADDRESS_ARRAY(
                cs->server), task->client_ip);

    int2buff(cs->server->id, resp->server_id);
    snprintf(resp->ip_addr, sizeof(resp->ip_addr), "%s",
            addr->conn.ip_addr);
    short2buff(addr->conn.port, resp->port);

    RESPONSE.header.body_len = sizeof(FDIRProtoGetServerResp);
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_GET_READABLE_SERVER_RESP;
    TASK_ARG->context.response_done = true;

    return 0;
}

static int server_parse_dentry_info(struct fast_task_info *task,
        char *start, FDIRDEntryFullName *fullname)
{
    FDIRProtoDEntryInfo *proto_dentry;

    proto_dentry = (FDIRProtoDEntryInfo *)start;
    fullname->ns.len = proto_dentry->ns_len;
    fullname->ns.str = proto_dentry->ns_str;
    fullname->path.len = buff2short(proto_dentry->path_len);
    fullname->path.str = proto_dentry->ns_str + fullname->ns.len;

    if (fullname->ns.len <= 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid namespace length: %d <= 0",
                fullname->ns.len);
        return EINVAL;
    }
    if (fullname->ns.len > NAME_MAX) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid namespace length: %d > %d",
                fullname->ns.len, NAME_MAX);
        return EINVAL;
    }

    if (fullname->path.len <= 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid path length: %d <= 0",
                fullname->path.len);
        return EINVAL;
    }
    if (fullname->path.len > PATH_MAX) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid path length: %d > %d",
                fullname->path.len, PATH_MAX);
        return EINVAL;
    }

    if (fullname->path.str[0] != '/') {
        RESPONSE.error.length = snprintf(
                RESPONSE.error.message,
                sizeof(RESPONSE.error.message),
                "invalid path: %.*s", fullname->path.len,
                fullname->path.str);
        return EINVAL;
    }

    return 0;
}

static int server_check_and_parse_dentry(struct fast_task_info *task,
        const int front_part_size, const int fixed_part_size,
        FDIRDEntryFullName *fullname)
{
    int result;
    int req_body_len;

    if ((result=server_check_body_length(task,
                    fixed_part_size + 1, fixed_part_size +
                    NAME_MAX + PATH_MAX)) != 0)
    {
        return result;
    }

    if ((result=server_parse_dentry_info(task, REQUEST.body +
                    front_part_size, fullname)) != 0)
    {
        return result;
    }

    req_body_len = fixed_part_size + fullname->ns.len +
        fullname->path.len;
    if (req_body_len != REQUEST.header.body_len) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "body length: %d != expect: %d",
                REQUEST.header.body_len, req_body_len);
        return EINVAL;
    }

    return 0;
}

static inline int alloc_record_object(struct fast_task_info *task)
{
    RECORD = (FDIRBinlogRecord *)fast_mblock_alloc_object(
            &((FDIRServerContext *)task->thread_data->arg)->
            service.record_allocator);
    if (RECORD == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "system busy, please try later");
        return EBUSY;
    }

    return 0;
}


static inline void free_record_object(struct fast_task_info *task)
{
    fast_mblock_free_object(&((FDIRServerContext *)task->thread_data->arg)->
            service.record_allocator, RECORD);
    RECORD = NULL;
}

static int server_binlog_produce(struct fast_task_info *task)
{
    ServerBinlogRecordBuffer *rbuffer;
    int result;

    if ((rbuffer=server_binlog_alloc_rbuffer()) == NULL) {
        return ENOMEM;
    }

    TASK_ARG->context.deal_func = NULL;
    rbuffer->data_version = RECORD->data_version;
    RECORD->timestamp = g_current_time;

    fast_buffer_reset(&rbuffer->buffer);
    result = binlog_pack_record(RECORD, &rbuffer->buffer);

    free_record_object(task);

    if (result == 0) {
        rbuffer->args = task;
        rbuffer->task_version = __sync_add_and_fetch(
                &((FDIRServerTaskArg *)task->arg)->task_version, 0);
        binlog_push_to_producer_queue(rbuffer);
        return SLAVE_SERVER_COUNT > 0 ? TASK_STATUS_CONTINUE : result;
    } else {
        server_binlog_free_rbuffer(rbuffer);
        return result;
    }
}

static inline void dentry_stat_output(struct fast_task_info *task,
        FDIRServerDentry *dentry)
{
    FDIRProtoStatDEntryResp *resp;

    resp = (FDIRProtoStatDEntryResp *)REQUEST.body;
    long2buff(dentry->inode, resp->inode);
    fdir_proto_pack_dentry_stat(&dentry->stat, &resp->stat);
    RESPONSE.header.body_len = sizeof(FDIRProtoStatDEntryResp);
    TASK_ARG->context.response_done = true;
}

static void record_deal_done_notify(FDIRBinlogRecord *record,
        const int result, const bool is_error)
{
    struct fast_task_info *task;

    task = (struct fast_task_info *)record->notify.args;
    if (result != 0) {
        int log_level;

        if (is_error) {
            log_level = LOG_ERR;
        } else {
            log_level = LOG_WARNING;
        }

        log_it_ex(&g_log_context, log_level,
                "file: "__FILE__", line: %d, "
                "client ip: %s, %s dentry fail, "
                "errno: %d, error info: %s, "
                "inode: %"PRId64", namespace: %.*s, path: %.*s",
                __LINE__, task->client_ip,
                get_operation_caption(record->operation),
                result, STRERROR(result), record->inode,
                record->fullname.ns.len, record->fullname.ns.str,
                record->fullname.path.len, record->fullname.path.str);
    } else {
        if (RESPONSE.header.cmd == FDIR_SERVICE_PROTO_CREATE_DENTRY_RESP ||
                RESPONSE.header.cmd == FDIR_SERVICE_PROTO_CREATE_BY_PNAME_RESP||
                RESPONSE.header.cmd == FDIR_SERVICE_PROTO_REMOVE_DENTRY_RESP)
        {
            dentry_stat_output(task, record->dentry);
        }
    }

    RESPONSE_STATUS = result;
    sf_nio_notify(task, SF_NIO_STAGE_CONTINUE);
}

static int handle_record_deal_done(struct fast_task_info *task)
{
    if (RESPONSE_STATUS == 0) {
        return server_binlog_produce(task);
    } else {
        return RESPONSE_STATUS;
    }
}

static inline int push_record_to_data_thread_queue(struct fast_task_info *task)
{
    int result;

    RECORD->notify.func = record_deal_done_notify; //call by data thread
    RECORD->notify.args = task;

    TASK_ARG->context.deal_func = handle_record_deal_done;
    result = push_to_data_thread_queue(RECORD);
    return result == 0 ? TASK_STATUS_CONTINUE : result;
}

static inline void service_init_record(struct fast_task_info *task)
{
    RECORD->inode = RECORD->data_version = 0;
    RECORD->options.flags = 0;
    RECORD->options.path_info.flags = BINLOG_OPTIONS_PATH_ENABLED;
    RECORD->hash_code = simple_hash(RECORD->fullname.ns.str,
            RECORD->fullname.ns.len);
}

static void service_set_record_path_info(struct fast_task_info *task,
        const int reserved_size)
{
    char *p;
    int length;

    service_init_record(task);
    length = RECORD->fullname.ns.len + RECORD->fullname.path.len;
    if (REQUEST.header.body_len > reserved_size) {
        if ((REQUEST.header.body_len + length) < task->size) {
            p = REQUEST.body + REQUEST.header.body_len;
            memcpy(p, RECORD->fullname.ns.str, length);
        } else {
            p = REQUEST.body + reserved_size;
            memmove(p, RECORD->fullname.ns.str, length);
        }
    } else {
        p = REQUEST.body + reserved_size;
        memcpy(p, RECORD->fullname.ns.str, length);
    }
    RECORD->fullname.ns.str = p;
    RECORD->fullname.path.str = p + RECORD->fullname.ns.len;
}

static void init_record_for_create(struct fast_task_info *task,
        const char *proto_mode)
{
    RECORD->stat.mode = buff2int(proto_mode);
    RECORD->operation = BINLOG_OP_CREATE_DENTRY_INT;
    RECORD->stat.ctime = RECORD->stat.mtime = g_current_time;
    RECORD->options.ctime = RECORD->options.mtime = 1;
    RECORD->options.mode = 1;
}

static int service_deal_create_dentry(struct fast_task_info *task)
{
    int result;

    if ((result=alloc_record_object(task)) != 0) {
        return result;
    }

    if ((result=server_check_and_parse_dentry(task,
                    sizeof(FDIRProtoCreateDEntryFront),
                    sizeof(FDIRProtoCreateDEntryBody),
                    &RECORD->fullname)) != 0)
    {
        free_record_object(task);
        return result;
    }

    service_set_record_path_info(task, sizeof(FDIRProtoStatDEntryResp));
    init_record_for_create(task, ((FDIRProtoCreateDEntryFront *)
                REQUEST.body)->mode);
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_CREATE_DENTRY_RESP;
    return push_record_to_data_thread_queue(task);
}

static inline int check_name_length(struct fast_task_info *task,
        const int length, const char *caption)
{
    if (length <= 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid %s length: %d <= 0",
                caption, length);
        return EINVAL;
    }
    if (length > NAME_MAX) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid %s length: %d > %d",
                caption, length, NAME_MAX);
        return EINVAL;
    }
    return 0;
}

static int service_deal_create_dentry_by_pname(struct fast_task_info *task)
{
    int result;
    FDIRProtoCreateDEntryByPNameReq *req;
    FDIRServerDentry *parent_dentry;
    int64_t parent_inode;
    char *ns_str;
    char *req_name_str;
    BufferInfo full_path;

    if ((result=server_check_body_length(task, sizeof(
                        FDIRProtoCreateDEntryByPNameReq) + 2,
                    sizeof(FDIRProtoCreateDEntryByPNameReq) +
                    2 * NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FDIRProtoCreateDEntryByPNameReq *)REQUEST.body;
    if ((result=check_name_length(task, req->ns_len, "namespace")) != 0) {
        return result;
    }
    if ((result=check_name_length(task, req->name_len, "path name")) != 0) {
        return result;
    }
    if (sizeof(FDIRProtoCreateDEntryByPNameReq) + req->ns_len +
            req->name_len != REQUEST.header.body_len)
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "body length: %d != expected: %d",
                REQUEST.header.body_len,
                (int)sizeof(FDIRProtoCreateDEntryByPNameReq) +
                req->ns_len + req->name_len);
        return EINVAL;
    }

    parent_inode = buff2long(req->parent_inode);
    if ((parent_dentry=inode_index_get_dentry(parent_inode)) == NULL) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "parent inode: %"PRId64" not exist", parent_inode);
        return ENOENT;
    }

    ns_str = REQUEST.body + sizeof(FDIRProtoStatDEntryResp) +
        REQUEST.header.body_len;
    full_path.buff = ns_str + req->ns_len;
    full_path.alloc_size = (task->data + task->size) - full_path.buff;
    full_path.length = 0;
    if ((result=dentry_get_full_path(parent_dentry, &full_path,
                    &RESPONSE.error)) != 0)
    {
        return result;
    }

    if (full_path.length + req->name_len + 1 >= full_path.alloc_size) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "path length: %d >= buff size: %d",
                full_path.length + req->name_len + 1,
                full_path.alloc_size);
        return ENOSPC;
    }

    req_name_str = req->ns_str + req->ns_len;
    memcpy(ns_str, req->ns_str, req->ns_len);
    full_path.length += sprintf(full_path.buff + full_path.length,
            "/%.*s", req->name_len, req_name_str);

    if ((result=alloc_record_object(task)) != 0) {
        return result;
    }

    RECORD->fullname.ns.str = ns_str;
    RECORD->fullname.ns.len = req->ns_len;
    RECORD->fullname.path.str = full_path.buff;
    RECORD->fullname.path.len = full_path.length;

    logInfo("file: "__FILE__", line: %d, "
            "ns: %.*s, path: %.*s", __LINE__, RECORD->fullname.ns.len,
            RECORD->fullname.ns.str, RECORD->fullname.path.len,
            RECORD->fullname.path.str);

    service_init_record(task);
    init_record_for_create(task, req->mode);
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_CREATE_BY_PNAME_RESP;
    return push_record_to_data_thread_queue(task);
}

static int service_deal_remove_dentry(struct fast_task_info *task)
{
    int result;

    if ((result=alloc_record_object(task)) != 0) {
        return result;
    }

    if ((result=server_check_and_parse_dentry(task,
                    0, sizeof(FDIRProtoRemoveDEntry),
                    &RECORD->fullname)) != 0)
    {
        free_record_object(task);
        return result;
    }

    service_set_record_path_info(task, sizeof(FDIRProtoStatDEntryResp));
    RECORD->operation = BINLOG_OP_REMOVE_DENTRY_INT;
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_REMOVE_DENTRY_RESP;
    return push_record_to_data_thread_queue(task);
}

static int service_deal_stat_dentry_by_path(struct fast_task_info *task)
{
    int result;
    FDIRDEntryFullName fullname;
    FDIRServerDentry *dentry;

    if ((result=server_check_and_parse_dentry(task, 0,
                    sizeof(FDIRProtoDEntryInfo), &fullname)) != 0)
    {
        return result;
    }

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_STAT_BY_PATH_RESP;
    if ((result=dentry_find(&fullname, &dentry)) != 0) {
        return result;
    }

    dentry_stat_output(task, dentry);
    return 0;
}

static int service_deal_lookup_inode(struct fast_task_info *task)
{
    int result;
    FDIRDEntryFullName fullname;
    FDIRServerDentry *dentry;
    FDIRProtoLookupInodeResp *resp;

    if ((result=server_check_and_parse_dentry(task, 0,
                    sizeof(FDIRProtoDEntryInfo), &fullname)) != 0)
    {
        return result;
    }

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_LOOKUP_INODE_RESP;
    if ((result=dentry_find(&fullname, &dentry)) != 0) {
        return result;
    }

    resp = (FDIRProtoLookupInodeResp *)REQUEST.body;
    long2buff(dentry->inode, resp->inode);
    RESPONSE.header.body_len = sizeof(FDIRProtoLookupInodeResp);
    TASK_ARG->context.response_done = true;
    return 0;
}

static inline int server_check_and_parse_inode(
        struct fast_task_info *task, int64_t *inode)
{
    int result;

    if ((result=server_expect_body_length(task, 8)) != 0) {
        return result;
    }

    *inode = buff2long(REQUEST.body);
    return 0;
}

static int service_deal_stat_dentry_by_inode(struct fast_task_info *task)
{
    FDIRServerDentry *dentry;
    int64_t inode;
    int result;

    if ((result=server_check_and_parse_inode(task, &inode)) != 0) {
        return result;
    }

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_STAT_BY_INODE_RESP;
    if ((dentry=inode_index_get_dentry(inode)) == NULL) {
        return ENOENT;
    }

    dentry_stat_output(task, dentry);
    return 0;
}

static int service_deal_stat_dentry_by_pname(struct fast_task_info *task)
{
    FDIRProtoStatDEntryByPNameReq *req;
    FDIRServerDentry *dentry;
    int64_t parent_inode;
    string_t name;
    int result;

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_STAT_BY_PNAME_RESP;
    if ((result=server_check_body_length(task, sizeof(
                        FDIRProtoStatDEntryByPNameReq) + 1,
                    sizeof(FDIRProtoStatDEntryByPNameReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FDIRProtoStatDEntryByPNameReq *)REQUEST.body;
    if (sizeof(FDIRProtoStatDEntryByPNameReq) + req->name_len !=
            REQUEST.header.body_len)
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "body length: %d != expected: %d",
                REQUEST.header.body_len, (int)sizeof(
                    FDIRProtoStatDEntryByPNameReq) + req->name_len);
        return EINVAL;
    }

    parent_inode = buff2long(req->parent_inode);
    name.str = req->name_str;
    name.len = req->name_len;
    if ((dentry=inode_index_get_dentry_by_pname(parent_inode, &name)) == NULL) {
        return ENOENT;
    }

    dentry_stat_output(task, dentry);
    return 0;
}

static FDIRServerDentry *set_dentry_size(struct fast_task_info *task,
        const char *ns_str, const int ns_len, const int64_t inode,
        const int64_t file_size, const bool force, int *result)
{
    int modified_flags;
    FDIRServerDentry *dentry;

    if ((*result=alloc_record_object(task)) != 0) {
        return NULL;
    }

    if ((dentry=inode_index_check_set_dentry_size(inode,
                    file_size, force, &modified_flags)) == NULL)
    {
        free_record_object(task);
        *result = ENOENT;
        return NULL;
    }

    if (modified_flags == 0) {  //no fields changed
        free_record_object(task);
        *result = 0;
        return dentry;
    }

    RECORD->inode = inode;
    RECORD->dentry = dentry;
    RECORD->hash_code = simple_hash(ns_str, ns_len);
    RECORD->options.flags = 0;
    if ((modified_flags & FDIR_DENTRY_FIELD_MODIFIED_FLAG_SIZE)) {
        RECORD->options.size = 1;
        RECORD->stat.size = RECORD->dentry->stat.size;
    }
    if ((modified_flags & FDIR_DENTRY_FIELD_MODIFIED_FLAG_MTIME)) {
        RECORD->options.mtime = 1;
        RECORD->stat.mtime = RECORD->dentry->stat.mtime;
    }
    RECORD->operation = BINLOG_OP_UPDATE_DENTRY_INT;
    RECORD->data_version = __sync_add_and_fetch(&DATA_CURRENT_VERSION, 1);

    *result = server_binlog_produce(task);
    return dentry;
}

static int service_deal_set_dentry_size(struct fast_task_info *task)
{
    FDIRProtoSetDentrySizeReq *req;
    FDIRServerDentry *dentry;
    int result;
    int64_t inode;
    int64_t file_size;

    if ((result=server_check_body_length(task,
                    sizeof(FDIRProtoSetDentrySizeReq) + 1,
                    sizeof(FDIRProtoSetDentrySizeReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FDIRProtoSetDentrySizeReq *)REQUEST.body;
    if (req->ns_len <= 0) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "namespace length: %d is invalid which <= 0",
                req->ns_len);
        return EINVAL;
    }
    if (sizeof(FDIRProtoSetDentrySizeReq) + req->ns_len !=
            REQUEST.header.body_len)
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "body length: %d != expected: %d",
                REQUEST.header.body_len, (int)sizeof(
                    FDIRProtoSetDentrySizeReq) + req->ns_len);
        return EINVAL;
    }

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_SET_DENTRY_SIZE_RESP;
    inode = buff2long(req->inode);
    file_size = buff2long(req->size);

    dentry = set_dentry_size(task, req->ns_str, req->ns_len, inode,
            file_size, req->force, &result);
    if (result == 0 || result == TASK_STATUS_CONTINUE) {
        if (dentry != NULL) {
            dentry_stat_output(task, dentry);
        }
    }

    return result;
}

static FDIRServerDentry *modify_dentry_stat(struct fast_task_info *task,
        const char *ns_str, const int ns_len, const int64_t inode,
        const int64_t flags, const FDIRDEntryStatus *stat, int *result)
{
    FDIRServerDentry *dentry;

    if ((*result=alloc_record_object(task)) != 0) {
        return NULL;
    }

    RECORD->inode = inode;
    RECORD->options.flags = flags;
    RECORD->stat = *stat;
    RECORD->hash_code = simple_hash(ns_str, ns_len);
    RECORD->operation = BINLOG_OP_UPDATE_DENTRY_INT;

    if ((dentry=inode_index_update_dentry(RECORD)) == NULL) {
        free_record_object(task);
        *result = ENOENT;
        return NULL;
    }

    RECORD->dentry = dentry;
    RECORD->data_version = __sync_add_and_fetch(&DATA_CURRENT_VERSION, 1);
    *result = server_binlog_produce(task);
    return dentry;
}

static int service_deal_modify_dentry_stat(struct fast_task_info *task)
{
    FDIRProtoModifyDentryStatReq *req;
    FDIRServerDentry *dentry;
    FDIRDEntryStatus stat;
    int64_t inode;
    int64_t flags;
    int64_t masked_flags;
    int result;

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_MODIFY_DENTRY_STAT_RESP;
    if ((result=server_check_body_length(task,
                    sizeof(FDIRProtoModifyDentryStatReq) + 1,
                    sizeof(FDIRProtoModifyDentryStatReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FDIRProtoModifyDentryStatReq *)REQUEST.body;
    if (req->ns_len <= 0) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "namespace length: %d is invalid which <= 0",
                req->ns_len);
        return EINVAL;
    }
    if (sizeof(FDIRProtoModifyDentryStatReq) + req->ns_len !=
            REQUEST.header.body_len)
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "body length: %d != expected: %d",
                REQUEST.header.body_len, (int)sizeof(
                    FDIRProtoModifyDentryStatReq) + req->ns_len);
        return EINVAL;
    }

    inode = buff2long(req->inode);
    flags = buff2long(req->mflags);
    masked_flags = (flags & dstat_mflags_mask);

    if (masked_flags == 0) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "invalid flags: %"PRId64, flags);
        return EINVAL;
    }

    fdir_proto_unpack_dentry_stat(&req->stat, &stat);
    dentry = modify_dentry_stat(task, req->ns_str, req->ns_len,
            inode, masked_flags, &stat, &result);

    /*
    logInfo("file: "__FILE__", line: %d, "
            "flags: %"PRId64" (0x%llX), masked_flags: %"PRId64", result: %d",
            __LINE__, flags, flags, masked_flags, result);
            */

    if (result == 0 || result == TASK_STATUS_CONTINUE) {
        if (dentry != NULL) {
            dentry_stat_output(task, dentry);
        }
    }

    return result;
}

static int compare_flock_task(FLockTask *flck, const FlockOwner *owner,
        const int64_t inode, const int64_t offset, const int64_t length)
{
    int sub;
    if ((sub=flck->owner.pid - owner->pid) != 0) {
        return sub;
    }

    if ((sub=fc_compare_int64(flck->owner.tid, owner->tid)) != 0) {
        return sub;
    }

    if ((sub=fc_compare_int64(flck->dentry->inode, inode)) != 0) {
        return sub;
    }

    if ((sub=fc_compare_int64(flck->region->offset, offset)) != 0) {
        return sub;
    }

    if ((sub=fc_compare_int64(flck->region->length, length)) != 0) {
        return sub;
    }

    return 0;
}

static int flock_unlock_dentry(struct fast_task_info *task,
        const FlockOwner *owner, const int64_t inode, const int64_t offset,
        const int64_t length)
{
    FLockTask *flck;
    fc_list_for_each_entry(flck, FTASK_HEAD_PTR, clink) {
        logInfo("==type: %d, which_queue: %d, inode: %"PRId64", offset: %"PRId64", length: %"PRId64", "
            "owner.tid: %"PRId64", owner.pid: %d", flck->type, flck->which_queue, flck->dentry->inode,
            flck->region->offset, flck->region->length, flck->owner.tid, flck->owner.pid);

        if (flck->which_queue != FDIR_FLOCK_TASK_IN_LOCKED_QUEUE) {
            continue;
        }

        if (compare_flock_task(flck, owner, inode, offset, length) == 0) {
            release_flock_task(task, flck);
            return 0;
        }
    }

    return ENOENT;
}

static int service_deal_flock_dentry(struct fast_task_info *task)
{
    FDIRProtoFlockDEntryReq *req;
    FLockTask *ftask;
    int result;
    short type;
    FlockOwner owner;
    int64_t inode;
    int64_t offset;
    int64_t length;
    short operation;

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_FLOCK_DENTRY_RESP;
    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoFlockDEntryReq))) != 0)
    {
        return result;
    }

    req = (FDIRProtoFlockDEntryReq *)REQUEST.body;
    inode = buff2long(req->inode);
    offset = buff2long(req->offset);
    length = buff2long(req->length);
    owner.tid = buff2long(req->owner.tid);
    owner.pid = buff2int(req->owner.pid);
    operation = buff2int(req->operation);

    logInfo("file: "__FILE__", line: %d, "
            "operation: %d, inode: %"PRId64", offset: %"PRId64", length: %"PRId64", "
            "owner.tid: %"PRId64", owner.pid: %d", __LINE__, operation, inode,
            offset, length, owner.tid, owner.pid);

    if (operation & LOCK_UN) {
        return flock_unlock_dentry(task, &owner, inode, offset, length);
    }

    if (operation & LOCK_EX) {
        type = LOCK_EX;
    } else if (operation & LOCK_SH) {
        type = LOCK_SH;
    } else {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid operation: %d", operation);
        return EINVAL;
    }

    if ((ftask=inode_index_flock_apply(inode, type, offset, length,
                    (operation & LOCK_NB) == 0, &owner, task,
                    &result)) == NULL)
    {
        return result;
    }

    logInfo("file: "__FILE__", line: %d, "
            "===operation: %d, inode: %"PRId64", offset: %"PRId64", length: %"PRId64", "
            "owner.tid: %"PRId64", owner.pid: %d, result: %d", __LINE__, operation, inode,
            offset, length, owner.tid, owner.pid, result);

    fc_list_add_tail(&ftask->clink, FTASK_HEAD_PTR);
    return result == 0 ? 0 : TASK_STATUS_CONTINUE;
}

static int service_deal_getlk_dentry(struct fast_task_info *task)
{
    FDIRProtoGetlkDEntryReq *req;
    FDIRProtoGetlkDEntryResp *resp;
    FLockTask ftask;
    int result;
    short type;
    int64_t inode;
    int64_t offset;
    int64_t length;
    short operation;
    FLockRegion region;

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_GETLK_DENTRY_RESP;
    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoGetlkDEntryReq))) != 0)
    {
        return result;
    }

    req = (FDIRProtoGetlkDEntryReq *)REQUEST.body;
    inode = buff2long(req->inode);
    offset = buff2long(req->offset);
    length = buff2long(req->length);
    operation = buff2int(req->operation);

    logInfo("file: "__FILE__", line: %d, "
            "operation: %d, inode: %"PRId64", "
            "offset: %"PRId64", length: %"PRId64, 
            __LINE__, operation, inode, offset, length);

    if (operation & LOCK_EX) {
        type = LOCK_EX;
    } else if (operation & LOCK_SH) {
        type = LOCK_SH;
    } else {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid operation: %d", operation);
        return EINVAL;
    }

    memset(&region, 0, sizeof(region));
    region.offset = offset;
    region.length = length;
    ftask.region = &region;  //for region compare
    if ((result=inode_index_flock_getlk(inode, &ftask)) == 0) {
        resp = (FDIRProtoGetlkDEntryResp *)REQUEST.body;
        int2buff(ftask.type, resp->type);
        long2buff(ftask.region->offset, resp->offset);
        long2buff(ftask.region->length, resp->length);
        long2buff(ftask.owner.tid, resp->owner.tid);
        int2buff(ftask.owner.pid, resp->owner.pid);
    }

    return result;
}

static void sys_lock_dentry_output(struct fast_task_info *task,
        FDIRServerDentry *dentry)
{
    FDIRProtoSysLockDEntryResp *resp;
    resp = (FDIRProtoSysLockDEntryResp *)REQUEST.body;

    long2buff(dentry->stat.size, resp->size);
    RESPONSE.header.body_len = sizeof(FDIRProtoSysLockDEntryResp);
    TASK_ARG->context.response_done = true;
}

static int handle_sys_lock_done(struct fast_task_info *task)
{
    if (SYS_LOCK_TASK == NULL) {
        return ENOENT;
    } else {
        logInfo("file: "__FILE__", line: %d, func: %s, "
                "inode: %"PRId64", file size: %"PRId64,
                __LINE__, __FUNCTION__,
                SYS_LOCK_TASK->dentry->inode,
                SYS_LOCK_TASK->dentry->stat.size);

        sys_lock_dentry_output(task, SYS_LOCK_TASK->dentry);
        return 0;
    }
}

static int service_deal_sys_lock_dentry(struct fast_task_info *task)
{
    FDIRProtoSysLockDEntryReq *req;
    int result;
    int flags;
    int64_t inode;

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_SYS_LOCK_DENTRY_RESP;
    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoSysLockDEntryReq))) != 0)
    {
        return result;
    }

    if (SYS_LOCK_TASK != NULL) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "sys lock already exist, locked inode: %"PRId64,
                SYS_LOCK_TASK->dentry->inode);
        return EEXIST;
    }

    req = (FDIRProtoSysLockDEntryReq *)REQUEST.body;
    inode = buff2long(req->inode);
    flags = buff2int(req->flags);

    if ((SYS_LOCK_TASK=inode_index_sys_lock_apply(inode, (flags & LOCK_NB) == 0,
                    task, &result)) == NULL)
    {
        return result;
    }

    if (result == 0) {
        sys_lock_dentry_output(task, SYS_LOCK_TASK->dentry);
        return 0;
    } else {
        logInfo("file: "__FILE__", line: %d, func: %s, "
                "waiting lock for inode: %"PRId64, __LINE__,
                __FUNCTION__, SYS_LOCK_TASK->dentry->inode);

        TASK_ARG->context.deal_func = handle_sys_lock_done;
        return TASK_STATUS_CONTINUE;
    }
}

static void on_sys_lock_release(FDIRServerDentry *dentry, void *args)
{
    struct fast_task_info *task;
    FDIRProtoSysUnlockDEntryReq *req;
    int64_t new_size;
    int result;

    task = (struct fast_task_info *)args;
    req = (FDIRProtoSysUnlockDEntryReq *)REQUEST.body;
    new_size = buff2long(req->new_size);
    set_dentry_size(task, req->ns_str, req->ns_len,
            SYS_LOCK_TASK->dentry->inode,
            new_size, req->force, &result);
    RESPONSE_STATUS = result;
}

static int service_deal_sys_unlock_dentry(struct fast_task_info *task)
{
    FDIRProtoSysUnlockDEntryReq *req;
    int result;
    int flags;
    int64_t inode;
    int64_t old_size;
    int64_t new_size;
    sys_lock_release_callback callback;

    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_SYS_UNLOCK_DENTRY_RESP;
    if ((result=server_check_body_length(task,
                    sizeof(FDIRProtoSysUnlockDEntryReq),
                    sizeof(FDIRProtoSysUnlockDEntryReq) + NAME_MAX)) != 0)
    {
        return result;
    }

    req = (FDIRProtoSysUnlockDEntryReq *)REQUEST.body;
    if (sizeof(FDIRProtoSysUnlockDEntryReq) + req->ns_len !=
            REQUEST.header.body_len)
    {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "body length: %d != expected: %d",
                REQUEST.header.body_len, (int)sizeof(
                    FDIRProtoSysUnlockDEntryReq) + req->ns_len);
        return EINVAL;
    }

    if (SYS_LOCK_TASK == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "sys lock not exist");
        return ENOENT;
    }

    inode = buff2long(req->inode);
    if (inode != SYS_LOCK_TASK->dentry->inode) {
        RESPONSE.error.length = sprintf(RESPONSE.error.message,
                "sys lock check fail, req inode: %"PRId64", "
                "expect: %"PRId64, inode, SYS_LOCK_TASK->dentry->inode);
        return EINVAL;
    }
    flags = buff2int(req->flags);

    if ((flags & FDIR_PROTO_SYS_UNLOCK_FLAGS_SET_SIZE)) {
        if (req->ns_len <= 0) {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "namespace length: %d is invalid which <= 0",
                    req->ns_len);
            return ENOENT;
        }

        old_size = buff2long(req->old_size);
        new_size = buff2long(req->new_size);
        if (old_size != SYS_LOCK_TASK->dentry->stat.size) {
            logWarning("file: "__FILE__", line: %d, "
                    "client ip: %s, inode: %"PRId64", old size: %"PRId64
                    ", != current size: %"PRId64", maybe changed by others",
                    __LINE__, task->client_ip, inode, old_size,
                    SYS_LOCK_TASK->dentry->stat.size);
        }
        if (new_size < 0) {
            RESPONSE.error.length = sprintf(RESPONSE.error.message,
                    "invalid new file size: %"PRId64" which < 0", new_size);
            return EINVAL;
        }
        callback = on_sys_lock_release;
    } else {
        callback = NULL;
    }

    if ((RESPONSE_STATUS=inode_index_sys_lock_release_ex(
                    SYS_LOCK_TASK, callback, task)) != 0)
    {
        return RESPONSE_STATUS;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "callback: %p, status: %d", __LINE__,
            __FUNCTION__, callback, RESPONSE_STATUS);

    SYS_LOCK_TASK = NULL;
    return RESPONSE_STATUS;  //status set by the callback
}

static int server_list_dentry_output(struct fast_task_info *task)
{
    FDIRProtoListDEntryRespBodyHeader *body_header;
    FDIRServerDentry **dentry;
    FDIRServerDentry **start;
    FDIRServerDentry **end;
    FDIRProtoListDEntryRespBodyPart *body_part;
    char *p;
    char *buf_end;
    int remain_count;
    int count;

    remain_count = DENTRY_LIST_CACHE.array.count -
        DENTRY_LIST_CACHE.offset;

    buf_end = task->data + task->size;
    p = REQUEST.body + sizeof(FDIRProtoListDEntryRespBodyHeader);
    start = DENTRY_LIST_CACHE.array.entries +
        DENTRY_LIST_CACHE.offset;
    end = start + remain_count;
    for (dentry=start; dentry<end; dentry++) {
        if (buf_end - p < sizeof(FDIRProtoListDEntryRespBodyPart) +
                (*dentry)->name.len)
        {
            break;
        }
        body_part = (FDIRProtoListDEntryRespBodyPart *)p;
        body_part->name_len = (*dentry)->name.len;
        memcpy(body_part->name_str, (*dentry)->name.str, (*dentry)->name.len);
        p += sizeof(FDIRProtoListDEntryRespBodyPart) + (*dentry)->name.len;
    }
    count = dentry - start;
    RESPONSE.header.body_len = p - REQUEST.body;
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_LIST_DENTRY_RESP;

    body_header = (FDIRProtoListDEntryRespBodyHeader *)REQUEST.body;
    int2buff(count, body_header->count);
    if (count < remain_count) {
        DENTRY_LIST_CACHE.offset += count;
        DENTRY_LIST_CACHE.expires = g_current_time + 60;
        DENTRY_LIST_CACHE.token = __sync_add_and_fetch(&next_token, 1);

        body_header->is_last = 0;
        long2buff(DENTRY_LIST_CACHE.token, body_header->token);
    } else {
        body_header->is_last = 1;
        long2buff(0, body_header->token);
    }

    TASK_ARG->context.response_done = true;
    return 0;
}

static int service_deal_list_dentry_first(struct fast_task_info *task)
{
    int result;
    FDIRDEntryFullName fullname;

    if ((result=server_check_and_parse_dentry(task,
                    0, sizeof(FDIRProtoListDEntryFirstBody),
                    &fullname)) != 0)
    {
        return result;
    }

    if ((result=dentry_list(&fullname, &DENTRY_LIST_CACHE.array)) != 0) {
        return result;
    }

    DENTRY_LIST_CACHE.offset = 0;
    return server_list_dentry_output(task);
}

static int service_deal_list_dentry_next(struct fast_task_info *task)
{
    FDIRProtoListDEntryNextBody *next_body;
    int result;
    int offset;
    int64_t token;

    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoListDEntryNextBody))) != 0)
    {
        return result;
    }

    if (DENTRY_LIST_CACHE.expires < g_current_time) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "dentry list cache expires, please try again");
        return ETIMEDOUT;
    }

    next_body = (FDIRProtoListDEntryNextBody *)REQUEST.body;
    token = buff2long(next_body->token);
    offset = buff2int(next_body->offset);
    if (token != DENTRY_LIST_CACHE.token) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid token for next list");
        return EINVAL;
    }
    if (offset != DENTRY_LIST_CACHE.offset) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "next list offset: %d != expected: %d",
                offset, DENTRY_LIST_CACHE.offset);
        return EINVAL;
    }
    return server_list_dentry_output(task);
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

    REQUEST.header.cmd = ((FDIRProtoHeader *)task->data)->cmd;
    REQUEST.header.body_len = task->length - sizeof(FDIRProtoHeader);
    REQUEST.body = task->data + sizeof(FDIRProtoHeader);
}

static inline int service_check_master(struct fast_task_info *task)
{
    if (CLUSTER_MYSELF_PTR != CLUSTER_MASTER_PTR) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "i am not master");
        return EINVAL;
    }

    return 0;
}

static inline int service_check_readable(struct fast_task_info *task)
{
    if (!(CLUSTER_MYSELF_PTR == CLUSTER_MASTER_PTR ||
                CLUSTER_MYSELF_PTR->status == FDIR_SERVER_STATUS_ACTIVE))
    {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "i am not active");
        return EINVAL;
    }

    return 0;
}

static int deal_task_done(struct fast_task_info *task)
{
    FDIRProtoHeader *proto_header;
    int r;
    int time_used;
    char time_buff[32];

    if (TASK_ARG->context.log_error && RESPONSE.error.length > 0) {
        logError("file: "__FILE__", line: %d, "
                "client ip: %s, cmd: %d (%s), req body length: %d, %s",
                __LINE__, task->client_ip, REQUEST.header.cmd,
                fdir_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len,
                RESPONSE.error.message);
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
    if (time_used > 20 * 1000) {
        lwarning("process a request timed used: %s us, "
                "cmd: %d (%s), req body len: %d, resp body len: %d",
                long_to_comma_str(time_used, time_buff),
                REQUEST.header.cmd, fdir_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len, RESPONSE.header.body_len);
    }

    if (REQUEST.header.cmd != FDIR_CLUSTER_PROTO_PING_MASTER_REQ) {
    logInfo("file: "__FILE__", line: %d, "
            "client ip: %s, req cmd: %d (%s), req body_len: %d, "
            "resp cmd: %d (%s), status: %d, resp body_len: %d, "
            "time used: %s us", __LINE__,
            task->client_ip, REQUEST.header.cmd,
            fdir_get_cmd_caption(REQUEST.header.cmd),
            REQUEST.header.body_len, RESPONSE.header.cmd,
            fdir_get_cmd_caption(RESPONSE.header.cmd),
            RESPONSE_STATUS, RESPONSE.header.body_len,
            long_to_comma_str(time_used, time_buff));
    }

    return r == 0 ? RESPONSE_STATUS : r;
}

int service_deal_task(struct fast_task_info *task)
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
                result = service_deal_actvie_test(task);
                break;
            case FDIR_SERVICE_PROTO_CREATE_DENTRY_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_create_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_CREATE_BY_PNAME_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_create_dentry_by_pname(task);
                }
                break;
            case FDIR_SERVICE_PROTO_REMOVE_DENTRY_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_remove_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_SET_DENTRY_SIZE_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_set_dentry_size(task);
                }
                break;
            case FDIR_SERVICE_PROTO_MODIFY_DENTRY_STAT_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_modify_dentry_stat(task);
                }
                break;
            case FDIR_SERVICE_PROTO_LOOKUP_INODE_REQ:
                if ((result=service_check_readable(task)) == 0) {
                    result = service_deal_lookup_inode(task);
                }
                break;
            case FDIR_SERVICE_PROTO_STAT_BY_PATH_REQ:
                if ((result=service_check_readable(task)) == 0) {
                    result = service_deal_stat_dentry_by_path(task);
                }
                break;
            case FDIR_SERVICE_PROTO_STAT_BY_INODE_REQ:
                if ((result=service_check_readable(task)) == 0) {
                    result = service_deal_stat_dentry_by_inode(task);
                }
                break;
            case FDIR_SERVICE_PROTO_STAT_BY_PNAME_REQ:
                if ((result=service_check_readable(task)) == 0) {
                    result = service_deal_stat_dentry_by_pname(task);
                }
                break;
            case FDIR_SERVICE_PROTO_LIST_DENTRY_FIRST_REQ:
                if ((result=service_check_readable(task)) == 0) {
                    result = service_deal_list_dentry_first(task);
                }
                break;
            case FDIR_SERVICE_PROTO_LIST_DENTRY_NEXT_REQ:
                if ((result=service_check_readable(task)) == 0) {
                    result = service_deal_list_dentry_next(task);
                }
                break;
            case FDIR_SERVICE_PROTO_FLOCK_DENTRY_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_flock_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_GETLK_DENTRY_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_getlk_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_SYS_LOCK_DENTRY_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_sys_lock_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_SYS_UNLOCK_DENTRY_REQ:
                if ((result=service_check_master(task)) == 0) {
                    result = service_deal_sys_unlock_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_SERVICE_STAT_REQ:
                result = service_deal_service_stat(task);
                break;
            case FDIR_SERVICE_PROTO_CLUSTER_STAT_REQ:
                result = service_deal_cluster_stat(task);
                break;
            case FDIR_SERVICE_PROTO_GET_MASTER_REQ:
                result = service_deal_get_master(task);
                break;
            case FDIR_SERVICE_PROTO_GET_SLAVES_REQ:
                result = service_deal_get_slaves(task);
                break;
            case FDIR_SERVICE_PROTO_GET_READABLE_SERVER_REQ:
                result = service_deal_get_readable_server(task);
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

void *service_alloc_thread_extra_data(const int thread_index)
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
    if (fast_mblock_init_ex2(&server_context->service.record_allocator,
                "binlog_record1", sizeof(FDIRBinlogRecord), 4 * 1024,
                NULL, NULL, false, NULL, NULL, NULL) != 0)
    {
        free(server_context);
        return NULL;
    }

    return server_context;
}
