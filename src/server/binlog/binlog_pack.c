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
#include <assert.h>
#include "fastcommon/logger.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/char_converter.h"
#include "sf/sf_global.h"
#include "../server_global.h"
#include "binlog_func.h"
#include "binlog_producer.h"
#include "binlog_pack.h"

#define BINLOG_RECORD_START_TAG_CHAR '<'
#define BINLOG_RECORD_START_TAG_STR  "<rec"
#define BINLOG_RECORD_START_TAG_LEN (sizeof(BINLOG_RECORD_START_TAG_STR) - 1)

#define BINLOG_RECORD_END_TAG_CHAR '\n'
#define BINLOG_RECORD_END_TAG_STR   "/rec>\n"
#define BINLOG_RECORD_END_TAG_LEN   (sizeof(BINLOG_RECORD_END_TAG_STR) - 1)

#define BINLOG_RECORD_FIELD_NAME_LENGTH         2

#define BINLOG_RECORD_FIELD_NAME_INODE         "id"
#define BINLOG_RECORD_FIELD_NAME_DATA_VERSION  "dv"
#define BINLOG_RECORD_FIELD_NAME_OPERATION     "op"
#define BINLOG_RECORD_FIELD_NAME_TIMESTAMP     "ts"
#define BINLOG_RECORD_FIELD_NAME_NAMESPACE     "ns"
#define BINLOG_RECORD_FIELD_NAME_PATH          "pt"
#define BINLOG_RECORD_FIELD_NAME_EXTRA_DATA    "ex"
#define BINLOG_RECORD_FIELD_NAME_USER_DATA     "us"
#define BINLOG_RECORD_FIELD_NAME_MODE          "md"
#define BINLOG_RECORD_FIELD_NAME_ATIME         "at"
#define BINLOG_RECORD_FIELD_NAME_CTIME         "ct"
#define BINLOG_RECORD_FIELD_NAME_MTIME         "mt"
#define BINLOG_RECORD_FIELD_NAME_UID           "ui"
#define BINLOG_RECORD_FIELD_NAME_GID           "gi"
#define BINLOG_RECORD_FIELD_NAME_FILE_SIZE     "sz"
#define BINLOG_RECORD_FIELD_NAME_HASH_CODE     "hc"

#define BINLOG_RECORD_FIELD_INDEX_INODE         ('i' * 256 + 'd')
#define BINLOG_RECORD_FIELD_INDEX_DATA_VERSION  ('d' * 256 + 'v')
#define BINLOG_RECORD_FIELD_INDEX_OPERATION     ('o' * 256 + 'p')
#define BINLOG_RECORD_FIELD_INDEX_TIMESTAMP     ('t' * 256 + 's')
#define BINLOG_RECORD_FIELD_INDEX_NAMESPACE     ('n' * 256 + 's')
#define BINLOG_RECORD_FIELD_INDEX_PATH          ('p' * 256 + 't')
#define BINLOG_RECORD_FIELD_INDEX_EXTRA_DATA    ('e' * 256 + 'x')
#define BINLOG_RECORD_FIELD_INDEX_USER_DATA     ('u' * 256 + 's')
#define BINLOG_RECORD_FIELD_INDEX_MODE          ('m' * 256 + 'd')
#define BINLOG_RECORD_FIELD_INDEX_ATIME         ('a' * 256 + 't')
#define BINLOG_RECORD_FIELD_INDEX_CTIME         ('c' * 256 + 't')
#define BINLOG_RECORD_FIELD_INDEX_MTIME         ('m' * 256 + 't')
#define BINLOG_RECORD_FIELD_INDEX_UID           ('u' * 256 + 'i')
#define BINLOG_RECORD_FIELD_INDEX_GID           ('g' * 256 + 'i')
#define BINLOG_RECORD_FIELD_INDEX_FILE_SIZE     ('s' * 256 + 'z')
#define BINLOG_RECORD_FIELD_INDEX_HASH_CODE     ('h' * 256 + 'c')

