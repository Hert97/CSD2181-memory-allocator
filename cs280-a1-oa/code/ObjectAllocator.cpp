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

static void WritePatternToBlock(void* object, size_t byteSize, uint8_t pattern)
{

    uint8_t* memStart = reinterpret_cast<uint8_t*>(object);

    if (byteSize == 0 || !memStart)
        return;

    memset(memStart, pattern, byteSize);

}
static void UpdateAllocationStats(OAStats& stats)
{
    //update stats
    ++stats.Allocations_;
    ++stats.ObjectsInUse_;
    stats.MostObjects_ = std::max(stats.MostObjects_, stats.ObjectsInUse_);
    --stats.FreeObjects_;
}
static void UpdateDeallocationStats(OAStats& stats)
{
    //update stats
    ++stats.FreeObjects_;
    ++stats.Deallocations_;
    --stats.ObjectsInUse_;
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

    if(config.UseCPPMemManager_)
    {
        //update stats
        UpdateAllocationStats(stats);
        return new uint8_t[stats.ObjectSize_] ;
    }

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
    UpdateAllocationStats(stats);

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

    if(config.UseCPPMemManager_)
    {
        //update stats
        UpdateDeallocationStats(stats);
        delete[] Object;
        return;
    }

    uint8_t* objBlock = reinterpret_cast<uint8_t*>(Object);

    if (objBlock)
    {
        // debug check
        if (config.DebugOn_)
        {
            if (IsMemoryFreed(objBlock))
            {
                throw OAException(OAException::E_MULTIPLE_FREE, "Double Free Detected: Memory is already freed\n");
            }
            GenericObject* pageFound = FindPage(objBlock);
            if (!pageFound) 
            {
                throw OAException(OAException::E_BAD_BOUNDARY, "Freeing: Out of range memory\n");
            }
            if(!IsValidAlignment(objBlock,pageFound))
            {
                throw OAException(OAException::E_BAD_BOUNDARY, "Freeing: Valid alignment\n");
            }
            //check corruption
            if(IsPaddingCorrupted(objBlock))
            {
                throw(OAException(OAException::E_CORRUPTED_BLOCK, "MemoryBlock: Corruption detected\n"));
            }

        }

        //clear to freed pattern
        memset(Object, FREED_PATTERN, stats.ObjectSize_);

        
        //last to prevent overwrite
        GenericObject* temp = reinterpret_cast<GenericObject*>(Object);
        temp->Next = freeList;
        freeList = temp;

        //update stats
        UpdateDeallocationStats(stats);

        //update headers info if any
        UpdateHeaderInfo(objBlock, freedFlag);

        
        if (config.HBlockInfo_.type_ == OAConfig::hbExternal)
        {
            FreeExternalHeader(objBlock);
        }

    }
}
void ObjectAllocator::AllocateExternalHeader(uint8_t* objBlock , const char* label)
{
    uint8_t* headerStart = objBlock - config.PadBytes_ - config.HBlockInfo_.size_;

    MemBlockInfo* infoBlock = nullptr;
    try
    {
        infoBlock = new MemBlockInfo;
    }
    catch (const std::bad_alloc&)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to create ExternalHeader: No system memory available.");
    }
    size_t len = label ? strlen(label) : 0; //strlen
    //allocate for string label
    try
    {
        
        infoBlock->label = new char[len + 1];

    }
    catch (const std::bad_alloc&)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to create ExternalHeader: No system memory available.");
    }
    //copy text label
    if(len > 0)
        strcpy(infoBlock->label,label);
    //copy external header address to block
    MemBlockInfo** tempHeaderPtr = reinterpret_cast<MemBlockInfo**>(headerStart);
    *tempHeaderPtr = infoBlock;

}
void ObjectAllocator::FreeExternalHeader(uint8_t* objBlock)
{
    uint8_t* headerStart = objBlock - config.PadBytes_ - config.HBlockInfo_.size_;

    MemBlockInfo* infoBlock = *reinterpret_cast<MemBlockInfo**>(headerStart);

    //free text mem
    delete[] infoBlock->label; 
    //free Memblockinfo
    delete infoBlock;

    //avoid dangling ptr
    GenericObject** tempHeaderPtr = reinterpret_cast<GenericObject**>(headerStart);
    *tempHeaderPtr = nullptr;

}

