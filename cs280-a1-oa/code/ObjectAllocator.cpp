#include "ObjectAllocator.h"
#include <cstring>
static constexpr size_t ptrSize = sizeof(void *);
static constexpr uint32_t freedFlag = 0x00u;
static constexpr uint32_t allocFlag = 0x01u;

// setters
void ObjectAllocator::SetDebugState(bool State) { config.DebugOn_ = State; } // true=enable, false=disable

// getters
const void *ObjectAllocator::GetFreeList() const { return freeList; } // returns a pointer to the internal free list
const void *ObjectAllocator::GetPageList() const { return pageList; } // returns a pointer to the internal page list
OAConfig ObjectAllocator::GetConfig() const { return config; }        // returns the configuration parameters
OAStats ObjectAllocator::GetStats() const { return stats; }           // returns the statistics for the allocator

#define SHOWREAL true

static void DumpPages(const ObjectAllocator* nm, unsigned width)
{
    const unsigned char* pages = static_cast<const unsigned char*>(nm->GetPageList());
    const unsigned char* realpage = pages;

    size_t header_size = nm->GetConfig().HBlockInfo_.size_;

    while (pages)
    {
        unsigned count = 0;

        if (SHOWREAL)
            printf("%p\n", pages);
        else
            printf("XXXXXXXX\n");

        // print column header
        for (unsigned j = 0; j < width; j++)
            printf(" %2i", j);
        printf("\n");

        // "Next page" pointer in the page
        if (SHOWREAL)
        {
            for (unsigned i = 0; i < sizeof(void*); i++, count++)
                printf(" %02X", *pages++);
        }
        else
        {
            for (unsigned j = 0; j < sizeof(void*); pages++, count++, j++)
                printf(" %s", "XX");
        }


        // Left leading alignment bytes
        if (nm->GetConfig().Alignment_ > 1)
        {
            // leading alignment block (if any)
            for (unsigned j = 0; j < nm->GetConfig().LeftAlignSize_; count++, j++)
            {
                if (count >= width)
                {
                    printf("\n");
                    count = 0;
                }
                printf(" %02X", *pages++);
            }
        }

        // Dump each object and its associated info
        for (unsigned int i = 0; i < nm->GetConfig().ObjectsPerPage_; i++)
        {
            // inter-block alignment (not on first block)
            if (i > 0)
            {
                for (unsigned j = 0; j < nm->GetConfig().InterAlignSize_; count++, j++)
                {
                    if (count >= width)
                    {
                        printf("\n");
                        count = 0;
                    }
                    printf(" %02X", *pages++);
                }
            }

            // header block bytes
            for (unsigned j = 0; j < header_size; count++, j++)
            {
                if (count >= width)
                {
                    printf("\n");
                    count = 0;
                }
                printf(" %02X", *pages++);
            }

            // left padding
            for (unsigned j = 0; j < nm->GetConfig().PadBytes_; count++, j++)
            {
                if (count >= width)
                {
                    printf("\n");
                    count = 0;
                }
                printf(" %02X", *pages++);
            }

            // possible next pointer (zero it out)
            for (unsigned j = 0; j < sizeof(void*); count++, pages++, j++)
            {
                if (count >= width)
                {
                    printf("\n");
                    count = 0;
                }
                if (SHOWREAL)
                    printf(" %02X", *pages);
                else
                    printf(" %s", "XX");
            }

            // remaining bytes
            for (unsigned j = 0; j < nm->GetStats().ObjectSize_ - sizeof(void*); count++, j++)
            {
                if (count >= width)
                {
                    printf("\n");
                    count = 0;
                }
                printf(" %02X", *pages++);
            }

            // right pad bytes
            for (unsigned j = 0; j < nm->GetConfig().PadBytes_; count++, j++)
            {
                if (count >= width)
                {
                    printf("\n");
                    count = 0;
                }
                printf(" %02X", *pages++);
            }

        }
        printf("\n\n");

        pages = reinterpret_cast<const unsigned char*>((reinterpret_cast<const GenericObject*>(realpage))->Next);
        realpage = pages;
    }
}

