// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);
static uint balloc(uint dev);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

uint block_refcnt[MAXBLOCKS];  //블록별 참조 카운트 배열

struct metadata_cache {
  struct spinlock lock;   // 락
  int             dirty;  //수정여부
  int             is_initialized;           //캐시가 초기화 되었고 사용가능한지
  uint            block_refcnt[MAXBLOCKS];  //블록별 참조 카운트 배열
} meta;

/**
 * @brief 전역 스냅샷 관리 구조체
 */
struct snapshot_table{
  struct spinlock lock;
  struct snapshot snapshots[MAX_SNAPSHOTS];
  int next_id;
} snapshot_table;

void copy_inode_metadata(struct inode *src, struct inode *dest);
void copy_tree_recursive(struct inode *src, struct inode *dest, int snap_id);
void decrease_block_references(struct inode *ip);
void delete_tree_recursive(struct inode *ip);
void clear_directory_recursive(struct inode *dp);
void refresh_from_snapshot(struct inode *snap_root, struct inode *curr_root);
void write_metadata_file(void);

struct inode *create_dir(char *path);

int find_snapshot_slot(int id);

static struct inode*  iget(uint dev, uint inum);
static void bfree(int dev, uint b);

/**
 * @brief 숫자를 문자로 변환
 */
void itoa(int n, char *s) {
  int i = 0, j = 0;
  char temp[16];

  if (n == 0) { 
    s[0] = '0'; s[1] = '\0'; return;
  }
  while (n > 0) {
    temp[i++] = (n % 10) + '0'; 
    n /= 10;
  }
  while (i > 0)
    s[j++] = temp[--i];
  s[j] = '\0';
}

/**
 * @brief 메타데이터 파일 생성/업데이트
 */
void write_metadata_file(void) {
  // ✅ 외부 begin_op() 제거
  
  struct inode *snap_dir = namei("/snapshot");
  if (snap_dir == 0) {
    return;
  }
  
  begin_op();  // ✅ 파일 생성용 트랜잭션
  
  ilock(snap_dir);
  
  struct inode *meta_ip;
  uint off;
  
  if ((meta_ip = dirlookup(snap_dir, "meta", &off)) != 0) {
    iunlock(snap_dir);
    iput(snap_dir);
    ilock(meta_ip);
  } else {
    if ((meta_ip = ialloc(snap_dir->dev, T_FILE)) == 0) {
      iunlockput(snap_dir);
      end_op();
      return;
    }
    
    ilock(meta_ip);
    meta_ip->major = 0;
    meta_ip->minor = 0;
    meta_ip->nlink = 1;
    iupdate(meta_ip);
    
    if (dirlink(snap_dir, "meta", meta_ip->inum) < 0) {
      meta_ip->nlink = 0;
      iupdate(meta_ip);
      iunlockput(meta_ip);
      iunlockput(snap_dir);
      end_op();
      return;
    }
    
    iunlock(snap_dir);
    iput(snap_dir);
  }
  
  itrunc(meta_ip);
  iunlock(meta_ip);
  
  end_op();  // ✅ 파일 생성 트랜잭션 종료
  
  // ✅ 5개씩 쓰기
  int offset = 0;
  int count = 0;
  
  begin_op();  // ✅ 쓰기용 트랜잭션 시작
  ilock(meta_ip);
  
  for (int blockno = 0; blockno < FSSIZE; blockno++) {
    if (block_refcnt[blockno] > 0) {
      char line[64];
      int len = 0;
      
      char *prefix = "Block ";
      for (int i = 0; prefix[i]; i++)
        line[len++] = prefix[i];
      
      char num[16];
      itoa(blockno, num);
      for (int i = 0; num[i]; i++)
        line[len++] = num[i];
      
      char *middle = ": refcnt=";
      for (int i = 0; middle[i]; i++)
        line[len++] = middle[i];
      
      itoa(block_refcnt[blockno], num);
      for (int i = 0; num[i]; i++)
        line[len++] = num[i];
      
      line[len++] = '\n';
      
      if (writei(meta_ip, line, offset, len) != len) {
        break;
      }
      offset += len;
      count++;
      
      // ✅ 5개마다 트랜잭션 재시작
      if (count >= 5) {
        iunlock(meta_ip);
        end_op();
        begin_op();
        ilock(meta_ip);
        count = 0;
      }
    }
  }
  
  iunlockput(meta_ip);
  end_op();  // ✅ 마지막 트랜잭션 종료
}

