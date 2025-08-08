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

#include "backup_coordinator.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include <zlib.h> // for crc32
#include <unistd.h> // for sleep
#include <errno.h>

// 全局插件实例
static SBackupCoordinator* g_backup_coordinator = NULL;
static pthread_mutex_t g_plugin_mutex = PTHREAD_MUTEX_INITIALIZER;

// 获取当前时间戳（毫秒）
static uint64_t get_current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

// 获取当前时间戳（秒）
static uint64_t get_current_time_s() {
    return (uint64_t)time(NULL);
}

// 错误码到错误信息的映射
static const char* get_error_message(int32_t error_code) {
    switch (error_code) {
        case BACKUP_SUCCESS:
            return "Success";
        case BACKUP_ERROR_INVALID_PARAM:
            return "Invalid parameter";
        case BACKUP_ERROR_INIT_FAILED:
            return "Initialization failed";
        case BACKUP_ERROR_NOT_INITIALIZED:
            return "Not initialized";
        case BACKUP_ERROR_MEMORY_ALLOC:
            return "Memory allocation failed";
        case BACKUP_ERROR_FILE_IO:
            return "File I/O error";
        case BACKUP_ERROR_NETWORK:
            return "Network error";
        case BACKUP_ERROR_TIMEOUT:
            return "Operation timeout";
        case BACKUP_ERROR_DATA_CORRUPTION:
            return "Data corruption detected";
        case BACKUP_ERROR_PERMISSION_DENIED:
            return "Permission denied";
        case BACKUP_ERROR_DISK_FULL:
            return "Disk full";
        case BACKUP_ERROR_CONNECTION_LOST:
            return "Connection lost";
        case BACKUP_ERROR_RETRY_EXHAUSTED:
            return "Retry attempts exhausted";
        default:
            return "Unknown error";
    }
}

// 检查错误是否可重试
static bool is_retryable_error(int32_t error_code) {
    switch (error_code) {
        case BACKUP_ERROR_NETWORK:
        case BACKUP_ERROR_TIMEOUT:
        case BACKUP_ERROR_CONNECTION_LOST:
        case BACKUP_ERROR_FILE_IO:
            return true;
        default:
            return false;
    }
}

SBackupCoordinator* backup_coordinator_init(const SBackupCoordinatorConfig* config,
                                           SBitmapEngine* bitmap_engine,
                                           SEventInterceptor* event_interceptor) {
    if (config == NULL || bitmap_engine == NULL || event_interceptor == NULL) {
        return NULL;
    }
    
    SBackupCoordinator* coordinator = (SBackupCoordinator*)malloc(sizeof(SBackupCoordinator));
    if (coordinator == NULL) {
        return NULL;
    }
    
    // 复制配置
    coordinator->config = *config;
    if (config->encryption_key != NULL) {
        coordinator->config.encryption_key = strdup(config->encryption_key);
    }
    if (config->error_store_path != NULL) {
        coordinator->config.error_store_path = strdup(config->error_store_path);
    }
    if (config->backup_path != NULL) {
        coordinator->config.backup_path = strdup(config->backup_path);
    }
    
    coordinator->bitmap_engine = bitmap_engine;
    coordinator->event_interceptor = event_interceptor;
    coordinator->active_cursor = NULL;
    
    // 初始化重试上下文
    backup_retry_context_init(&coordinator->retry_context, 
                             config->error_retry_max, 
                             config->error_retry_interval);
    
    // 初始化错误处理
    coordinator->last_error_message = NULL;
    coordinator->error_count = 0;
    coordinator->retry_count = 0;
    
    // 初始化统计信息
    coordinator->total_backup_blocks = 0;
    coordinator->total_backup_size = 0;
    coordinator->backup_duration_ms = 0;
    
    return coordinator;
}

