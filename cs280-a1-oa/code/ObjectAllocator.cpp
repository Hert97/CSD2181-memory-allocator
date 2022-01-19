#include "ObjectAllocator.h"
#include <cstring>

static constexpr size_t ptrSize = sizeof(void*);
static constexpr uint32_t freedFlag = 0x00u;
static constexpr uint32_t allocFlag = 0x01u;

//setters
void ObjectAllocator::SetDebugState(bool State) { config.DebugOn_ = State; }             // true=enable, false=disable

//getters
const void* ObjectAllocator::GetFreeList()  const { return freeList;}       // returns a pointer to the internal free list
const void* ObjectAllocator::GetPageList()  const { return pageList;}       // returns a pointer to the internal page list
OAConfig    ObjectAllocator::GetConfig()    const { return config;  }       // returns the configuration parameters
OAStats     ObjectAllocator::GetStats()     const { return stats;   }       // returns the statistics for the allocator




ObjectAllocator::ObjectAllocator(size_t ObjectSize, const OAConfig& configuration)
    : pageList{nullptr} , freeList{nullptr} , config{configuration} , stats{}
{
    
   

    size_t allocateBytes =  ptrSize + config.LeftAlignSize_ + 
                            (config.HBlockInfo_.size_ 
                            + config.PadBytes_ 
                            + ObjectSize 
                            + config.PadBytes_ 
                            + config.InterAlignSize_) 
                            * config.ObjectsPerPage_ ;

    stats.ObjectSize_ = ObjectSize;
    stats.PageSize_ =  allocateBytes;
    stats.MostObjects_ = 0;
    CreateNewPage();
    
}


ObjectAllocator::~ObjectAllocator()
{
   
    while(pageList)
    {   
        GenericObject* next = pageList->Next;   
        char* rawMem = reinterpret_cast<char*>(pageList);
        delete[] rawMem;
        pageList = next;
    }
  
}

void ObjectAllocator::CreateNewPage()
{
    
    char* rawMem = nullptr;
    try
    {
        rawMem = new char [ stats.PageSize_];

        if(config.DebugOn_)
            memset(rawMem,UNALLOCATED_PATTERN, stats.PageSize_);
    }
    catch (std::bad_alloc&)
    {
        throw OAException(OAException::E_NO_MEMORY, "Failed to allocate new page: No system memory available.");
    }
    

    if(!pageList)   //first page
    {
        pageList = reinterpret_cast<GenericObject*>(rawMem);
        pageList->Next = nullptr; // no additional page
    }
    else
    {
        GenericObject* temp = pageList;
        pageList = reinterpret_cast<GenericObject*>(rawMem);
        pageList->Next = temp;
    }
    

    size_t freeListOffset =  ptrSize; //ptrSize + config.LeftAlignSize_;
    if(!freeList)
    {
        freeList = reinterpret_cast<GenericObject*>(rawMem + freeListOffset);
        freeList->Next = nullptr;
    }
    else
    {
        GenericObject* temp = pageList;        
        freeList->Next = reinterpret_cast<GenericObject*>(rawMem + freeListOffset);
        freeList->Next = temp;
    }
   
    for (size_t i = freeListOffset ; i <  stats.PageSize_ ; i += stats.ObjectSize_)
    {
        GenericObject* prev = freeList;
        freeList = reinterpret_cast<GenericObject*>(rawMem + i) ;
        freeList->Next = prev;
    }

    // if ( /* Object is on a valid page boundary */ )
    // // put it on the free list
    // else
    //     throw OAException(OAException::E_BAD_BOUNDARY, "validate_object: Object not on a boundary.");
    
  
    ++stats.PagesInUse_;
    stats.FreeObjects_ += config.ObjectsPerPage_;

}