/**
 * @brief metadata_cache 초기화 함수
 */
void snapinit(void) {
  //1. 스핀락 초기화
  initlock(&snapshot_table.lock, "snapshot_table");

  //2. 스냅샷 ID 초기화
  snapshot_table.next_id = 1;

  //3. 스냅샷 배열 초기화
  for (int i = 0; i < MAX_SNAPSHOTS; i++) {
    snapshot_table.snapshots[i].id = 0;
    snapshot_table.snapshots[i].valid = 0;
  }

  for(int i = 0; i < MAXBLOCKS; i++)
    block_refcnt[i] = 0;

  cprintf("snapinit\n");
}



/**
 * @brief 스냅샷 생성
 */
int snapshot_create(void) {
  acquire(&snapshot_table.lock);
  
  int slot = -1;
  for(int i = 0; i < MAX_SNAPSHOTS; i++) {
    if (!snapshot_table.snapshots[i].valid) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    release(&snapshot_table.lock);
    return -1;
  }

  int snap_id = snapshot_table.next_id++;
  snapshot_table.snapshots[slot].id = snap_id;
  snapshot_table.snapshots[slot].valid = 1;

  release(&snapshot_table.lock);

  // ✅ begin_op() 제거! 내부 함수들이 각자 트랜잭션 관리

  struct inode *snap_dir = namei("/snapshot");
  if (snap_dir == 0) {
    snap_dir = create_dir("/snapshot");  // 내부에서 트랜잭션 처리
    if (snap_dir == 0) {
      acquire(&snapshot_table.lock);
      snapshot_table.snapshots[slot].valid = 0;
      release(&snapshot_table.lock);
      return -1;
    }
  }

  char snap_path[32];
  char num_buf[16];
  itoa(snap_id, num_buf);
  safestrcpy(snap_path, "/snapshot/", sizeof(snap_path));
  safestrcpy(snap_path + 10, num_buf, sizeof(snap_path) - 10);
  
  struct inode *snap_root = create_dir(snap_path);  // 내부에서 트랜잭션 처리
  if (snap_root == 0) {
    acquire(&snapshot_table.lock);
    snapshot_table.snapshots[slot].valid = 0;
    release(&snapshot_table.lock);
    return -1;
  }

  struct inode *root = namei("/");
  if (root == 0) {
    acquire(&snapshot_table.lock);
    snapshot_table.snapshots[slot].valid = 0;
    release(&snapshot_table.lock);
    return -1;
  }

  copy_tree_recursive(root, snap_root, snap_id);  // 내부에서 트랜잭션 처리

  iput(root);
  iput(snap_root);

  // ✅ end_op() 제거!

  write_metadata_file();  // 내부에서 트랜잭션 처리

  acquire(&snapshot_table.lock);
  snapshot_table.snapshots[slot].root_inode = snap_root->inum;
  release(&snapshot_table.lock);

  cprintf("Snapshot created %d\n", snap_id);
  return snap_id;
}


/**
 * @brief 디렉토리 생성 함수
 */
struct inode *create_dir(char *path) {
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0) {
    return 0;
  }

  begin_op();  // ✅ 트랜잭션 시작

  ilock(dp);

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    end_op();  // ✅ 트랜잭션 종료
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return 0;
    }
    iunlock(ip);
    return ip;
  }

  if ((ip = ialloc(dp->dev, T_DIR)) == 0) {
    iunlockput(dp);
    end_op();  // ✅ 트랜잭션 종료
    return 0;
  }

  ilock(ip);

  ip->major = 0;
  ip->minor = 0;
  ip->nlink = 1;
  iupdate(ip);

  if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
    panic("create_dir dots");

  if (dirlink(dp, name, ip->inum) < 0)
    panic("create_dir link");

  dp->nlink++;
  iupdate(dp);
  iunlockput(dp);
  iunlock(ip);

  end_op();  // ✅ 트랜잭션 종료

  return ip;
}

