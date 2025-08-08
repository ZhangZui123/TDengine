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

#include "../include/backup_coordinator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

// 测试辅助函数
static void print_test_result(const char* test_name, bool passed) {
    printf("[%s] %s\n", passed ? "PASS" : "FAIL", test_name);
}

static int64_t get_current_timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// 测试1: 基本初始化和销毁
static void test_basic_init_destroy() {
    printf("\n=== 测试1: 基本初始化和销毁 ===\n");
    
    // 创建位图引擎
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    // 创建事件拦截器
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    // 创建备份协同器
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    print_test_result("初始化备份协同器", true);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
    print_test_result("销毁备份协同器", true);
}

// 测试2: 获取脏块
static void test_get_dirty_blocks() {
    printf("\n=== 测试2: 获取脏块 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    int64_t timestamp = get_current_timestamp();
    
    // 添加一些脏块
    bitmap_engine_mark_dirty(bitmap_engine, 1001, 1000, timestamp);
    bitmap_engine_mark_dirty(bitmap_engine, 1002, 2000, timestamp + 1000);
    bitmap_engine_mark_dirty(bitmap_engine, 1003, 3000, timestamp + 2000);
    bitmap_engine_mark_dirty(bitmap_engine, 1004, 4000, timestamp + 3000);
    
    // 获取WAL范围内的脏块
    uint64_t block_ids[10];
    uint32_t count = backup_coordinator_get_dirty_blocks(coordinator, 1500, 3500, block_ids, 10);
    
    assert(count >= 2); // 应该至少包含1002和1003
    print_test_result("获取脏块", true);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试3: 创建和销毁游标
static void test_cursor_operations() {
    printf("\n=== 测试3: 创建和销毁游标 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    int64_t start_time = get_current_timestamp();
    int64_t end_time = start_time + 1000000; // 1秒后
    
    // 创建游标
    SIncrementalCursor* cursor = backup_coordinator_create_cursor(coordinator,
                                                                 CURSOR_TYPE_TIME,
                                                                 start_time, end_time,
                                                                 1000, 5000);
    assert(cursor != NULL);
    print_test_result("创建游标", true);
    
    // 验证游标属性
    assert(cursor->type == CURSOR_TYPE_TIME);
    assert(cursor->start_time == start_time);
    assert(cursor->end_time == end_time);
    assert(cursor->start_wal == 1000);
    assert(cursor->end_wal == 5000);
    assert(cursor->has_more == true);
    
    // 销毁游标
    backup_coordinator_destroy_cursor(coordinator, cursor);
    print_test_result("销毁游标", true);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试4: 批量获取增量数据
static void test_get_next_batch() {
    printf("\n=== 测试4: 批量获取增量数据 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    int64_t timestamp = get_current_timestamp();
    
    // 添加一些块
    for (int i = 0; i < 10; i++) {
        uint64_t block_id = 2000 + i;
        bitmap_engine_mark_dirty(bitmap_engine, block_id, block_id * 10, timestamp + i);
    }
    
    // 创建游标
    SIncrementalCursor* cursor = backup_coordinator_create_cursor(coordinator,
                                                                 CURSOR_TYPE_WAL,
                                                                 timestamp, timestamp + 1000,
                                                                 20000, 30000);
    assert(cursor != NULL);
    
    // 获取下一批数据
    SIncrementalBlock blocks[5];
    uint32_t count = backup_coordinator_get_next_batch(coordinator, cursor, blocks, 5);
    
    assert(count > 0);
    print_test_result("批量获取增量数据", true);
    
    // 验证块信息
    for (uint32_t i = 0; i < count; i++) {
        assert(blocks[i].block_id >= 2000 && blocks[i].block_id < 2010);
        assert(blocks[i].state == BLOCK_STATE_DIRTY);
    }
    
    backup_coordinator_destroy_cursor(coordinator, cursor);
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试5: 估算备份大小
static void test_estimate_size() {
    printf("\n=== 测试5: 估算备份大小 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    int64_t timestamp = get_current_timestamp();
    
    // 添加一些块
    for (int i = 0; i < 20; i++) {
        uint64_t block_id = 3000 + i;
        bitmap_engine_mark_dirty(bitmap_engine, block_id, block_id * 10, timestamp + i);
    }
    
    // 估算备份大小
    uint64_t estimated_blocks, estimated_size;
    int32_t result = backup_coordinator_estimate_size(coordinator, 30000, 50000, &estimated_blocks, &estimated_size);
    
    assert(result == 0);
    assert(estimated_blocks > 0);
    assert(estimated_size > 0);
    print_test_result("估算备份大小", true);
    
    printf("估算块数: %lu, 估算大小: %lu 字节\n", estimated_blocks, estimated_size);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试6: 生成元数据
static void test_generate_metadata() {
    printf("\n=== 测试6: 生成元数据 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    // 生成元数据
    void* metadata;
    uint32_t metadata_size;
    int32_t result = backup_coordinator_generate_metadata(coordinator, 1000, 5000, &metadata, &metadata_size);
    
    assert(result == 0);
    assert(metadata != NULL);
    assert(metadata_size > 0);
    print_test_result("生成元数据", true);
    
    // 清理元数据
    free(metadata);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试7: 验证备份完整性
static void test_validate_backup() {
    printf("\n=== 测试7: 验证备份完整性 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    int64_t timestamp = get_current_timestamp();
    
    // 添加一些块
    for (int i = 0; i < 5; i++) {
        uint64_t block_id = 4000 + i;
        bitmap_engine_mark_dirty(bitmap_engine, block_id, block_id * 10, timestamp + i);
    }
    
    // 创建测试块数组
    SIncrementalBlock blocks[5];
    for (int i = 0; i < 5; i++) {
        blocks[i].block_id = 4000 + i;
        blocks[i].wal_offset = (4000 + i) * 10;
        blocks[i].timestamp = timestamp + i;
        blocks[i].state = BLOCK_STATE_DIRTY;
        blocks[i].data = NULL;
        blocks[i].data_size = 0;
    }
    
    // 验证备份完整性
    int32_t result = backup_coordinator_validate_backup(coordinator, 40000, 50000, blocks, 5);
    
    assert(result == 0);
    print_test_result("验证备份完整性", true);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试8: 统计信息
static void test_statistics() {
    printf("\n=== 测试8: 统计信息 ===\n");
    
    // 创建组件
    SBitmapEngine* bitmap_engine = bitmap_engine_init();
    assert(bitmap_engine != NULL);
    
    SEventInterceptorConfig interceptor_config = {
        .enable_interception = true,
        .event_buffer_size = 1000,
        .callback_threads = 2,
        .callback = NULL,
        .callback_user_data = NULL
    };
    
    SEventInterceptor* event_interceptor = event_interceptor_init(&interceptor_config, bitmap_engine);
    assert(event_interceptor != NULL);
    
    SBackupCoordinatorConfig config = {
        .max_blocks_per_batch = 100,
        .batch_timeout_ms = 5000,
        .enable_compression = true,
        .enable_encryption = false,
        .encryption_key = NULL
    };
    
    SBackupCoordinator* coordinator = backup_coordinator_init(&config, bitmap_engine, event_interceptor);
    assert(coordinator != NULL);
    
    // 获取统计信息
    uint64_t total_blocks, total_size, duration_ms;
    backup_coordinator_get_stats(coordinator, &total_blocks, &total_size, &duration_ms);
    
    printf("总块数: %lu, 总大小: %lu 字节, 持续时间: %lu ms\n", 
           total_blocks, total_size, duration_ms);
    print_test_result("统计信息", true);
    
    backup_coordinator_destroy(coordinator);
    event_interceptor_destroy(event_interceptor);
    bitmap_engine_destroy(bitmap_engine);
}

// 测试9: taosX插件接口
static void test_plugin_api() {
    printf("\n=== 测试9: taosX插件接口 ===\n");
    
    // 测试插件名称和版本
    const char* name = backup_plugin_name();
    const char* version = backup_plugin_version();
    
    assert(name != NULL);
    assert(version != NULL);
    printf("插件名称: %s, 版本: %s\n", name, version);
    print_test_result("插件名称和版本", true);
    
    // 测试插件初始化
    const char* config = "{\"max_blocks_per_batch\": 100}";
    int32_t result = backup_plugin_init(config, strlen(config));
    
    if (result == 0) {
        print_test_result("插件初始化", true);
        
        // 测试获取脏块
        uint64_t block_ids[10];
        uint32_t count = backup_plugin_get_dirty_blocks(1000, 5000, block_ids, 10);
        printf("获取到 %u 个脏块\n", count);
        
        // 测试估算备份大小
        uint64_t estimated_blocks, estimated_size;
        result = backup_plugin_estimate_backup_size(1000, 5000, &estimated_blocks, &estimated_size);
        if (result == 0) {
            printf("估算块数: %lu, 估算大小: %lu 字节\n", estimated_blocks, estimated_size);
        }
        
        // 清理插件
        backup_plugin_cleanup();
        print_test_result("插件清理", true);
    } else {
        print_test_result("插件初始化", false);
    }
}



int main() {
    printf("开始备份协同器测试...\n");
    
    test_basic_init_destroy();
    test_get_dirty_blocks();
    test_cursor_operations();
    test_get_next_batch();
    test_estimate_size();
    test_generate_metadata();
    test_validate_backup();
    test_statistics();
    test_plugin_api();

    
    printf("\n所有测试完成！\n");
    return 0;
} 