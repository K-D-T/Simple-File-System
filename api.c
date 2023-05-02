/*
    Implementation of API. 
    Treat the comments inside a function as friendly hints; but you are free to implement in your own way as long as it meets the functionality expectation. 
*/

#include "def.h"

pthread_mutex_t mutex_for_fs_stat;

//initialize file system - should be called as the first thing in application code
int RSFS_init(){

    //initialize data blocks
    for(int i=0; i<NUM_DBLOCKS; i++){
      void *block = malloc(BLOCK_SIZE); //a data block is allocated from memory
      if(block==NULL){
        printf("[sys_init] fails to init data_blocks\n");
        return -1;
      }
      data_blocks[i] = block;
    } 

    //initialize bitmaps
    for(int i=0; i<NUM_DBLOCKS; i++) data_bitmap[i]=0;
    pthread_mutex_init(&data_bitmap_mutex,NULL);
    for(int i=0; i<NUM_INODES; i++) inode_bitmap[i]=0;
    pthread_mutex_init(&inode_bitmap_mutex,NULL);    

    //initialize inodes
    for(int i=0; i<NUM_INODES; i++){
        inodes[i].length=0;
        for(int j=0; j<NUM_POINTER; j++) 
            inodes[i].block[j]=-1; //pointer value -1 means the pointer is not used
        
    }
    pthread_mutex_init(&inodes_mutex,NULL); 

    //initialize tracker
    for(int i = 0; i < NUM_DBLOCKS;i++)
	    bytes_track[i] = 0;
    //initialize open file table
    for(int i=0; i<NUM_OPEN_FILE; i++){
        struct open_file_entry entry=open_file_table[i];
        entry.used=0; //each entry is not used initially
        pthread_mutex_init(&entry.entry_mutex,NULL);
        entry.position=0;
        entry.access_flag=-1;
    }
    pthread_mutex_init(&open_file_table_mutex,NULL); 

    //initialize root directory
    root_dir.head = root_dir.tail = NULL;

    //initialize mutex_for_fs_stat
    pthread_mutex_init(&mutex_for_fs_stat,NULL);

    //return 0 means success
    return 0;
}


//create file with the provided file name
//if file does not exist, create the file and return 0;
//if file_name already exists, return -1; 
//otherwise, return -2.
int RSFS_create(char *file_name){

    //search root_dir for dir_entry matching provided file_name
    struct dir_entry *dir_entry = search_dir(file_name);

    if(dir_entry){//already exists
        printf("[create] file (%s) already exists.\n", file_name);
        return -1;
    }else{

        if(DEBUG) printf("[create] file (%s) does not exist.\n", file_name);

        //construct and insert a new dir_entry with given file_name
        dir_entry = insert_dir(file_name);
        if(DEBUG) printf("[create] insert a dir_entry with file_name:%s.\n", dir_entry->name);
        
        //access inode-bitmap to get a free inode 
        int inode_number = allocate_inode();
        if(inode_number<0){
            printf("[create] fail to allocate an inode.\n");
            return -2;
        } 
        if(DEBUG) printf("[create] allocate inode with number:%d.\n", inode_number);

        //save inode-number to dir-entry
        dir_entry->inode_number = inode_number;
        
        return 0;
    }
}



//open a file with RSFS_RDONLY or RSFS_RDWR flags
//return the file descriptor (i.e., the index of the open file entry in the open file table) if succeed, or -1 in case of error
int RSFS_open(char *file_name, int access_flag){

    //sanity test: access_flag should be either RSFS_RDONLY or RSFS_RDWR
    if(access_flag != RSFS_RDONLY && access_flag != RSFS_RDWR)
	   return -1; 
    //find dir_entry matching file_name
    struct dir_entry* dir_entry = search_dir(file_name);
    if(dir_entry == NULL)
	    return -1;
    //find the corresponding inode
    int i = dir_entry->inode_number; 
    if(i < 0)
	    return -1;
    //find an unused open-file-entry in open-file-table and use it
    int of = allocate_open_file_entry(access_flag,dir_entry);
    //return the file descriptor (i.e., the index of the open file entry in the open file table)
    return of;
}