void copy_tree_recursive(struct inode *src, struct inode *dest, int snap_id) {
  struct dirent de;

  ilock(src);
  
  for (uint off = 0; off < src->size; off += sizeof(de)) {
    if (readi(src, (char *)&de, off, sizeof(de)) != sizeof(de))
      break;
    
    if (de.inum == 0 || strncmp(de.name, ".", DIRSIZ) == 0 || 
        strncmp(de.name, "..", DIRSIZ) == 0)
      continue;

    if (strncmp(de.name, "snapshot", DIRSIZ) == 0)
      continue;

    struct inode *child = iget(src->dev, de.inum);
    ilock(child);
    
    if (child->type == T_DEV) {
      iunlockput(child);
      continue;
    }
    
    short child_type = child->type;
    short child_major = child->major;
    short child_minor = child->minor;
    uint child_size = child->size;
    uint child_addrs[NDIRECT+1];
    
    for(int i = 0; i <= NDIRECT; i++) {
      child_addrs[i] = child->addrs[i];
    }
    
    iunlock(child);
    iunlock(src);

    if (child_type == T_DIR) {
      // ✅ 디렉토리: 트랜잭션 3개로 분할
      
      // 트랜잭션 1: inode 할당
      begin_op();
      struct inode *new_child = ialloc(dest->dev, T_DIR);
      ilock(new_child);
      new_child->type = T_DIR;
      new_child->major = child_major;
      new_child->minor = child_minor;
      new_child->nlink = 1;
      new_child->size = 0;
      iupdate(new_child);
      iunlock(new_child);
      end_op();
      
      // 트랜잭션 2: . 링크
      begin_op();
      ilock(new_child);
      dirlink(new_child, ".", new_child->inum);
      iunlock(new_child);
      end_op();
      
      // 트랜잭션 3: .. 링크
      begin_op();
      ilock(new_child);
      dirlink(new_child, "..", dest->inum);
      iunlock(new_child);
      end_op();
      
      // 트랜잭션 4: 부모 링크
      begin_op();
      ilock(dest);
      dirlink(dest, de.name, new_child->inum);
      iunlock(dest);
      end_op();
      
      // 재귀 호출
      copy_tree_recursive(child, new_child, snap_id);
      
      iput(child);
      iput(new_child);
      ilock(src);
    }
    else {
      // ✅ 파일: 트랜잭션 2개로 분할
      
      // 트랜잭션 1: inode 할당 및 블록 참조 복사
      begin_op();
      struct inode *new_child = ialloc(dest->dev, T_FILE);
      ilock(new_child);
      
      new_child->type = child_type;
      new_child->major = child_major;
      new_child->minor = child_minor;
      new_child->nlink = 1;
      new_child->size = child_size;
      
      for(int i = 0; i < NDIRECT; i++) {
        if(child_addrs[i]) {
          new_child->addrs[i] = child_addrs[i];
          block_refcnt[child_addrs[i]]++;
        }
      }
      
      if(child_addrs[NDIRECT]) {
        new_child->addrs[NDIRECT] = child_addrs[NDIRECT];
        block_refcnt[child_addrs[NDIRECT]]++;
        
        struct buf *bp = bread(new_child->dev, child_addrs[NDIRECT]);
        uint *indirect = (uint*)bp->data;
        for(int j = 0; j < NINDIRECT; j++) {
          if(indirect[j])
            block_refcnt[indirect[j]]++;
        }
        brelse(bp);
      }
      iupdate(new_child);
      iunlock(new_child);
      end_op();
      
      // 트랜잭션 2: 부모 디렉토리 링크
      begin_op();
      ilock(dest);
      dirlink(dest, de.name, new_child->inum);
      iunlock(dest);
      end_op();
      
      iput(child);
      iput(new_child);
      ilock(src);
    }
  }
  
  iunlock(src);
}

