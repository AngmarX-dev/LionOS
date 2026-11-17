#include <stdint.h>
#include "diskfs.h"
#include "ext2_vfs.h"
#include "ramfs.h"
#include "vfs.h"
#include "process.h"
#include "spinlock.h"
#include "heap.h"
#include "tty.h"

#define VFS_F_READ 1u
#define VFS_F_WRITE 2u
#define VFS_F_APPEND 4u

struct vfs_fd { uint32_t used, refs, backend, flags, offset; char path[VFS_PATH_MAX]; };
#define VFS_PROCESS_OPEN_MAX (LIONOS_PROCESS_MAX * PROCESS_FD_MAX)
static struct vfs_fd kernel_fds[VFS_FD_MAX];
static struct vfs_fd process_open_files[VFS_PROCESS_OPEN_MAX];
static struct spinlock vfs_lock;

static void release_process_open_file(uint32_t index) {
    struct vfs_fd *file;
    if (index >= VFS_PROCESS_OPEN_MAX) return;
    file = &process_open_files[index];
    if (!file->used || !file->refs) return;
    --file->refs;
    if (!file->refs) {
        file->used = 0u;
        file->backend = 0u;
        file->flags = 0u;
        file->offset = 0u;
        file->path[0] = 0;
    }
}

static int path_normalize(char *dst, const char *src) {
    if (!dst || !src || !*src) return -1;
    char components[VFS_PATH_MAX];
    uint32_t out=0, i=0;
    (void)components;
    dst[0]=0;
    while (src[i]) {
        while (src[i]=='/') ++i;
        if (!src[i]) break;
        uint32_t start=i, len=0;
        while (src[i] && src[i]!='/') { if (++len>=VFS_PATH_MAX) return -1; ++i; }
        if (len==1 && src[start]=='.') continue;
        if (len==2 && src[start]=='.' && src[start+1]=='.') {
            while (out && dst[out-1]!='/') --out;
            if (out) --out;
            dst[out]=0;
            continue;
        }
        if (out) { if (out+1u>=VFS_PATH_MAX) return -1; dst[out++]='/'; }
        if (out+len>=VFS_PATH_MAX) return -1;
        for (uint32_t j=0;j<len;++j) dst[out++]=src[start+j];
        dst[out]=0;
    }
    return out ? 0 : -1;
}
static uint32_t backend_for(const char *p) {
    if (ramfs_data(p)) return VFS_BACKEND_RAMFS;
    if (diskfs_available() && diskfs_exists(p)) return VFS_BACKEND_DISKFS;
    if (ext2_vfs_exists(p)) return VFS_BACKEND_EXT2_RO;
    return VFS_BACKEND_NONE;
}
static int fd_valid(const struct vfs_fd *f) { return f && f->used; }
static uint32_t file_size(uint32_t backend,const char *path) {
    if (backend == VFS_BACKEND_DISKFS) return diskfs_size(path);
    if (backend == VFS_BACKEND_EXT2_RO) return ext2_vfs_size(path);
    return ramfs_size(path);
}
static int open_slot(struct vfs_fd *fds,const char *path,uint32_t flags) {
    uint32_t backend=backend_for(path);
    if (backend == VFS_BACKEND_EXT2_RO && (flags & VFS_F_WRITE))
        return VFS_FD_INVALID;
    if (backend==VFS_BACKEND_NONE) {
        if (ext2_vfs_path(path) || !(flags&VFS_F_WRITE)) return VFS_FD_INVALID;
        if (diskfs_available()) {
            static const char empty[1]={0};
            if (diskfs_write(path,empty,0)==0) backend=VFS_BACKEND_DISKFS;
        }
        if (backend==VFS_BACKEND_NONE) {
            static const char empty[1]={0};
            if (ramfs_write(path,empty,0)==0) backend=VFS_BACKEND_RAMFS;
            else return VFS_FD_INVALID;
        }
    }
    for (uint32_t i=0;i<VFS_FD_MAX;++i) if (!fds[i].used) {
        fds[i].used=1; fds[i].backend=backend; fds[i].flags=flags;
        fds[i].offset=(flags&VFS_F_APPEND)?file_size(backend,path):0u;
        uint32_t j=0; while(path[j]) { fds[i].path[j]=path[j]; ++j; } fds[i].path[j]=0;
        return (int)i;
    }
    return VFS_FD_INVALID;
}
static int close_slot(struct vfs_fd *f) { if(!fd_valid(f)) return -1; f->used=0; return 0; }
static int read_slot(struct vfs_fd *f,void *buffer,uint32_t length) {
    if(!fd_valid(f)||!buffer||length>VFS_IO_MAX||(f->flags&VFS_F_READ)==0)return -1;
    uint32_t size=file_size(f->backend,f->path); if(f->offset>=size)return 0;
    uint32_t wanted=length; if(wanted>size-f->offset)wanted=size-f->offset;
    if (f->backend == VFS_BACKEND_EXT2_RO) {
        int rr = ext2_vfs_read(f->path, f->offset, buffer, wanted);
        if (rr < 0 || (uint32_t)rr != wanted) return -1;
    } else if(f->backend==VFS_BACKEND_DISKFS) {
        uint8_t *temp=(uint8_t*)kmalloc(VFS_IO_MAX); if(!temp)return -1; int rr=diskfs_read(f->path,temp,VFS_IO_MAX); if(rr<0){kfree(temp);return -1;}
        for(uint32_t i=0;i<wanted;++i)((uint8_t*)buffer)[i]=temp[f->offset+i];
        kfree(temp);
    } else {
        const uint8_t *src=(const uint8_t*)ramfs_data(f->path); if(!src)return -1;
        for(uint32_t i=0;i<wanted;++i)((uint8_t*)buffer)[i]=src[f->offset+i];
    }
    f->offset+=wanted; return (int)wanted;
}
static int write_slot(struct vfs_fd *f,const void *buffer,uint32_t length) {
    if(!fd_valid(f)||!buffer||length>VFS_IO_MAX||(f->flags&VFS_F_WRITE)==0 ||
       f->backend == VFS_BACKEND_EXT2_RO)return -1;
    if(f->offset!=0 && !(f->flags&VFS_F_APPEND))return -1;
    if(f->flags&VFS_F_APPEND){
        uint32_t old_size=file_size(f->backend,f->path);
        uint32_t max_size=f->backend==VFS_BACKEND_DISKFS?DISKFS_MAX_FILE_SIZE:4096u;
        if(old_size>max_size||length>max_size-old_size)return -1;
        uint8_t *tmp=(uint8_t*)kmalloc(max_size);
        if(!tmp)return -1;
        int old_read=f->backend==VFS_BACKEND_DISKFS?diskfs_read(f->path,tmp,max_size):(int)old_size;
        if(f->backend==VFS_BACKEND_RAMFS){
            const uint8_t *old=(const uint8_t*)ramfs_data(f->path);
            if(old)for(uint32_t i=0;i<old_size;++i)tmp[i]=old[i];
        }
        if(old_read<0){kfree(tmp);return -1;}
        for(uint32_t i=0;i<length;++i)tmp[old_size+i]=((const uint8_t*)buffer)[i];
        int r=f->backend==VFS_BACKEND_DISKFS?diskfs_write(f->path,tmp,old_size+length):ramfs_write(f->path,(const char*)tmp,old_size+length);
        kfree(tmp);
        if(r<0)return -1;
        f->offset=old_size+length;
        return (int)length;
    }
    int r=f->backend==VFS_BACKEND_DISKFS?diskfs_write(f->path,buffer,length):ramfs_write(f->path,buffer,length);
    if(r<0)return -1;
    f->offset+=length;
    return (int)length;
}

