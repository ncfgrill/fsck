#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

// The entirety of the fs.h xv6 header file has been copied into this source
// file for portability purposes. All variables, structs, and macros defined
// within the fs.h file are credited to their respective authors.
//
// Additional variables and macros defined below the dirent struct are my own.

// Block 0 is unused.
// Block 1 is super block.
// Inodes start at block 2.

#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// File system super block
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
};

#define NDIRECT (12)
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses
};

#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Special device

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i)     ((i) / IPB + 2)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block containing bit for block b
#define BBLOCK(b, ninodes) (b/BPB + (ninodes)/IPB + 3)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

// Dirents per block
#define DPB (BSIZE/sizeof(struct dirent))

#define CHECKBIT(bm, b_addr) (((*(bm + b_addr / 8)) & (1 << (b_addr % 8))) > 0)

int *used_datablocks;
int *in_use_inums;

// check #1
int check_valid_inodes(int type) {
  switch(type) {
    case T_DIR :
      break;
    case T_FILE :
      break;
    case T_DEV :
      break;
    default : // non-standard type, error
      return -1;
  }
  return 0;
}

// check #2A
int check_valid_direct(struct dinode *node, int size) {
  uint b_addr;
  for (int i = 0; i < NDIRECT; i++) { // loop through all direct blocks
    b_addr = node->addrs[i];
    if (b_addr == 0) // address not in use
      continue;

    // address outside of possible address space, error
    if ((b_addr < 0) || (b_addr >= size))
      return -1;
  }
  return 0;
}

// check #2B
int check_valid_indirect(void *mem_start, struct dinode *node, int size) {
  uint b_addr = node->addrs[NDIRECT];
  if (b_addr == 0) // address not in use
    return 0;
  // address outside of possible address space, error
  else if ((b_addr < 0) || (b_addr >= size))
    return -1;

  uint *i_block = (uint *) (mem_start + b_addr*BSIZE);
  // loop through indirect blocks
  for (int i = 0; i < NINDIRECT; i++, i_block++) {
    b_addr = *(i_block);
    if (b_addr == 0) // address not in use
      continue;

    // address outside of possible address space, error
    if ((b_addr < 0) || (b_addr >= size))
      return -1;
  }

  return 0;
}

// check #3 and #4
int check_valid_dir(void *mem_start, struct dinode *node, int inum) {
  uint b_addr;
  struct dirent *d_entry;
  int cd, pd; // used for tracking current directory and parent directory
  cd = pd = 0;

  // loop through all direct blocks
  for (int i = 0; i < NDIRECT; i++) {
    b_addr = node->addrs[i];
    if (b_addr == 0) // address not in use
      continue;

    d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
    // loop through all dirents in block
    for (int j = 0; j < DPB; j++, d_entry++) {
      if (strcmp(d_entry->name, ".") == 0) { // found current directory
        cd = 1;
        if (d_entry->inum != inum) // cd not properly numbered, error
	  return -1;
      }
    
      if (strcmp(d_entry->name, "..") == 0) { // found parent directory
        pd = 1;
	if (inum != 1) { // not in root directory
          // if not found current directory and
	  // parent directory not properly numbered, error
          if (!cd && (d_entry->inum != inum))
	    return -1;
	} else { // in root directory
          if (d_entry->inum != inum) // rd not properly numbered, error
	    return -1;
	}
      }

      if (cd && pd) // found both current and root directories, success
        return 0;
    }
  }
  return -1; // current and/or parent directory not found, error
}

// check #5
int check_valid_bitmap(void *mem_start,
		       struct dinode *node,
		       int ninodes,
		       int inum) {
  
  int b = BBLOCK(0, ninodes); // find block containing initial inode
  char *bm = (char *) (mem_start + b*BSIZE); // bitmap
  uint b_addr;

  // loop through all address blocks (direct and indirect) in each inode
  for (int i = 0; i <= NDIRECT; i++) {
    b_addr = node->addrs[i];
    if (b_addr == 0) // address not in use
      continue;
    if (i == NDIRECT) { // reached indirect block
      uint *i_block = (uint *) (mem_start + b_addr*BSIZE);
      // loop through all indirect blocks
      for (int j = 0; j < NINDIRECT; j++, i_block++) {
        b_addr = *i_block;
	if (b_addr == 0) // address not in use
          continue;
	// if inode in use but marked free in bitmap, error
	if (!CHECKBIT(bm, b_addr)) {
	  return -1;
	}
      }
      // if inode in use but marked free in bitmap, error
    } else if (!CHECKBIT(bm, b_addr)) {
      return -1;
    }
  }
  return 0;
}