//read from file: read up to size bytes from the current position of the file of descriptor fd to buf;
//read will not go beyond the end of the file; 
//return the number of bytes actually read if succeed, or -1 in case of error.
int RSFS_read(int fd, void *buf, int size){

    //sanity test of fd nd size
    if((fd < 0 || fd > 8) || size < 0)
	    return -1;
    //get the open file entry of fd
    struct open_file_entry* entry = &open_file_table[fd];
    //lock the open file entry
    pthread_mutex_lock(&entry->entry_mutex); //mutex to guard M.E. access to this entry
    //get the dir entry
    struct dir_entry* dir_entry = entry->dir_entry;
    //get the inode
    int inode = dir_entry->inode_number;

    //copy data from the data block(s) to buf and update current position
    int bytes_read=0, buf_pos=0, cur_pos = entry->position, 
	file_length = inodes[inode].length, cur_iblock = 0, bytes_left = 0, cur_loc = 0;
 

    //way past the point of return
    if(cur_pos >= file_length){
	    return 0;
    }

    //don't stop until the end of the file
    while(bytes_read < size && cur_pos < file_length){
	    cur_iblock = cur_pos / 32;// where we should point in block
	    cur_loc =  cur_pos % 32; // where we start
	    bytes_left = abs(cur_loc - (bytes_track[inodes[inode].block[cur_iblock]])); // finding how much should be there
	    
	    
	    //read bytes to a buffer, add up the additional spots 
	    memcpy(buf+buf_pos,data_blocks[inodes[inode].block[cur_iblock]]+cur_loc, bytes_left);
	    bytes_read+=bytes_left;
	    cur_pos+=bytes_left;
	    buf_pos+=bytes_left;
    }
    entry->position = cur_pos;

    //unlock the open file entry
    pthread_mutex_unlock(&entry->entry_mutex);
    //return the number of bytes
    return bytes_read;
}


//write file: write size bytes from buf to the file with fd
//return the number of bytes that have been written if succeed; return -1 in case of error
int RSFS_write(int fd, void *buf, int size){

    //sanity test of fd and size
    if((fd < 0 || fd > 8) || size < 0) 
	    return -1; 
    //get the open file entry
    struct open_file_entry* entry = &open_file_table[fd];
    //lock the open file entry
    pthread_mutex_lock(&entry->entry_mutex);     
    //get the dir entry
    struct dir_entry* dir_entry = entry->dir_entry;
    //get the inode
    int inode = dir_entry->inode_number; 
    //copy data from buf to the data block(s) and update current position;
    //new data blocks may be allocated for the file if needed
 
    int bytes_written = 0, buf_pos = 0,cur_bl = 0, inode_pos = entry->position, 
	block_ct = inode_pos / 32, cur = 0; 
    //variables for bytes written, buf position, and current block, bytes_track
    
    //choose the correct block with enough space
    for(cur = 0; cur < NUM_DBLOCKS; cur++){
    	if(bytes_track[cur] >=0 && bytes_track[cur] < 32 && find(inode,cur) != 2){
		cur_bl = cur;
		break;
	}
    } 
    //if there is space, let's place data  
    while(buf_pos < size && cur_bl < NUM_DBLOCKS){

	    //how many bytes should we add
	    int bytes = (size - buf_pos > BLOCK_SIZE) ? BLOCK_SIZE : size-buf_pos;
	    
	    //check the parallel buffer to see if we'll add twice or once
	    if(bytes + bytes_track[cur_bl] > 32){
	    	
		    //if there is overflow from over 32 spots
		    int bytes_to_write = abs(32 - bytes_track[cur_bl]);
		    int bytes_left_over = abs(bytes-bytes_track[cur_bl]);

		    //add bytes to buffer, update all variables
		    memcpy(data_blocks[cur_bl]+bytes_track[cur_bl],buf+buf_pos,bytes_to_write);// write just enough
		    bytes_track[cur_bl]+=bytes_to_write;
		    buf_pos+=bytes_to_write;

		    int i = 0;
		    //point where you should point
		    while(i < 8){
			    if(inodes[inode].block[i] < 0 && bytes_track[cur_bl] < 32){
				    inodes[inode].block[i] = cur_bl;
				    data_bitmap[cur_bl] = 1;
				    break;
			    }else if(find(inode,cur_bl) == 1 && inodes[inode].block[i] >= 0 &&
					    bytes_track[cur_bl] < 32){
				    data_bitmap[cur_bl] = 1;
				    break;
			    
			    }
			    i++;
		    }

		    //increment one block, push the remaining bytes to buffer
		    cur_bl++;
		    memcpy(data_blocks[cur_bl]+bytes_track[cur_bl],buf+buf_pos,bytes_left_over);

		    buf_pos+=bytes_left_over;
		    inodes[inode].length+=bytes;

		    //update remaining variables
		    bytes_track[cur_bl]+=bytes_left_over;
		    entry->position+=bytes;
		    bytes_written+=bytes;
	    }else{
		    //copy bit of data offset by the number of bytes at that location
		    memcpy(data_blocks[cur_bl]+bytes_track[cur_bl],buf+buf_pos,bytes);
		    
		    //update track of bytes at specific location
		    bytes_track[cur_bl]+=bytes;
		    buf_pos+=bytes; bytes_written+=bytes;
		    inodes[inode].length+=bytes;

		    int i = 0;
		    //run through blocks to update pointer with location to datablock section
		    while(i < 8){
			    //update bitmap when placing bytes into buffer
			    if(inodes[inode].block[i] < 0 && bytes_track[cur_bl] < 32){
				    inodes[inode].block[i] = cur_bl;
				    data_bitmap[cur_bl] = 1; 
				    break;
			    }
			    //update bitmap when placing bytes
			    else if(find(inode,cur_bl) == 1 && bytes_track[cur_bl] < 32 
					    && inodes[inode].block[i] >= 0){
				    data_bitmap[cur_bl] = 1;
				    inodes[inode].block[i] = cur_bl;
				    break;
			    }
			    i++;
		    }
		    entry->position+=bytes;
	    }
  }
    //unlock the open file entry
    pthread_mutex_unlock(&entry->entry_mutex);
    //
    return bytes_written;
}