/**
 * @brief COW 블록 처리
 */
void copy_file_with_cow(struct inode *src, struct inode *dest, int snap_id) {
  dest->size = src->size;

  //직접 블록
  for(int i = 0; i < NDIRECT; i++) {
    if (src->addrs[i]) {
      dest->addrs[i] = src->addrs[i];
      //블록 참조 카운트 증가
      block_refcnt[src->addrs[i]]++;
    }
  }

  //간접 블록 처리
  if (src->addrs[NDIRECT]) {
    dest->addrs[NDIRECT] = src->addrs[NDIRECT];
    block_refcnt[src->addrs[NDIRECT]]++;
  }
}

/**
 * @brief 메타데이터 복사 (child->new_child)
 */
void copy_inode_metadata(struct inode *src, struct inode *dest) {
  dest->type = src->type;
  dest->major = src->major;
  dest->minor = src->minor;
  dest->nlink = 1;  //부모 디렉토리와의 연결로 1로 초기화
  dest->size = src->size;
  iupdate(dest);
}

/**
 * @brief snapshot_rollback() 구현
 */
int snapshot_rollback(int snap_id) {
  // 1. 스냅샷 찾기
  acquire(&snapshot_table.lock);
  int slot = -1;
  for (int i = 0; i < MAX_SNAPSHOTS; i++) {
    if (snapshot_table.snapshots[i].valid && 
        snapshot_table.snapshots[i].id == snap_id) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    release(&snapshot_table.lock);
    return -1;
  }
  release(&snapshot_table.lock);
  
  // ✅ begin_op() 제거!
  
  // 2. 스냅샷 디렉토리 경로 생성
  char snap_path[32];
  char num_buf[16];
  itoa(snap_id, num_buf);
  safestrcpy(snap_path, "/snapshot/", sizeof(snap_path));
  safestrcpy(snap_path + 10, num_buf, sizeof(snap_path) - 10);
  
  // 3. 스냅샷 디렉토리 열기
  struct inode *snap_root = namei(snap_path);
  if (snap_root == 0) {
    cprintf("snapshot_rollback: snapshot directory not found\n");
    return -1;
  }
  
  ilock(snap_root);
  if (snap_root->type != T_DIR) {
    iunlockput(snap_root);
    cprintf("snapshot_rollback: not a directory\n");
    return -1;
  }
  iunlock(snap_root);
  
  // 4. 루트 디렉토리 열기
  struct inode *root = namei("/");
  if (root == 0) {
    iput(snap_root);
    return -1;
  }
  
  // 5. 현재 파일시스템 삭제 (/snapshot 제외)
  clear_directory_recursive(root);  // 내부에서 트랜잭션 처리
  
  // 6. 스냅샷에서 복구
  refresh_from_snapshot(snap_root, root);  // 내부에서 트랜잭션 처리
  
  iput(snap_root);
  iput(root);
  
  // ✅ end_op() 제거!
  
  cprintf("Rolled back to snapshot %d\n", snap_id);
  return 0;
}

/**
 * @brief 스냅샷 내용을 현재 루트로 복사 (롤백)
 */
void refresh_from_snapshot(struct inode *snap_root, struct inode *curr_root) {
// 이미 구현한 copy_tree_recursive를 재활용합니다.
  // snap_id 인자는 롤백 상황이므로 -1 등을 주거나, 
  // copy_tree_recursive 내부에서 스냅샷 생성이 아닐 때 처리를 해야 하지만
  // 간단히 현재 로직을 그대로 쓴다면 새로운 스냅샷 ID를 부여받는 꼴이 됩니다.
  // 롤백은 "스냅샷의 상태로 현재를 되돌리는 것"이므로
  // copy_tree_recursive(snap_root, curr_root, 0); 형태로 호출하면 됩니다.
  
  copy_tree_recursive(snap_root, curr_root, 0);
}

/**
 * @brief 현재 디렉토리 비우기 (롤백 전처리)
 */