static bool IsMemoryFreed(void* mem,size_t size)
{
    uint8_t* rawMem = reinterpret_cast<uint8_t*>(mem);

    // for (size_t i = 0; i < size; ++i)
    //     printf("%02X ", rawMem[i]);
  
    for (size_t i = 0; i < size; ++i)
    {
        if(rawMem[i] != ObjectAllocator::FREED_PATTERN)
            return false;
    }
    return true;
    
    //return *rawMem == ObjectAllocator::FREED_PATTERN;

}

ObjectAllocator::ObjectAllocator(size_t ObjectSize, const OAConfig &configuration)
    : pageList{nullptr}, freeList{nullptr}, config{configuration}, stats{}
{

    size_t allocateBytes = ptrSize + config.LeftAlignSize_ +
                           (config.HBlockInfo_.size_ + config.PadBytes_ + ObjectSize + config.PadBytes_ + config.InterAlignSize_) * config.ObjectsPerPage_;

    stats.ObjectSize_ = ObjectSize;
    stats.PageSize_ = allocateBytes;
    stats.MostObjects_ = 0;
    CreateNewPage();
}

ObjectAllocator::~ObjectAllocator()
{

    while (pageList)
    {
        GenericObject *next = pageList->Next;
        uint8_t *rawMem = reinterpret_cast<uint8_t *>(pageList);
        delete[] rawMem;
        pageList = next;
    }
}

void ObjectAllocator::CreateNewPage()
{

    uint8_t *rawMem = nullptr;
    try
    {
        rawMem = new uint8_t[stats.PageSize_];

        //if (config.DebugOn_)
            memset(rawMem, UNALLOCATED_PATTERN, stats.PageSize_);
    }
    catch (std::bad_alloc &)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to allocate new page: No system memory available.");
    }

    if (!pageList) // first page
    {
        pageList = reinterpret_cast<GenericObject *>(rawMem);
        pageList->Next = nullptr; // no additional page
    }
    else
    {
        GenericObject *temp = pageList;
        pageList = reinterpret_cast<GenericObject *>(rawMem);
        pageList->Next = temp;
    }

    size_t freeListOffset = ptrSize; // ptrSize + config.LeftAlignSize_;
    if (!freeList)
    {
        freeList = reinterpret_cast<GenericObject *>(rawMem + freeListOffset);
        freeList->Next = nullptr;
    }
    else
    {
        GenericObject *temp = pageList;
        freeList = reinterpret_cast<GenericObject *>(rawMem + freeListOffset);
        freeList->Next = temp;
    }

    for (size_t i = freeListOffset; i < stats.PageSize_; i += stats.ObjectSize_)
    {
        GenericObject *prev = freeList;
        freeList = reinterpret_cast<GenericObject *>(rawMem + i);
        freeList->Next = prev;
    }

    ++stats.PagesInUse_;
    stats.FreeObjects_ += config.ObjectsPerPage_;
}

void *ObjectAllocator::Allocate(const char *label)
{

    // out of blocks for more obj create new pages
    if (stats.FreeObjects_ == 0)
    {
        if (config.MaxPages_ == 0 || config.MaxPages_ > stats.PagesInUse_)
        {
            // printf("Create new page\n");
            CreateNewPage();
        }
        else
        {
            throw OAException(OAException::E_NO_PAGES,
                              "Failed to create new page: Max pages of " + std::to_string(config.MaxPages_) + " has already been created.");
        }
    }

    uint8_t *freeBlock = reinterpret_cast<uint8_t *>(freeList);
    freeList = freeList->Next;

    uint8_t *headerBlock = freeBlock; // start ptr to headerblock

    switch (config.HBlockInfo_.type_)
    {
    case OAConfig::HBLOCK_TYPE::hbBasic:
    {
        // basic header block format: allocation num(4 bytes) , flag(1 bytes)
        uint32_t *allocationNum = reinterpret_cast<uint32_t *>(headerBlock);
        ++*allocationNum;                              // allocation number
        *(headerBlock + sizeof(uint32_t)) = allocFlag; // flag to indicate is block allocated or not
        break;
    }
    case OAConfig::HBLOCK_TYPE::hbExtended:
    {
        // extended format: user defined , use count (2 bytes) , allocation num(4 bytes) ,flag(1 bytes)
        // skip user define data
        size_t accumlateOffset = config.HBlockInfo_.additional_;
        uint16_t *useCount = reinterpret_cast<uint16_t *>(headerBlock + accumlateOffset);
        ++*useCount;
        accumlateOffset += sizeof(uint16_t);

        // allocation num
        uint32_t *allocationNum = reinterpret_cast<uint32_t *>(headerBlock + accumlateOffset);
        ++*allocationNum;
        accumlateOffset += sizeof(uint32_t);

        // flag
        *(headerBlock + accumlateOffset) = freedFlag;

        break;
    }

    case OAConfig::HBLOCK_TYPE::hbExternal:
        break;
    case OAConfig::HBLOCK_TYPE::hbNone:
    default:
        break;
    }

    // write to data section
    // if(config.DebugOn_)
    {
        uint8_t *dataBlock = headerBlock + config.HBlockInfo_.size_ + config.PadBytes_;
        memset(dataBlock, ALLOCATED_PATTERN, stats.ObjectSize_);
    }

    // stats
    ++stats.Allocations_;
    ++stats.ObjectsInUse_;
    stats.MostObjects_ = std::max(stats.MostObjects_, stats.ObjectsInUse_);
    --stats.FreeObjects_;
    stats.FreeObjects_ = std::max(0u, stats.FreeObjects_);

    return headerBlock;
}

