#ifndef _FDIR_TYPES_H
#define _FDIR_TYPES_H

#include "fastcommon/common_define.h"
#include "fastcommon/server_id_func.h"

#define FDIR_ERROR_INFO_SIZE   256
#define FDIR_REPLICA_KEY_SIZE    8

#define FDIR_NETWORK_TIMEOUT_DEFAULT    30
#define FDIR_CONNECT_TIMEOUT_DEFAULT     5

#define FDIR_DEFAULT_BINLOG_BUFFER_SIZE (64 * 1024)

#define FDIR_SERVER_DEFAULT_CLUSTER_PORT  11011
#define FDIR_SERVER_DEFAULT_SERVICE_PORT  11012

#define FDIR_MAX_PATH_COUNT       128

#define FDIR_SERVER_STATUS_INIT       0
#define FDIR_SERVER_STATUS_BUILDING  10
#define FDIR_SERVER_STATUS_DUMPING   20
#define FDIR_SERVER_STATUS_OFFLINE   21
#define FDIR_SERVER_STATUS_SYNCING   22
#define FDIR_SERVER_STATUS_ACTIVE    23

typedef struct {
    int body_len;      //body length
    short flags;
    short status;
    unsigned char cmd; //command
} FDIRHeaderInfo;

typedef struct {
    FDIRHeaderInfo header;
    char *body;
} FDIRRequestInfo;

typedef struct {
    int length;
    char message[FDIR_ERROR_INFO_SIZE];
} FDIRErrorInfo;

typedef struct {
    FDIRHeaderInfo header;
    FDIRErrorInfo error;
} FDIRResponseInfo;

typedef struct fdir_dentry_full_name {
    string_t ns;    //namespace
    string_t path;  //full path
} FDIRDEntryFullName;

typedef struct fdir_dentry_status {
    int mode;
    uid_t uid;
    gid_t gid;
    int atime;  /* access time */
    int ctime;  /* status change time */
    int mtime;  /* modify time */
    int64_t size;   /* file size in bytes */
} FDIRDEntryStatus;

typedef struct fdir_dentry_info {
    int64_t inode;
    FDIRDEntryStatus stat;
} FDIRDEntryInfo;

typedef union {
    int64_t flags;
    struct {
        union {
            int flags: 4;
            struct {
                bool ns: 1;  //namespace
                bool pt: 1;  //path
            };
        } path_info;
        bool hash_code : 1;  //required field, for unpack check
        bool user_data : 1;
        bool extra_data: 1;
        bool mode : 1;
        bool atime: 1;
        bool ctime: 1;
        bool mtime: 1;
        bool gid:   1;
        bool uid:   1;
        bool size : 1;
    };
} FDIRStatModifyFlags;

#endif
