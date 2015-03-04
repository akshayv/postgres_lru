a) A0074611M - Akshay Viswanathan
   A0091545A - Bathini Yathish

b) 
The LRU stack implementation used is actually a dynamically allocated array with the size of the array allocated during initialization (We use ShmemInitStruct for the array separately). The array pointer is part of the static BufferStrategyControl instance for convenience. This struct also contains the head and tail of the linked list (array). The array is of a newly created struct BufferElement which contains the index of the previous node and next node in the stack and the id of the current buffer. 

The stack usage with array in our case is a little counter-intuitive (since it is an array, we can technically access the array elements directly rather than scan through the “pointers” of the linked list; also the array index is the same as the buffer_id, but we save both anyway), but it is implemented in this way to keep as “true” to the in-memory linked list implementation as possible (in which case, access by index is not possible) which LRU uses. To achieve this, we use the prev and next members of the BufferElement object to navigate through the linked list.

The implementation of the retrieval of the LRU buffer is standard, move from the tail of the linked list to get the least recently used, and free buffer and move it to the top of the linked list (Instead of pointer manipulation, we manipulate the index stored in the prev, next, linkedListHead and linkedListTail members). 

Adding and removing a buffer from the linked list also follow a standard implementation but with array index manipulation rather that pointer manipulation.
  
We had attempted to use and dynamic in-memory linked with memory allocated upon requirement instead of the array but that implementation was far more complex if we did not rely on malloc or calloc and caused several “unknown” elements to be inserted into the linked list when we did use malloc. If we did use ShmemInitStruct for this linkedlist, we would need to either allocate all the elements during initialisation(which would make it very similar to the array) or we need to use ShmemInitStruct for each element upon requirement which would make it more complex; so we decided to move use a dynamically allocated array of fixed size. 


c)
In this case, the LRU algorithm outperforms the clock algorithm in terms of hit-ratio and in terms of average latency. 

Since LRU is better in terms of hit-ratio, it could mean that the benchmark dataset conforms to the principle of temporal locality, and a page that has been accessed once is accessed again. Since LRU only evicts pages that are relatively unused, the benchmark dataset will benefit from LRU. On the other hand, since clock evicts pages in a round robin manner, it could be that case that a page required by the benchmark dataset had been removed on many occasions.

The improved latency with LRU could also mean that, in addition to the fact that less buffers had to be re-inserted to the stack, the pages required by the benchmark dataset were close to the head of the stack. This means that the pages that were required, had been accessed very recently and were at the head of the stack during subsequent access. So, the traversal through the stack for LRU would be minimal but the clock sweep algorithm maybe slower in this respect. This cannot really be verified, as the effect of the improved latency only because of the higher hit-ratio cannot be independently determined.