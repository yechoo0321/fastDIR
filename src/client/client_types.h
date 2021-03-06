#ifndef _FDIR_CLIENT_TYPES_H
#define _FDIR_CLIENT_TYPES_H

#include "fastcommon/common_define.h"
#include "fastcommon/connection_pool.h"
#include "fdir_types.h"

struct fdir_client_context;

typedef ConnectionInfo *(*fdir_get_connection_func)(
        struct fdir_client_context *client_ctx, int *err_no);

typedef ConnectionInfo *(*fdir_get_spec_connection_func)(
        struct fdir_client_context *client_ctx,
        const ConnectionInfo *target, int *err_no);

typedef void (*fdir_release_connection_func)(
        struct fdir_client_context *client_ctx, ConnectionInfo *conn);
typedef void (*fdir_close_connection_func)(
        struct fdir_client_context *client_ctx, ConnectionInfo *conn);

typedef struct fdir_dstatus {
    int64_t inode;
    mode_t mode;
    int ctime;  /* create time */
    int mtime;  /* modify time */
    int atime;  /* last access time */
    int64_t size;   /* file size in bytes */
} FDIRDStatus;

typedef struct fdir_client_server_entry {
    int server_id;
    ConnectionInfo conn;
    char status;
} FDIRClientServerEntry;

typedef struct fdir_server_group {
    int alloc_size;
    int count;
    ConnectionInfo *servers;
} FDIRServerGroup;

typedef struct fdir_connection_manager {
    /* get the specify connection by ip and port */
    fdir_get_spec_connection_func get_spec_connection;

    /* get one connection of the configured servers */
    fdir_get_connection_func get_connection;

    /* get the master connection from the server */
    fdir_get_connection_func get_master_connection;

    /* get one readable connection from the server */
    fdir_get_connection_func get_readable_connection;
    
    /* push back to connection pool when use connection pool */
    fdir_release_connection_func release_connection;

     /* disconnect the connecton on network error */
    fdir_close_connection_func close_connection;

    /* master connection cache */
    struct {
        ConnectionInfo *conn;
        ConnectionInfo holder;
    } master_cache;

    void *args[2];   //extra data
} FDIRConnectionManager;

typedef struct fdir_client_session {
    struct fdir_client_context *ctx;
    ConnectionInfo *mconn;  //master connection
} FDIRClientSession;

typedef enum {
    conn_manager_type_simple = 1,
    conn_manager_type_pooled,
    conn_manager_type_other
} FDIRClientConnManagerType;

typedef struct fdir_client_context {
    FDIRServerGroup server_group;
    FDIRConnectionManager conn_manager;
    FDIRClientConnManagerType conn_manager_type;
    bool cloned;
} FDIRClientContext;

#endif