//update current position: return the current position; if the position is not updated, return the original position
//if whence == RSFS_SEEK_SET, change the position to offset
//if whence == RSFS_SEEK_CUR, change the position to position+offset
//if whence == RSFS_SEEK_END, change hte position to END-OF-FILE-Position + offset
//position cannot be out of the file's range; otherwise, it is not updated
int RSFS_fseek(int fd, int offset, int whence){
    //sanity test of fd and whence    
    if((fd < 0 || fd > 8) || (whence < 0 || whence > 2))
	    return fd;
    //get the open file entry of fd
    struct open_file_entry* entry = &open_file_table[fd];
    //lock the entry
    pthread_mutex_lock(&entry->entry_mutex);
    //get the current position
    int pos = entry->position; 
    //get the dir entry
    struct dir_entry* dir_entry = entry->dir_entry;
    //get the inode 
    int cur_inode = dir_entry->inode_number;
    //change the position
    if(whence == RSFS_SEEK_SET)
	    pos = offset;
    else if(whence == RSFS_SEEK_CUR)
	    pos += offset;
    else 
	    pos = EOF + offset;
    entry->position = pos;
    //unlock the entry
    pthread_mutex_unlock(&entry->entry_mutex);
    //return the current position
    return pos;
}


//close file: return 0 if succeed, or -1 if fd is invalid
int RSFS_close(int fd){

    //sanity test of fd and whence    
    if(fd < 0 || fd > 8 )
	    return -1;
    //free the open file entry
    free_open_file_entry(fd); 
    return 0;
}


//delete file with provided file name: return 0 if succeed, or -1 in case of error
int RSFS_delete(char *file_name){

    //find the dir_entry; if find, continue, otherwise, return -1. 
    struct dir_entry* dir_entry = search_dir(file_name);
    if(dir_entry == NULL)
	   return -1; 
    //find the inode
    int inode = dir_entry->inode_number;
    

    //find the data blocks, free them in data-bitmap, also removed the count from bytes_track, setting it to zero
    for(int i = 0; i < NUM_INODES; i++){
	    bytes_track[inodes[inode].block[i]] = 0;
	    free_data_block(inodes[inode].block[i]); 
    }
   

    //free the inode in inode-bitmap
    free_inode(inode);     
    //free the dir_entry
    delete_dir(file_name);
    

    return 0;
}

//complicated way to check if there's a pointer to db_loc
int find(int cur_inode, int db_loc){
	for(int i = 0; i < NUM_INODES; i++)
		for(int j = 0; j < NUM_INODES; j++){
			if(i == cur_inode && inodes[i].block[j] == db_loc)
				return 1;
			if(i != cur_inode && inodes[i].block[j] == db_loc)
				return 2;
		}
	return 0;
}

//print status of the file system
void RSFS_stat(){

    pthread_mutex_lock(&mutex_for_fs_stat);

    printf("\nCurrent status of the file system:\n\n %16s%10s%10s\n", "File Name", "Length", "iNode #");

    //list files
    struct dir_entry *dir_entry = root_dir.head;
    while(dir_entry!=NULL){

        int inode_number = dir_entry->inode_number;
        struct inode *inode = &inodes[inode_number];
        
        printf("%16s%10d%10d\n", dir_entry->name, inode->length, inode_number);
        dir_entry = dir_entry->next;
    }
    
    //data blocks
    int db_used=0;
    for(int i=0; i<NUM_DBLOCKS; i++) db_used+=data_bitmap[i];
    printf("\nTotal Data Blocks: %4d,  Used: %d,  Unused: %d\n", NUM_DBLOCKS, db_used, NUM_DBLOCKS-db_used);

    //inodes
    int inodes_used=0;
    for(int i=0; i<NUM_INODES; i++) inodes_used+=inode_bitmap[i];
    printf("Total iNode Blocks: %3d,  Used: %d,  Unused: %d\n", NUM_INODES, inodes_used, NUM_INODES-inodes_used);

    //open files
    int of_num=0;
    for(int i=0; i<NUM_OPEN_FILE; i++) of_num+=open_file_table[i].used;
    printf("Total Opened Files: %3d\n\n", of_num);

    pthread_mutex_unlock(&mutex_for_fs_stat);
}