void ObjectAllocator::UpdateHeaderInfo(uint8_t* objBlock, uint8_t flag)
{
    uint8_t* headerStart = objBlock - config.PadBytes_ - config.HBlockInfo_.size_;

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


GenericObject* ObjectAllocator::FindPage(uint8_t *objBlock) const
{
    // range check
    uint8_t* page = reinterpret_cast<uint8_t*>(pageList);
   
    //loop through all pages to check if block is within valid address range
    while (page)
    {
        uint8_t* pageEnd = page + stats.PageSize_;
      
        if(objBlock >= page && objBlock < pageEnd) //found page
        {
            return reinterpret_cast<GenericObject*>(page);
        }
   
        page = reinterpret_cast<uint8_t*>(reinterpret_cast<GenericObject*>(page)->Next);
    }

    return nullptr;
}

bool ObjectAllocator::IsMemoryFreed(uint8_t* objBlock) const 
{
    GenericObject* currBlock = freeList;
    GenericObject* obj = reinterpret_cast<GenericObject*>(objBlock);
    //scan through freeList
    while(currBlock) 
    {
        if(obj == currBlock) // aldy exist on freeList
            return true;
        currBlock = currBlock->Next;
    }
    return false;

}

bool ObjectAllocator::IsValidAlignment(uint8_t *objBlock ,GenericObject* pageLocation) const
{
    if(!pageLocation)
        return false;

    //page first data block section
    uint8_t* dataBlockStart =  
    reinterpret_cast<uint8_t*>(pageLocation) 
    + ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;
    //objects alignment size
    size_t objAlignmentSize = stats.ObjectSize_ 
    + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;
    
    size_t actualSize = objBlock - dataBlockStart;
    
    return actualSize % objAlignmentSize == 0;
}
bool ObjectAllocator::IsPaddingCorrupted(uint8_t *objBlock) const
{
    if(config.PadBytes_ == 0) //early out
        return false; 
    uint8_t* padStart = objBlock - config.PadBytes_ ;
    uint8_t* padEnd = padStart + stats.ObjectSize_ + config.PadBytes_;

    for (size_t i = 0; i < config.PadBytes_; ++i)
    {   
        if(padStart[i] != PAD_PATTERN)
            return true;
        if(padEnd[i] != PAD_PATTERN)
            return true;
    }   
    return false;
}

unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const
{
    unsigned int counter = 0;

    GenericObject* page = pageList;
    size_t toDataOffset 
    = ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;
    
    //obj data alignment
    const size_t objAlignment = 
    stats.ObjectSize_ + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;
   
    while(page)
    {
        uint8_t* objData =  reinterpret_cast<uint8_t*>(page) + toDataOffset; 
        bool usedFlag = false;

        //loop per object in page
        for (size_t i = 0; i < config.ObjectsPerPage_; ++i)
        {
            //read header info if available
            if(config.HBlockInfo_.type_ != OAConfig::hbNone)
            {   
                uint8_t* headerStart = objData - config.PadBytes_ - config.HBlockInfo_.size_;
                
                switch (config.HBlockInfo_.type_)
                {
                    case OAConfig::HBLOCK_TYPE::hbBasic:
                    {
                        headerStart += sizeof(uint32_t);
                        //flag bit
                        usedFlag = *headerStart ;
                        break;
                    }            
                    case OAConfig::HBLOCK_TYPE::hbExtended:
                    {
                        headerStart += config.HBlockInfo_.additional_;  //skip user define block
                        //skip used counter
                        headerStart += sizeof(uint16_t) + sizeof(uint32_t);
                        //flag bit
                        usedFlag = *headerStart;
                        break;
                    }
                    case OAConfig::HBLOCK_TYPE::hbExternal:
                    {
                        MemBlockInfo* infoBlock = *reinterpret_cast<MemBlockInfo**>(headerStart);

                        if (infoBlock) //if valid
                        {
                        usedFlag = infoBlock->in_use;
                        }  
                        break;
                    }
                    default:
                        break;
                }

            
            }
            //else loop through freelist
            else
            {   
                usedFlag = !IsMemoryFreed(objData);
            }

            //block is in use
            if(usedFlag) 
            {
                fn(objData,stats.ObjectSize_);
                ++counter;
            }   

            //if is last object on page dont add alignment as it contains interalignment
            if(i != config.ObjectsPerPage_ - 1 )
            {
                objData += objAlignment;   
            }
           
        } 
        page = page->Next;
    }
    return counter;
}

unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const
{   
    unsigned int counter = 0;

    if(config.PadBytes_ > 0)   
    {
        GenericObject* page = pageList;
        size_t toDataOffset 
        = ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;
        //obj data alignment
        const size_t objAlignment = 
        stats.ObjectSize_ + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;
    
        while(page)
        {
            uint8_t* objData =  reinterpret_cast<uint8_t*>(page) + toDataOffset; 

            for (size_t i = 0; i < config.ObjectsPerPage_; ++i)
            {
                if(IsPaddingCorrupted(objData))
                {
                    fn(objData,stats.ObjectSize_);
                    ++counter;
                } 
                //if is last object on page dont add alignment as it contains interalignment
                if(i != config.ObjectsPerPage_ - 1 )
                {
                    objData += objAlignment;   
                }
                  
            }     
            page = page->Next;
        }
    }
    return counter;
}

unsigned ObjectAllocator::FreeEmptyPages()
{
    return 0;
}