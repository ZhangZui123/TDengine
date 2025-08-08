/*
 * Copyright (c) 2024 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TDENGINE_BACKUP_COORDINATOR_H
#define TDENGINE_BACKUP_COORDINATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "bitmap_engine.h"
#include "event_interceptor.h"

// 插件导出宏定义
#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 错误码定义
#define BACKUP_SUCCESS                   0
#define BACKUP_ERROR_INVALID_PARAM      -1
#define BACKUP_ERROR_INIT_FAILED        -2
#define BACKUP_ERROR_NOT_INITIALIZED    -3
#define BACKUP_ERROR_MEMORY_ALLOC       -4
#define BACKUP_ERROR_FILE_IO            -5
#define BACKUP_ERROR_NETWORK            -6
#define BACKUP_ERROR_TIMEOUT            -7
#define BACKUP_ERROR_DATA_CORRUPTION    -8
#define BACKUP_ERROR_PERMISSION_DENIED  -9
#define BACKUP_ERROR_DISK_FULL          -10
#define BACKUP_ERROR_CONNECTION_LOST    -11
#define BACKUP_ERROR_RETRY_EXHAUSTED    -12
#define BACKUP_ERROR_UNKNOWN            -99

// 重试状态
typedef enum {
    RETRY_STATE_IDLE = 0,      // 空闲状态
    RETRY_STATE_RETRYING,      // 重试中
    RETRY_STATE_SUCCESS,       // 重试成功
    RETRY_STATE_FAILED         // 重试失败
} ERetryState;

// 重试上下文
typedef struct {
    uint32_t current_retry;     // 当前重试次数
    uint32_t max_retry;         // 最大重试次数
    uint32_t retry_interval;    // 重试间隔(秒)
    uint64_t last_retry_time;   // 上次重试时间
    ERetryState state;          // 重试状态
    int32_t last_error;         // 最后一次错误码
    char* error_message;        // 错误信息
} SRetryContext;

// 增量游标类型
typedef enum {
    CURSOR_TYPE_TIME = 0,      // 时间点游标
    CURSOR_TYPE_WAL = 1,       // WAL偏移量游标
    CURSOR_TYPE_HYBRID = 2     // 混合游标
} ECursorType;

// 增量游标
typedef struct {
    ECursorType type;          // 游标类型
    int64_t start_time;        // 开始时间
    int64_t end_time;          // 结束时间
    uint64_t start_wal;        // 开始WAL偏移量
    uint64_t end_wal;          // 结束WAL偏移量
    uint64_t current_block;    // 当前块ID
    uint32_t block_count;      // 块总数
    bool has_more;             // 是否还有更多数据
} SIncrementalCursor;

// 增量数据块
typedef struct {
    uint64_t block_id;         // 块ID
    uint64_t wal_offset;       // WAL偏移量
    int64_t timestamp;         // 时间戳
    void* data;                // 块数据
    uint32_t data_size;        // 数据大小
    EBlockState state;         // 块状态
} SIncrementalBlock;

// 备份协同器配置
typedef struct {
    uint32_t max_blocks_per_batch;  // 每批最大块数
    uint32_t batch_timeout_ms;      // 批处理超时时间
    bool enable_compression;        // 是否启用压缩
    bool enable_encryption;         // 是否启用加密
    char* encryption_key;           // 加密密钥
    
    // taosX要求的重试参数
    uint32_t error_retry_max;       // 最大错误重试次数，默认10
    uint32_t error_retry_interval;  // 错误重试间隔(秒)，默认5s
    
    // 异常处理配置
    char* error_store_path;         // 出错数据存储路径
    bool enable_error_logging;      // 是否启用错误日志记录
    uint32_t error_buffer_size;     // 错误缓冲区大小
    
    // 备份文件配置
    char* backup_path;              // 备份文件存储路径
    uint64_t backup_max_size;       // 单个备份文件最大大小(字节)
    uint8_t compression_level;      // 压缩等级：1=fastest, 2=balanced, 3=best
} SBackupCoordinatorConfig;

// 备份协同器实例
typedef struct {
    SBackupCoordinatorConfig config;
    SBitmapEngine* bitmap_engine;
    SEventInterceptor* event_interceptor;
    
    // 当前活跃的游标
    SIncrementalCursor* active_cursor;
    
    // 重试和异常处理
    SRetryContext retry_context;
    char* last_error_message;
    uint64_t error_count;
    uint64_t retry_count;
    
    // 统计信息
    uint64_t total_backup_blocks;
    uint64_t total_backup_size;
    uint64_t backup_duration_ms;
} SBackupCoordinator;

// 备份协同器API

/**
 * 初始化备份协同器
 * @param config 协同器配置
 * @param bitmap_engine 位图引擎实例
 * @param event_interceptor 事件拦截器实例
 * @return 协同器实例，失败返回NULL
 */