// helper for check #6
void find_used_datablocks(void *mem_start, struct dinode *dip, int ninodes) {
  uint b_addr, *i_block;

  // loop through all address blocks (direct and indirect) in each inode
  for (int i = 0; i <= NDIRECT; i++) {
    b_addr = dip->addrs[i];
    if (b_addr == 0) // address not in use
      continue;

    used_datablocks[b_addr] = 1; // mark block as in use

    if (i == NDIRECT) { // reached indirect block
      i_block = (uint *) (mem_start + b_addr*BSIZE);

      // loop through all indirect blocks
      for (int j = 0; j < NINDIRECT; j++, i_block++) {
        b_addr = *i_block;
	if (b_addr == 0) // address not in use
	  continue;

	used_datablocks[b_addr] = 1; // mark block as in use
      }
    }
  }
}

// check #6
int check_valid_blocks_in_bitmap(void *mem_start,
				 struct superblock *sb,
				 uint db1) {
  int i;
  struct dinode *dip = (struct dinode *) (mem_start + 2*BSIZE);
  int b = BBLOCK(0, sb->ninodes); // find block containing initial inode
  char *bm = (char *) (mem_start + b*BSIZE); // bitmap
  uint b_addr;

  // loop through all inodes
  for (i = 0; i < sb->ninodes; i++, dip++) {
    if (dip->type == 0) // inodes not in use
      continue;

    find_used_datablocks(mem_start, dip, sb->ninodes);
  }

  // loop through all data blocks, starting with first data block (db1)
  for (i = db1; i < sb->nblocks; i++) {
    b_addr = i;
    // if data block not in use but marked in use in bitmap, error
    if (used_datablocks[i] == 0 && CHECKBIT(bm, b_addr))
      return -1;
  }
  
  return 0;
}

// check #7
int check_direct_addr_use(void *mem_start, int ninodes) {
  struct dinode *dip = (struct dinode *) (mem_start + 2*BSIZE);
  uint addr;
  uint *used_addrs;
  int i, j, k, total;
  k = total = 0;

  if ((used_addrs = malloc(sizeof(uint)*ninodes*NDIRECT)) == NULL)
    exit(1);

  // loop through all inodes
  for (i = 0; i < ninodes; i++, dip++) {
    if (dip->type == 0) // inode not in use
      continue;

    // loop through all direct blocks
    for (j = 0; j < NDIRECT; j++) {
      addr = dip->addrs[j];
      if (addr == 0) // address not in use
	continue;

      k = 0;
      while (k < total) { // loop through all found addresses
        if (used_addrs[k] == addr) { // found repeat address, error
	  free(used_addrs);
	  return -1;
	}
	k++;
      }
      used_addrs[k] = addr; // add new address
      total++; // increment total addresses found
    }
  }

  free(used_addrs);
  return 0;
}

// check #8
int check_indirect_addr_use(void *mem_start, int ninodes) {
  struct dinode *dip = (struct dinode *) (mem_start + 2*BSIZE);
  uint addr;
  uint *used_addrs;
  int i, j, k, total;
  k = total = 0;

  if ((used_addrs = malloc(sizeof(uint)*ninodes*NINDIRECT)) == NULL)
    exit(1);

  // loop through all inodes
  for (i = 0; i < ninodes; i++, dip++) {
    if (dip->type == 0) // inode not in use
      continue;

    addr = dip->addrs[NDIRECT];
    if (addr == 0) // address not in use
      continue;

    uint *i_block = (uint *) (mem_start + addr*BSIZE);
    // loop through all indrect blocks
    for (j = 0; j < NINDIRECT; j++, i_block++) {
      addr = *i_block;
      if (addr == 0) // address not in use
	continue;

      k = 0;
      while (k < total) { // loop through all found addresses
        if (used_addrs[k] == addr) { // found repeat address, error
	  free(used_addrs);
	  return -1;
	}
	k++;
      }
      used_addrs[k] = addr; // add new address
      total++; // increment total addresses found
    }
  }

  free(used_addrs);
  return 0;
}