int vfs_init(void){
    spinlock_init(&vfs_lock);
    for(uint32_t i=0;i<VFS_FD_MAX;++i)kernel_fds[i].used=0u;
    for(uint32_t i=0;i<VFS_PROCESS_OPEN_MAX;++i)process_open_files[i].used=0u;
    return 0;
}

int vfs_stdio_init_process(struct process *p) {
    uint32_t objects[3], found = 0u;
    if (!p) return -1;
    uint32_t irq = spinlock_irqsave_acquire(&vfs_lock);
    if (p->fd_closing || p->fd_used[0] || p->fd_used[1] || p->fd_used[2]) {
        spinlock_irqrestore_release(&vfs_lock, irq); return -1;
    }
    for (uint32_t i=0u;i<VFS_PROCESS_OPEN_MAX&&found<3u;++i)
        if (!process_open_files[i].used) objects[found++]=i;
    if (found!=3u) { spinlock_irqrestore_release(&vfs_lock,irq); return -1; }
    for (uint32_t i=0u;i<3u;++i) {
        struct vfs_fd *file=&process_open_files[objects[i]];
        file->used=1u; file->refs=1u; file->backend=VFS_BACKEND_TTY;
        file->flags=i==0u?VFS_F_READ:VFS_F_WRITE;
        file->offset=0u; file->path[0]=0;
        p->fd_used[i]=1u; p->fd_object[i]=objects[i];
    }
    spinlock_irqrestore_release(&vfs_lock,irq);
    return 0;
}