void backup_coordinator_destroy(SBackupCoordinator* coordinator) {
    if (coordinator == NULL) {
        return;
    }
    
    // 销毁活跃游标
    if (coordinator->active_cursor) {
        backup_coordinator_destroy_cursor(coordinator, coordinator->active_cursor);
    }
    
    // 销毁重试上下文
    backup_retry_context_destroy(&coordinator->retry_context);
    
    // 销毁配置
    if (coordinator->config.encryption_key) {
        free(coordinator->config.encryption_key);
    }
    if (coordinator->config.error_store_path) {
        free(coordinator->config.error_store_path);
    }
    if (coordinator->config.backup_path) {
        free(coordinator->config.backup_path);
    }
    
    // 销毁错误信息
    if (coordinator->last_error_message) {
        free(coordinator->last_error_message);
    }
    
    free(coordinator);
}

uint32_t backup_coordinator_get_dirty_blocks(SBackupCoordinator* coordinator,
                                            uint64_t start_wal, uint64_t end_wal,
                                            uint64_t* block_ids, uint32_t max_count) {
    if (coordinator == NULL || block_ids == NULL || max_count == 0) {
        return 0;
    }
    
    return bitmap_engine_get_dirty_blocks_by_wal(coordinator->bitmap_engine,
                                                start_wal, end_wal, block_ids, max_count);
}

SIncrementalCursor* backup_coordinator_create_cursor(SBackupCoordinator* coordinator,
                                                    ECursorType cursor_type,
                                                    int64_t start_time, int64_t end_time,
                                                    uint64_t start_wal, uint64_t end_wal) {
    if (coordinator == NULL) {
        return NULL;
    }
    
    SIncrementalCursor* cursor = (SIncrementalCursor*)malloc(sizeof(SIncrementalCursor));
    if (cursor == NULL) {
        return NULL;
    }
    
    cursor->type = cursor_type;
    cursor->start_time = start_time;
    cursor->end_time = end_time;
    cursor->start_wal = start_wal;
    cursor->end_wal = end_wal;
    cursor->current_block = 0;
    cursor->block_count = 0;
    cursor->has_more = true;
    
    // 计算总块数
    uint64_t temp_block_ids[1];
    uint32_t count = bitmap_engine_get_dirty_blocks_by_wal(coordinator->bitmap_engine,
                                                          start_wal, end_wal, temp_block_ids, 1);
    if (count > 0) {
        // 这里简化实现，实际需要获取完整的块列表来计算总数
        cursor->block_count = 1000; // 估算值
    }
    
    return cursor;
}

void backup_coordinator_destroy_cursor(SBackupCoordinator* coordinator, SIncrementalCursor* cursor) {
    if (cursor == NULL) {
        return;
    }
    
    free(cursor);
}

uint32_t backup_coordinator_get_next_batch(SBackupCoordinator* coordinator,
                                          SIncrementalCursor* cursor,
                                          SIncrementalBlock* blocks, uint32_t max_count) {
    if (coordinator == NULL || cursor == NULL || blocks == NULL || max_count == 0) {
        return 0;
    }
    
    if (!cursor->has_more) {
        return 0;
    }
    
    // 获取下一批块ID
    uint64_t* block_ids = (uint64_t*)malloc(sizeof(uint64_t) * max_count);
    if (block_ids == NULL) {
        return 0;
    }
    
    uint32_t count = bitmap_engine_get_dirty_blocks_by_wal(coordinator->bitmap_engine,
                                                          cursor->start_wal, cursor->end_wal,
                                                          block_ids, max_count);
    
    // 填充增量块信息
    for (uint32_t i = 0; i < count; i++) {
        SBlockMetadata metadata;
        if (bitmap_engine_get_block_metadata(coordinator->bitmap_engine, block_ids[i], &metadata) == 0) {
            blocks[i].block_id = block_ids[i];
            blocks[i].wal_offset = metadata.wal_offset;
            blocks[i].timestamp = metadata.timestamp;
            blocks[i].state = metadata.state;
            blocks[i].data = NULL; // 实际需要从存储引擎读取
            blocks[i].data_size = 0;
        }
    }
    
    free(block_ids);
    
    // 更新游标状态
    cursor->current_block += count;
    if (cursor->current_block >= cursor->block_count || count < max_count) {
        cursor->has_more = false;
    }
    
    return count;
}