bool ObjectAllocator::IsValidMemoryRange(uint8_t *headerBlock)
{
    bool withinRange = false;
    // range check
    uint8_t *page = reinterpret_cast<uint8_t *>(pageList);

    while (page && !withinRange)
    {
        size_t pageSize = static_cast<size_t>(headerBlock - page);
        // printf("PageSize:%lu\n",pageSize);
        // check for valid address if the object is aligned
        //  bool aligned = (pageSize % stats.ObjectSize_) - ptrSize == 0;
        //  if(!aligned)
        //  {
        //      printf("Throw corrupted\n");
        //      throw OAException(OAException::E_CORRUPTED_BLOCK, "Memory Block Corrupted\n");
        //  }

        withinRange = headerBlock >= page && pageSize < stats.PageSize_;
        page = reinterpret_cast<uint8_t *>(reinterpret_cast<GenericObject *>(page)->Next);
    }
    return withinRange;
}

void ObjectAllocator::Free(void *Object)
{
    if (Object)
    {

        uint8_t *headerBlock = reinterpret_cast<uint8_t *>(Object);
        // debug check
        if (config.DebugOn_)
        {     
            //uint8_t *dataBlock = headerBlock + config.HBlockInfo_.size_ + config.PadBytes_;
            // printf("Hex:%02X,Dec:%i\n",*dataBlock,*dataBlock);
            
            if (IsMemoryFreed(headerBlock , stats.ObjectSize_))
            {
                throw OAException(OAException::E_MULTIPLE_FREE, "Double Free Detected: Memory is already freed\n");
            }
            if (!IsValidMemoryRange(headerBlock))
            {
                throw OAException(OAException::E_BAD_BOUNDARY, "Freeing out of range memory\n");
            }
        }
        
        //update headerblock flags
        switch (config.HBlockInfo_.type_)
        {
            case OAConfig::HBLOCK_TYPE::hbBasic:
            {
                //basic header block format: allocation num(4 bytes) , flag(1 bytes)
                *(headerBlock + sizeof(uint32_t)) = freedFlag;       // flag to indicate is block allocated or not
                break;
            }
            case OAConfig::HBLOCK_TYPE::hbExtended:
            {
                //extended format: user defined , use count (2 bytes) , allocation num(4 bytes) ,flag(1 bytes)
                //skip user define data
                size_t accumlateOffset =  config.HBlockInfo_.additional_ + sizeof(uint16_t) + sizeof(uint32_t);
                //flag
                *(headerBlock + accumlateOffset) = freedFlag;

                break;
            }
            case OAConfig::HBLOCK_TYPE::hbExternal:
                break;
            case OAConfig::HBLOCK_TYPE::hbNone:
            default:
                break;
        }

        GenericObject *temp = reinterpret_cast<GenericObject *>(Object);
        temp->Next = freeList;
        freeList = temp;

        //DumpPages(this,24);
        uint8_t *dataBlock = headerBlock + config.HBlockInfo_.size_ + config.PadBytes_;
        memset(dataBlock, FREED_PATTERN, stats.ObjectSize_);

        // stats
        ++stats.FreeObjects_;
        ++stats.Deallocations_;
        --stats.ObjectsInUse_;
    }
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