void* ObjectAllocator::Allocate(const char *label)
{

    //out of blocks for more obj create new pages
    if(stats.FreeObjects_ == 0)
    {   
        if(config.MaxPages_ == 0 || config.MaxPages_ > stats.PagesInUse_)
        {
            //printf("Create new page\n");
            CreateNewPage();  
        }
        else
        {
            throw 
            OAException(OAException::E_NO_PAGES, 
            "Failed to create new page: Max pages of " 
            + std::to_string(config.MaxPages_) 
            + " has already been created.");
        }
    }
      

    char* freeBlock = reinterpret_cast<char*>(freeList);
    freeList = freeList->Next;

    char* headerBlock = freeBlock ; //start ptr to headerblock

    switch (config.HBlockInfo_.type_)
    {   
    case OAConfig::HBLOCK_TYPE::hbBasic:
    {
        //basic header block format: allocation num(4 bytes) , flag(1 bytes)
        uint32_t* allocationNum = reinterpret_cast<uint32_t*>(headerBlock);
        *allocationNum += 1;                           // allocation number 
        *(headerBlock + sizeof(uint32_t)) = allocFlag;       // flag to indicate is block allocated or not
        break;
    }
    case OAConfig::HBLOCK_TYPE::hbExtended:
    {
        //extended format: user defined , use count (2 bytes) , allocation num(4 bytes) ,flag(1 bytes)
        //skip user define data
        size_t accumlateOffset =  config.HBlockInfo_.additional_; 
        uint16_t* useCount = reinterpret_cast<uint16_t*>( headerBlock + accumlateOffset);
        ++*useCount;
        accumlateOffset += sizeof(uint16_t);

        //allocation num
        uint32_t* allocationNum = reinterpret_cast<uint32_t*>(headerBlock + accumlateOffset);
        *allocationNum += 1;                           
        accumlateOffset += sizeof(uint32_t);

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
    
    //stats 
    ++stats.Allocations_;
    ++stats.ObjectsInUse_;
    stats.MostObjects_ 
    = std::max(stats.MostObjects_,stats.ObjectsInUse_);
    --stats.FreeObjects_;  
    stats.FreeObjects_ = std::max(0u, stats.FreeObjects_);

    //write to data section
    if(config.DebugOn_)
    {
        char* dataBlock = headerBlock + config.HBlockInfo_.size_ + config.PadBytes_;
        memset(dataBlock,ALLOCATED_PATTERN,stats.ObjectSize_);
    }
    

    return headerBlock ;
    
}

void ObjectAllocator::Free(void *Object)
{
    if(Object)
    {
       
        char* headerBlock = reinterpret_cast<char*>(Object);
        //debug check
        if(config.DebugOn_)
        {   
            //check for valid address if the object is aligned
            // bool aligned = reinterpret_cast<uintptr_t>(Object) % stats.ObjectSize_ == 0;
            // if(!aligned)
            // {
            //     throw OAException(OAException::E_CORRUPTED_BLOCK, "Memory Block Corrupted\n");
            // }
            //range check
            char* page = reinterpret_cast<char*>(pageList);
            bool inRange = false;
            while(page && !inRange)
            {     
                inRange =  headerBlock >= page && static_cast<size_t>(headerBlock - page) < stats.PageSize_;
                page = reinterpret_cast<char*>(reinterpret_cast<GenericObject*>(page)->Next);
            }

            //bool doubleFree = *headerBlock == FREED_PATTERN;
            //check free flag
            bool doubleFree =  *(headerBlock + config.HBlockInfo_.size_ - 1) == freedFlag;
            if(doubleFree)
            {
               throw OAException(OAException::E_MULTIPLE_FREE, "Double Free Detected: Memory is already freed\n");
            }
            
            if(!inRange)
            {
                throw OAException(OAException::E_BAD_BOUNDARY, "Freeing out of range memory\n");
            }
            
            

            //char* dataBlock = headerBlock + config.HBlockInfo_.size_ + config.PadBytes_;
            memset(headerBlock,FREED_PATTERN,stats.ObjectSize_);

        }
        
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
                *(headerBlock + accumlateOffset) = 0x0;      
                
                break;
            }

            case OAConfig::HBLOCK_TYPE::hbExternal:
                break;
            case OAConfig::HBLOCK_TYPE::hbNone:
            default:
                break;
        }

        
        GenericObject* temp = reinterpret_cast<GenericObject*>(Object);
        temp->Next = freeList;
        freeList = temp;


        //stats
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