int32_t backup_coordinator_estimate_size(SBackupCoordinator* coordinator,
                                        uint64_t start_wal, uint64_t end_wal,
                                        uint64_t* estimated_blocks, uint64_t* estimated_size) {
    if (coordinator == NULL || estimated_blocks == NULL || estimated_size == NULL) {
        return -1;
    }
    
    // 获取块数量
    uint64_t temp_block_ids[1];
    uint32_t count = bitmap_engine_get_dirty_blocks_by_wal(coordinator->bitmap_engine,
                                                          start_wal, end_wal, temp_block_ids, 1);
    
    // 估算块数（这里简化实现）
    *estimated_blocks = count * 1000; // 估算值
    
    // 估算大小（假设每个块平均1MB）
    *estimated_size = *estimated_blocks * 1024 * 1024;
    
    return 0;
}

int32_t backup_coordinator_generate_metadata(SBackupCoordinator* coordinator,
                                            uint64_t start_wal, uint64_t end_wal,
                                            void** metadata, uint32_t* metadata_size) {
    if (coordinator == NULL || metadata == NULL || metadata_size == NULL) {
        return -1;
    }
    
    // 生成元数据结构
    typedef struct {
        uint64_t start_wal;
        uint64_t end_wal;
        uint64_t block_count;
        uint64_t total_size;
        int64_t create_time;
        uint32_t version;
    } SBackupMetadata;
    
    SBackupMetadata* meta = (SBackupMetadata*)malloc(sizeof(SBackupMetadata));
    if (meta == NULL) {
        return -1;
    }
    
    meta->start_wal = start_wal;
    meta->end_wal = end_wal;
    meta->create_time = get_current_time_ms();
    meta->version = 1;
    
    // 计算块数和大小
    backup_coordinator_estimate_size(coordinator, start_wal, end_wal,
                                   &meta->block_count, &meta->total_size);
    
    *metadata = meta;
    *metadata_size = sizeof(SBackupMetadata);
    
    return 0;
}

int32_t backup_coordinator_validate_backup(SBackupCoordinator* coordinator,
                                          uint64_t start_wal, uint64_t end_wal,
                                          const SIncrementalBlock* blocks, uint32_t block_count) {
    if (coordinator == NULL || blocks == NULL) {
        return -1;
    }
    
    // 验证块完整性
    for (uint32_t i = 0; i < block_count; i++) {
        SBlockMetadata metadata;
        if (bitmap_engine_get_block_metadata(coordinator->bitmap_engine, blocks[i].block_id, &metadata) != 0) {
            return -1; // 块不存在
        }
        
        if (metadata.wal_offset < start_wal || metadata.wal_offset > end_wal) {
            return -1; // 块不在指定范围内
        }
    }
    
    return 0;
}

void backup_coordinator_get_stats(SBackupCoordinator* coordinator,
                                 uint64_t* total_blocks, uint64_t* total_size, uint64_t* duration_ms) {
    if (coordinator == NULL) {
        return;
    }
    
    if (total_blocks) *total_blocks = coordinator->total_backup_blocks;
    if (total_size) *total_size = coordinator->total_backup_size;
    if (duration_ms) *duration_ms = coordinator->backup_duration_ms;
}

// taosX插件接口实现

const char* backup_plugin_name(void) {
    return "incremental_bitmap_backup";
}

const char* backup_plugin_version(void) {
    return "1.0.0";
}

