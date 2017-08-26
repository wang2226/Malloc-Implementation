//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the allocator as indicated in the handout.
// 
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include "MyMalloc.h"

#define ALLOCATED 1
#define NOT_ALLOCATED 0
#define ARENA_SIZE 2097152

pthread_mutex_t mutex;

static bool verbose = false;

extern void atExitHandlerInC()
{
  if (verbose)
    print();
}

static void * getMemoryFromOS(size_t size)
{
  // Use sbrk() to get memory from OS
  _heapSize += size;
 
  void *mem = sbrk(size);

  if(!_initialized){
      _memStart = mem;
  }

  return mem;
}


/*
 * @brief retrieves a new 2MB chunk of memory from the OS
 * and adds "dummy" boundary tags
 * @param size of the request
 * @return a FreeObject pointer to the beginning of the chunk
 */
static FreeObject * getNewChunk(size_t size)
{
  void *mem = getMemoryFromOS(size);

  // establish fence posts
  BoundaryTag *fencePostHead = (BoundaryTag *)mem;
  setAllocated(fencePostHead, ALLOCATED);
  setSize(fencePostHead, 0);

  char *temp = (char *)mem + size - sizeof(BoundaryTag);
  BoundaryTag *fencePostFoot = (BoundaryTag *)temp;
  setAllocated(fencePostFoot, ALLOCATED);
  setSize(fencePostFoot, 0);
 
  return (FreeObject *)((char *)mem + sizeof(BoundaryTag));
}

/**
 * @brief If no blocks have been allocated, get more memory and 
 * set up the free list
 */
static void initialize()
{
  verbose = true;

  pthread_mutex_init(&mutex, NULL);

  // print statistics at exit
  atexit(atExitHandlerInC);

  FreeObject *firstChunk = getNewChunk(ARENA_SIZE);

  // initialize the list to point to the firstChunk
  _freeList = &_freeListSentinel;
  setSize(&firstChunk->boundary_tag, ARENA_SIZE - (2*sizeof(BoundaryTag))); // ~2MB
  firstChunk->boundary_tag._leftObjectSize = 0;
  setAllocated(&firstChunk->boundary_tag, NOT_ALLOCATED);

  // link list pointer hookups
  firstChunk->free_list_node._next = _freeList;
  firstChunk->free_list_node._prev = _freeList;
  _freeList->free_list_node._prev = firstChunk;
  _freeList->free_list_node._next = firstChunk;

  _initialized = 1;
}

/**
 * @brief TODO: PART 1
 * This function should perform allocation to the program appropriately,
 * giving pieces of memory that large enough to satisfy the request. 
 * Currently, it just sbrk's memory every time.
 *
 * @param size size of the request
 *
 * @return pointer to the first usable byte in memory for the requesting
 * program
 */