int vfs_open_for_process(struct process *p,const char *path,uint32_t flags){
    if(!p)return VFS_FD_INVALID;
    char clean[VFS_PATH_MAX]; if(path_normalize(clean,path)<0)return VFS_FD_INVALID;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    if(p->fd_closing){spinlock_irqrestore_release(&vfs_lock,irq);return VFS_FD_INVALID;}
    uint32_t backend=backend_for(clean);
    if (backend == VFS_BACKEND_EXT2_RO && (flags & VFS_F_WRITE)) {
        spinlock_irqrestore_release(&vfs_lock,irq); return VFS_FD_INVALID;
    }
    if(backend==VFS_BACKEND_NONE){
        if(ext2_vfs_path(clean) || !(flags&VFS_F_WRITE)){spinlock_irqrestore_release(&vfs_lock,irq);return VFS_FD_INVALID;}
        static const char empty[1]={0};
        if(diskfs_available()&&diskfs_write(clean,empty,0)==0)backend=VFS_BACKEND_DISKFS;
        else if(ramfs_write(clean,empty,0)==0)backend=VFS_BACKEND_RAMFS;
        else {spinlock_irqrestore_release(&vfs_lock,irq);return VFS_FD_INVALID;}
    }
    int fd=VFS_FD_INVALID;
    uint32_t object=VFS_PROCESS_OPEN_MAX;
    for(uint32_t i=0;i<PROCESS_FD_MAX;++i)if(!p->fd_used[i]){fd=(int)i;break;}
    if(fd>=0){
        for(uint32_t i=0;i<VFS_PROCESS_OPEN_MAX;++i)
            if(!process_open_files[i].used){object=i;break;}
    }
    if(fd>=0&&object<VFS_PROCESS_OPEN_MAX){
        struct vfs_fd *file=&process_open_files[object];
        file->used=1u;file->refs=1u;file->backend=backend;file->flags=flags;
        file->offset=(flags&VFS_F_APPEND)?file_size(backend,clean):0u;
        uint32_t j=0;while(clean[j]&&j<VFS_PATH_MAX-1u){file->path[j]=clean[j];++j;}file->path[j]=0;
        p->fd_used[fd]=1u;p->fd_object[fd]=object;
    }else fd=VFS_FD_INVALID;
    spinlock_irqrestore_release(&vfs_lock,irq);return fd;
}

int vfs_close_for_process(struct process *p,int fd){
    if(!p||fd<0||(uint32_t)fd>=PROCESS_FD_MAX)return-1;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    if(!p->fd_used[fd]){spinlock_irqrestore_release(&vfs_lock,irq);return-1;}
    release_process_open_file(p->fd_object[fd]);
    p->fd_used[fd]=0u;p->fd_object[fd]=0u;
    spinlock_irqrestore_release(&vfs_lock,irq);return 0;
}

int vfs_fork_process(struct process *parent,struct process *child){
    if(!parent||!child||parent==child)return-1;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    if(parent->fd_closing||child->fd_closing){spinlock_irqrestore_release(&vfs_lock,irq);return-1;}
    for(uint32_t i=0;i<PROCESS_FD_MAX;++i){
        if(parent->fd_used[i]){
            uint32_t object=parent->fd_object[i];
            if(object>=VFS_PROCESS_OPEN_MAX||!process_open_files[object].used||
               !process_open_files[object].refs||process_open_files[object].refs==UINT32_MAX){
                spinlock_irqrestore_release(&vfs_lock,irq);return-1;
            }
        }
        if(child->fd_used[i]){
            spinlock_irqrestore_release(&vfs_lock,irq);return-1;
        }
    }
    for(uint32_t i=0;i<PROCESS_FD_MAX;++i){
        if(parent->fd_used[i]){
            uint32_t object=parent->fd_object[i];
            ++process_open_files[object].refs;
            child->fd_used[i]=1u;child->fd_object[i]=object;
        }
    }
    spinlock_irqrestore_release(&vfs_lock,irq);return 0;
}

