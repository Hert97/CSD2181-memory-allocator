#include "ObjectAllocator.h"
#include <cstring>
#include <assert.h>

static constexpr size_t ptrSize = sizeof(void*);
static constexpr uint32_t freedFlag = 0x00u;
static constexpr uint32_t allocFlag = 0x01u;

// setters
void ObjectAllocator::SetDebugState(bool State) { config.DebugOn_ = State; } // true=enable, false=disable

// getters
const void* ObjectAllocator::GetFreeList() const { return freeList; } // returns a pointer to the internal free list
const void* ObjectAllocator::GetPageList() const { return pageList; } // returns a pointer to the internal page list
OAConfig ObjectAllocator::GetConfig() const { return config; }        // returns the configuration parameters
OAStats ObjectAllocator::GetStats() const { return stats; }           // returns the statistics for the allocator



static void PrintList(const char* label, GenericObject* n)
{
    printf("%s\n", label);
    while (n)
    {
        printf("%p\n", n);
        n = n->Next;
    }
}

static bool IsMemoryFreed(void* mem, size_t byteSize)
{
    uint8_t* rawMem = reinterpret_cast<uint8_t*>(mem);

    // for (size_t i = 0; i < size; ++i)
    //     printf("%02X ", rawMem[i]);

    for (size_t i = 0; i < byteSize; ++i)
    {
        if (rawMem[i] != ObjectAllocator::FREED_PATTERN)
            return false;
    }
    return true;

    //return *rawMem == ObjectAllocator::FREED_PATTERN;

}
static void WritePatternToBlock(void* object, size_t byteSize, uint8_t pattern)
{

    uint8_t* memStart = reinterpret_cast<uint8_t*>(object);

    if (byteSize == 0 || !memStart)
        return;

    memset(memStart, pattern, byteSize);

}

ObjectAllocator::ObjectAllocator(size_t ObjectSize, const OAConfig& configuration)
    : pageList{ nullptr }, freeList{ nullptr }, config{ configuration }, stats{}
{


    size_t totalBlockSize = config.ObjectsPerPage_ * (config.HBlockInfo_.size_ + config.PadBytes_ + ObjectSize + config.PadBytes_ + config.InterAlignSize_);
    size_t allocateSize = ptrSize + config.LeftAlignSize_ + totalBlockSize - config.InterAlignSize_;

    stats.ObjectSize_ = ObjectSize;
    stats.PageSize_ = allocateSize;
    stats.MostObjects_ = 0;
    CreatePage();
}

ObjectAllocator::~ObjectAllocator()
{

    while (pageList)
    {
        GenericObject* next = pageList->Next;
        uint8_t* rawMem = reinterpret_cast<uint8_t*>(pageList);
        delete[] rawMem;
        pageList = next;
    }
}

void ObjectAllocator::CreatePage()
{

    uint8_t* rawMem = nullptr;
    try
    {
        rawMem = new uint8_t[stats.PageSize_];
    }
    catch (const std::bad_alloc&)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to allocate new page: No system memory available.");
    }
    //set unallocated pattern for all
    memset(rawMem, UNALLOCATED_PATTERN, stats.PageSize_);

    GenericObject* pageStart = pageList;
    pageList = reinterpret_cast<GenericObject*>(rawMem);
    pageList->Next = pageStart;

    //PrintFreeList("PageList:", pageList);

    uint8_t* advancePtr = rawMem; //ptr used for advancement

    advancePtr += ptrSize;
    WritePatternToBlock(advancePtr, config.LeftAlignSize_, ALIGN_PATTERN);
    advancePtr += config.LeftAlignSize_;

    uint8_t* pagePtrEnd = rawMem + stats.PageSize_;

    while (advancePtr != pagePtrEnd)
    {
        //header
        WritePatternToBlock(advancePtr, config.HBlockInfo_.size_, 0);
        advancePtr += config.HBlockInfo_.size_;
        //start padding 
        WritePatternToBlock(advancePtr, config.PadBytes_, PAD_PATTERN);
        advancePtr += config.PadBytes_;

        //link freelist
        GenericObject* prev = freeList;
        freeList = reinterpret_cast<GenericObject*>(advancePtr);
        freeList->Next = prev;

        //skip object pattern as its aldy set
        advancePtr += stats.ObjectSize_;
        //end padding 
        WritePatternToBlock(advancePtr, config.PadBytes_, PAD_PATTERN);
        advancePtr += config.PadBytes_;

        if (advancePtr != pagePtrEnd) //skip last block interalignment
        {
            WritePatternToBlock(advancePtr, config.InterAlignSize_, ALIGN_PATTERN);
            advancePtr += config.InterAlignSize_;
        }
    }

    //PrintList("Create freeList:", freeList);
    ++stats.PagesInUse_;
    stats.FreeObjects_ += config.ObjectsPerPage_;


}