int32_t backup_plugin_init(const char* config_str, uint32_t config_len) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator != NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1; // 已经初始化
    }
    
    // 解析配置字符串（简化实现）
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 1000,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL,
        .error_retry_max = 10,           // 默认最大重试次数
        .error_retry_interval = 5,        // 默认重试间隔5秒
        .error_store_path = NULL,
        .enable_error_logging = true,
        .error_buffer_size = 1000,
        .backup_path = NULL,
        .backup_max_size = 1024 * 1024 * 1024,  // 默认1GB
        .compression_level = 1            // 默认fastest
    };
    
    // 创建位图引擎
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    if (bitmap_engine == NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    // 创建事件拦截器
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 10000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    if (event_interceptor == NULL) {
        bitmap_engine_destroy(bitmap_engine);
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    // 创建备份协同器
    g_backup_coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    if (g_backup_coordinator == NULL) {
        event_interceptor_destroy(event_interceptor);
        bitmap_engine_destroy(bitmap_engine);
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    // 启动事件拦截器
    if (event_interceptor_start(event_interceptor) != 0) {
        backup_coordinator_destroy(g_backup_coordinator);
        event_interceptor_destroy(event_interceptor);
        bitmap_engine_destroy(bitmap_engine);
        g_backup_coordinator = NULL;
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return 0;
}

void backup_plugin_cleanup(void) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator != NULL) {
        // 停止事件拦截器
        event_interceptor_stop(g_backup_coordinator->event_interceptor);
        
        // 销毁所有组件
        backup_coordinator_destroy(g_backup_coordinator);
        event_interceptor_destroy(g_backup_coordinator->event_interceptor);
        bitmap_engine_destroy(g_backup_coordinator->bitmap_engine);
        
        g_backup_coordinator = NULL;
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
}

uint32_t backup_plugin_get_dirty_blocks(uint64_t start_wal, uint64_t end_wal,
                                       uint64_t* block_ids, uint32_t max_count) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator == NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return 0;
    }
    
    uint32_t count = backup_coordinator_get_dirty_blocks(g_backup_coordinator,
                                                        start_wal, end_wal, block_ids, max_count);
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return count;
}

void* backup_plugin_create_incremental_cursor(int32_t cursor_type,
                                             int64_t start_time, int64_t end_time,
                                             uint64_t start_wal, uint64_t end_wal) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator == NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return NULL;
    }
    
    SIncrementalCursor* cursor = backup_coordinator_create_cursor(g_backup_coordinator,
                                                                 (ECursorType)cursor_type,
                                                                 start_time, end_time,
                                                                 start_wal, end_wal);
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return cursor;
}

int32_t backup_plugin_estimate_backup_size(uint64_t start_wal, uint64_t end_wal,
                                          uint64_t* estimated_blocks, uint64_t* estimated_size) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator == NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    int32_t result = backup_coordinator_estimate_size(g_backup_coordinator,
                                                     start_wal, end_wal,
                                                     estimated_blocks, estimated_size);
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return result;
} 

void backup_plugin_destroy_cursor(void* cursor) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator != NULL && cursor != NULL) {
        backup_coordinator_destroy_cursor(g_backup_coordinator, (SIncrementalCursor*)cursor);
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
}

uint32_t backup_plugin_get_next_batch(void* cursor, SIncrementalBlock* blocks, uint32_t max_count) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator == NULL || cursor == NULL || blocks == NULL || max_count == 0) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return 0;
    }
    
    uint32_t count = backup_coordinator_get_next_batch(g_backup_coordinator,
                                                      (SIncrementalCursor*)cursor,
                                                      blocks, max_count);
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return count;
}

int32_t backup_plugin_generate_metadata(uint64_t start_wal, uint64_t end_wal,
                                       void** metadata, uint32_t* metadata_size) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator == NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    int32_t result = backup_coordinator_generate_metadata(g_backup_coordinator,
                                                         start_wal, end_wal,
                                                         metadata, metadata_size);
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return result;
}

int32_t backup_plugin_validate_backup(uint64_t start_wal, uint64_t end_wal,
                                     const SIncrementalBlock* blocks, uint32_t block_count) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator == NULL) {
        pthread_mutex_unlock(&g_plugin_mutex);
        return -1;
    }
    
    int32_t result = backup_coordinator_validate_backup(g_backup_coordinator,
                                                       start_wal, end_wal,
                                                       blocks, block_count);
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return result;
}