void vfs_close_all_process(struct process *p){
    if(!p)return;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    p->fd_closing=1u;
    for(uint32_t i=0;i<PROCESS_FD_MAX;++i){
        if(p->fd_used[i]){
            release_process_open_file(p->fd_object[i]);
            p->fd_used[i]=0u;p->fd_object[i]=0u;
        }
    }
    spinlock_irqrestore_release(&vfs_lock,irq);
}

int vfs_read_for_process(struct process *p,int fd,void *buffer,uint32_t length){
    if(!p||fd<0||(uint32_t)fd>=PROCESS_FD_MAX||!buffer||length>VFS_IO_MAX)return-1;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    if(!p->fd_used[fd]||p->fd_object[fd]>=VFS_PROCESS_OPEN_MAX||!process_open_files[p->fd_object[fd]].used){
        spinlock_irqrestore_release(&vfs_lock,irq);return-1;
    }
    struct vfs_fd *file=&process_open_files[p->fd_object[fd]];
    if(file->backend==VFS_BACKEND_TTY){
        if(!(file->flags&VFS_F_READ)||file->refs==UINT32_MAX){spinlock_irqrestore_release(&vfs_lock,irq);return-1;}
        ++file->refs;
        uint32_t sid=p->sid,object=p->fd_object[fd];
        spinlock_irqrestore_release(&vfs_lock,irq);
        int r=tty_read(sid,(uint8_t*)buffer,length);
        irq=spinlock_irqsave_acquire(&vfs_lock);release_process_open_file(object);
        spinlock_irqrestore_release(&vfs_lock,irq);return r;
    }
    int r=read_slot(file,buffer,length);
    spinlock_irqrestore_release(&vfs_lock,irq);return r;
}

int vfs_write_for_process(struct process *p,int fd,const void *buffer,uint32_t length){
    if(!p||fd<0||(uint32_t)fd>=PROCESS_FD_MAX||(!buffer&&length)||length>VFS_IO_MAX)return-1;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    if(!p->fd_used[fd]||p->fd_object[fd]>=VFS_PROCESS_OPEN_MAX||!process_open_files[p->fd_object[fd]].used){
        spinlock_irqrestore_release(&vfs_lock,irq);return-1;
    }
    struct vfs_fd *file=&process_open_files[p->fd_object[fd]];
    if(file->backend==VFS_BACKEND_TTY){
        if(!(file->flags&VFS_F_WRITE)||file->refs==UINT32_MAX){spinlock_irqrestore_release(&vfs_lock,irq);return-1;}
        ++file->refs;
        uint32_t sid=p->sid,object=p->fd_object[fd];
        spinlock_irqrestore_release(&vfs_lock,irq);
        int r=tty_write(sid,(const uint8_t*)buffer,length);
        irq=spinlock_irqsave_acquire(&vfs_lock);release_process_open_file(object);
        spinlock_irqrestore_release(&vfs_lock,irq);return r;
    }
    int r=write_slot(file,buffer,length);
    spinlock_irqrestore_release(&vfs_lock,irq);return r;
}