// helper method for checks #9-12
void get_inode_info(void *mem_start, int ninodes) {
  struct dinode *dip = (struct dinode *) (mem_start + 2*BSIZE);
  struct dirent *d_entry;
  uint b_addr, *i_block;
  int i, j, k;

  // loop through all inodes
  for (i = 0; i < ninodes; i++, dip++) {
    if (dip->type == T_DIR) {
      // loop through all direct blocks
      for (j = 0; j < NDIRECT; j++) {
        b_addr = dip->addrs[j];
	if (b_addr == 0) // address not in use
	  continue;

        d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
	// loop through all dirents in current block
	for (k = 0; k < DPB; k++, d_entry++) {
          if ((strcmp(d_entry->name, ".") == 0) ||
	      (strcmp(d_entry->name, "..") == 0))
            continue;
          in_use_inums[d_entry->inum]++; // increment current inum count
	}
      }
      b_addr = dip->addrs[j]; // address of indirect block
      if (b_addr == 0) // address not in use
	continue;

      i_block = (uint *) (mem_start + b_addr*BSIZE);
      // loop through all indirect blocks
      for (j = 0; j < NINDIRECT; j++, i_block++) {
        b_addr = *i_block;
        if (b_addr == 0) // address not in use
	  continue;
        
	d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
	// loop through all dirents in current block
	for (k = 0; k < DPB; k++, d_entry++) {
          if ((strcmp(d_entry->name, ".") == 0) ||
	      (strcmp(d_entry->name, "..") == 0))
            continue;
          in_use_inums[d_entry->inum]++; // increment current inum count
	}
      }
    }
  }
}

// extra check #1
int check_parent_dir(void *mem_start, int ninodes) {
  struct dinode *node = (struct dinode *) (mem_start + 2*BSIZE);
  struct dirent *d_entry;
  uint b_addr;
  int i, j, k, n, found;

  // reuse mem to track dir inums
  int index = 0;
  free(in_use_inums);
  if ((in_use_inums = calloc(ninodes, sizeof(int))) == NULL)
    exit(1);

  // loop through all inodes
  for (i = 0; i < ninodes; i++, node++) {
    if (node->type != T_DIR)
      continue;

    // look through all blocks (direct and indirect) of current inode
    for (j = 0; j <= NDIRECT; j++) {
      b_addr = node->addrs[j];
      if (b_addr == 0) // address not in use
	continue;

      d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
      // loop through all dirents in current block
      for (k = 0; k < DPB; k++, d_entry++) {
        if (strcmp(d_entry->name, ".") == 0) {
          in_use_inums[d_entry->inum] = d_entry->inum;
	  index++;
	}
      }
    }
  } 
  
  node = (struct dinode *) (mem_start + 2*BSIZE);

  // loop through all inodes
  for (i = 0; i < ninodes; i++, node++) {
    if (node->type != T_DIR)
      continue;

    // look through all blocks (direct and indirect) of current inode
    for (j = 0; j <= NDIRECT; j++) {
      b_addr = node->addrs[j];
      if (b_addr == 0) // address not in use
	continue;

      d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
      // loop through all dirents in current block
      for (k = 0; k < DPB; k++, d_entry++) {
        if (strcmp(d_entry->name, "..") == 0) {
	  found = 0;
	  n = 0;
	  while (n < ninodes) {
            //m = in_use_inums[n];
	    if (in_use_inums[n] == 0) {
	      n++;
	      continue;
	    }
            if (d_entry->inum == n) {
	      found = 1;
	      break;
	    }
            n++;
	  }
          if (!found)
	    return -1;
	}
      }
    }
  }
  return 0;
}

// helper for extra check #2
int recurse_dir(void *mem_start, struct dinode *node, int *circle) {
  struct dinode *new_node;
  uint b_addr;
  struct dirent *d_entry;
  int i, j, k, check, size;
  size = sizeof(struct dinode);

  for (i = 0; i < NDIRECT; i++) {
    b_addr = node->addrs[i];
    if (b_addr == 0)
      continue;

    d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
    for (j = 0; j < DPB; j++, d_entry++) {
      if (d_entry->inum == 0)
        continue;

      if (strcmp(d_entry->name, "..") != 0)
        continue;

      if (d_entry->inum == 1)
	continue;

      k = 0;
      while (circle[k] != 0) {
        if (d_entry->inum == circle[k])
	  return -1;
	k++;
      }
      circle[k] = d_entry->inum;

      new_node =
	(struct dinode *) (mem_start + 2*BSIZE + (d_entry->inum)*size);
      check = recurse_dir(mem_start, new_node, circle);
      
      if (check == -1)
	return -1;
    }
  }
  return 0;
}