void backup_plugin_get_stats(uint64_t* total_blocks, uint64_t* total_size, uint64_t* duration_ms) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator != NULL) {
        backup_coordinator_get_stats(g_backup_coordinator, total_blocks, total_size, duration_ms);
    } else {
        if (total_blocks) *total_blocks = 0;
        if (total_size) *total_size = 0;
        if (duration_ms) *duration_ms = 0;
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
}

const char* backup_plugin_get_last_error(void) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    const char* error_msg = NULL;
    if (g_backup_coordinator != NULL) {
        error_msg = backup_get_last_error(g_backup_coordinator);
    } else {
        error_msg = "Plugin not initialized";
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
    return error_msg;
}

void backup_plugin_get_error_stats(uint64_t* error_count, uint64_t* retry_count) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator != NULL) {
        backup_get_error_stats(g_backup_coordinator, error_count, retry_count);
    } else {
        if (error_count) *error_count = 0;
        if (retry_count) *retry_count = 0;
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
}

void backup_plugin_clear_error(void) {
    pthread_mutex_lock(&g_plugin_mutex);
    
    if (g_backup_coordinator != NULL) {
        backup_clear_error(g_backup_coordinator);
    }
    
    pthread_mutex_unlock(&g_plugin_mutex);
}

// 写入taosX标准Header
int write_taosx_backup_header(FILE* fp, const TaosxBackupHeader* header) {
    if (!fp || !header) return -1;
    size_t written = 0;
    written += fwrite(header->magic, 1, TAOSX_FILE_MAGIC_LEN, fp);
    written += fwrite(&header->version, 1, 2, fp);
    written += fwrite(header->api_commit_id, 1, TAOSX_COMMIT_ID_LEN, fp);
    written += fwrite(header->server_commit_id, 1, TAOSX_COMMIT_ID_LEN, fp);
    written += fwrite(&header->obj_name_len, 1, 1, fp);
    written += fwrite(header->obj_name, 1, header->obj_name_len, fp);
    written += fwrite(&header->timestamp, 1, 8, fp);
    written += fwrite(&header->vg_id, 1, 1, fp);
    written += fwrite(&header->file_seq, 1, 4, fp);
    return (written > 0) ? 0 : -1;
}

// 读取taosX标准Header
int read_taosx_backup_header(FILE* fp, TaosxBackupHeader* header) {
    if (!fp || !header) return -1;
    if (fread(header->magic, 1, TAOSX_FILE_MAGIC_LEN, fp) != TAOSX_FILE_MAGIC_LEN) return -1;
    header->magic[TAOSX_FILE_MAGIC_LEN] = '\0'; // 添加字符串结束符
    if (fread(&header->version, 1, 2, fp) != 2) return -1;
    if (fread(header->api_commit_id, 1, TAOSX_COMMIT_ID_LEN, fp) != TAOSX_COMMIT_ID_LEN) return -1;
    header->api_commit_id[TAOSX_COMMIT_ID_LEN] = '\0'; // 添加字符串结束符
    if (fread(header->server_commit_id, 1, TAOSX_COMMIT_ID_LEN, fp) != TAOSX_COMMIT_ID_LEN) return -1;
    header->server_commit_id[TAOSX_COMMIT_ID_LEN] = '\0'; // 添加字符串结束符
    if (fread(&header->obj_name_len, 1, 1, fp) != 1) return -1;
    if (header->obj_name_len > TAOSX_OBJ_NAME_MAX_LEN) return -1;
    if (fread(header->obj_name, 1, header->obj_name_len, fp) != header->obj_name_len) return -1;
    header->obj_name[header->obj_name_len] = '\0';
    if (fread(&header->timestamp, 1, 8, fp) != 8) return -1;
    if (fread(&header->vg_id, 1, 1, fp) != 1) return -1;
    if (fread(&header->file_seq, 1, 4, fp) != 4) return -1;
    return 0;
}