#define BINLOG_FIELD_TYPE_INTEGER   'i'
#define BINLOG_FIELD_TYPE_STRING    's'

typedef struct {
    const char *name;
    int type;
    union {
        int64_t n;
        string_t s;
    } value;
} BinlogFieldValue;

typedef struct {
    const char *p;
    const char *rec_end;
    BinlogFieldValue fv;
    char *error_info;
    int error_size;
} FieldParserContext;

static FastCharConverter char_converter;

int binlog_pack_init()
{
#define ESCAPE_CHAR_PAIR_COUNT 8
    FastCharPair pairs[ESCAPE_CHAR_PAIR_COUNT];

    FAST_CHAR_MAKE_PAIR(pairs[0], '\0', '0');
    FAST_CHAR_MAKE_PAIR(pairs[1], '\n', 'n');
    FAST_CHAR_MAKE_PAIR(pairs[2], '\v', 'v');
    FAST_CHAR_MAKE_PAIR(pairs[3], '\f', 'f');
    FAST_CHAR_MAKE_PAIR(pairs[4], '\r', 'r');
    FAST_CHAR_MAKE_PAIR(pairs[5], '\\', '\\');
    FAST_CHAR_MAKE_PAIR(pairs[6], '<',  'l');
    FAST_CHAR_MAKE_PAIR(pairs[7], '>',  'g');

    return char_converter_init_ex(&char_converter, pairs,
            ESCAPE_CHAR_PAIR_COUNT, FAST_CHAR_OP_ADD_BACKSLASH);
}

static inline const char *get_operation_label(const int operation)
{
    switch (operation) {
        case BINLOG_OP_CREATE_DENTRY_INT:
            return BINLOG_OP_CREATE_DENTRY_STR;
        case BINLOG_OP_REMOVE_DENTRY_INT:
            return BINLOG_OP_REMOVE_DENTRY_STR;
        case BINLOG_OP_RENAME_DENTRY_INT:
            return BINLOG_OP_RENAME_DENTRY_STR;
        case BINLOG_OP_UPDATE_DENTRY_INT:
            return BINLOG_OP_UPDATE_DENTRY_STR;
        default:
            return BINLOG_OP_NONE_STR;
    }
}

static inline int get_operation_integer(const string_t *operation)
{
    if (fc_string_equal2(operation, BINLOG_OP_UPDATE_DENTRY_STR,
                BINLOG_OP_UPDATE_DENTRY_LEN))
    {
        return BINLOG_OP_UPDATE_DENTRY_INT;
    } else if (fc_string_equal2(operation, BINLOG_OP_CREATE_DENTRY_STR,
                BINLOG_OP_CREATE_DENTRY_LEN))
    {
        return BINLOG_OP_CREATE_DENTRY_INT;
    } else if (fc_string_equal2(operation, BINLOG_OP_REMOVE_DENTRY_STR,
                BINLOG_OP_REMOVE_DENTRY_LEN))
    {
        return BINLOG_OP_REMOVE_DENTRY_INT;
    } else if (fc_string_equal2(operation, BINLOG_OP_RENAME_DENTRY_STR,
                BINLOG_OP_RENAME_DENTRY_LEN))
    {
        return BINLOG_OP_RENAME_DENTRY_INT;
    } else {
        return BINLOG_OP_NONE_INT;
    }
}

static void binlog_pack_stringl(FastBuffer *buffer, const char *name,
        const char *val, const int len, const bool need_escape)
{
    char len_buff[16];
    int len1, len2;
    int escape_len;

    fast_buffer_append(buffer, " %s=%d,", name, len);
    if (need_escape) {
        fast_char_escape(&char_converter, val, len,
                buffer->data + buffer->length, &escape_len,
                buffer->alloc_size - buffer->length);
        if (escape_len != len) {
            len1 = snprintf(NULL, 0, "%d", len);
            len2 = snprintf(len_buff, sizeof(len_buff), "%d", escape_len);
            if (len2 == len1) {
                memcpy(buffer->data + buffer->length - (len1 + 1),
                        len_buff, len2);  //replace the length only
            } else {
                buffer->length -= len1 + 1;
                fast_buffer_append(buffer, "%d,", len2);
                fast_char_escape(&char_converter, val, len,
                        buffer->data + buffer->length, &escape_len,
                        buffer->alloc_size - buffer->length);
            }
        }
        buffer->length += escape_len;
    } else {
        fast_buffer_append_buff(buffer, val, len);
    }
}