// extra check #2
int check_no_loops(void *mem_start, int ninodes) {
  struct dinode *node = (struct dinode *) (mem_start + 2*BSIZE);
  int *dir_circle;
  dir_circle = calloc(ninodes, sizeof(int));
  int i, check;

  for (i = 0; i < ninodes; i++, node++) {
    dir_circle = calloc(ninodes, sizeof(int));
    if (node->type != T_DIR)
      continue;
    
    check = recurse_dir(mem_start, node, dir_circle);

    if (check == -1) {
      free(dir_circle);
      return -1;
    }

    free(dir_circle);
  }
  return 0;
}

// extra repair checks
void repair(void *mem_start, struct dinode *node, int ninodes) {
  if ((in_use_inums = calloc(ninodes, sizeof(int))) < 0)
    exit(1);
  get_inode_info(mem_start, ninodes);

  struct dirent *d_entry;
  uint b_addr;
  struct dinode *lost_found =
	  (struct dinode *) (mem_start + 2*BSIZE + 29*sizeof(struct dinode));

  node = (struct dinode *) (mem_start +  2*BSIZE);
  int i, j, k, found;

  for (i = 0; i < ninodes; i++, node++) {
    if (i < 2)
      continue;

    if ((node->type != 0) && (in_use_inums[i] == 0)) {
      for (j = 0; j < NDIRECT; j++) {
	b_addr = lost_found->addrs[j];

	found = 0;
	d_entry = (struct dirent *) (mem_start + b_addr*BSIZE);
        for (k = 0; k < DPB; k++, d_entry++) {
          if (d_entry->inum == 0) {
            d_entry->inum = i;
	    found = 1;
	    break;
	  }
	}
	if (found)
	  break;
      }
    }
  }
  free(in_use_inums);
}


