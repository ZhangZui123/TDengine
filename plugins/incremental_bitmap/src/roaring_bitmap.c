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

#include "../include/bitmap_interface.h"
#include <roaring/roaring64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// RoaringBitmap 适配器实现
typedef struct SRoaringBitmap {
    roaring64_bitmap_t* roaring_bitmap;
} SRoaringBitmap;

// 内部函数声明 - 使用 tdengine_ 前缀避免与 CRoaring 冲突
static void tdengine_roaring_add(void* bitmap, uint64_t value);
static void tdengine_roaring_remove(void* bitmap, uint64_t value);
static bool tdengine_roaring_contains(void* bitmap, uint64_t value);
static uint32_t tdengine_roaring_cardinality(void* bitmap);
static void tdengine_roaring_clear(void* bitmap);
static void tdengine_roaring_union_with(void* bitmap, const void* other);
static void tdengine_roaring_intersect_with(void* bitmap, const void* other);
static void tdengine_roaring_subtract(void* bitmap, const void* other);
static uint32_t tdengine_roaring_to_array(void* bitmap, uint64_t* array, uint32_t max_count);
static size_t tdengine_roaring_serialized_size(void* bitmap);
static int32_t tdengine_roaring_serialize(void* bitmap, void* buffer, size_t buffer_size);
static int32_t tdengine_roaring_deserialize(void** bitmap, const void* buffer, size_t buffer_size);
static void tdengine_roaring_destroy(void* bitmap);
static size_t tdengine_roaring_memory_usage(void* bitmap);
static void* tdengine_roaring_create(void);
static void* tdengine_roaring_clone(const void* bitmap);

// 实现函数
static void tdengine_roaring_add(void* bitmap, uint64_t value) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        roaring64_bitmap_add(rb->roaring_bitmap, value);
    }
}

static void tdengine_roaring_remove(void* bitmap, uint64_t value) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        roaring64_bitmap_remove(rb->roaring_bitmap, value);
    }
}

static bool tdengine_roaring_contains(void* bitmap, uint64_t value) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        return roaring64_bitmap_contains(rb->roaring_bitmap, value);
    }
    return false;
}

static uint32_t tdengine_roaring_cardinality(void* bitmap) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        return (uint32_t)roaring64_bitmap_get_cardinality(rb->roaring_bitmap);
    }
    return 0;
}

static void tdengine_roaring_clear(void* bitmap) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        roaring64_bitmap_clear(rb->roaring_bitmap);
    }
}

static void tdengine_roaring_union_with(void* bitmap, const void* other) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    SRoaringBitmap* other_rb = (SRoaringBitmap*)other;
    if (rb && rb->roaring_bitmap && other_rb && other_rb->roaring_bitmap) {
        roaring64_bitmap_or_inplace(rb->roaring_bitmap, other_rb->roaring_bitmap);
    }
}

static void tdengine_roaring_intersect_with(void* bitmap, const void* other) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    SRoaringBitmap* other_rb = (SRoaringBitmap*)other;
    if (rb && rb->roaring_bitmap && other_rb && other_rb->roaring_bitmap) {
        roaring64_bitmap_and_inplace(rb->roaring_bitmap, other_rb->roaring_bitmap);
    }
}

static void tdengine_roaring_subtract(void* bitmap, const void* other) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    SRoaringBitmap* other_rb = (SRoaringBitmap*)other;
    if (rb && rb->roaring_bitmap && other_rb && other_rb->roaring_bitmap) {
        roaring64_bitmap_andnot_inplace(rb->roaring_bitmap, other_rb->roaring_bitmap);
    }
}

static uint32_t tdengine_roaring_to_array(void* bitmap, uint64_t* array, uint32_t max_count) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (!rb || !rb->roaring_bitmap || !array) {
        return 0;
    }
    
    uint64_t cardinality = roaring64_bitmap_get_cardinality(rb->roaring_bitmap);
    uint32_t count = (uint32_t)(cardinality < max_count ? cardinality : max_count);
    
    if (count > 0) {
        roaring64_bitmap_to_uint64_array(rb->roaring_bitmap, array);
    }
    
    return count;
}

static size_t tdengine_roaring_serialized_size(void* bitmap) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        return roaring64_bitmap_portable_size_in_bytes(rb->roaring_bitmap);
    }
    return 0;
}