void* ObjectAllocator::Allocate(const char* label)
{

    //PrintFreeList("FreeList:", freeList);

    // out of blocks for more obj create new pages
    if (!freeList)
    {
        if (config.MaxPages_ == 0 || config.MaxPages_ > stats.PagesInUse_)
        {
            //printf("Create new page\n");
            CreatePage();
        }
        else
        {
            throw OAException(OAException::E_NO_PAGES,
                "Failed to create new page: Max pages of " + std::to_string(config.MaxPages_) + " has already been created.");
        }
    }

    //PrintList("B FreeList:", freeList);

    assert(freeList);
    uint8_t* freeBlock = reinterpret_cast<uint8_t*>(freeList);
    freeList = freeList->Next;

    assert(freeBlock);

    uint8_t* headerBlock = freeBlock; // start ptr to headerblock

    //PrintList("A FreeList:", freeList);

    memset(freeBlock, ALLOCATED_PATTERN, stats.ObjectSize_);

    //update stats
    ++stats.Allocations_;
    ++stats.ObjectsInUse_;
    stats.MostObjects_ = std::max(stats.MostObjects_, stats.ObjectsInUse_);
    --stats.FreeObjects_;

    //create external header block
    if (config.HBlockInfo_.type_ == OAConfig::hbExternal)
    {
        AllocateExternalHeader(headerBlock,label);
    }
    //update headers info if any
    UpdateHeaderInfo(headerBlock, allocFlag);


    return freeBlock;
}


void ObjectAllocator::Free(void* Object)
{
    if (Object)
    {

        uint8_t* headerBlock = reinterpret_cast<uint8_t*>(Object);
        //uint8_t *dataBlock = headerBlock + config.HBlockInfo_.size_ + config.PadBytes_;

        // debug check
        if (config.DebugOn_)
        {

            if (IsMemoryFreed(headerBlock, stats.ObjectSize_))
            {
                throw OAException(OAException::E_MULTIPLE_FREE, "Double Free Detected: Memory is already freed\n");
            }
            if (!IsValidMemoryRange(headerBlock))
            {
                throw OAException(OAException::E_BAD_BOUNDARY, "Freeing out of range memory\n");
            }
        }

        //clear to freed pattern
        memset(headerBlock, FREED_PATTERN, stats.ObjectSize_);

        
        //last to prevent overwrite
        GenericObject* temp = reinterpret_cast<GenericObject*>(Object);
        temp->Next = freeList;
        freeList = temp;

        //update stats
        ++stats.FreeObjects_;
        ++stats.Deallocations_;
        --stats.ObjectsInUse_;

        //update headers info if any
        UpdateHeaderInfo(headerBlock, freedFlag);

        
        if (config.HBlockInfo_.type_ == OAConfig::hbExternal)
        {
            FreeExternalHeader(headerBlock);
        }

    }
}
void ObjectAllocator::AllocateExternalHeader(void* objBlock , const char* label)
{
    uint8_t* headerStart =
        reinterpret_cast<uint8_t*>(objBlock)
        - config.PadBytes_ - config.HBlockInfo_.size_;

    MemBlockInfo* infoBlock = nullptr;
    try
    {
        infoBlock = new MemBlockInfo;
    }
    catch (const std::bad_alloc&)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to create ExternalHeader: No system memory available.");
    }
    //allocate for string label
    try
    {
        size_t len = strlen(label); //strlen
        infoBlock->label = new char[len + 1];
        
    }
    catch (const std::bad_alloc&)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to create ExternalHeader: No system memory available.");
    }
    //copy text label
    strcpy(infoBlock->label,label);
    //copy external header address to block
    MemBlockInfo** tempHeaderPtr = reinterpret_cast<MemBlockInfo**>(headerStart);
    *tempHeaderPtr = infoBlock;

}
void ObjectAllocator::FreeExternalHeader(void* objBlock)
{
    uint8_t* headerStart =
        reinterpret_cast<uint8_t*>(objBlock)
        - config.PadBytes_ - config.HBlockInfo_.size_;

    MemBlockInfo* infoBlock = *reinterpret_cast<MemBlockInfo**>(headerStart);

    //free text mem
    delete infoBlock->label; 
    //free Memblockinfo
    delete infoBlock;

    //avoid dangling ptr
    GenericObject** tempHeaderPtr = reinterpret_cast<GenericObject**>(headerStart);
    *tempHeaderPtr = nullptr;

}

