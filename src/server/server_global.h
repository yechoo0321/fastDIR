
#ifndef _FDIR_SERVER_GLOBAL_H
#define _FDIR_SERVER_GLOBAL_H

#include "fastcommon/common_define.h"
#include "fastcommon/server_id_func.h"
#include "sf/sf_global.h"
#include "server_types.h"

typedef struct server_global_vars {
    struct {
        string_t username;
        string_t secret_key;
    } admin;

    int namespace_hashtable_capacity;

    int dentry_max_data_size;

    int reload_interval_ms;

    int check_alive_interval;

    struct {
        short id;  //cluster id for generate inode
        FDIRClusterServerInfo *master;
        FDIRClusterServerInfo *myself;
        struct {
            FCServerConfig ctx;
            unsigned char md5_digest[16];
            int cluster_group_index;
            int service_group_index;
        } config;

        FDIRClusterServerArray server_array;

        SFContext sf_context;  //for cluster communication
    } cluster;

    struct {
        struct {
            int64_t cluster;      //cluster id part
            volatile int64_t sn;  //sn part
        } generator;

        struct {
            int shared_locks_count;
            int64_t hashtable_capacity;
        } entries;
    } inode;

    struct {
        volatile uint64_t current_version; //binlog version
        string_t path;   //data path
        int binlog_buffer_size;
        int thread_count;
    } data;

    /*
    struct {
        char key[FDIR_REPLICA_KEY_SIZE];   //slave distribute to master
    } replica;
    */
} FDIRServerGlobalVars;

#define CLUSTER_CONFIG_CTX      g_server_global_vars.cluster.config.ctx

#define CLUSTER_MYSELF_PTR      g_server_global_vars.cluster.myself
#define MYSELF_IS_MASTER        CLUSTER_MYSELF_PTR->is_master
#define CLUSTER_MASTER_PTR      g_server_global_vars.cluster.master

#define CLUSTER_SERVER_ARRAY    g_server_global_vars.cluster.server_array

#define CLUSTER_ID              g_server_global_vars.cluster.id
#define CLUSTER_MY_SERVER_ID    CLUSTER_MYSELF_PTR->server->id

#define CLUSTER_SF_CTX          g_server_global_vars.cluster.sf_context

#define DENTRY_MAX_DATA_SIZE    g_server_global_vars.dentry_max_data_size
#define BINLOG_BUFFER_SIZE      g_server_global_vars.data.binlog_buffer_size
#define CURRENT_INODE_SN        g_server_global_vars.inode.generator.sn
#define INODE_CLUSTER_PART      g_server_global_vars.inode.generator.cluster
#define INODE_SHARED_LOCKS_COUNT g_server_global_vars.inode.entries.shared_locks_count
#define INODE_HASHTABLE_CAPACITY g_server_global_vars.inode.entries.hashtable_capacity
#define DATA_CURRENT_VERSION    g_server_global_vars.data.current_version
#define DATA_THREAD_COUNT       g_server_global_vars.data.thread_count
#define DATA_PATH               g_server_global_vars.data.path
#define DATA_PATH_STR           DATA_PATH.str
#define DATA_PATH_LEN           DATA_PATH.len

#define SLAVE_SERVER_COUNT      (FC_SID_SERVER_COUNT(CLUSTER_CONFIG_CTX) - 1)

#define REPLICA_KEY_BUFF        CLUSTER_MYSELF_PTR->key

#define CLUSTER_GROUP_INDEX     g_server_global_vars.cluster.config.cluster_group_index
#define SERVICE_GROUP_INDEX     g_server_global_vars.cluster.config.service_group_index

#define CLUSTER_GROUP_ADDRESS_ARRAY(server) \
    (server)->group_addrs[CLUSTER_GROUP_INDEX].address_array
#define SERVICE_GROUP_ADDRESS_ARRAY(server) \
    (server)->group_addrs[SERVICE_GROUP_INDEX].address_array

#define CLUSTER_GROUP_ADDRESS_FIRST_PTR(server) \
    (*(server)->group_addrs[CLUSTER_GROUP_INDEX].address_array.addrs)
#define SERVICE_GROUP_ADDRESS_FIRST_PTR(server) \
    (*(server)->group_addrs[SERVICE_GROUP_INDEX].address_array.addrs)

#define CLUSTER_GROUP_ADDRESS_FIRST_IP(server) \
    CLUSTER_GROUP_ADDRESS_FIRST_PTR(server)->conn.ip_addr
#define CLUSTER_GROUP_ADDRESS_FIRST_PORT(server) \
    CLUSTER_GROUP_ADDRESS_FIRST_PTR(server)->conn.port

#define SERVICE_GROUP_ADDRESS_FIRST_IP(server) \
    SERVICE_GROUP_ADDRESS_FIRST_PTR(server)->conn.ip_addr
#define SERVICE_GROUP_ADDRESS_FIRST_PORT(server) \
    SERVICE_GROUP_ADDRESS_FIRST_PTR(server)->conn.port

#define CLUSTER_CONFIG_SIGN_BUF g_server_global_vars.cluster.config.md5_digest
#define CLUSTER_CONFIG_SIGN_LEN 16

#ifdef __cplusplus
extern "C" {
#endif

    extern FDIRServerGlobalVars g_server_global_vars;

#ifdef __cplusplus
}
#endif

#endif
