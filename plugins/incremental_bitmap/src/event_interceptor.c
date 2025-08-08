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

#include "event_interceptor.h"
#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

// 回调线程参数
typedef struct {
    SEventInterceptor* interceptor;
    uint32_t thread_id;
    bool running;
} SCallbackThreadParam;

// 获取当前时间戳（纳秒）
static int64_t get_current_timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// 回调线程函数
static void* callback_thread_func(void* arg) {
    SEventInterceptor* interceptor = (SEventInterceptor*)arg;
    
    while (!interceptor->stop_threads) {
        // 从环形队列中获取事件
        void* event_ptr;
        int32_t result = ring_buffer_dequeue_blocking(interceptor->event_buffer, &event_ptr, 1000); // 1秒超时
        
        if (result == 0) {
            SBlockEvent* event = (SBlockEvent*)event_ptr;
            // 处理事件
            if (interceptor->config.callback) {
                interceptor->config.callback(event, interceptor->config.callback_user_data);
            }
            
            // 更新统计信息
            pthread_mutex_lock(&interceptor->mutex);
            interceptor->events_processed++;
            pthread_mutex_unlock(&interceptor->mutex);
            
            // 释放事件内存
            free(event);
        }
    }
    
    return NULL;
}

SEventInterceptor* event_interceptor_init(const SEventInterceptorConfig* config,
                                         SBitmapEngine* bitmap_engine) {
    if (!config || !bitmap_engine) {
        return NULL;
    }
    
    SEventInterceptor* interceptor = (SEventInterceptor*)malloc(sizeof(SEventInterceptor));
    if (!interceptor) {
        return NULL;
    }
    
    // 复制配置
    memcpy(&interceptor->config, config, sizeof(SEventInterceptorConfig));
    interceptor->bitmap_engine = bitmap_engine;
    interceptor->buffer_size = config->event_buffer_size;
    interceptor->thread_count = config->callback_threads;
    interceptor->stop_threads = false;
    interceptor->events_processed = 0;
    interceptor->events_dropped = 0;
    
    // 初始化事件缓冲区（环形队列）
    interceptor->event_buffer = ring_buffer_init(interceptor->buffer_size);
    if (!interceptor->event_buffer) {
        free(interceptor);
        return NULL;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&interceptor->mutex, NULL) != 0) {
        ring_buffer_destroy(interceptor->event_buffer);
        free(interceptor);
        return NULL;
    }
    
    // 初始化条件变量
    if (pthread_cond_init(&interceptor->condition, NULL) != 0) {
        pthread_mutex_destroy(&interceptor->mutex);
        ring_buffer_destroy(interceptor->event_buffer);
        free(interceptor);
        return NULL;
    }
    
    // 分配回调线程数组
    interceptor->callback_threads = (pthread_t*)malloc(sizeof(pthread_t) * interceptor->thread_count);
    if (!interceptor->callback_threads) {
        pthread_cond_destroy(&interceptor->condition);
        pthread_mutex_destroy(&interceptor->mutex);
        ring_buffer_destroy(interceptor->event_buffer);
        free(interceptor);
        return NULL;
    }
    
    return interceptor;
}

void event_interceptor_destroy(SEventInterceptor* interceptor) {
    if (!interceptor) {
        return;
    }
    
    // 停止所有线程
    event_interceptor_stop(interceptor);
    
    // 销毁环形队列
    if (interceptor->event_buffer) {
        ring_buffer_destroy(interceptor->event_buffer);
    }
    
    // 销毁条件变量
    pthread_cond_destroy(&interceptor->condition);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&interceptor->mutex);
    
    // 释放线程数组
    if (interceptor->callback_threads) {
        free(interceptor->callback_threads);
    }
    
    free(interceptor);
}