void ObjectAllocator::UpdateHeaderInfo(void* objBlock, uint8_t flag)
{
    uint8_t* headerStart =
        reinterpret_cast<uint8_t*>(objBlock)
        - config.PadBytes_ - config.HBlockInfo_.size_;

    bool isFromAllocateFunc = flag == allocFlag;

    switch (config.HBlockInfo_.type_)
    {
    case OAConfig::HBLOCK_TYPE::hbBasic:
    {
        //allocation count
        uint32_t* allocNum = reinterpret_cast<uint32_t*>(headerStart);
        *allocNum = isFromAllocateFunc ? stats.Allocations_ : 0;
        headerStart += sizeof(uint32_t);
        //flag bit
        *headerStart = flag;
        break;
    }
    case OAConfig::HBLOCK_TYPE::hbExtended:
    {
        headerStart += config.HBlockInfo_.additional_;  //skip user define block
        //used counter
        if (isFromAllocateFunc)
        {
            uint16_t* useCounter = reinterpret_cast<uint16_t*>(headerStart);
            ++*useCounter;
        }

        headerStart += sizeof(uint16_t);
        //allocation count
        uint32_t* allocNum = reinterpret_cast<uint32_t*>(headerStart);
        *allocNum = isFromAllocateFunc ? stats.Allocations_ : 0;
        headerStart += sizeof(uint32_t);
        //flag bit
        *headerStart = flag;
        break;
    }
    case OAConfig::HBLOCK_TYPE::hbExternal:
    {
        MemBlockInfo* infoBlock = *reinterpret_cast<MemBlockInfo**>(headerStart);

        if (infoBlock) //if valid
        {
            infoBlock->in_use = isFromAllocateFunc;
            infoBlock->alloc_num = isFromAllocateFunc ? stats.Allocations_ : 0;
        }
       
        break;
    }
    default:
        break;
    }
}


GenericObject* ObjectAllocator::FindPage(void *objBlock)
{
    return nullptr;
}

bool ObjectAllocator::IsValidAlignment(void *objBlock)
{
    return false;
}

bool ObjectAllocator::IsValidMemoryRange(void* objBlock)
{
    bool withinRange = false;
    uint8_t* block = reinterpret_cast<uint8_t*>(objBlock);
    // range check
    uint8_t* page = reinterpret_cast<uint8_t*>(pageList);

    //loop through all pages to check if block is within valid address range
    while (page && !withinRange)
    {
        size_t pageSize = static_cast<size_t>(block - page);
        withinRange = block >= page && pageSize < stats.PageSize_;
        GenericObject* temp = reinterpret_cast<GenericObject*>(page)->Next;
        page = reinterpret_cast<uint8_t*>(temp);
    }
    return withinRange;
}

unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const
{
    return 0;
}

unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const
{
    return 0;
}

unsigned ObjectAllocator::FreeEmptyPages()
{
    return 0;
}