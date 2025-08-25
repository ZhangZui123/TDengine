#include "bitmap_engine.h"
#include "event_interceptor.h"
#include "backup_coordinator.h"
#include "storage_engine_interface.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

// 增量备份工具配置
typedef struct {
    char* source_host;
    int source_port;
    char* database;
    char* backup_path;
    char* bitmap_cache_path;
    int64_t since_timestamp;
    uint32_t batch_size;
    bool enable_compression;
    bool enable_encryption;
} SIncrementalBackupConfig;

// 增量备份工具状态
typedef struct {
    SIncrementalBackupConfig config;
    SBitmapEngine* bitmap_engine;
    SEventInterceptor* event_interceptor;
    SBackupCoordinator* backup_coordinator;
    SStorageEngineInterface* storage_interface;
    bool is_running;
    uint64_t total_blocks;
    uint64_t processed_blocks;
    uint64_t failed_blocks;
} SIncrementalBackupTool;

// 创建增量备份工具
SIncrementalBackupTool* incremental_backup_tool_create(const SIncrementalBackupConfig* config) {
    if (!config) {
        return NULL;
    }
    
    SIncrementalBackupTool* tool = malloc(sizeof(SIncrementalBackupTool));
    if (!tool) {
        return NULL;
    }
    
    memset(tool, 0, sizeof(SIncrementalBackupTool));
    memcpy(&tool->config, config, sizeof(SIncrementalBackupConfig));
    
    // 初始化位图引擎
    SBitmapEngineConfig bitmap_config = {
        .max_memory_mb = 1024,
        .persistence_enabled = true,
        .persistence_path = config->bitmap_cache_path
    };
    
    tool->bitmap_engine = bitmap_engine_init(&bitmap_config);
    if (!tool->bitmap_engine) {
        free(tool);
        return NULL;
    }
    
    // 初始化事件拦截器
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 10000,
        .callback_threads = 4,
        .callback = NULL, // 将在后面设置
        .callback_user_data = tool
    };
    
    tool->event_interceptor = event_interceptor_init(&interceptor_config, tool->bitmap_engine);
    if (!tool->event_interceptor) {
        bitmap_engine_destroy(tool->bitmap_engine);
        free(tool);
        return NULL;
    }
    
    // 初始化备份协调器
    SBackupConfig backup_config = {
        .batch_size = config->batch_size,
        .max_retries = 3,
        .retry_interval_ms = 1000,
        .timeout_ms = 30000,
        .enable_compression = config->enable_compression,
        .enable_encryption = config->enable_encryption,
        .backup_path = config->backup_path,
        .temp_path = "/tmp"
    };
    
    tool->backup_coordinator = backup_coordinator_init(&backup_config, tool->bitmap_engine);
    if (!tool->backup_coordinator) {
        event_interceptor_destroy(tool->event_interceptor);
        bitmap_engine_destroy(tool->bitmap_engine);
        free(tool);
        return NULL;
    }
    
    // 获取存储引擎接口
    tool->storage_interface = get_storage_engine_interface("tdengine");
    if (tool->storage_interface) {
        event_interceptor_set_storage_interface(tool->event_interceptor, tool->storage_interface);
    }
    
    tool->is_running = false;
    return tool;
}