SBackupCoordinator* backup_coordinator_init(const SBackupCoordinatorConfig* config,
                                           SBitmapEngine* bitmap_engine,
                                           SEventInterceptor* event_interceptor);

/**
 * 销毁备份协同器
 * @param coordinator 协同器实例
 */
void backup_coordinator_destroy(SBackupCoordinator* coordinator);

/**
 * 获取指定WAL偏移量范围内的脏块
 * @param coordinator 协同器实例
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param block_ids 输出块ID数组
 * @param max_count 最大返回数量
 * @return 实际返回的块数量
 */
uint32_t backup_coordinator_get_dirty_blocks(SBackupCoordinator* coordinator,
                                            uint64_t start_wal, uint64_t end_wal,
                                            uint64_t* block_ids, uint32_t max_count);

/**
 * 创建增量游标
 * @param coordinator 协同器实例
 * @param cursor_type 游标类型
 * @param start_time 开始时间
 * @param end_time 结束时间
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @return 游标实例，失败返回NULL
 */
SIncrementalCursor* backup_coordinator_create_cursor(SBackupCoordinator* coordinator,
                                                    ECursorType cursor_type,
                                                    int64_t start_time, int64_t end_time,
                                                    uint64_t start_wal, uint64_t end_wal);

/**
 * 销毁增量游标
 * @param coordinator 协同器实例
 * @param cursor 游标实例
 */
void backup_coordinator_destroy_cursor(SBackupCoordinator* coordinator, SIncrementalCursor* cursor);

/**
 * 从游标获取下一批增量数据
 * @param coordinator 协同器实例
 * @param cursor 游标实例
 * @param blocks 输出增量块数组
 * @param max_count 最大返回数量
 * @return 实际返回的块数量
 */
uint32_t backup_coordinator_get_next_batch(SBackupCoordinator* coordinator,
                                          SIncrementalCursor* cursor,
                                          SIncrementalBlock* blocks, uint32_t max_count);

/**
 * 估算增量备份大小
 * @param coordinator 协同器实例
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param estimated_blocks 估算块数
 * @param estimated_size 估算大小(字节)
 * @return 0成功，非0失败
 */
int32_t backup_coordinator_estimate_size(SBackupCoordinator* coordinator,
                                        uint64_t start_wal, uint64_t end_wal,
                                        uint64_t* estimated_blocks, uint64_t* estimated_size);

/**
 * 生成增量备份元数据
 * @param coordinator 协同器实例
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param metadata 输出元数据
 * @param metadata_size 元数据大小
 * @return 0成功，非0失败
 */
int32_t backup_coordinator_generate_metadata(SBackupCoordinator* coordinator,
                                            uint64_t start_wal, uint64_t end_wal,
                                            void** metadata, uint32_t* metadata_size);

/**
 * 验证增量备份完整性
 * @param coordinator 协同器实例
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param blocks 块数组
 * @param block_count 块数量
 * @return 0成功，非0失败
 */
int32_t backup_coordinator_validate_backup(SBackupCoordinator* coordinator,
                                          uint64_t start_wal, uint64_t end_wal,
                                          const SIncrementalBlock* blocks, uint32_t block_count);

/**
 * 获取备份统计信息
 * @param coordinator 协同器实例
 * @param total_blocks 总块数
 * @param total_size 总大小
 * @param duration_ms 持续时间
 */
