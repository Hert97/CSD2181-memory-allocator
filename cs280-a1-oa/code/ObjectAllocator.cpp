/**
 * @file    ObjectAllocator.cpp
 * @author  Ho Yi Guan 
 *          SIT     : 2001595@sit.singaporetech.edu.sg 
 *          Digipen : Yiguan.ho@digipen.edu.sh
 * @brief   Contains the implementation for an ObjectAllocator that acts
 *          as a memory management system
 *           
 * @date    2022-01-21
 * 
 */

#include "ObjectAllocator.h"
#include <cstring>
#include <assert.h>

static constexpr size_t ptrSize = sizeof(void*);
static constexpr uint32_t freedFlag = 0x00u;
static constexpr uint32_t allocFlag = 0x01u;

// setters //
/**
 * @brief   Sets the debug state of the allocator
 * 
 * @param   State
 *          true    - enable error checking 
 *          false   - disable error checking
 */
void ObjectAllocator::SetDebugState(bool State) { config.DebugOn_ = State; }

// getters //
/**
 * @brief   Getter for the pointer to the internal free list
 * 
 * @return  The pointer to the internal free list
 */
const void* ObjectAllocator::GetFreeList() const { return freeList; } 

/**
 * @brief   Getter for the pointer to the internal page list
 * 
 * @return  The pointer to the internal page list
 */
const void* ObjectAllocator::GetPageList() const { return pageList; } 

/**
 * @brief   Getter to the allocator's configuration parameters 
 * 
 * @return  The structure to configuration parameters of the allocator 
 */
OAConfig ObjectAllocator::GetConfig() const { return config; }        

/**
 * @brief 
 * 
 * @return  OAStats 
 */
OAStats ObjectAllocator::GetStats() const { return stats; }           // returns the statistics for the allocator




/**
 * @brief   Prints the addresses sequence of a linked list used for debugging
 *            
 * @param   label
 *          A header label before the linklist print out 
 * @param   list
 *          The linklist to output
 */
// static void PrintList(const char* label, GenericObject* list)
// {
// 	printf("%s\n", label);
// 	uint32_t counter = 0;
// 	while (list)
// 	{
// 		++counter;
// 		printf("E%u:%p\n", counter, static_cast<void*>(list));
// 		list = list->Next;
// 	}
// 	if (!list)
// 		printf("nullptr\n\n");
// }

/**
 * @brief   Helper function to write memory pattern to a block on memory
 * 
 * @param   object
 *          The ptr to the starting block to write
 * @param   byteSize 
 *          Number of bytes to write to param object
 * @param   pattern
 *          Pattern to write to block
 *          
 */
static void WritePatternToBlock(void* object, size_t byteSize, uint8_t pattern)
{

    uint8_t* memStart = reinterpret_cast<uint8_t*>(object);

    if (byteSize == 0 || !memStart)
        return;

    memset(memStart, pattern, byteSize);

}

/**
 * @brief   Helper function to update allocation stats
 *          for ObjectAllocator::Allocate function
 * 
 * @param   stats 
 *          The stats object to update
 */
static void UpdateAllocationStats(OAStats& stats)
{
    //update stats
    ++stats.Allocations_;
    ++stats.ObjectsInUse_;
    stats.MostObjects_ = std::max(stats.MostObjects_, stats.ObjectsInUse_);
    --stats.FreeObjects_;
}

/**
 * @brief   Helper function to update deallocation stats
 *          for ObjectAllocator::Free function
 * 
 * @param   stats 
 *          The stats object to update
 */
static void UpdateDeallocationStats(OAStats& stats)
{
    //update stats
    ++stats.FreeObjects_;
    ++stats.Deallocations_;
    --stats.ObjectsInUse_;
}


/**
 * @brief   Construct a new Object Allocator
 * 
 * @param   ObjectSize
 *          The allocation's object size in bytes
 * 
 * @param   configuration
 *          A structure of configurations to apply 
 *          for allocator
 */