int main(int argc, char *argv[]) {
  if ((argc < 2) || (argc > 3)) {
    fprintf(stderr, "Usage: xv6_fsck <file_system_image>.\n");
    exit(1);
  }

  int rc;
  struct stat sbuf;
  void *img_ptr;
  struct superblock *sb;
  struct dinode *dip;

  if ((argc == 3) && (strcmp(argv[1], "-r") == 0))
    goto repair;
  else if (argc == 3)
    exit(1);

  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "image not found.\n");
    exit(1);
  }

  //int rc;
  //struct stat sbuf;
  rc = fstat(fd, &sbuf);
  if (rc != 0)
    exit(1);

  img_ptr = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (img_ptr == MAP_FAILED)
    exit(1);
  if (close(fd) < 0)
    exit(1);
  
  sb = (struct superblock *) (img_ptr + BSIZE);
  dip = (struct dinode *) (img_ptr + (2*BSIZE));
  uint db1 = ((sb->ninodes / IPB) + 1) + ((sb->size / BPB) + 1) + 2;
  int i, failed;
  failed = 0;

  for (i = 0; i < sb->ninodes; i++, dip++) {
    if (dip->type == 0) // unallocated inode, skip
      continue;

    // check #1
    // each inode is either unallocated or a valid type
    if (check_valid_inodes(dip->type) < 0) {
      fprintf(stderr, "ERROR: bad inode.\n");
      failed = 1;
      goto clean_and_exit;
    }

    // check #2A
    // each address used by direct block in inode is valid
    if (check_valid_direct(dip, sb->size) < 0) {
      fprintf(stderr, "ERROR: bad direct address in inode.\n");
      failed = 1;
      goto clean_and_exit;
    }

    // check #2B
    // each address used by indirect block in inode is valid
    if (check_valid_indirect(img_ptr, dip, sb->size) < 0) {
      fprintf(stderr, "ERROR: bad indirect address in inode.\n");
      failed = 1;
      goto clean_and_exit;
    }

    // check #3
    // root directory exists, inode number is 1, parent of root is self
    if (i == 1) {
      if ((dip->type != T_DIR) || (check_valid_dir(img_ptr, dip, i) < 0)) {
        fprintf(stderr, "ERROR: root directory does not exist.\n");
        failed = 1;
	goto clean_and_exit;
      }
    }
    // check #4
    // each directory contsin . and .., . points to directory itself
    if ((dip->type == T_DIR) && (check_valid_dir(img_ptr, dip, i) < 0)) {
      fprintf(stderr, "ERROR: directory not properly formatted.\n");
      failed = 1;
      goto clean_and_exit;
    }
    
    // check #5
    // for in-use inodes, each address in use is also marked in use in bitmap
    if ((check_valid_bitmap(img_ptr, dip, sb->ninodes, i)) < 0) {
      fprintf(stderr,
              "ERROR: address used by inode but marked free in bitmap.\n");
      failed = 1;  
      goto clean_and_exit;
    } 
  }

  if ((used_datablocks = calloc(sb->nblocks, sizeof(int))) == NULL)
    exit(1);

  // check #6
  // for blocks marked in-use in bitmap, actually is in-use somewhere
  if (check_valid_blocks_in_bitmap(img_ptr, sb, db1) < 0) {
    fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
    failed = 1;
    goto clean_and_exit;
  }

  // check #7
  // for in-use inodes, direct address in use is only used once
  if (check_direct_addr_use(img_ptr, sb->ninodes) < 0) {
    fprintf(stderr, "ERROR: direct address used more than once.\n");
    failed = 1;
    goto clean_and_exit;
  }

  // check #8
  // for in-use inodes, indirect address in use is only used once
  if (check_indirect_addr_use(img_ptr, sb->ninodes) < 0) {
    fprintf(stderr, "ERROR: indirect address used more than once.\n");
    failed = 1;
    goto clean_and_exit;
  }

  if ((in_use_inums = calloc(sb->ninodes, sizeof(int))) < 0)
    exit(1);
  get_inode_info(img_ptr, sb->ninodes);

  dip = (struct dinode *) (img_ptr + (2*BSIZE));
  for (i = 0; i < sb->ninodes; i++, dip++) {
    if (i < 2)
     continue;

    // check #9
    // inode marked in use must be referred to in at least one directory
    if ((dip->type != 0) && (in_use_inums[i] == 0)) {
      fprintf(stderr,
	      "ERROR: inode marked use but not found in a directory.\n");
      failed = 1;
      goto clean_and_exit;
    }

    // check #10
    // all inodes referred to in valid director are actually in use
    if ((in_use_inums[i] != 0) && (dip->type == 0)) {
      fprintf(stderr,
	      "ERROR: inode referred to in directory but marked free.\n");
      failed = 1;
      goto clean_and_exit;
    }
    // check #11
    // reference counts for regular files match number of times
    // file is referred to in directories
    if ((dip->type == T_FILE) && (dip->nlink != in_use_inums[i])) {
      fprintf(stderr,
	      "ERROR: bad reference count for file.\n");
      failed = 1;
      goto clean_and_exit;
    }
    // check #12
    // each directory only appears in one other directory
    if ((dip->type == T_DIR) && (in_use_inums[i] > 1)) {
      fprintf(stderr,
	      "ERROR: directory appears more than once in file system.\n");
      failed = 1;
      goto clean_and_exit;
    }
  }

  // EXTRA TESTS

  // check #E1
  // each .. entry in directory points to proper parent inode
  // and parent inode points back to it
  if (check_parent_dir(img_ptr, sb->ninodes) < 0) {
    fprintf(stderr, "ERROR: parent directory mismatch.\n");
    failed = 1;
    goto clean_and_exit;
  }

  // check #E2
  // no loops in directory tree
  if (check_no_loops(img_ptr, sb->ninodes) < 0) {
    fprintf(stderr, "ERROR: inaccessible directory exists.\n");
    failed = 1;
    goto clean_and_exit;
  }

 clean_and_exit: ;
  free(in_use_inums);
  free(used_datablocks);
  if (munmap(img_ptr, sbuf.st_size) < 0)
    exit(1);
  
  if (failed)
    exit(1);

  return 0;

  // repair image
 repair: ;
  fd = open(argv[2], O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "image not found.\n");
    exit(1);
  }

  rc = fstat(fd, &sbuf);
  if (rc != 0)
    exit(1);

  img_ptr = mmap(NULL,
		 sbuf.st_size,
		 PROT_READ | PROT_WRITE,
		 MAP_SHARED,
		 fd,
		 0);
  if (img_ptr == MAP_FAILED)
    exit(1);
  if (close(fd) < 0)
    exit(1);
  
  sb = (struct superblock *) (img_ptr + BSIZE);
  dip = (struct dinode *) (img_ptr + (2*BSIZE));

  repair(img_ptr, dip, sb->ninodes);

  if (munmap(img_ptr, sbuf.st_size) < 0)
    exit(1);

  return 0;
}