// 写入一个Block（不含Body CRC）
int write_taosx_backup_block(FILE* fp, const TaosxBackupBlockHeader* block, const void* msg_body) {
    if (!fp || !block || (!msg_body && block->msg_len > 0)) return -1;
    size_t written = 0;
    written += fwrite(&block->block_type, 1, 1, fp);
    written += fwrite(&block->msg_len, 1, 4, fp);
    written += fwrite(&block->msg_type, 1, 2, fp);
    if (block->msg_len > 0) {
        written += fwrite(msg_body, 1, block->msg_len, fp);
    }
    return (written > 0) ? 0 : -1;
}

// 读取一个Block（不含Body CRC）
int read_taosx_backup_block(FILE* fp, TaosxBackupBlockHeader* block, void* msg_body_buf, size_t buf_size) {
    if (!fp || !block) return -1;
    if (fread(&block->block_type, 1, 1, fp) != 1) return -1;
    if (fread(&block->msg_len, 1, 4, fp) != 4) return -1;
    if (fread(&block->msg_type, 1, 2, fp) != 2) return -1;
    if (block->msg_len > 0) {
        if (!msg_body_buf || buf_size < block->msg_len) return -1;
        if (fread(msg_body_buf, 1, block->msg_len, fp) != block->msg_len) return -1;
    }
    return 0;
}

// 写入Body CRC32
int write_taosx_body_crc32(FILE* fp, const void* body_buf, size_t body_len) {
    if (!fp || !body_buf || body_len == 0) return -1;
    uint32_t crc = crc32(0, (const Bytef*)body_buf, body_len);
    return fwrite(&crc, 1, 4, fp) == 4 ? 0 : -1;
}

// 读取Body CRC32
int read_taosx_body_crc32(FILE* fp, uint32_t* crc) {
    if (!fp || !crc) return -1;
    return fread(crc, 1, 4, fp) == 4 ? 0 : -1;
}

// 重试和异常处理实现

int32_t backup_retry_context_init(SRetryContext* retry_context, uint32_t max_retry, uint32_t retry_interval) {
    if (retry_context == NULL) {
        return BACKUP_ERROR_INVALID_PARAM;
    }
    
    retry_context->current_retry = 0;
    retry_context->max_retry = max_retry;
    retry_context->retry_interval = retry_interval;
    retry_context->last_retry_time = 0;
    retry_context->state = RETRY_STATE_IDLE;
    retry_context->last_error = BACKUP_SUCCESS;
    retry_context->error_message = NULL;
    
    return BACKUP_SUCCESS;
}

void backup_retry_context_destroy(SRetryContext* retry_context) {
    if (retry_context == NULL) {
        return;
    }
    
    if (retry_context->error_message) {
        free(retry_context->error_message);
        retry_context->error_message = NULL;
    }
}

int32_t backup_execute_with_retry(SRetryContext* retry_context, 
                                  int32_t (*operation)(void*), void* user_data) {
    if (retry_context == NULL || operation == NULL) {
        return BACKUP_ERROR_INVALID_PARAM;
    }
    
    retry_context->state = RETRY_STATE_RETRYING;
    retry_context->current_retry = 0;
    
    while (retry_context->current_retry <= retry_context->max_retry) {
        int32_t result = operation(user_data);
        
        if (result == BACKUP_SUCCESS) {
            retry_context->state = RETRY_STATE_SUCCESS;
            return BACKUP_SUCCESS;
        }
        
        retry_context->last_error = result;
        
        if (!is_retryable_error(result) || retry_context->current_retry >= retry_context->max_retry) {
            retry_context->state = RETRY_STATE_FAILED;
            return result;
        }
        
        retry_context->current_retry++;
        backup_wait_for_retry(retry_context);
    }
    
    retry_context->state = RETRY_STATE_FAILED;
    return BACKUP_ERROR_RETRY_EXHAUSTED;
}

bool backup_should_retry(SRetryContext* retry_context, int32_t error_code) {
    if (retry_context == NULL) {
        return false;
    }
    
    return is_retryable_error(error_code) && 
           retry_context->current_retry < retry_context->max_retry;
}

void backup_wait_for_retry(SRetryContext* retry_context) {
    if (retry_context == NULL) {
        return;
    }
    
    retry_context->last_retry_time = get_current_time_s();
    sleep(retry_context->retry_interval);
}