ObjectAllocator::ObjectAllocator(size_t ObjectSize, const OAConfig& configuration)
    : pageList{ nullptr }, freeList{ nullptr }, config{ configuration }, stats{}
{

    //calculate alignments
    if (config.Alignment_ > 0)
    {
        size_t leftAlignSize = ptrSize + config.HBlockInfo_.size_ + config.PadBytes_;
        //is leftAlignSize is a multiples of Alignment
        unsigned int remainder = static_cast<unsigned>(leftAlignSize % config.Alignment_);

        if (remainder != 0) //if not calculate how many bytes more needed
            config.LeftAlignSize_ = config.Alignment_ - remainder;

        size_t interAlignSize = ObjectSize + config.HBlockInfo_.size_ + static_cast<size_t>(config.PadBytes_ * 2u);
        //is interAlignSize is a multiples of Alignment
        remainder = static_cast<unsigned>(interAlignSize % config.Alignment_);

        if (remainder != 0) //if not calculate how many bytes more needed
            config.InterAlignSize_ = config.Alignment_ - remainder;
    }

    size_t totalBlockSize = config.ObjectsPerPage_ * (config.HBlockInfo_.size_ + config.PadBytes_ + ObjectSize + config.PadBytes_ + config.InterAlignSize_);
    size_t allocateSize = ptrSize + config.LeftAlignSize_ + totalBlockSize - config.InterAlignSize_;

    stats.ObjectSize_ = ObjectSize;
    stats.MostObjects_ = 0;
    stats.PageSize_ = allocateSize;

    CreatePage();
}

/**
 * @brief   Destructor of the Object Allocator
 * 
 */
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

/**
 * @brief   Creates a new free page in allocator
 *          
 */
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

/**
 * @brief   Allocate space for a object using memory from the allocator
 *          unless override by config.UseCPPMemManager_ flag
 * 
 * @param   label 
 *          The label for external header if in use
 * 
 * @return  A ptr to given memoryblock
 *          
 */
void* ObjectAllocator::Allocate(const char* label)
{

    if (config.UseCPPMemManager_)
    {
        //update stats
        UpdateAllocationStats(stats);
        return new uint8_t[stats.ObjectSize_];
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
        AllocateExternalHeader(headerBlock, label);
    }
    //update headers info if any
    UpdateHeaderInfo(headerBlock, allocFlag);


    return freeBlock;
}

/**
 * @brief   Free an object memory allocated by the allocator
 * 
 * @param   Object
 *          The object memory pointer
 */
void ObjectAllocator::Free(void* Object)
{
    uint8_t* objBlock = reinterpret_cast<uint8_t*>(Object);

    if (config.UseCPPMemManager_)
    {
        //update stats
        UpdateDeallocationStats(stats);
        delete[] objBlock;
        return;
    }
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
            if (!IsValidAlignment(objBlock, pageFound))
            {
                throw OAException(OAException::E_BAD_BOUNDARY, "Freeing: Valid alignment\n");
            }
            //check corruption
            if (IsPaddingCorrupted(objBlock))
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

/**
 * @brief   Helper function to allocate memory & copy string buffer 
 *          for external header type configuration
 * 
 * @param   objBlock 
 *          The pointer to the start of object data block 
 *          (after the memory to external header pointer)
 * 
 * @param   label 
 *          The string ptr for the external header's label
 */
void ObjectAllocator::AllocateExternalHeader(uint8_t* objBlock, const char* label)
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
    //ensure null teminated if label is nullptr
    infoBlock->label[0] = 0;
    //copy text label
    if (len > 0)
        strcpy(infoBlock->label,label);
    //copy external header address to block
    MemBlockInfo** tempHeaderPtr = reinterpret_cast<MemBlockInfo**>(headerStart);
    *tempHeaderPtr = infoBlock;

}

/**
 * @brief   Helper function to free memory allocated by 
 *          ObjectAllocator::AllocateExternalHeader function
 *          Also reseting the externalheader pointer in the before 
 *          objBlock
 * 
 * @param   objBlock 
 *          The pointer to the start of object data block 
 *          (after the memory to external header pointer)
 */
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

/**
 * @brief   Helper function to update header information
 *          depending on the configuration
 * 
 * @param   objBlock 
 *          The pointer to the start of object data block 
 * 
 * @param   flag 
 *          a flag to indicate the 2 possible calling function
 *          either allocate/deallocate
 */
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
            ++* useCounter;
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

/**
 * @brief   Finds the page a given object memory block resides
 *          
 * @param   objBlock 
 *          The pointer to the start of object data block 
 * 
 * @return  A ptr to the found page else
 *          nullptr to indicate not found
 */
GenericObject* ObjectAllocator::FindPage(uint8_t* objBlock) const
{
    // range check
    uint8_t* page = reinterpret_cast<uint8_t*>(pageList);

    //loop through all pages to check if block is within valid address range
    while (page)
    {
        uint8_t* pageEnd = page + stats.PageSize_;

        if (objBlock >= page && objBlock < pageEnd) //found page
        {
            return reinterpret_cast<GenericObject*>(page);
        }

        page = reinterpret_cast<uint8_t*>(reinterpret_cast<GenericObject*>(page)->Next);
    }

    return nullptr;
}

/**
 * @brief   Enquire whether an objectblock is memory 
 *          that has been previously freed
 *          
 * @param   objBlock
 *          The pointer to the start of object data block 
 * 
 * @return  true    - memory has been freed previously
 * @return  false   - memory has not been freed previously
 */