int32_t event_interceptor_start(SEventInterceptor* interceptor) {
    if (!interceptor) {
        return -1;
    }
    
    pthread_mutex_lock(&interceptor->mutex);
    
    if (interceptor->stop_threads) {
        // 已经启动
        pthread_mutex_unlock(&interceptor->mutex);
        return 0;
    }
    
    interceptor->stop_threads = false;
    
    // 创建回调线程
    for (uint32_t i = 0; i < interceptor->thread_count; i++) {
        if (pthread_create(&interceptor->callback_threads[i], NULL,
                          callback_thread_func, interceptor) != 0) {
            // 创建失败，停止已创建的线程
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(interceptor->callback_threads[j], NULL);
            }
            pthread_mutex_unlock(&interceptor->mutex);
            return -1;
        }
    }
    
    pthread_mutex_unlock(&interceptor->mutex);
    return 0;
}

int32_t event_interceptor_stop(SEventInterceptor* interceptor) {
    if (!interceptor) {
        return -1;
    }
    
    pthread_mutex_lock(&interceptor->mutex);
    
    if (!interceptor->stop_threads) {
        // 已经停止
        pthread_mutex_unlock(&interceptor->mutex);
        return 0;
    }
    
    interceptor->stop_threads = true;
    
    // 等待所有线程结束
    for (uint32_t i = 0; i < interceptor->thread_count; i++) {
        pthread_join(interceptor->callback_threads[i], NULL);
    }
    
    pthread_mutex_unlock(&interceptor->mutex);
    return 0;
}

int32_t event_interceptor_on_block_create(SEventInterceptor* interceptor,
                                         uint64_t block_id, uint64_t wal_offset, int64_t timestamp) {
    if (!interceptor || !interceptor->config.enable_interception) {
        return 0;
    }
    
    SBlockEvent* event = (SBlockEvent*)malloc(sizeof(SBlockEvent));
    if (!event) {
        return -1;
    }
    
    event->event_type = EVENT_BLOCK_CREATE;
    event->block_id = block_id;
    event->wal_offset = wal_offset;
    event->timestamp = timestamp;
    event->user_data = NULL;
    
    int32_t result = ring_buffer_enqueue(interceptor->event_buffer, event);
    if (result != 0) {
        pthread_mutex_lock(&interceptor->mutex);
        interceptor->events_dropped++;
        pthread_mutex_unlock(&interceptor->mutex);
        free(event);
    }
    
    return result;
}

int32_t event_interceptor_on_block_update(SEventInterceptor* interceptor,
                                         uint64_t block_id, uint64_t wal_offset, int64_t timestamp) {
    if (!interceptor || !interceptor->config.enable_interception) {
        return 0;
    }
    
    SBlockEvent* event = (SBlockEvent*)malloc(sizeof(SBlockEvent));
    if (!event) {
        return -1;
    }
    
    event->event_type = EVENT_BLOCK_UPDATE;
    event->block_id = block_id;
    event->wal_offset = wal_offset;
    event->timestamp = timestamp;
    event->user_data = NULL;
    
    int32_t result = ring_buffer_enqueue(interceptor->event_buffer, event);
    if (result != 0) {
        pthread_mutex_lock(&interceptor->mutex);
        interceptor->events_dropped++;
        pthread_mutex_unlock(&interceptor->mutex);
        free(event);
    }
    
    return result;
}

int32_t event_interceptor_on_block_flush(SEventInterceptor* interceptor,
                                        uint64_t block_id, uint64_t wal_offset, int64_t timestamp) {
    if (!interceptor || !interceptor->config.enable_interception) {
        return 0;
    }
    
    SBlockEvent* event = (SBlockEvent*)malloc(sizeof(SBlockEvent));
    if (!event) {
        return -1;
    }
    
    event->event_type = EVENT_BLOCK_FLUSH;
    event->block_id = block_id;
    event->wal_offset = wal_offset;
    event->timestamp = timestamp;
    event->user_data = NULL;
    
    int32_t result = ring_buffer_enqueue(interceptor->event_buffer, event);
    if (result != 0) {
        pthread_mutex_lock(&interceptor->mutex);
        interceptor->events_dropped++;
        pthread_mutex_unlock(&interceptor->mutex);
        free(event);
    }
    
    return result;
}