void clear_directory_recursive(struct inode *dp) {
  struct dirent de;

  ilock(dp);

  for(uint off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      break;

    if(de.inum == 0)
      continue;
    if(strncmp(de.name, ".", DIRSIZ) == 0 || strncmp(de.name, "..", DIRSIZ) == 0)
      continue;
    
    if(strncmp(de.name, "snapshot", DIRSIZ) == 0) {
      continue;
    }

    struct inode *child = iget(dp->dev, de.inum);
    ilock(child);
    short child_type = child->type;
    iunlock(child);
    
    // 재귀적으로 하위 내용 삭제
    if(child_type == T_DIR) {
      iunlock(dp);
      clear_directory_recursive(child);
      ilock(dp);
    }
    
    iput(child);
    
    // ✅ 각 엔트리마다 트랜잭션으로 삭제
    iunlock(dp);
    begin_op();
    ilock(dp);
    
    // 디렉토리 엔트리 초기화 (삭제 처리)
    de.inum = 0;
    writei(dp, (char*)&de, off, sizeof(de));
    
    iunlock(dp);
    end_op();
    ilock(dp);
  }
  
  iunlock(dp);
}

/**
 * @brief 스냅샷 슬롯 찾기
 */
int find_snapshot_slot(int id) {
  acquire(&snapshot_table.lock);
  for(int i = 0; i < MAX_SNAPSHOTS; i++) {
    if (snapshot_table.snapshots[i].valid && snapshot_table.snapshots[i].id == id) {
      release(&snapshot_table.lock);
      return i;
    }
  }
  release(&snapshot_table.lock);
  return -1;
}


/**
 * @brief snapshot_delete() 구현
 */
int snapshot_delete(int snap_id) {
  int slot = find_snapshot_slot(snap_id);
  if (slot < 0)
    return -1;

  // 스냅샷 디렉토리 삭제
  char snap_path[32];
  char num_buf[16];
  itoa(snap_id, num_buf);
  safestrcpy(snap_path, "/snapshot/", sizeof(snap_path));
  safestrcpy(snap_path + 10, num_buf, sizeof(snap_path) - 10);

  struct inode *snap_root = namei(snap_path);
  if (snap_root) {
    delete_tree_recursive(snap_root);  // 내부에서 트랜잭션 처리
    
    // ✅ 블록 참조 감소 (트랜잭션 필요)
    begin_op();
    decrease_block_references(snap_root);
    iput(snap_root);
    end_op();
  }

  acquire(&snapshot_table.lock);
  snapshot_table.snapshots[slot].valid = 0;
  release(&snapshot_table.lock);
  
  return 0;
}

/**
 * @brief 디렉토리 트리 삭제 (재귀적)
 */
void delete_tree_recursive(struct inode *dp) {
  struct dirent de;
  uint off;
  
  ilock(dp);
  
  if (dp->type != T_DIR) {
    iunlock(dp);
    return;
  }
  
  // 디렉토리 내용 순회
  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      break;
    
    if (de.inum == 0)
      continue;
    
    if (strncmp(de.name, "snapshot", DIRSIZ) == 0)
      continue;
    
    struct inode *ip = iget(dp->dev, de.inum);
    ilock(ip);
    
    if (ip->type == T_DEV) {
      iunlockput(ip);
      continue;
    }
    
    short ip_type = ip->type;
    iunlock(ip);
    iunlock(dp);
    
    if (ip_type == T_DIR) {
      // 재귀적으로 삭제
      delete_tree_recursive(ip);
    }
    
    // ✅ 각 항목마다 트랜잭션으로 삭제
    begin_op();
    
    // 블록 참조 감소
    ilock(ip);
    decrease_block_references(ip);
    iunlock(ip);
    
    // 디렉토리 엔트리 제거
    ilock(dp);
    de.inum = 0;
    if (writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("delete_tree_recursive: writei");
    iunlock(dp);
    
    end_op();
    
    iput(ip);
    ilock(dp);
  }
  
  iunlock(dp);
}

/**
 * @brief 블록 참조 카운트 감소 (스냅샷 삭제 시 사용)
 */
/**
 * @brief 블록 참조 카운트 감소 (스냅샷 삭제 시 사용)
 * 주의: 반드시 트랜잭션 내에서 호출되어야 함
 */