// 事件回调函数
static void backup_event_callback(const SBlockEvent* event, void* user_data) {
    SIncrementalBackupTool* tool = (SIncrementalBackupTool*)user_data;
    
    // 将事件转换为存储事件
    SStorageEvent storage_event = {
        .event_type = (EStorageEventType)event->event_type,
        .block_id = event->block_id,
        .wal_offset = event->wal_offset,
        .timestamp = event->timestamp,
        .user_data = NULL
    };
    
    // 触发存储引擎事件
    if (tool->storage_interface) {
        tool->storage_interface->trigger_event(&storage_event);
    }
    
    // 更新位图引擎
    switch (event->event_type) {
        case EVENT_BLOCK_CREATE:
            bitmap_engine_mark_new(tool->bitmap_engine, event->block_id, 
                                 event->wal_offset, event->timestamp);
            break;
        case EVENT_BLOCK_UPDATE:
            bitmap_engine_mark_dirty(tool->bitmap_engine, event->block_id, 
                                   event->wal_offset, event->timestamp);
            break;
        case EVENT_BLOCK_FLUSH:
            bitmap_engine_mark_clean(tool->bitmap_engine, event->block_id, 
                                   event->wal_offset, event->timestamp);
            break;
        case EVENT_BLOCK_DELETE:
            bitmap_engine_mark_deleted(tool->bitmap_engine, event->block_id, 
                                     event->wal_offset, event->timestamp);
            break;
    }
}

// 启动增量备份
int32_t incremental_backup_tool_start(SIncrementalBackupTool* tool) {
    if (!tool || tool->is_running) {
        return -1;
    }
    
    // 设置事件回调
    SEventInterceptorConfig config = {
        .enable_interception = true,
        .event_buffer_size = 10000,
        .callback_threads = 4,
        .callback = backup_event_callback,
        .callback_user_data = tool
    };
    
    event_interceptor_update_config(tool->event_interceptor, &config);
    
    // 启动事件拦截器
    int32_t result = event_interceptor_start(tool->event_interceptor);
    if (result != 0) {
        return result;
    }
    
    // 安装存储引擎拦截
    if (tool->storage_interface) {
        result = event_interceptor_install_storage_interception(tool->event_interceptor);
        if (result != 0) {
            event_interceptor_stop(tool->event_interceptor);
            return result;
        }
    }
    
    tool->is_running = true;
    printf("[增量备份] 工具启动成功\n");
    return 0;
}

// 执行增量备份
int32_t incremental_backup_tool_backup(SIncrementalBackupTool* tool, int64_t since_timestamp) {
    if (!tool) {
        return -1;
    }
    
    printf("[增量备份] 开始执行增量备份，时间戳: %ld\n", since_timestamp);
    
    // 获取增量块
    SIncrementalBlock* blocks = NULL;
    uint32_t block_count = 0;
    
    int32_t result = backup_coordinator_get_incremental_blocks(tool->backup_coordinator,
                                                              since_timestamp,
                                                              &blocks, &block_count);
    if (result != 0) {
        printf("[增量备份] 获取增量块失败: %d\n", result);
        return result;
    }
    
    if (block_count == 0) {
        printf("[增量备份] 没有发现增量数据\n");
        return 0;
    }
    
    printf("[增量备份] 发现 %u 个增量块\n", block_count);
    
    // 执行备份
    SBackupStats stats;
    result = backup_coordinator_backup_blocks(tool->backup_coordinator,
                                            blocks, block_count, &stats);
    
    if (result == 0) {
        printf("[增量备份] 备份完成: 处理=%lu, 失败=%lu, 大小=%lu bytes\n",
               stats.processed_blocks, stats.failed_blocks, stats.total_size);
        
        tool->total_blocks += stats.total_blocks;
        tool->processed_blocks += stats.processed_blocks;
        tool->failed_blocks += stats.failed_blocks;
    }
    
    // 清理内存
    for (uint32_t i = 0; i < block_count; i++) {
        if (blocks[i].data) {
            free(blocks[i].data);
        }
    }
    free(blocks);
    
    return result;
}

