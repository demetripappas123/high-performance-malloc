#include "allocator.h"
#include "unistd.h"

#define MAGIC 0x12345678

typedef struct block_header{
    size_t size;
    int free;
    int magic;
    struct block_header* next;
    struct block_header* prev;
} block_header;

block_header* head = NULL;

block_header* request_space(block_header* last, size_t size){
  block_header* block = sbrk(sizeof(block_header)+size);
  if(block == (void* -1)){
    return NULL;
  }
  
  block -> size = size;
  block -> next = NULL;
  block -> free = 0;
  block -> magic = MAGIC;

  if(last){
    block -> prev = last;
    last -> next = block;
  } else {
    head = block;
    block -> prev = NULL;
  }

  return block;
}

block_header* find_free_block(size_t size, block_header** last){
    block_header* curr = head;
    while (curr){
        if(curr -> size >= size && curr -> free == 1){
            return curr;
        }
        else{
            *last = curr;
            curr = curr -> next;
        }
    }
    return NULL;
}

void* my_malloc(size_t size){
    if(size == 0){
        return NULL;
    }

    block_header* block;
    block_header* last = NULL;

    block = find_free_block(size, &last);

    if(!block){
        block = request_space(last, size);
        if(!block){
            return NULL;
        }
    } else {
        block -> free = 0;
        if(block -> size >= (size + sizeof(block_header) + 1)){
            block_header* next = (block_header*)((char*)(block + 1) + size);
            next -> free = 1;
            next -> size = block -> size - size - sizeof(blockheader);
            block -> size = size;
            next -> magic = MAGIC;

            next -> prev = block;
            next -> next = block -> next;
            if(next -> next){
                next -> next -> prev = next;
            }
            block -> next = next;
        }
    }

    return (block + 1);
}


void my_free(void* ptr){
    block_header* block = NULL;
    block = (block_header*)ptr - 1;
    block -> free = 1;

    if(block -> prev && block -> prev -> free){
        block -> prev -> size += (block -> size + sizeof(block_header));
        block -> prev -> next = block -> next;
        if(block -> next){
            block -> next -> prev = block -> prev;
        }
        block = block -> prev;
    }
    if(block -> next && block -> next -> free){
        block -> size += (block -> next -> size + sizeof(block_header));
        block -> next = block -> next -> next;
        if(block -> next -> next){ 
           block -> next -> prev = block;
        }
    }
}