void backup_coordinator_get_stats(SBackupCoordinator* coordinator,
                                 uint64_t* total_blocks, uint64_t* total_size, uint64_t* duration_ms);

// 重试和异常处理API

/**
 * 初始化重试上下文
 * @param retry_context 重试上下文
 * @param max_retry 最大重试次数
 * @param retry_interval 重试间隔(秒)
 * @return 0成功，非0失败
 */
int32_t backup_retry_context_init(SRetryContext* retry_context, uint32_t max_retry, uint32_t retry_interval);

/**
 * 销毁重试上下文
 * @param retry_context 重试上下文
 */
void backup_retry_context_destroy(SRetryContext* retry_context);

/**
 * 执行重试操作
 * @param retry_context 重试上下文
 * @param operation 操作函数指针
 * @param user_data 用户数据
 * @return 0成功，非0失败
 */
int32_t backup_execute_with_retry(SRetryContext* retry_context, 
                                  int32_t (*operation)(void*), void* user_data);

/**
 * 检查是否应该重试
 * @param retry_context 重试上下文
 * @param error_code 错误码
 * @return true应该重试，false不应该重试
 */
bool backup_should_retry(SRetryContext* retry_context, int32_t error_code);

/**
 * 等待重试间隔
 * @param retry_context 重试上下文
 */
void backup_wait_for_retry(SRetryContext* retry_context);

/**
 * 记录错误信息
 * @param coordinator 协同器实例
 * @param error_code 错误码
 * @param error_message 错误信息
 */
void backup_record_error(SBackupCoordinator* coordinator, int32_t error_code, const char* error_message);

/**
 * 获取最后一次错误信息
 * @param coordinator 协同器实例
 * @return 错误信息字符串
 */
const char* backup_get_last_error(SBackupCoordinator* coordinator);

/**
 * 清除错误信息
 * @param coordinator 协同器实例
 */
void backup_clear_error(SBackupCoordinator* coordinator);

/**
 * 获取错误统计信息
 * @param coordinator 协同器实例
 * @param error_count 错误次数
 * @param retry_count 重试次数
 */
void backup_get_error_stats(SBackupCoordinator* coordinator, uint64_t* error_count, uint64_t* retry_count);

/**
 * 带重试的文件写入操作
 * @param coordinator 协同器实例
 * @param file_path 文件路径
 * @param data 数据指针
 * @param data_size 数据大小
 * @return 0成功，非0失败
 */
int32_t backup_write_file_with_retry(SBackupCoordinator* coordinator, 
                                     const char* file_path, 
                                     const void* data, 
                                     size_t data_size);

// taosX插件接口函数

/**
 * 获取插件名称
 * @return 插件名称字符串
 */
PLUGIN_EXPORT const char* backup_plugin_name(void);

/**
 * 获取插件版本
 * @return 插件版本字符串
 */
PLUGIN_EXPORT const char* backup_plugin_version(void);

/**
 * 初始化插件
 * @param config_str 配置字符串
 * @param config_len 配置长度
 * @return 0成功，非0失败
 */
PLUGIN_EXPORT int32_t backup_plugin_init(const char* config_str, uint32_t config_len);

/**
 * 清理插件
 */
PLUGIN_EXPORT void backup_plugin_cleanup(void);

/**
 * 获取增量块集合
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param block_ids 输出块ID数组
 * @param max_count 最大返回数量
 * @return 实际返回的块数量
 */
PLUGIN_EXPORT uint32_t backup_plugin_get_dirty_blocks(uint64_t start_wal, uint64_t end_wal,
                                       uint64_t* block_ids, uint32_t max_count);

/**
 * 创建增量游标
 * @param cursor_type 游标类型
 * @param start_time 开始时间
 * @param end_time 结束时间
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @return 游标句柄，失败返回NULL
 */
PLUGIN_EXPORT void* backup_plugin_create_incremental_cursor(int32_t cursor_type,
                                             int64_t start_time, int64_t end_time,
                                             uint64_t start_wal, uint64_t end_wal);