bool ObjectAllocator::IsMemoryFreed(uint8_t* objBlock) const
{
    GenericObject* currBlock = freeList;
    GenericObject* obj = reinterpret_cast<GenericObject*>(objBlock);
    //scan through freeList
    while (currBlock)
    {
        if (obj == currBlock) // aldy exist on freeList
            return true;
        currBlock = currBlock->Next;
    }
    return false;

}

/**
 * @brief   Enquire whether an objectblock is aligned 
 *          to each other
 * 
 * @param   objBlock 
 *          The pointer to the start of object data block 
 * 
 * @param   pageLocation 
 *          The page the param objBlock resides in
 *          
 * @return  true    - Objectblocks are aligned
 * @return  false   - Objectblocks are not aligned
 */
bool ObjectAllocator::IsValidAlignment(uint8_t* objBlock, GenericObject* pageLocation) const
{
    if (!pageLocation)
        return false;

    //page first data block section
    uint8_t* dataBlockStart =
        reinterpret_cast<uint8_t*>(pageLocation)
        + ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;

    //next object's advancement size
    size_t objAdvSize = stats.ObjectSize_
        + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;

    size_t actualSize = objBlock - dataBlockStart;

    return actualSize % objAdvSize == 0;
}

/**
 * @brief   Enquire whether an objectblock's padding has been corrupted
 * 
 * @param   objBlock 
 *          The pointer to the start of object data block 
 * 
 * @return  true    - block corrupted
 * @return  false   - block is not corrupted
 */
bool ObjectAllocator::IsPaddingCorrupted(uint8_t* objBlock) const
{
    if (config.PadBytes_ == 0) //early out
        return false;
    uint8_t* padStart = objBlock - config.PadBytes_;
    uint8_t* padEnd = padStart + stats.ObjectSize_ + config.PadBytes_;

    for (size_t i = 0; i < config.PadBytes_; ++i)
    {
        if (padStart[i] != PAD_PATTERN)
            return true;
        if (padEnd[i] != PAD_PATTERN)
            return true;
    }
    return false;
}

/**
 * @brief   Enquire whether an objectblock is allocated to client(in-use)
 *              
 * @param   objBlock 
 *          The pointer to the start of object data block
 * 
 * @return  true    - In use
 * @return  false   - Not in use
 */
