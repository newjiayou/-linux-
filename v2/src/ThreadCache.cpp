#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"
#include <cstdlib>

namespace Kama_memoryPool
{

void* ThreadCache::allocate(size_t size)
{
    // 处理0大小的分配请求
    if (size == 0)
    {
        size = ALIGNMENT; // 至少分配一个对齐大小
    }
    
    if (size > MAX_BYTES)
    {
        // 大对象直接从系统分配
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);

    // 先尝试从线程本地自由链表取
    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        if (freeListSize_[index] > 0)
        {
            freeListSize_[index]--;
        }
        return ptr;
    }
    
    // 如果线程本地自由链表为空，则从中心缓存获取一批内存
    return fetchFromCentralCache(index);
}

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 插入到线程本地自由链表
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    // 更新对应自由链表的长度计数
    freeListSize_[index]++; 

    // 先关闭“回流 CentralCache”路径：当前实现在高并发下会破坏链表，先保证稳定性
    // if (shouldReturnToCentralCache(index))
    // {
    //     returnToCentralCache(freeList_[index], size);
    // }
}

// 判断是否需要将内存回收给中心缓存
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    // 设定阈值，例如：当自由链表的大小超过一定数量时
    size_t threshold = 256; 
    return (freeListSize_[index] > threshold);
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    // 从中心缓存批量获取内存
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    // 取一个返回，其余放入自由链表
    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);
    
    // 只统计“仍在 freeList_ 里”的空闲块数量（不包含 result）
    size_t batchNum = 0;
    void* current = freeList_[index];

    // 计算从中心缓存获取后，线程本地自由链表新增的块数量
    while (current != nullptr)
    {
        batchNum++;
        current = *reinterpret_cast<void**>(current); // 遍历下一个内存块
    }

    // 更新freeListSize_，增加新增空闲块数量
    freeListSize_[index] += batchNum;
    
    return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    // 根据大小计算对应的索引
    size_t index = SizeClass::getIndex(size);

    // 获取对齐后的实际块大小
    size_t alignedSize = SizeClass::roundUp(size);

    // 基于实际链表长度计算，避免计数漂移导致越界
    size_t batchNum = 0;
    void* scan = start;
    while (scan != nullptr)
    {
        batchNum++;
        scan = *reinterpret_cast<void**>(scan);
    }
    if (batchNum <= 1) return; // 如果只有一个块，则不归还

    // 保留一部分在ThreadCache中（比如保留1/4）
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 使用对齐后的大小计算分割点
    char* splitNode = static_cast<char*>(start);
    for (size_t i = 0; i < keepNum - 1; ++i) 
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
    }

    // 将要返回的部分和要保留的部分断开
    void* nextNode = *reinterpret_cast<void**>(splitNode);
    *reinterpret_cast<void**>(splitNode) = nullptr; // 断开连接

    // 更新ThreadCache的空闲链表
    freeList_[index] = start;

    // 更新自由链表大小
    freeListSize_[index] = keepNum;

    // 将剩余部分返回给CentralCache
    if (returnNum > 0 && nextNode != nullptr)
    {
        CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
    }
}

} // namespace memoryPool