// 生成taosdump兼容的增量备份脚本
int32_t incremental_backup_tool_generate_taosdump_script(SIncrementalBackupTool* tool,
                                                        const char* script_path) {
    if (!tool || !script_path) {
        return -1;
    }
    
    FILE* file = fopen(script_path, "w");
    if (!file) {
        return -1;
    }
    
    // 生成bash脚本
    fprintf(file, "#!/bin/bash\n\n");
    fprintf(file, "# TDengine增量备份脚本 - 由位图插件生成\n");
    fprintf(file, "# 生成时间: %s\n\n", ctime(&(time_t){time(NULL)}));
    
    fprintf(file, "SOURCE_HOST=%s\n", tool->config.source_host);
    fprintf(file, "SOURCE_PORT=%d\n", tool->config.source_port);
    fprintf(file, "DATABASE=%s\n", tool->config.database);
    fprintf(file, "BACKUP_PATH=%s\n", tool->config.backup_path);
    fprintf(file, "SINCE_TIMESTAMP=%ld\n\n", tool->config.since_timestamp);
    
    // 1. 使用位图插件检测增量块
    fprintf(file, "echo \"步骤1: 检测增量数据块...\"\n");
    fprintf(file, "./incremental_bitmap_tool --detect \\\n");
    fprintf(file, "  --host $SOURCE_HOST --port $SOURCE_PORT \\\n");
    fprintf(file, "  --database $DATABASE \\\n");
    fprintf(file, "  --since $SINCE_TIMESTAMP \\\n");
    fprintf(file, "  --output incremental_blocks.json\n\n");
    
    // 2. 使用taosdump备份增量数据
    fprintf(file, "echo \"步骤2: 使用taosdump备份增量数据...\"\n");
    fprintf(file, "taosdump -h $SOURCE_HOST -P $SOURCE_PORT \\\n");
    fprintf(file, "  -D $DATABASE \\\n");
    fprintf(file, "  -S $SINCE_TIMESTAMP \\\n");
    fprintf(file, "  -o $BACKUP_PATH/incremental_$(date +%%Y%%m%%d_%%H%%M%%S)\n\n");
    
    // 3. 使用位图插件验证备份完整性
    fprintf(file, "echo \"步骤3: 验证备份完整性...\"\n");
    fprintf(file, "./incremental_bitmap_tool --verify \\\n");
    fprintf(file, "  --backup $BACKUP_PATH \\\n");
    fprintf(file, "  --blocks incremental_blocks.json \\\n");
    fprintf(file, "  --report backup_verification_report.json\n\n");
    
    fprintf(file, "echo \"增量备份完成!\"\n");
    
    fclose(file);
    
    // 设置执行权限
    chmod(script_path, 0755);
    
    printf("[增量备份] 生成taosdump脚本: %s\n", script_path);
    return 0;
}

// 停止增量备份
int32_t incremental_backup_tool_stop(SIncrementalBackupTool* tool) {
    if (!tool || !tool->is_running) {
        return -1;
    }
    
    // 停止事件拦截器
    event_interceptor_stop(tool->event_interceptor);
    
    // 卸载存储引擎拦截
    if (tool->storage_interface) {
        event_interceptor_uninstall_storage_interception(tool->event_interceptor);
    }
    
    tool->is_running = false;
    printf("[增量备份] 工具已停止\n");
    return 0;
}

// 销毁增量备份工具
void incremental_backup_tool_destroy(SIncrementalBackupTool* tool) {
    if (!tool) {
        return;
    }
    
    if (tool->is_running) {
        incremental_backup_tool_stop(tool);
    }
    
    if (tool->backup_coordinator) {
        backup_coordinator_destroy(tool->backup_coordinator);
    }
    
    if (tool->event_interceptor) {
        event_interceptor_destroy(tool->event_interceptor);
    }
    
    if (tool->bitmap_engine) {
        bitmap_engine_destroy(tool->bitmap_engine);
    }
    
    free(tool);
}

// 获取备份统计信息
void incremental_backup_tool_get_stats(SIncrementalBackupTool* tool,
                                      uint64_t* total_blocks,
                                      uint64_t* processed_blocks,
                                      uint64_t* failed_blocks) {
    if (!tool) {
        return;
    }
    
    if (total_blocks) {
        *total_blocks = tool->total_blocks;
    }
    if (processed_blocks) {
        *processed_blocks = tool->processed_blocks;
    }
    if (failed_blocks) {
        *failed_blocks = tool->failed_blocks;
    }
}