void decrease_block_references(struct inode *ip) {
  // ✅ 이미 lock된 상태로 호출됨
  
  // 직접 블록
  for(int i = 0; i < NDIRECT; i++) {
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  // 간접 블록
  if (ip->addrs[NDIRECT]) {
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT]);
    uint *a = (uint *)bp->data;

    for(int j = 0; j < NINDIRECT; j++) {
      if (a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);

    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
  
  iupdate(ip);
}



/**
 * @brief bcow 구현
 * 
 * @param oldbp 이미 읽혀진 버퍼 포인터
 * @param dev 디바이스 번호, 어느 디스크/파티션에서 작업하는지
 * 
 * @return new_blockno : 새 블록 번호, 실패 시 -1
 */
uint bcow(int dev, uint blockno) {
  //1. 참조 카운트 확인
  //todo : 동시성 문제 방지를 위해 Lock 고려
  if (block_refcnt[blockno] <= 1) {
    return blockno; //COW 불필요
  }

  //2. 새 블록 할당
  uint newblk = balloc(dev);
  if (newblk == 0) {
    panic("bcow: balloc failed\n");
  }

  //3. 데이터 복사
  struct buf *oldbp = bread(dev, blockno);
  struct buf *newbp = bread(dev, newblk);
  memmove(newbp->data, oldbp->data, BSIZE);
  log_write(newbp);

  //4. 참조 카운트 업데이트
  //todo : 동시성 문제 방지를 위해 Lock 고려
  block_refcnt[blockno]--;
  block_refcnt[newblk] = 1;

  //5. 버퍼 해제
  brelse(oldbp);
  brelse(newbp);

  return newblk;
}

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  //1. 참조 카운트 감소
  if (block_refcnt[b] > 0) {
    block_refcnt[b]--;
  }

  //2. 참조가 0이되면 실제로 메모리 해제
  if (block_refcnt[b] == 0) {
    struct buf *bp;
    int bi, m;

    bp = bread(dev, BBLOCK(b, sb));
    bi = b % BPB;
    m = 1 << (bi % 8);
    if((bp->data[bi/8] & m) == 0)
      panic("freeing free block");
    bp->data[bi/8] &= ~m;
    log_write(bp);
    brelse(bp);
  }
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    //새 블록이 할당되는 경우
    if((addr = ip->addrs[bn]) == 0) {
      ip->addrs[bn] = addr = balloc(ip->dev);
      //새 블록 참조 카운트 설정
      block_refcnt[addr] = 1;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0) {
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
      block_refcnt[addr] = 1; //간접 블록 참조 카운트
    }

    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      block_refcnt[addr] = 1; //데이터 블록 참조 카운트
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m = 0;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint old_blk = bmap(ip, off/BSIZE);
    uint new_blk = bcow(ip->dev, old_blk);  //COW 적용

    if (new_blk != old_blk) {
      //inode의 블록 포인터 업데이트
      if (off/BSIZE < NDIRECT) {
        ip->addrs[off/BSIZE] = new_blk;
        iupdate(ip);
      }
      else {  //indirect 블록 처리
        uint indirect_idx = (off / BSIZE) - NDIRECT;

        //간접 블록 자체에 대한 COW 처리
        if (ip->addrs[NDIRECT] != 0) {
          uint old_indirect = ip->addrs[NDIRECT];
          uint new_indirect = bcow(ip->dev, old_indirect);

          if (new_indirect != old_indirect) {
            //간접 블록이 복사된 경우
            ip->addrs[NDIRECT] = new_indirect;
            iupdate(ip);
          }

          //간접 블록 내의 엔터리 업데이트
          struct buf *indbp = bread(ip->dev, ip->addrs[NDIRECT]);
          uint *indirect_entreis = (uint *)indbp->data;
          indirect_entreis[indirect_idx] = new_blk;
          log_write(indbp);
          brelse(indbp);
        }
      }
    }

    //4. 실제 데이터 쓰기
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  //5. 파일 크기 업데이트
  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
