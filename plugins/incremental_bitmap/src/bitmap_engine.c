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

#include "bitmap_engine.h"
#include "taosx_integration.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "skiplist.h"
#include <fcntl.h>
#include <stdio.h>



// 哈希函数
static uint32_t hash_block_id(uint64_t block_id, uint32_t map_size) {
    return (uint32_t)(block_id % map_size);
}

// 获取当前时间戳（纳秒）
static int64_t get_current_timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}





// 深拷贝字符串
static char* deep_copy_string(const char* src) {
    if (src == NULL) {
        return NULL;
    }
    
    size_t len = strlen(src);
    char* dst = (char*)malloc(len + 1);
    if (dst == NULL) {
        return NULL;
    }
    
    strcpy(dst, src);
    return dst;
}











// 查找块元数据
static SBlockMetadataNode* find_block_metadata(SBitmapEngine* engine, uint64_t block_id) {
    uint32_t hash = hash_block_id(block_id, engine->metadata_map_size);
    SBlockMetadataNode* node = engine->metadata_map[hash];
    
    while (node != NULL) {
        if (node->block_id == block_id) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

// 插入块元数据
static int32_t insert_block_metadata(SBitmapEngine* engine, const SBlockMetadata* metadata) {
    uint32_t hash = hash_block_id(metadata->block_id, engine->metadata_map_size);
    
    // 检查是否已存在
    SBlockMetadataNode* existing = find_block_metadata(engine, metadata->block_id);
    if (existing != NULL) {
        existing->metadata = *metadata;
        return 0;
    }
    
    // 创建新节点
    SBlockMetadataNode* node = (SBlockMetadataNode*)malloc(sizeof(SBlockMetadataNode));
    if (node == NULL) {
        return -1;
    }
    
    node->block_id = metadata->block_id;
    node->metadata = *metadata;
    node->next = engine->metadata_map[hash];
    engine->metadata_map[hash] = node;
    engine->metadata_count++;
    
    return 0;
}

// 删除块元数据
static int32_t remove_block_metadata(SBitmapEngine* engine, uint64_t block_id) {
    uint32_t hash = hash_block_id(block_id, engine->metadata_map_size);
    SBlockMetadataNode* node = engine->metadata_map[hash];
    SBlockMetadataNode* prev = NULL;
    
    while (node != NULL) {
        if (node->block_id == block_id) {
            if (prev == NULL) {
                engine->metadata_map[hash] = node->next;
            } else {
                prev->next = node->next;
            }
            free(node);
            engine->metadata_count--;
            
            return 0;
        }
        prev = node;
        node = node->next;
    }
    
    return -1;
}

// 添加时间索引
static int32_t add_time_index(SBitmapEngine* engine, int64_t timestamp, uint64_t block_id) {
    SBitmapInterface* bm = (SBitmapInterface*)skiplist_find(engine->time_index, timestamp);
    if (!bm) {
        bm = bitmap_interface_create();
        skiplist_insert(engine->time_index, timestamp, bm);
    }
    bm->add(bm->bitmap, block_id);
    return 0;
}

// 添加WAL索引
static int32_t add_wal_index(SBitmapEngine* engine, uint64_t wal_offset, uint64_t block_id) {
    SBitmapInterface* bm = (SBitmapInterface*)skiplist_find(engine->wal_index, wal_offset);
    if (!bm) {
        bm = bitmap_interface_create();
        skiplist_insert(engine->wal_index, wal_offset, bm);
    }
    bm->add(bm->bitmap, block_id);
    return 0;
}











SBitmapEngine* bitmap_engine_init(void) {
    SBitmapEngine* engine = (SBitmapEngine*)malloc(sizeof(SBitmapEngine));
    if (engine == NULL) {
        return NULL;
    }
    
    // 初始化位图
    engine->dirty_blocks = bitmap_interface_create();
    engine->new_blocks = bitmap_interface_create();
    engine->deleted_blocks = bitmap_interface_create();
    
    if (engine->dirty_blocks == NULL || engine->new_blocks == NULL || engine->deleted_blocks == NULL) {
        bitmap_engine_destroy(engine);
        return NULL;
    }
    
    // 初始化元数据映射
    engine->metadata_map_size = 10000; // 默认大小
    engine->metadata_map = (SBlockMetadataNode**)calloc(engine->metadata_map_size, sizeof(SBlockMetadataNode*));
    if (engine->metadata_map == NULL) {
        bitmap_engine_destroy(engine);
        return NULL;
    }
    
    // 初始化索引
    engine->time_index_head = NULL;
    engine->wal_index_head = NULL;
    
    // 初始化跳表索引
    engine->time_index = skiplist_create();
    engine->wal_index = skiplist_create();
    
    // 初始化统计信息
    engine->total_blocks = 0;
    engine->dirty_count = 0;
    engine->new_count = 0;
    engine->deleted_count = 0;
    engine->metadata_count = 0;
    
    // 初始化线程同步
    if (pthread_mutex_init(&engine->mutex, NULL) != 0) {
        bitmap_engine_destroy(engine);
        return NULL;
    }
    
    if (pthread_rwlock_init(&engine->rwlock, NULL) != 0) {
        pthread_mutex_destroy(&engine->mutex);
        bitmap_engine_destroy(engine);
        return NULL;
    }
    
    return engine;
}

void bitmap_engine_destroy(SBitmapEngine* engine) {
    if (engine == NULL) {
        return;
    }
    
    // 销毁位图
    if (engine->dirty_blocks) {
        bitmap_interface_destroy(engine->dirty_blocks);
    }
    if (engine->new_blocks) {
        bitmap_interface_destroy(engine->new_blocks);
    }
    if (engine->deleted_blocks) {
        bitmap_interface_destroy(engine->deleted_blocks);
    }
    
    // 销毁元数据映射
    if (engine->metadata_map) {
        for (uint32_t i = 0; i < engine->metadata_map_size; i++) {
            SBlockMetadataNode* node = engine->metadata_map[i];
            while (node != NULL) {
                SBlockMetadataNode* next = node->next;
                free(node);
                node = next;
            }
        }
        free(engine->metadata_map);
    }
    
    // 销毁时间索引
    STimeIndexNode* time_node = engine->time_index_head;
    while (time_node != NULL) {
        STimeIndexNode* next = time_node->next;
        if (time_node->block_ids) {
            bitmap_interface_destroy(time_node->block_ids);
        }
        free(time_node);
        time_node = next;
    }
    
    // 销毁WAL索引
    SWalIndexNode* wal_node = engine->wal_index_head;
    while (wal_node != NULL) {
        SWalIndexNode* next = wal_node->next;
        if (wal_node->block_ids) {
            bitmap_interface_destroy(wal_node->block_ids);
        }
        free(wal_node);
        wal_node = next;
    }
    
    // 销毁跳表索引
    if (engine->time_index) skiplist_destroy(engine->time_index);
    if (engine->wal_index) skiplist_destroy(engine->wal_index);
    
    // 销毁线程同步
    pthread_mutex_destroy(&engine->mutex);
    pthread_rwlock_destroy(&engine->rwlock);
    
    free(engine);
}

int32_t bitmap_engine_mark_dirty(SBitmapEngine* engine, uint64_t block_id, 
                                uint64_t wal_offset, int64_t timestamp) {
    if (engine == NULL) {
        return ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&engine->mutex);
    
    // 获取当前块状态（如果存在）
    EBlockState current_state = BLOCK_STATE_CLEAN; // 默认状态
    SBlockMetadataNode* existing_node = find_block_metadata(engine, block_id);
    if (existing_node != NULL) {
        current_state = existing_node->metadata.state;
    }
    
    // 验证状态转换
    if (bitmap_engine_validate_state_transition(current_state, BLOCK_STATE_DIRTY) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return ERR_INVALID_STATE_TRANS;
    }
    
    // 创建或更新元数据
    SBlockMetadata metadata;
    metadata.block_id = block_id;
    metadata.wal_offset = wal_offset;
    metadata.timestamp = timestamp;
    metadata.state = BLOCK_STATE_DIRTY;
    
    if (insert_block_metadata(engine, &metadata) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    
    // 添加到位图
    engine->dirty_blocks->add(engine->dirty_blocks->bitmap, block_id);
    
    // 添加到索引
    add_time_index(engine, timestamp, block_id);
    add_wal_index(engine, wal_offset, block_id);
    
    // 更新统计信息
    engine->dirty_count++;
    engine->total_blocks++;
    
    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

int32_t bitmap_engine_mark_new(SBitmapEngine* engine, uint64_t block_id,
                              uint64_t wal_offset, int64_t timestamp) {
    if (engine == NULL) {
        return ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&engine->mutex);
    
    // 获取当前块状态（如果存在）
    EBlockState current_state = BLOCK_STATE_CLEAN; // 默认状态
    SBlockMetadataNode* existing_node = find_block_metadata(engine, block_id);
    if (existing_node != NULL) {
        current_state = existing_node->metadata.state;
    }
    
    // 验证状态转换
    if (bitmap_engine_validate_state_transition(current_state, BLOCK_STATE_NEW) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return ERR_INVALID_STATE_TRANS;
    }
    
    // 创建或更新元数据
    SBlockMetadata metadata;
    metadata.block_id = block_id;
    metadata.wal_offset = wal_offset;
    metadata.timestamp = timestamp;
    metadata.state = BLOCK_STATE_NEW;
    
    if (insert_block_metadata(engine, &metadata) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    
    // 添加到位图
    engine->new_blocks->add(engine->new_blocks->bitmap, block_id);
    
    // 添加到索引
    add_time_index(engine, timestamp, block_id);
    add_wal_index(engine, wal_offset, block_id);
    
    // 更新统计信息
    engine->new_count++;
    engine->total_blocks++;
    
    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

int32_t bitmap_engine_mark_deleted(SBitmapEngine* engine, uint64_t block_id,
                                  uint64_t wal_offset, int64_t timestamp) {
    if (engine == NULL) {
        return ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&engine->mutex);
    
    // 获取当前块状态（如果存在）
    EBlockState current_state = BLOCK_STATE_CLEAN; // 默认状态
    SBlockMetadataNode* existing_node = find_block_metadata(engine, block_id);
    if (existing_node != NULL) {
        current_state = existing_node->metadata.state;
    }
    
    // 验证状态转换
    if (bitmap_engine_validate_state_transition(current_state, BLOCK_STATE_DELETED) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return ERR_INVALID_STATE_TRANS;
    }
    
    // 创建或更新元数据
    SBlockMetadata metadata;
    metadata.block_id = block_id;
    metadata.wal_offset = wal_offset;
    metadata.timestamp = timestamp;
    metadata.state = BLOCK_STATE_DELETED;
    
    if (insert_block_metadata(engine, &metadata) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    
    // 添加到位图
    engine->deleted_blocks->add(engine->deleted_blocks->bitmap, block_id);
    
    // 添加到索引
    add_time_index(engine, timestamp, block_id);
    add_wal_index(engine, wal_offset, block_id);
    
    // 更新统计信息
    engine->deleted_count++;
    engine->total_blocks++;
    
    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

int32_t bitmap_engine_clear_block(SBitmapEngine* engine, uint64_t block_id) {
    if (engine == NULL) {
        return ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&engine->mutex);
    
    // 获取当前块状态
    SBlockMetadataNode* existing_node = find_block_metadata(engine, block_id);
    if (existing_node == NULL) {
        pthread_mutex_unlock(&engine->mutex);
        return ERR_BLOCK_NOT_FOUND;
    }
    
    EBlockState current_state = existing_node->metadata.state;
    
    // 验证状态转换（清除块相当于转换为CLEAN状态）
    if (bitmap_engine_validate_state_transition(current_state, BLOCK_STATE_CLEAN) != 0) {
        pthread_mutex_unlock(&engine->mutex);
        return ERR_INVALID_STATE_TRANS;
    }
    
    // 从位图中移除
    engine->dirty_blocks->remove(engine->dirty_blocks->bitmap, block_id);
    engine->new_blocks->remove(engine->new_blocks->bitmap, block_id);
    engine->deleted_blocks->remove(engine->deleted_blocks->bitmap, block_id);
    
    // 删除元数据
    remove_block_metadata(engine, block_id);
    
    // 更新统计信息
    engine->dirty_count = engine->dirty_blocks->cardinality(engine->dirty_blocks->bitmap);
    engine->new_count = engine->new_blocks->cardinality(engine->new_blocks->bitmap);
    engine->deleted_count = engine->deleted_blocks->cardinality(engine->deleted_blocks->bitmap);
    engine->total_blocks = engine->metadata_count;
    
    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

uint32_t bitmap_engine_get_dirty_blocks_by_time(SBitmapEngine* engine,
                                               int64_t start_time, int64_t end_time,
                                               uint64_t* block_ids, uint32_t max_count) {
    if (engine == NULL || block_ids == NULL || max_count == 0) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&engine->rwlock);
    
    SBitmapInterface* result = bitmap_interface_create();
    uint32_t count = 0;
    
    // 跳表范围查询
    void range_cb(int64_t key, void* bm_ptr, void* user_data) {
        SBitmapInterface* bm = (SBitmapInterface*)bm_ptr;
        SBitmapInterface* intersection = bitmap_interface_create();
        
        // 复制dirty_blocks到intersection
        intersection->union_with(intersection->bitmap, engine->dirty_blocks->bitmap);
        // 与bm求交集
        intersection->intersect_with(intersection->bitmap, bm->bitmap);
        
        // 与result求并集
        result->union_with(result->bitmap, intersection->bitmap);
        
        bitmap_interface_destroy(intersection);
    }
    skiplist_range_query(engine->time_index, start_time, end_time, false, range_cb, NULL);
    
    // 获取结果
    count = result->to_array(result->bitmap, block_ids, max_count);
    
    bitmap_interface_destroy(result);
    pthread_rwlock_unlock(&engine->rwlock);
    
    return count;
}

uint32_t bitmap_engine_get_dirty_blocks_by_wal(SBitmapEngine* engine,
                                              uint64_t start_offset, uint64_t end_offset,
                                              uint64_t* block_ids, uint32_t max_count) {
    if (engine == NULL || block_ids == NULL || max_count == 0) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&engine->rwlock);
    
    SBitmapInterface* result = bitmap_interface_create();
    uint32_t count = 0;
    
    // 跳表范围查询
    void range_cb(int64_t key, void* bm_ptr, void* user_data) {
        SBitmapInterface* bm = (SBitmapInterface*)bm_ptr;
        SBitmapInterface* intersection = bitmap_interface_create();
        
        // 复制dirty_blocks到intersection
        intersection->union_with(intersection->bitmap, engine->dirty_blocks->bitmap);
        // 与bm求交集
        intersection->intersect_with(intersection->bitmap, bm->bitmap);
        
        // 与result求并集
        result->union_with(result->bitmap, intersection->bitmap);
        
        bitmap_interface_destroy(intersection);
    }
    skiplist_range_query(engine->wal_index, start_offset, end_offset, false, range_cb, NULL);
    
    // 获取结果
    count = result->to_array(result->bitmap, block_ids, max_count);
    
    bitmap_interface_destroy(result);
    pthread_rwlock_unlock(&engine->rwlock);
    
    return count;
}

int32_t bitmap_engine_get_block_metadata(SBitmapEngine* engine, uint64_t block_id,
                                        SBlockMetadata* metadata) {
    if (engine == NULL || metadata == NULL) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&engine->rwlock);
    
    SBlockMetadataNode* node = find_block_metadata(engine, block_id);
    if (node == NULL) {
        pthread_rwlock_unlock(&engine->rwlock);
        return -1;
    }
    
    *metadata = node->metadata;
    pthread_rwlock_unlock(&engine->rwlock);
    
    return 0;
}





void bitmap_engine_get_stats(SBitmapEngine* engine, uint64_t* total_blocks,
                           uint64_t* dirty_count, uint64_t* new_count, uint64_t* deleted_count) {
    if (engine == NULL) {
        return;
    }
    
    pthread_mutex_lock(&engine->mutex);
    
    if (total_blocks) *total_blocks = engine->total_blocks;
    if (dirty_count) *dirty_count = engine->dirty_count;
    if (new_count) *new_count = engine->new_count;
    if (deleted_count) *deleted_count = engine->deleted_count;
    
    pthread_mutex_unlock(&engine->mutex);
}



// 状态转换验证实现

// 状态转换规则矩阵
// 行：当前状态，列：目标状态
// 1表示允许转换，0表示不允许转换
static const int8_t STATE_TRANSITION_MATRIX[4][4] = {
    // CLEAN  DIRTY  NEW    DELETED
    { 0,     1,     1,     1 },  // CLEAN
    { 1,     0,     0,     1 },  // DIRTY
    { 0,     1,     0,     1 },  // NEW
    { 0,     0,     0,     0 }   // DELETED (不可转换为任何状态)
};

int32_t bitmap_engine_validate_state_transition(EBlockState current_state, EBlockState target_state) {
    // 检查状态值是否有效
    if (current_state < 0 || current_state >= 4 || target_state < 0 || target_state >= 4) {
        return ERR_INVALID_STATE_TRANS;
    }
    
    // 检查转换是否允许
    if (STATE_TRANSITION_MATRIX[current_state][target_state]) {
        return 0; // 允许转换
    } else {
        return ERR_INVALID_STATE_TRANS; // 不允许转换
    }
}

const char* bitmap_engine_get_state_transition_error(EBlockState current_state, EBlockState target_state) {
    static char error_msg[256];
    
    const char* state_names[] = {"CLEAN", "DIRTY", "NEW", "DELETED"};
    
    if (current_state < 0 || current_state >= 4 || target_state < 0 || target_state >= 4) {
        snprintf(error_msg, sizeof(error_msg), "Invalid state values: current=%d, target=%d", 
                current_state, target_state);
        return error_msg;
    }
    
    if (STATE_TRANSITION_MATRIX[current_state][target_state]) {
        snprintf(error_msg, sizeof(error_msg), "State transition from %s to %s is valid", 
                state_names[current_state], state_names[target_state]);
        return error_msg;
    }
    
    // 根据具体的不允许转换情况提供详细错误信息
    if (current_state == BLOCK_STATE_DELETED) {
        snprintf(error_msg, sizeof(error_msg), 
                "Cannot transition from DELETED state to %s state. DELETED blocks cannot be modified.", 
                state_names[target_state]);
    } else if (current_state == BLOCK_STATE_CLEAN && target_state == BLOCK_STATE_NEW) {
        snprintf(error_msg, sizeof(error_msg), 
                "Cannot transition from CLEAN to NEW state. CLEAN blocks must first become DIRTY.");
    } else if (current_state == BLOCK_STATE_CLEAN && target_state == BLOCK_STATE_DELETED) {
        snprintf(error_msg, sizeof(error_msg), 
                "Cannot transition from CLEAN to DELETED state. CLEAN blocks must first become DIRTY.");
    } else if (current_state == BLOCK_STATE_NEW && target_state == BLOCK_STATE_CLEAN) {
        snprintf(error_msg, sizeof(error_msg), 
                "Cannot transition from NEW to CLEAN state. NEW blocks can only become DIRTY or DELETED.");
    } else {
        snprintf(error_msg, sizeof(error_msg), 
                "Invalid state transition from %s to %s", 
                state_names[current_state], state_names[target_state]);
    }
    
    return error_msg;
}

int32_t bitmap_engine_get_block_state(SBitmapEngine* engine, uint64_t block_id, EBlockState* state) {
    if (engine == NULL || state == NULL) {
        return ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&engine->mutex);
    
    SBlockMetadataNode* node = find_block_metadata(engine, block_id);
    if (node == NULL) {
        pthread_mutex_unlock(&engine->mutex);
        return ERR_BLOCK_NOT_FOUND;
    }
    
    *state = node->metadata.state;
    
    pthread_mutex_unlock(&engine->mutex);
    return 0;
} 