#define BINLOG_PACK_STRING(buffer, name, value) \
    binlog_pack_stringl(buffer, name, value.str, value.len, true)

int binlog_pack_record(const FDIRBinlogRecord *record, FastBuffer *buffer)
{
    string_t op_caption;
    int old_len;
    int expect_len;
    int record_len;
    int result;

    expect_len = 512;
    if (record->options.path_info.flags != 0) {
        expect_len += record->fullname.ns.len +
                record->fullname.path.len;
    }
    if (record->options.extra_data) {
        expect_len += record->extra_data.len;
    }
    if (record->options.user_data) {
        expect_len += record->user_data.len;
    }
    expect_len *= 2;
    if ((result=fast_buffer_check_capacity(buffer, expect_len)) != 0) {
        return result;
    }

    //reserve record size spaces
    old_len = buffer->length;
    buffer->length += BINLOG_RECORD_SIZE_STRLEN;

    fast_buffer_append_buff(buffer, BINLOG_RECORD_START_TAG_STR,
            BINLOG_RECORD_START_TAG_LEN);

    fast_buffer_append(buffer, " %s=%"PRId64,
            BINLOG_RECORD_FIELD_NAME_DATA_VERSION, record->data_version);

    fast_buffer_append(buffer, " %s=%"PRId64,
            BINLOG_RECORD_FIELD_NAME_INODE, record->inode);

    op_caption.str = (char *)get_operation_label(record->operation);
    op_caption.len = strlen(op_caption.str);
    binlog_pack_stringl(buffer, BINLOG_RECORD_FIELD_NAME_OPERATION,
            op_caption.str, op_caption.len, false);

    fast_buffer_append(buffer, " %s=%d",
            BINLOG_RECORD_FIELD_NAME_TIMESTAMP, record->timestamp);

    if (record->options.path_info.flags != 0) {
        BINLOG_PACK_STRING(buffer, BINLOG_RECORD_FIELD_NAME_NAMESPACE,
                record->fullname.ns);

        BINLOG_PACK_STRING(buffer, BINLOG_RECORD_FIELD_NAME_PATH,
                record->fullname.path);
    }

    fast_buffer_append(buffer, " %s=%u",
            BINLOG_RECORD_FIELD_NAME_HASH_CODE,
            record->hash_code);

    if (record->options.extra_data) {
        BINLOG_PACK_STRING(buffer, BINLOG_RECORD_FIELD_NAME_EXTRA_DATA,
                record->extra_data);
    }

    if (record->options.user_data) {
        BINLOG_PACK_STRING(buffer, BINLOG_RECORD_FIELD_NAME_USER_DATA,
                record->user_data);
    }

    if (record->options.mode) {
        fast_buffer_append(buffer, " %s=%d",
                BINLOG_RECORD_FIELD_NAME_MODE, record->stat.mode);
    }

    if (record->options.atime) {
        fast_buffer_append(buffer, " %s=%d",
                BINLOG_RECORD_FIELD_NAME_ATIME, record->stat.atime);
    }

    if (record->options.ctime) {
        fast_buffer_append(buffer, " %s=%d",
                BINLOG_RECORD_FIELD_NAME_CTIME, record->stat.ctime);
    }

    if (record->options.mtime) {
        fast_buffer_append(buffer, " %s=%d",
                BINLOG_RECORD_FIELD_NAME_MTIME, record->stat.mtime);
    }

    if (record->options.uid) {
        fast_buffer_append(buffer, " %s=%d",
                BINLOG_RECORD_FIELD_NAME_UID, record->stat.uid);
    }

    if (record->options.gid) {
        fast_buffer_append(buffer, " %s=%d",
                BINLOG_RECORD_FIELD_NAME_GID, record->stat.gid);
    }

    if (record->options.size) {
        fast_buffer_append(buffer, " %s=%"PRId64,
                BINLOG_RECORD_FIELD_NAME_FILE_SIZE, record->stat.size);
    }

    fast_buffer_append_buff(buffer, BINLOG_RECORD_END_TAG_STR,
            BINLOG_RECORD_END_TAG_LEN);

    record_len = buffer->length - old_len - BINLOG_RECORD_SIZE_STRLEN;
    if (record_len > BINLOG_RECORD_MAX_SIZE) {
        logError("file: "__FILE__", line: %d, "
                "record length: %d is too large, exceeds %d",
                __LINE__, record_len, BINLOG_RECORD_MAX_SIZE);
        return EOVERFLOW;
    }

    sprintf(buffer->data + old_len, BINLOG_RECORD_SIZE_PRINTF_FMT, record_len);
    *(buffer->data + old_len + BINLOG_RECORD_SIZE_STRLEN) =
        BINLOG_RECORD_START_TAG_CHAR;  //restore the start char
    return 0;
}