void backup_record_error(SBackupCoordinator* coordinator, int32_t error_code, const char* error_message) {
    if (coordinator == NULL) {
        return;
    }
    
    coordinator->error_count++;
    
    // 释放旧的错误信息
    if (coordinator->last_error_message) {
        free(coordinator->last_error_message);
    }
    
    // 分配新的错误信息
    if (error_message != NULL) {
        size_t len = strlen(error_message) + 1;
        coordinator->last_error_message = (char*)malloc(len);
        if (coordinator->last_error_message) {
            strcpy(coordinator->last_error_message, error_message);
        }
    } else {
        coordinator->last_error_message = NULL;
    }
    
    // 记录到错误存储路径（如果配置了）
    if (coordinator->config.error_store_path && coordinator->config.enable_error_logging) {
        char error_file[512];
        snprintf(error_file, sizeof(error_file), "%s/backup_error_%lu.log", 
                coordinator->config.error_store_path, get_current_time_ms());
        
        FILE* fp = fopen(error_file, "a");
        if (fp) {
            fprintf(fp, "[%lu] Error %d: %s\n", get_current_time_ms(), 
                    error_code, error_message ? error_message : get_error_message(error_code));
            fclose(fp);
        }
    }
}

const char* backup_get_last_error(SBackupCoordinator* coordinator) {
    if (coordinator == NULL) {
        return "Coordinator is NULL";
    }
    
    if (coordinator->last_error_message) {
        return coordinator->last_error_message;
    }
    
    return get_error_message(coordinator->retry_context.last_error);
}

void backup_clear_error(SBackupCoordinator* coordinator) {
    if (coordinator == NULL) {
        return;
    }
    
    if (coordinator->last_error_message) {
        free(coordinator->last_error_message);
        coordinator->last_error_message = NULL;
    }
    
    coordinator->retry_context.last_error = BACKUP_SUCCESS;
    coordinator->retry_context.state = RETRY_STATE_IDLE;
}

void backup_get_error_stats(SBackupCoordinator* coordinator, uint64_t* error_count, uint64_t* retry_count) {
    if (coordinator == NULL) {
        if (error_count) *error_count = 0;
        if (retry_count) *retry_count = 0;
        return;
    }
    
    if (error_count) *error_count = coordinator->error_count;
    if (retry_count) *retry_count = coordinator->retry_count;
}

// 带重试的文件写入操作示例
typedef struct {
    const char* file_path;
    const void* data;
    size_t data_size;
} SFileWriteContext;

static int32_t file_write_operation(void* user_data) {
    SFileWriteContext* ctx = (SFileWriteContext*)user_data;
    if (ctx == NULL || ctx->file_path == NULL || ctx->data == NULL) {
        return BACKUP_ERROR_INVALID_PARAM;
    }
    
    FILE* fp = fopen(ctx->file_path, "wb");
    if (fp == NULL) {
        return BACKUP_ERROR_FILE_IO;
    }
    
    size_t written = fwrite(ctx->data, 1, ctx->data_size, fp);
    fclose(fp);
    
    if (written != ctx->data_size) {
        return BACKUP_ERROR_FILE_IO;
    }
    
    return BACKUP_SUCCESS;
}

int32_t backup_write_file_with_retry(SBackupCoordinator* coordinator, 
                                     const char* file_path, 
                                     const void* data, 
                                     size_t data_size) {
    if (coordinator == NULL || file_path == NULL || data == NULL) {
        return BACKUP_ERROR_INVALID_PARAM;
    }
    
    // 使用静态变量避免局部变量被销毁的问题
    static SFileWriteContext ctx;
    ctx.file_path = file_path;
    ctx.data = data;
    ctx.data_size = data_size;
    
    int32_t result = backup_execute_with_retry(&coordinator->retry_context, 
                                              file_write_operation, &ctx);
    
    if (result != BACKUP_SUCCESS) {
        backup_record_error(coordinator, result, "File write operation failed");
    }
    
    return result;
} 