int vfs_open(const char *path,uint32_t flags){char clean[VFS_PATH_MAX];if(path_normalize(clean,path)<0)return VFS_FD_INVALID;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=open_slot(kernel_fds,clean,flags);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_close(int fd){if(fd<0||fd>=(int)VFS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=close_slot(&kernel_fds[fd]);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_read(int fd,void *buffer,uint32_t length){if(fd<0||fd>=(int)VFS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=read_slot(&kernel_fds[fd],buffer,length);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_write(int fd,const void *buffer,uint32_t length){if(fd<0||fd>=(int)VFS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=write_slot(&kernel_fds[fd],buffer,length);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_remove(const char*path){char clean[VFS_PATH_MAX];if(path_normalize(clean,path)<0)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=ext2_vfs_path(clean)?-1:(ramfs_data(clean)?ramfs_remove(clean):(diskfs_available()&&diskfs_exists(clean)?diskfs_remove(clean):-1));spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_stat_path(const char*path,struct vfs_stat*st){char clean[VFS_PATH_MAX];if(!st||path_normalize(clean,path)<0)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);uint32_t ext2_size=0u;int ext2_directory=0;if(ext2_vfs_stat(clean,&ext2_size,&ext2_directory)==0){st->size=ext2_size;st->backend=VFS_BACKEND_EXT2_RO;st->flags=VFS_STAT_READ_ONLY|(ext2_directory?VFS_STAT_DIRECTORY:0u);spinlock_irqrestore_release(&vfs_lock,irq);return 0;}if(ramfs_data(clean)){st->size=ramfs_size(clean);st->backend=VFS_BACKEND_RAMFS;st->flags=0;spinlock_irqrestore_release(&vfs_lock,irq);return 0;}if(diskfs_available()&&diskfs_exists(clean)){st->size=diskfs_size(clean);st->backend=VFS_BACKEND_DISKFS;st->flags=0;spinlock_irqrestore_release(&vfs_lock,irq);return 0;}spinlock_irqrestore_release(&vfs_lock,irq);return-1;}
int vfs_lstat_path(const char *path, struct vfs_stat *st) {
    char clean[VFS_PATH_MAX];
    uint32_t size = 0u;
    int is_directory = 0;
    int is_symlink = 0;
    uint32_t irq;
    if (!st || path_normalize(clean, path) < 0) return -1;
    irq = spinlock_irqsave_acquire(&vfs_lock);
    if (ext2_vfs_path(clean)) {
        if (ext2_vfs_lstat(clean, &size, &is_directory, &is_symlink) < 0) {
            spinlock_irqrestore_release(&vfs_lock, irq);
            return -1;
        }
        st->size = size;
        st->backend = VFS_BACKEND_EXT2_RO;
        st->flags = VFS_STAT_READ_ONLY |
                    (is_directory ? VFS_STAT_DIRECTORY : 0u) |
                    (is_symlink ? VFS_STAT_SYMLINK : 0u);
        spinlock_irqrestore_release(&vfs_lock, irq);
        return 0;
        }
    if (ramfs_data(clean)) {
        st->size = ramfs_size(clean); st->backend = VFS_BACKEND_RAMFS;
        st->flags = 0u;
        spinlock_irqrestore_release(&vfs_lock, irq); return 0;
    }
    if (diskfs_available() && diskfs_exists(clean)) {
        st->size = diskfs_size(clean); st->backend = VFS_BACKEND_DISKFS;
        st->flags = 0u;
        spinlock_irqrestore_release(&vfs_lock, irq); return 0;
    }
    spinlock_irqrestore_release(&vfs_lock, irq);
    return -1;
}

int vfs_readlink(const char *path, char *buffer, uint32_t capacity) {
    char clean[VFS_PATH_MAX];
    int result;
    uint32_t irq;
    if (!buffer || capacity == 0u || capacity > VFS_IO_MAX ||
        path_normalize(clean, path) < 0) return -1;
    irq = spinlock_irqsave_acquire(&vfs_lock);
    result = ext2_vfs_readlink(clean, buffer, capacity);
    spinlock_irqrestore_release(&vfs_lock, irq);
    return result;
}

const char*vfs_name(uint32_t index){uint32_t n=ramfs_count();if(index<n)return ramfs_name(index);return diskfs_name(index-n);}
uint32_t vfs_count(void){uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);uint32_t n=ramfs_count()+(diskfs_available()?diskfs_count():0u);spinlock_irqrestore_release(&vfs_lock,irq);return n;}

int vfs_readdir(const char *path, uint32_t *offset,
                struct vfs_dirent *entry) {
    char clean[VFS_PATH_MAX];
    struct ext2_ro_dirent ext2_entry;
    int result;
    uint32_t irq;

    if (!offset || !entry || path_normalize(clean, path) < 0)
        return -1;
    irq = spinlock_irqsave_acquire(&vfs_lock);
    if (!ext2_vfs_path(clean)) {
        spinlock_irqrestore_release(&vfs_lock, irq);
        return -1;
    }
    result = ext2_vfs_readdir(clean, offset, &ext2_entry);
    if (result > 0) {
        entry->inode = ext2_entry.inode;
        entry->type = ext2_entry.type;
        for (uint32_t i = 0u; i <= VFS_DIRENT_NAME_MAX; ++i)
            entry->name[i] = ext2_entry.name[i];
    }
    spinlock_irqrestore_release(&vfs_lock, irq);
    return result;
}