bool ObjectAllocator::IsObjectBlockInUse(uint8_t* objBlock) const
{
    bool usedFlag = false;
    //read header info if available
    if (config.HBlockInfo_.type_ != OAConfig::hbNone)
    {
        uint8_t* headerStart = objBlock - config.PadBytes_ - config.HBlockInfo_.size_;

        switch (config.HBlockInfo_.type_)
        {
        case OAConfig::HBLOCK_TYPE::hbBasic:
        {
            headerStart += sizeof(uint32_t);
            //flag bit
            usedFlag = *headerStart;
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
        usedFlag = !IsMemoryFreed(objBlock);
    }
    return usedFlag;
}

/**
 * @brief   Dumps info on all memory in use
 *          
 * @param   fn 
 *          Function callback if memory is in use
 * 
 * @return  Number of memory blocks in use 
 */
unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const
{
    unsigned int counter = 0;

    GenericObject* page = pageList;
    size_t pageToDataAdv
        = ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;

    //offset to next obj
    const size_t objAdvancement =
        stats.ObjectSize_ + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;

    while (page)
    {
        uint8_t* objData = reinterpret_cast<uint8_t*>(page) + pageToDataAdv;
        //loop per object in page
        for (size_t i = 0; i < config.ObjectsPerPage_; ++i)
        {
            //block is in use
            if (IsObjectBlockInUse(objData))
            {
                fn(objData, stats.ObjectSize_);
                ++counter;
            }

            //if is last object on page dont add alignment as it contains interalignment
            if (i != config.ObjectsPerPage_ - 1)
            {
                objData += objAdvancement;
            }

        }
        page = page->Next;
    }
    return counter;
}

/**
 * @brief   Validates all object blocks in allocator 
 *          for potential corruption blocks  
 * 
 * @param   fn 
 *          Function callback for each block that is potentially corrupted
 * 
 * @return  Number of potentially corrupted block 
 */
unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const
{
    unsigned int counter = 0;

    if (config.PadBytes_ > 0)
    {
        GenericObject* page = pageList;
        size_t pageToDataAdv
            = ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;
        //offset to next obj
        const size_t objAdvancement =
            stats.ObjectSize_ + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;

        while (page)
        {
            uint8_t* objData = reinterpret_cast<uint8_t*>(page) + pageToDataAdv;

            for (size_t i = 0; i < config.ObjectsPerPage_; ++i)
            {
                if (IsPaddingCorrupted(objData))
                {
                    fn(objData, stats.ObjectSize_);
                    ++counter;
                }
                //if is last object on page dont add alignment as it contains interalignment
                if (i != config.ObjectsPerPage_ - 1)
                {
                    objData += objAdvancement;
                }

            }
            page = page->Next;
        }
    }
    return counter;
}

/**
 * @brief   Helper function to free a page in the allocator
 *          
 * @param   currPage 
 *          The page to free
 * @param   prevPage 
 *          The previous page before the param currPage
 */
void ObjectAllocator::FreePage(GenericObject*& currPage, GenericObject* prevPage )
{
	if (!currPage)
		return;

	uint8_t* rawMem = reinterpret_cast<uint8_t*>(currPage);
	uint8_t* objMem = rawMem + ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;
	if (config.HBlockInfo_.type_ == OAConfig::hbExternal)
	{
		FreeExternalHeader(objMem);
	}
	const size_t objAdvSize = stats.ObjectSize_ + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;

	GenericObject* currFreeBlock = freeList;
	GenericObject* prevFreeBlock = nullptr;
	//PrintList("List:", freeList);
	uint32_t objFreed = 0;
	while (currFreeBlock)
	{
		//serach in page for address
		for (size_t i = 0; i < config.ObjectsPerPage_; ++i)
		{
			if (currFreeBlock == reinterpret_cast<GenericObject*>(objMem))
			{
				if (prevFreeBlock)// not head
				{
					prevFreeBlock->Next = currFreeBlock->Next;
				}
				else
				{
					freeList = currFreeBlock->Next;
					currFreeBlock = freeList;
				}
				++objFreed;
			}
			if (i != config.ObjectsPerPage_ - 1)
				objMem += objAdvSize;
		}
		if (objFreed == config.ObjectsPerPage_) //early out
			break;

		prevFreeBlock = currFreeBlock;
		if (currFreeBlock)
			currFreeBlock = currFreeBlock->Next;
	}


	if (prevPage)// not head
	{
		prevPage->Next = currPage->Next;
		//update the currPage in freeemptypage function
		currPage = currPage->Next;
	}
	else
	{
		pageList = currPage->Next;
		//update the currPage in freeemptypage function
		currPage = pageList;
	}

	//update stats
	--stats.PagesInUse_;
	stats.PagesInUse_ = std::max(0u, stats.PagesInUse_);
	stats.FreeObjects_ -= config.ObjectsPerPage_;
	stats.FreeObjects_ = std::max(0u, stats.FreeObjects_);

}

/**
 * @brief   Free all empty pages in the allocator
 * 
 * @return  Number of freed pages 
 */
unsigned ObjectAllocator::FreeEmptyPages()
{
	unsigned int counter = 0;

	GenericObject* currPage = pageList;
	GenericObject* prevPage = nullptr;

	size_t pageToDataOffset
		= ptrSize + config.LeftAlignSize_ + config.HBlockInfo_.size_ + config.PadBytes_;

	//offset to next obj
	const size_t objAdvancement =
		stats.ObjectSize_ + config.PadBytes_ * 2 + config.InterAlignSize_ + config.HBlockInfo_.size_;
	uint8_t** memQueue = nullptr;
	
	try
	{
		memQueue = new uint8_t*[stats.PagesInUse_];
	}
	catch (const std::bad_alloc&)
	{
		throw OAException(OAException::E_NO_MEMORY, "Failed to create memory queue: No system memory available.");
	}


	while (currPage)
	{
		bool updatePage = true;
		uint8_t* objData = reinterpret_cast<uint8_t*>(currPage) + pageToDataOffset;
		uint32_t objectsIsUse = 0;
		for (size_t i = 0; i < config.ObjectsPerPage_; ++i)
		{
			if (IsObjectBlockInUse(objData))
			{
				++objectsIsUse;
			}
			//if is last object on page dont add alignment as it contains interalignment
			if (i != config.ObjectsPerPage_ - 1)
			{
				objData += objAdvancement;
			}

		}
		//no objects in page is used
		if (objectsIsUse == 0)
		{
			memQueue[counter] = reinterpret_cast<uint8_t*>(currPage);
			FreePage(currPage, prevPage);	
			++counter;
			updatePage = false; //skip update
		}
		if (updatePage)
		{
			prevPage = currPage;
			if (currPage)
				currPage = currPage->Next;
		}
		
	}
	//free everything at end
	for (size_t i = 0; i < counter; ++i)
	{
		delete[] memQueue[i];
	}
	delete[] memQueue;
	return counter;
}