static int32_t tdengine_roaring_serialize(void* bitmap, void* buffer, size_t buffer_size) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (!rb || !rb->roaring_bitmap || !buffer) {
        return -1;
    }
    
    size_t required_size = roaring64_bitmap_portable_size_in_bytes(rb->roaring_bitmap);
    if (buffer_size < required_size) {
        return -1;
    }
    
    size_t serialized_size = roaring64_bitmap_portable_serialize(rb->roaring_bitmap, (char*)buffer);
    return (int32_t)serialized_size;
}

static int32_t tdengine_roaring_deserialize(void** bitmap, const void* buffer, size_t buffer_size) {
    if (!bitmap || !buffer) {
        return -1;
    }
    
    SRoaringBitmap* rb = (SRoaringBitmap*)malloc(sizeof(SRoaringBitmap));
    if (!rb) {
        return -1;
    }
    
    size_t deserialized_size = roaring64_bitmap_portable_deserialize_size((const char*)buffer, buffer_size);
    if (deserialized_size == 0 || deserialized_size > buffer_size) {
        free(rb);
        return -1;
    }
    
    rb->roaring_bitmap = roaring64_bitmap_portable_deserialize_safe((const char*)buffer, buffer_size);
    if (!rb->roaring_bitmap) {
        free(rb);
        return -1;
    }
    
    *bitmap = rb;
    return 0;
}

static void tdengine_roaring_destroy(void* bitmap) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb) {
        if (rb->roaring_bitmap) {
            roaring64_bitmap_free(rb->roaring_bitmap);
        }
        free(rb);
    }
}

static size_t tdengine_roaring_memory_usage(void* bitmap) {
    SRoaringBitmap* rb = (SRoaringBitmap*)bitmap;
    if (rb && rb->roaring_bitmap) {
        // 返回 RoaringBitmap 的内存使用量（这是一个估算值）
        roaring64_statistics_t stats;
        roaring64_bitmap_statistics(rb->roaring_bitmap, &stats);
        return sizeof(SRoaringBitmap) + stats.n_bytes_array_containers + stats.n_bytes_run_containers + stats.n_bytes_bitset_containers;
    }
    return sizeof(SRoaringBitmap);
}

static void* tdengine_roaring_create(void) {
    SRoaringBitmap* rb = (SRoaringBitmap*)malloc(sizeof(SRoaringBitmap));
    if (!rb) {
        return NULL;
    }
    
    rb->roaring_bitmap = roaring64_bitmap_create();
    if (!rb->roaring_bitmap) {
        free(rb);
        return NULL;
    }
    
    return rb;
}

static void* tdengine_roaring_clone(const void* bitmap) {
    SRoaringBitmap* src = (SRoaringBitmap*)bitmap;
    if (!src || !src->roaring_bitmap) {
        return NULL;
    }
    
    SRoaringBitmap* rb = (SRoaringBitmap*)malloc(sizeof(SRoaringBitmap));
    if (!rb) {
        return NULL;
    }
    
    rb->roaring_bitmap = roaring64_bitmap_copy(src->roaring_bitmap);
    if (!rb->roaring_bitmap) {
        free(rb);
        return NULL;
    }
    
    return rb;
}

// 创建 RoaringBitmap 接口
SBitmapInterface* roaring_bitmap_interface_create(void) {
    SBitmapInterface* interface = (SBitmapInterface*)malloc(sizeof(SBitmapInterface));
    if (!interface) {
        return NULL;
    }
    
    interface->bitmap = tdengine_roaring_create();
    if (!interface->bitmap) {
        free(interface);
        return NULL;
    }
    
    // 设置函数指针
    interface->add = tdengine_roaring_add;
    interface->remove = tdengine_roaring_remove;
    interface->contains = tdengine_roaring_contains;
    interface->cardinality = tdengine_roaring_cardinality;
    interface->clear = tdengine_roaring_clear;
    interface->union_with = tdengine_roaring_union_with;
    interface->intersect_with = tdengine_roaring_intersect_with;
    interface->subtract = tdengine_roaring_subtract;
    interface->to_array = tdengine_roaring_to_array;
    interface->serialized_size = tdengine_roaring_serialized_size;
    interface->serialize = tdengine_roaring_serialize;
    interface->deserialize = tdengine_roaring_deserialize;
    interface->destroy = tdengine_roaring_destroy;
    interface->memory_usage = tdengine_roaring_memory_usage;
    interface->create = tdengine_roaring_create;
    interface->clone = tdengine_roaring_clone;
    
    return interface;
} 