static void * allocateObject(size_t size)
{
  // Make sure that allocator is initialized
  if (!_initialized)
    initialize();

  //round up the requested size to the next 8 byte boundary
  size_t round_size = (size + 8 - 1) & ~(8 - 1);

  //add the size of the block's header
  size_t real_size = round_size + sizeof(BoundaryTag);

  FreeObject * p = _freeList->free_list_node._next;

  //flag to signal that the list doesn't have enought memory
  int flag = 0;

  //traverse the free list from the beginning
  while(p != _freeList){
  	flag = 0;	

	size_t obj_size = getSize(&(p->boundary_tag));

	//the block is not large enough to be split, simply remove the block from the list and return it
	if(obj_size >= real_size && obj_size < real_size + sizeof(BoundaryTag) + sizeof(FreeListNode) + 8){
		//set the last bit of _objectSizeAndAlloc
		setAllocated(&(p->boundary_tag),1);

		//remove the block from the list relink
		p->free_list_node._next->free_list_node._prev = p->free_list_node._prev;
		p->free_list_node._prev->free_list_node._next = p->free_list_node._next;

		break;
	}

	//the block needs to be split in two
	else if(obj_size >= real_size + sizeof(BoundaryTag) + sizeof(FreeListNode) + 8){
		//update the current block size
		setSize(&(p->boundary_tag),obj_size - real_size);

		//set a pointer to where to split
		char * temp = (char *)p + obj_size - real_size;

		//new boundary tag
		//update the size, left object size and allocated bit of newTag
		BoundaryTag * newChunk = (BoundaryTag *)temp;
		setSize(newChunk,real_size);
		newChunk->_leftObjectSize = getSize(&(p->boundary_tag);
		setAllocated(newChunk,1);
		
		//update newChunk's next's _leftObjSize
		temp = temp + real_size;
		BoundaryTag * rightHeader = (BoundaryTag *)temp;
		rightHeader->_leftObjectSize = getSize(newChunk);

		p = (FreeObject *)newChunk;
		break;
	}

	//the list doesn't have enough memory, request a new 2MB block, insert the block into the free list
	else {
		flag = 1;
	}

	//update the pointer
	p = p->free_list_node._next;
  }

  //handle the case that the list doesn/t have enough memory
  if(flag)
	  return getNewChunk(size);

  pthread_mutex_unlock(&mutex);
  return (void *)((BoundaryTag *)p + 1);
}

/**
 * @brief TODO: PART 2
 * This funtion takes a pointer to memory returned by the program, and
 * appropriately reinserts it back into the free list.
 * You will have to manage all coalescing as needed
 *
 * @param ptr
 */
static void freeObject(void *ptr)
{
  //p points to the start of the free list
  FreeObject * p = _freeList->free_list_node._next;

  int leftFree = 0;
  int rightFree = 0;

  //curr points to the current free object
  char * temp = (char *)ptr - sizeof(BoundaryTag);
  BoundaryTag * curr = (BoundaryTag *)temp;

  //left points to the left free object and set leftFree
  temp = temp - curr->_leftObjectSize;
  BoundaryTag * left = (BoundaryTag *)temp;

  if(!isAllocated(left)){
	  leftFree = 1;
  }

  //right points to the right free object and set rightFree
  temp = temp + getSize(left) + getSize(curr);
  BoundaryTag * right = (BoundaryTag *)temp;

  if(!isAllocated(right)){
	  rightFree = 1;
  }

  //merge both
  if(leftFree && rightFree){
	  size_t temp_size = getSize(left) + getSize(curr) + getSize(right);
	  setSize(left,temp_size);

	  //remove right from free list
	  while(p->free_list_node._next != (FreeObject *)right){
		  p = p->free_list_node._next;
	  }
	  p->free_list_node._next = ((FreeObject *)right)->free_list_node._next;
	  ((FreeObject *)right)->free_list_node._next->free_list_node._prev = p; 

	  //update right's next's _leftObjSize
	  char * temp = (char *)right + getSize(right);
	  BoundaryTag * right_right = (BoundaryTag *)temp;	
	  right_right->_leftObjectSize = getSize(left);
  }

  //merge with left
  else if(leftFree){
	  //update left object size
	  setSize(left,getSize(left) + getSize(curr));
	
	  //update right's left_object_size
	  right->_leftObjectSize = getSize(left);
  }

  //merge with right
  else if(rightFree){
	  //update the curr object size
	  setSize(curr, getSize(curr) + getSize(right));

	  //relink the free list
	  FreeObject * curr_obj = (FreeObject *) curr;
	  FreeObject * right_obj = (FreeObject *) right;

	  curr_obj->free_list_node._next = right_obj->free_list_node._next;
	  curr_obj->free_list_node._prev = right_obj->free_list_node._prev;
	  right_obj->free_list_node._prev->free_list_node._next = curr_obj;
	  right_obj->free_list_node._next->free_list_node._prev = curr_obj;
	  
	  //set the last bit of objectSizeAndAlloc
	  setAllocated(curr,0);

	  //update _leftObjectSize of the right's right
	  char * temp = (char *)right + getSize(right);
	  BoundaryTag * rightRight = (BoundaryTag *)temp;
	  rightRight->_leftObjectSize = getSize(curr);
  }

  //merge neither
  else {
	  //add the block to the head of the free list
	  ((FreeObject *)curr)->free_list_node._next = _freeList->free_list_node._next;
	  ((FreeObject *)curr)->free_list_node._prev = _freeList;
	  _freeList->free_list_node._next = (FreeObject *)curr;

	  //set the last bit of objectSizeAndAlloc
	  setAllocated(curr,0);
  }

  return;
}

void print()
{
  printf("\n-------------------\n");

  printf("HeapSize:\t%zd bytes\n", _heapSize);
  printf("# mallocs:\t%d\n", _mallocCalls);
  printf("# reallocs:\t%d\n", _reallocCalls);
  printf("# callocs:\t%d\n", _callocCalls);
  printf("# frees:\t%d\n", _freeCalls);

  printf("\n-------------------\n");
}

void print_list()
{
    printf("FreeList: ");
    if (!_initialized) 
        initialize();
    FreeObject *ptr = _freeList->free_list_node._next;
    while (ptr != _freeList) {
        long offset = (long)ptr - (long)_memStart;
        printf("[offset:%ld,size:%zd]", offset, getSize(&ptr->boundary_tag));
        ptr = ptr->free_list_node._next;
        if (ptr != NULL)
            printf("->");
    }
    printf("\n");
}

void increaseMallocCalls() { _mallocCalls++; }

void increaseReallocCalls() { _reallocCalls++; }

void increaseCallocCalls() { _callocCalls++; }

void increaseFreeCalls() { _freeCalls++; }

//
// C interface
//

extern void * malloc(size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseMallocCalls();
  
  return allocateObject(size);
}

extern void free(void *ptr)
{
  pthread_mutex_lock(&mutex);
  increaseFreeCalls();
  
  if (ptr == 0) {
    // No object to free
    pthread_mutex_unlock(&mutex);
    return;
  }
  
  freeObject(ptr);
}

extern void * realloc(void *ptr, size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseReallocCalls();

  // Allocate new object
  void *newptr = allocateObject(size);

  // Copy old object only if ptr != 0
  if (ptr != 0) {

    // copy only the minimum number of bytes
    FreeObject *o = (FreeObject *)((char *) ptr - sizeof(BoundaryTag));
    size_t sizeToCopy = getSize(&o->boundary_tag);
    if (sizeToCopy > size) {
      sizeToCopy = size;
    }

    memcpy(newptr, ptr, sizeToCopy);

    //Free old object
    freeObject(ptr);
  }

  return newptr;
}

extern void * calloc(size_t nelem, size_t elsize)
{
  pthread_mutex_lock(&mutex);
  increaseCallocCalls();
    
  // calloc allocates and initializes
  size_t size = nelem * elsize;

  void *ptr = allocateObject(size);

  if (ptr) {
    // No error
    // Initialize chunk with 0s
    memset(ptr, 0, size);
  }

  return ptr;
}