static int binlog_get_next_field_value(FieldParserContext *pcontext)
{
    int remain;
    unsigned char equal_ch;
    const char *value;
    char *endptr;
    int64_t n;

    remain = pcontext->rec_end - pcontext->p;
    if (*pcontext->p == '/') {
        if (remain == BINLOG_RECORD_END_TAG_LEN) {
            pcontext->p += BINLOG_RECORD_END_TAG_LEN;
            return ENOENT;
        } else {
            sprintf(pcontext->error_info, "expect field name, "
                    "but charactor \"/\" found");
            return EINVAL;
        }
    }
    if (*pcontext->p != ' ') {
        sprintf(pcontext->error_info, "expect space charactor, "
                "but charactor %c(0x%02X) found",
                *pcontext->p, (unsigned char)*pcontext->p);
        return EINVAL;
    }
    pcontext->p++;

    if (remain - 1 < BINLOG_RECORD_END_TAG_LEN + 4) {
        sprintf(pcontext->error_info, "record remain length: %d "
                "is too short", remain);
        return EINVAL;
    }

    pcontext->fv.name = pcontext->p;
    equal_ch = *(pcontext->fv.name + BINLOG_RECORD_FIELD_NAME_LENGTH);
    if (equal_ch != '=') {
        sprintf(pcontext->error_info, "unexpect char: %c(0x%02X), expect "
                "the equal char: =", equal_ch, equal_ch);
        return EINVAL;
    }

    value = pcontext->fv.name + BINLOG_RECORD_FIELD_NAME_LENGTH + 1;
    endptr = NULL;
    n = strtoll(value, &endptr, 10);
    if (*endptr == ',') {
        pcontext->fv.type = BINLOG_FIELD_TYPE_STRING;

        FC_SET_STRING_EX(pcontext->fv.value.s, (char *)endptr + 1, n);
        pcontext->p = pcontext->fv.value.s.str + n;
        if (pcontext->rec_end - pcontext->p < BINLOG_RECORD_END_TAG_LEN) {
            sprintf(pcontext->error_info, "field: %.*s, value length: %"PRId64
                    " out of bound", BINLOG_RECORD_FIELD_NAME_LENGTH,
                    pcontext->fv.name, n);
            return EINVAL;
        }

        fast_char_unescape(&char_converter, pcontext->fv.value.s.str,
                &pcontext->fv.value.s.len);
    } else if ((*endptr == ' ') || (*endptr == '/' && pcontext->rec_end -
                endptr == BINLOG_RECORD_END_TAG_LEN))
    {
        pcontext->fv.type = BINLOG_FIELD_TYPE_INTEGER;
        pcontext->fv.value.n = n;
        pcontext->p = endptr;
    } else {
        snprintf(pcontext->error_info, pcontext->error_size,
                "unexpect char: %c(0x%02X), expect comma or space char, "
                "record segment: %.*s", *endptr, (unsigned char)*endptr,
                (int)(pcontext->rec_end - pcontext->p), pcontext->p);
        return EINVAL;
    }

    return 0;
}