/**
 * 估算增量备份大小
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param estimated_blocks 估算块数
 * @param estimated_size 估算大小
 * @return 0成功，非0失败
 */
PLUGIN_EXPORT int32_t backup_plugin_estimate_backup_size(uint64_t start_wal, uint64_t end_wal,
                                          uint64_t* estimated_blocks, uint64_t* estimated_size);

/**
 * 销毁增量游标
 * @param cursor 游标句柄
 */
PLUGIN_EXPORT void backup_plugin_destroy_cursor(void* cursor);

/**
 * 获取下一批数据块
 * @param cursor 游标句柄
 * @param blocks 输出数据块数组
 * @param max_count 最大返回数量
 * @return 实际返回的块数量
 */
PLUGIN_EXPORT uint32_t backup_plugin_get_next_batch(void* cursor, SIncrementalBlock* blocks, uint32_t max_count);

/**
 * 生成备份元数据
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param metadata 输出元数据指针
 * @param metadata_size 输出元数据大小
 * @return 0成功，非0失败
 */
PLUGIN_EXPORT int32_t backup_plugin_generate_metadata(uint64_t start_wal, uint64_t end_wal,
                                       void** metadata, uint32_t* metadata_size);

/**
 * 验证备份完整性
 * @param start_wal 开始WAL偏移量
 * @param end_wal 结束WAL偏移量
 * @param blocks 数据块数组
 * @param block_count 块数量
 * @return 0成功，非0失败
 */
PLUGIN_EXPORT int32_t backup_plugin_validate_backup(uint64_t start_wal, uint64_t end_wal,
                                     const SIncrementalBlock* blocks, uint32_t block_count);

/**
 * 获取插件统计信息
 * @param total_blocks 总块数
 * @param total_size 总大小
 * @param duration_ms 持续时间
 */
PLUGIN_EXPORT void backup_plugin_get_stats(uint64_t* total_blocks, uint64_t* total_size, uint64_t* duration_ms);

/**
 * 获取最后一次错误信息
 * @return 错误信息字符串
 */
PLUGIN_EXPORT const char* backup_plugin_get_last_error(void);

/**
 * 获取错误统计信息
 * @param error_count 错误次数
 * @param retry_count 重试次数
 */
PLUGIN_EXPORT void backup_plugin_get_error_stats(uint64_t* error_count, uint64_t* retry_count);

/**
 * 清除错误信息
 */
PLUGIN_EXPORT void backup_plugin_clear_error(void);

// taosX标准备份文件头定义
#define TAOSX_FILE_MAGIC "TAOSZ"
#define TAOSX_FILE_MAGIC_LEN 4
#define TAOSX_HEADER_VERSION 010  // 八进制，1.0
#define TAOSX_COMMIT_ID_LEN 40
#define TAOSX_OBJ_NAME_MAX_LEN 256

typedef struct {
    char magic[TAOSX_FILE_MAGIC_LEN];      // "TAOSZ"
    uint16_t version;                      // 0o10
    char api_commit_id[TAOSX_COMMIT_ID_LEN];   // taosX commit id
    char server_commit_id[TAOSX_COMMIT_ID_LEN];// TDengine commit id
    uint8_t obj_name_len;                  // 备份对象名长度
    char obj_name[TAOSX_OBJ_NAME_MAX_LEN]; // 备份对象名（最大256，实际可变）
    int64_t timestamp;                     // 毫秒级时间戳
    int8_t vg_id;                          // vgroup id
    uint32_t file_seq;                     // 文件序号
    // ...Body部分紧随其后
} __attribute__((packed)) TaosxBackupHeader;

// taosX标准备份文件Body Block头定义
typedef struct {
    uint8_t block_type;        // 1/2/3
    uint32_t msg_len;          // 消息体长度
    uint16_t msg_type;         // 消息类型
    // uint8_t msg_body[];     // 消息体，长度为msg_len
} __attribute__((packed)) TaosxBackupBlockHeader;

#ifdef __cplusplus
}
#endif

#endif // TDENGINE_BACKUP_COORDINATOR_H 