int32_t event_interceptor_on_block_delete(SEventInterceptor* interceptor,
                                         uint64_t block_id, uint64_t wal_offset, int64_t timestamp) {
    if (!interceptor || !interceptor->config.enable_interception) {
        return 0;
    }
    
    SBlockEvent* event = (SBlockEvent*)malloc(sizeof(SBlockEvent));
    if (!event) {
        return -1;
    }
    
    event->event_type = EVENT_BLOCK_DELETE;
    event->block_id = block_id;
    event->wal_offset = wal_offset;
    event->timestamp = timestamp;
    event->user_data = NULL;
    
    int32_t result = ring_buffer_enqueue(interceptor->event_buffer, event);
    if (result != 0) {
        pthread_mutex_lock(&interceptor->mutex);
        interceptor->events_dropped++;
        pthread_mutex_unlock(&interceptor->mutex);
        free(event);
    }
    
    return result;
}

void event_interceptor_get_stats(SEventInterceptor* interceptor,
                                uint64_t* events_processed, uint64_t* events_dropped) {
    if (!interceptor) {
        return;
    }
    
    pthread_mutex_lock(&interceptor->mutex);
    
    if (events_processed) {
        *events_processed = interceptor->events_processed;
    }
    if (events_dropped) {
        *events_dropped = interceptor->events_dropped;
    }
    
    pthread_mutex_unlock(&interceptor->mutex);
}

// 存储引擎接口拦截实现
// 注意：这里需要根据实际的TDengine API进行调整

// 原始函数指针
static int (*original_taos_write_block)(void* taos, int rows, char* pData, const char* tbname) = NULL;
static int (*original_taos_write_block_with_reqid)(void* taos, int rows, char* pData, 
                                                  const char* tbname, int64_t reqid) = NULL;

// 拦截的taosWriteBlock函数
int taos_write_block_intercepted(void* taos, int rows, char* pData, const char* tbname) {
    // 调用原始函数
    int result = original_taos_write_block(taos, rows, pData, tbname);
    
    // 如果成功，记录事件
    if (result == 0) {
        // 这里需要根据实际的TDengine实现来获取块ID和WAL偏移量
        // 简化实现，实际需要从taos连接或数据中提取
        uint64_t block_id = 0; // TODO: 从数据中提取块ID
        uint64_t wal_offset = 0; // TODO: 从WAL中获取偏移量
        int64_t timestamp = get_current_timestamp();
        
        // 通知事件拦截器（需要全局拦截器实例）
        // TODO: 实现全局拦截器访问机制
    }
    
    return result;
}

// 拦截的taosWriteBlockWithReqid函数
int taos_write_block_with_reqid_intercepted(void* taos, int rows, char* pData, 
                                           const char* tbname, int64_t reqid) {
    // 调用原始函数
    int result = original_taos_write_block_with_reqid(taos, rows, pData, tbname, reqid);
    
    // 如果成功，记录事件
    if (result == 0) {
        // 这里需要根据实际的TDengine实现来获取块ID和WAL偏移量
        uint64_t block_id = 0; // TODO: 从数据中提取块ID
        uint64_t wal_offset = 0; // TODO: 从WAL中获取偏移量
        int64_t timestamp = get_current_timestamp();
        
        // 通知事件拦截器（需要全局拦截器实例）
        // TODO: 实现全局拦截器访问机制
    }
    
    return result;
}

int32_t install_storage_engine_interception(SEventInterceptor* interceptor) {
    // 这里需要实现动态库函数替换
    // 可以使用dlsym和函数指针替换的方式
    // 或者使用LD_PRELOAD的方式
    
    // 简化实现，实际需要：
    // 1. 获取原始函数地址
    // 2. 替换函数指针
    // 3. 设置全局拦截器实例
    
    // TODO: 实现完整的函数拦截机制
    return 0;
}

int32_t uninstall_storage_engine_interception(SEventInterceptor* interceptor) {
    // 恢复原始函数指针
    // TODO: 实现完整的函数恢复机制
    return 0;
} 