static inline const char *get_field_type_caption(const int type)
{
    switch (type) {
        case BINLOG_FIELD_TYPE_INTEGER:
            return "integer";
        case BINLOG_FIELD_TYPE_STRING:
            return "string";
        default:
            return "unkown";
    }
}

static int binlog_set_field_value(FieldParserContext *pcontext,
        FDIRBinlogRecord *record)
{
    int index;
    int expect_type;

    index = pcontext->fv.name[0] * 256 + pcontext->fv.name[1];
    switch (index) {
        case BINLOG_RECORD_FIELD_INDEX_INODE:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->inode = pcontext->fv.value.n;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_DATA_VERSION:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->data_version = pcontext->fv.value.n;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_OPERATION:
            expect_type = BINLOG_FIELD_TYPE_STRING;
            if (pcontext->fv.type == expect_type) {
                record->operation = get_operation_integer(
                        &pcontext->fv.value.s);
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_TIMESTAMP:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->timestamp = pcontext->fv.value.n;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_NAMESPACE:
            expect_type = BINLOG_FIELD_TYPE_STRING;
            if (pcontext->fv.type == expect_type) {
                record->fullname.ns = pcontext->fv.value.s;
                record->options.path_info.ns = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_PATH:
            expect_type = BINLOG_FIELD_TYPE_STRING;
            if (pcontext->fv.type == expect_type) {
                record->fullname.path = pcontext->fv.value.s;
                record->options.path_info.pt = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_EXTRA_DATA:
            expect_type = BINLOG_FIELD_TYPE_STRING;
            if (pcontext->fv.type == expect_type) {
                record->extra_data = pcontext->fv.value.s;
                record->options.extra_data = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_USER_DATA:
            expect_type = BINLOG_FIELD_TYPE_STRING;
            if (pcontext->fv.type == expect_type) {
                record->user_data = pcontext->fv.value.s;
                record->options.user_data = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_MODE:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.mode = pcontext->fv.value.n;
                record->options.mode = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_ATIME:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.atime = pcontext->fv.value.n;
                record->options.atime = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_CTIME:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.ctime = pcontext->fv.value.n;
                record->options.ctime = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_MTIME:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.mtime = pcontext->fv.value.n;
                record->options.mtime = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_UID:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.uid = pcontext->fv.value.n;
                record->options.uid = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_GID:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.gid = pcontext->fv.value.n;
                record->options.gid = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_FILE_SIZE:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->stat.size = pcontext->fv.value.n;
                record->options.size = 1;
            }
            break;
        case BINLOG_RECORD_FIELD_INDEX_HASH_CODE:
            expect_type = BINLOG_FIELD_TYPE_INTEGER;
            if (pcontext->fv.type == expect_type) {
                record->hash_code = pcontext->fv.value.n;
                record->options.hash_code = 1;
            }
            break;
        default:
            sprintf(pcontext->error_info, "unkown field name: %.*s",
                    BINLOG_RECORD_FIELD_NAME_LENGTH, pcontext->fv.name);
            return ENOENT;
    }

    if (pcontext->fv.type != expect_type) {
        sprintf(pcontext->error_info, "field: %.*s, value type: %s != "
                "expected: %s", BINLOG_RECORD_FIELD_NAME_LENGTH,
                pcontext->fv.name, get_field_type_caption(pcontext->fv.type),
                get_field_type_caption(expect_type));
        return EINVAL;
    }

    return 0;
}
 
static int binlog_check_required_fields(FieldParserContext *pcontext,
        FDIRBinlogRecord *record)
{
    if (record->inode <= 0) {
        sprintf(pcontext->error_info, "expect inode field: %s",
                BINLOG_RECORD_FIELD_NAME_INODE);
        return ENOENT;
    }

    if (record->data_version <= 0) {
        sprintf(pcontext->error_info, "expect data version field: %s",
                BINLOG_RECORD_FIELD_NAME_DATA_VERSION);
        return ENOENT;
    }

    if (record->options.hash_code == 0) {
        sprintf(pcontext->error_info, "expect hash code field: %s",
                BINLOG_RECORD_FIELD_NAME_HASH_CODE);
        return ENOENT;
    }

    if (record->operation == BINLOG_OP_NONE_INT) {
        sprintf(pcontext->error_info, "expect operation field: %s",
                BINLOG_RECORD_FIELD_NAME_OPERATION);
        return ENOENT;
    }

    if (record->timestamp <= 0) {
        sprintf(pcontext->error_info, "expect timestamp field: %s",
                BINLOG_RECORD_FIELD_NAME_TIMESTAMP);
        return ENOENT;
    }

    if (record->options.path_info.flags != 0) {
        if (record->options.path_info.ns == 0) {
            sprintf(pcontext->error_info, "expect namespace field: %s",
                    BINLOG_RECORD_FIELD_NAME_NAMESPACE);
            return ENOENT;
        }
        if (record->options.path_info.pt == 0) {
            sprintf(pcontext->error_info, "expect path field: %s",
                    BINLOG_RECORD_FIELD_NAME_PATH);
            return ENOENT;
        }
    }

    return 0;
}

static int binlog_parse_first_field(FieldParserContext *pcontext,
        FDIRBinlogRecord *record)
{
    int result;

    if ((result=binlog_get_next_field_value(pcontext)) != 0) {
        return result;
    }

    if (memcmp(pcontext->fv.name, BINLOG_RECORD_FIELD_NAME_DATA_VERSION,
                BINLOG_RECORD_FIELD_NAME_LENGTH) != 0)
    {
        sprintf(pcontext->error_info, "first field must be data version (%s)",
                BINLOG_RECORD_FIELD_NAME_DATA_VERSION);
        return EINVAL;
    }

    return binlog_set_field_value(pcontext, record);
}

static int binlog_parse_fields(FieldParserContext *pcontext,
        FDIRBinlogRecord *record)
{
    int result;

    if ((result=binlog_parse_first_field(pcontext, record)) != 0) {
        return result;
    }

    while ((result=binlog_get_next_field_value(pcontext)) == 0) {
        if ((result=binlog_set_field_value(pcontext, record)) != 0) {
            if (result == ENOENT) {
                logWarning("file: "__FILE__", line: %d, "
                        "%s", __LINE__, pcontext->error_info);
            } else {
                return result;
            }
        }
    }

    if (result != ENOENT) {
        return result;
    }

    return binlog_check_required_fields(pcontext, record);
}

static inline int binlog_check_rec_length(const int len,
        FieldParserContext *pcontext)
{
    if (len < BINLOG_RECORD_MIN_SIZE) {
        sprintf(pcontext->error_info, "string length: %d is too short", len);
        return EAGAIN;
    }

    return 0;
}

static int binlog_check_record(const char *str, const int len,
        FieldParserContext *pcontext)
{
    int result;
    int record_len;
    char *rec_start;

    if ((result=binlog_check_rec_length(len, pcontext)) != 0) {
        return result;
    }

    rec_start = NULL;
    record_len  = strtol(str, &rec_start, 10);
    if (*rec_start != BINLOG_RECORD_START_TAG_CHAR) {
        sprintf(pcontext->error_info, "unexpect char: %c(0x%02X), "
                "expect start tag char: %c", *rec_start,
                (unsigned char)*rec_start,
                BINLOG_RECORD_START_TAG_CHAR);
        return EINVAL;
    }

    if (memcmp(rec_start, BINLOG_RECORD_START_TAG_STR,
                BINLOG_RECORD_START_TAG_LEN) != 0)
    {
        sprintf(pcontext->error_info, "expect record start tag: %s, "
                "but the start string is: %.*s",
                BINLOG_RECORD_START_TAG_STR,
                (int)BINLOG_RECORD_START_TAG_LEN, rec_start);
        return EINVAL;
    }

    if (record_len < BINLOG_RECORD_MIN_SIZE - BINLOG_RECORD_SIZE_STRLEN)
    {
        sprintf(pcontext->error_info, "record length: %d is too short",
                record_len);
        return EINVAL;
    }
    if (record_len > len - BINLOG_RECORD_SIZE_STRLEN) {
        sprintf(pcontext->error_info, "record length: %d out of bound",
                record_len);
        return EOVERFLOW;
    }

    pcontext->rec_end = str + BINLOG_RECORD_SIZE_STRLEN + record_len;
    if (memcmp(pcontext->rec_end - BINLOG_RECORD_END_TAG_LEN,
                BINLOG_RECORD_END_TAG_STR,
                BINLOG_RECORD_END_TAG_LEN) != 0)
    {
        char output[32];
        int out_len;

        fast_char_convert(&char_converter,
                pcontext->rec_end - BINLOG_RECORD_END_TAG_LEN,
                BINLOG_RECORD_END_TAG_LEN, output, &out_len, sizeof(output));

        sprintf(pcontext->error_info, "expect record end tag: %s, "
                "but the string is: %.*s",
                BINLOG_RECORD_END_TAG_STR, out_len, output);
        return EINVAL;
    }

    pcontext->p = rec_start + BINLOG_RECORD_START_TAG_LEN;
    return 0;
}

#define BINLOG_PACK_SET_ERROR_INFO(pcontext, errinfo, errsize) \
    do { \
        *errinfo = '\0';   \
        pcontext.error_info = errinfo;  \
        pcontext.error_size = errsize;  \
    } while (0)

int binlog_unpack_record(const char *str, const int len,
        FDIRBinlogRecord *record, const char **record_end,
        char *error_info, const int error_size)
{
    FieldParserContext pcontext;
    int result;

    memset(record, 0, (long)(&((FDIRBinlogRecord*)0)->notify));
    BINLOG_PACK_SET_ERROR_INFO(pcontext, error_info, error_size);
    if ((result=binlog_check_record(str, len, &pcontext)) != 0) {
        *record_end = NULL;
        return result;
    }

    *record_end = pcontext.rec_end;
    return binlog_parse_fields(&pcontext, record);
}

int binlog_detect_record(const char *str, const int len,
        int64_t *data_version, const char **rec_end,
        char *error_info, const int error_size)
{
    FDIRBinlogRecord record;
    FieldParserContext pcontext;
    int result;

    BINLOG_PACK_SET_ERROR_INFO(pcontext, error_info, error_size);
    if ((result=binlog_check_record(str, len, &pcontext)) != 0) {
        return result;
    }

    if ((result=binlog_parse_first_field(&pcontext, &record)) != 0) {
        return result;
    }

    *rec_end = pcontext.rec_end;
    *data_version = record.data_version;
    return 0;
}

static bool binlog_is_record_start(const char *str, const int len,
        FieldParserContext *pcontext)
{
    int record_len;
    const char *rec_start;

    if (len < BINLOG_RECORD_MIN_SIZE) {
        return false;
    }

    record_len = strtol(str, (char **)&rec_start, 10);
    if (*rec_start != BINLOG_RECORD_START_TAG_CHAR) {
       return false;
    }
    if (rec_start - str != BINLOG_RECORD_SIZE_STRLEN) {
        return false;
    }

    if (record_len > len - BINLOG_RECORD_SIZE_STRLEN) {
        return false;
    }

    if (memcmp(rec_start, BINLOG_RECORD_START_TAG_STR,
                BINLOG_RECORD_START_TAG_LEN) != 0)
    {
        return false;
    }

    pcontext->rec_end = str + BINLOG_RECORD_SIZE_STRLEN + record_len;
    if (memcmp(pcontext->rec_end - BINLOG_RECORD_END_TAG_LEN,
                BINLOG_RECORD_END_TAG_STR,
                BINLOG_RECORD_END_TAG_LEN) == 0)
    {
        pcontext->p = rec_start + BINLOG_RECORD_START_TAG_LEN;
        return true;
    }
    return false;
}

int binlog_detect_record_forward(const char *str, const int len,
        int64_t *data_version, int *rstart_offset, int *rend_offset,
        char *error_info, const int error_size)
{
    FDIRBinlogRecord record;
    FieldParserContext pcontext;
    const char *rec_start;
    const char *p;
    const char *end;
    int result;

    BINLOG_PACK_SET_ERROR_INFO(pcontext, error_info, error_size);
    *rstart_offset = -1;
    p = str;
    end = str + len;
    while ((end - p >= BINLOG_RECORD_MIN_SIZE) && (rec_start=
                (const char *)memchr(p, BINLOG_RECORD_START_TAG_CHAR,
                    end - p)) != NULL)
    {
        const char *start;
        start = rec_start - BINLOG_RECORD_SIZE_STRLEN;
        if ((start >= str) && binlog_is_record_start(
                    start, end - start, &pcontext))
        {
            *rstart_offset = start - str;
            break;
        }

        p = rec_start + 1;
    }

    if (*rstart_offset < 0) {
        sprintf(error_info, "can't found record start");
        return ENOENT;
    }

    if ((result=binlog_parse_first_field(&pcontext, &record)) != 0) {
        return result;
    }

    *rend_offset = pcontext.rec_end - str;
    *data_version = record.data_version;
    return 0;
}

int binlog_detect_record_reverse(const char *str, const int len,
        int64_t *data_version, const char **rec_end,
        char *error_info, const int error_size)
{
    FDIRBinlogRecord record;
    FieldParserContext pcontext;
    const char *start;
    const char *rec_start;
    int offset;
    int l;
    int result;

    BINLOG_PACK_SET_ERROR_INFO(pcontext, error_info, error_size);
    if ((result=binlog_check_rec_length(len, &pcontext)) != 0) {
        return result;
    }

    offset = -1;
    l = len;
    while (l > 0 && (rec_start=fc_memrchr(str,
                    BINLOG_RECORD_START_TAG_CHAR, l)) != NULL)
    {
        start = rec_start - BINLOG_RECORD_SIZE_STRLEN;
        if ((start >= str) && binlog_is_record_start(
                    start, len - (start - str), &pcontext))
        {
            offset = start - str;
            break;
        }

        l = (rec_start - 1) - str;
    }

    if (offset < 0) {
        sprintf(error_info, "can't found record start");
        return ENOENT;
    }

    if (rec_end != NULL) {
        *rec_end = pcontext.rec_end;
    }

    if ((result=binlog_parse_first_field(&pcontext, &record)) != 0) {
        return result;
    }

    *data_version = record.data_version;
    return 0;
}

int binlog_detect_last_record_end(const char *str, const int len,
        const char **rec_end)
{
    int l;

    l = len;
    while (l > 0 && (*rec_end=fc_memrchr(str,
                    BINLOG_RECORD_END_TAG_CHAR, l)) != NULL)
    {
        *rec_end += 1;
        l = *rec_end - str;
        if ((l >= BINLOG_RECORD_END_TAG_LEN) && memcmp(
                    *rec_end - BINLOG_RECORD_END_TAG_LEN,
                    BINLOG_RECORD_END_TAG_STR,
                    BINLOG_RECORD_END_TAG_LEN) == 0)
        {
            return 0;
        }
    }

    return ENOENT;
}
