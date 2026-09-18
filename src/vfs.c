#include <stdint.h>
#include "diskfs.h"
#include "ramfs.h"
#include "vfs.h"
#include "process.h"
#include "spinlock.h"

#define VFS_F_READ 1u
#define VFS_F_WRITE 2u
#define VFS_F_APPEND 4u

struct vfs_fd { uint32_t used, backend, flags, offset; char path[VFS_PATH_MAX]; };
static struct vfs_fd kernel_fds[VFS_FD_MAX];
static struct spinlock vfs_lock;

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
    return VFS_BACKEND_NONE;
}
static int fd_valid(const struct vfs_fd *f) { return f && f->used; }
static uint32_t file_size(uint32_t backend,const char *path) { return backend==VFS_BACKEND_DISKFS?diskfs_size(path):ramfs_size(path); }
static int open_slot(struct vfs_fd *fds,const char *path,uint32_t flags) {
    uint32_t backend=backend_for(path);
    if (backend==VFS_BACKEND_NONE) {
        if (!(flags&VFS_F_WRITE)) return VFS_FD_INVALID;
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
    if(f->backend==VFS_BACKEND_DISKFS) {
        uint8_t temp[VFS_IO_MAX]; if(diskfs_read(f->path,temp,sizeof(temp))<0)return -1;
        for(uint32_t i=0;i<wanted;++i)((uint8_t*)buffer)[i]=temp[f->offset+i];
    } else {
        const uint8_t *src=(const uint8_t*)ramfs_data(f->path); if(!src)return -1;
        for(uint32_t i=0;i<wanted;++i)((uint8_t*)buffer)[i]=src[f->offset+i];
    }
    f->offset+=wanted; return (int)wanted;
}
static int write_slot(struct vfs_fd *f,const void *buffer,uint32_t length) {
    if(!fd_valid(f)||!buffer||length>VFS_IO_MAX||(f->flags&VFS_F_WRITE)==0)return -1;
    if(f->offset!=0 && !(f->flags&VFS_F_APPEND))return -1;
    int r=f->backend==VFS_BACKEND_DISKFS?diskfs_write(f->path,buffer,length):ramfs_write(f->path,buffer,length);
    if(r<0)return -1; f->offset=length; return (int)length;
}

int vfs_init(void){spinlock_init(&vfs_lock);for(uint32_t i=0;i<VFS_FD_MAX;++i)kernel_fds[i].used=0;return 0;}

int vfs_open_for_process(struct process *p,const char *path,uint32_t flags){
    if(!p)return VFS_FD_INVALID;
    char clean[VFS_PATH_MAX]; if(path_normalize(clean,path)<0)return VFS_FD_INVALID;
    uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);
    uint32_t backend=backend_for(clean);
    if(backend==VFS_BACKEND_NONE){
        if(!(flags&VFS_F_WRITE)){spinlock_irqrestore_release(&vfs_lock,irq);return VFS_FD_INVALID;}
        static const char empty[1]={0};
        if(diskfs_available()&&diskfs_write(clean,empty,0)==0)backend=VFS_BACKEND_DISKFS;
        else if(ramfs_write(clean,empty,0)==0)backend=VFS_BACKEND_RAMFS;
        else {spinlock_irqrestore_release(&vfs_lock,irq);return VFS_FD_INVALID;}
    }
    int fd=VFS_FD_INVALID;
    for(uint32_t i=0;i<PROCESS_FD_MAX;++i)if(!p->fd_used[i]){
        p->fd_used[i]=1;p->fd_backend[i]=(uint8_t)backend;p->fd_flags[i]=flags;
        p->fd_offset[i]=(flags&VFS_F_APPEND)?file_size(backend,clean):0u;
        uint32_t j=0;while(clean[j]){p->fd_path[i][j]=clean[j];++j;}p->fd_path[i][j]=0;
        fd=(int)i;break;
    }
    spinlock_irqrestore_release(&vfs_lock,irq);return fd;
}
int vfs_close_for_process(struct process *p,int fd){if(!p||fd<0||fd>=PROCESS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=p->fd_used[fd]?0:-1;p->fd_used[fd]=0;spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_read_for_process(struct process *p,int fd,void *buffer,uint32_t length){if(!p||fd<0||fd>=PROCESS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);if(!p->fd_used[fd]){spinlock_irqrestore_release(&vfs_lock,irq);return-1;}struct vfs_fd f={1u,p->fd_backend[fd],p->fd_flags[fd],p->fd_offset[fd],{0}};for(uint32_t i=0;i<VFS_PATH_MAX;++i)f.path[i]=p->fd_path[fd][i];int r=read_slot(&f,buffer,length);p->fd_offset[fd]=f.offset;spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_write_for_process(struct process *p,int fd,const void *buffer,uint32_t length){if(!p||fd<0||fd>=PROCESS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);if(!p->fd_used[fd]){spinlock_irqrestore_release(&vfs_lock,irq);return-1;}struct vfs_fd f={1u,p->fd_backend[fd],p->fd_flags[fd],p->fd_offset[fd],{0}};for(uint32_t i=0;i<VFS_PATH_MAX;++i)f.path[i]=p->fd_path[fd][i];int r=write_slot(&f,buffer,length);p->fd_offset[fd]=f.offset;spinlock_irqrestore_release(&vfs_lock,irq);return r;}

int vfs_open(const char *path,uint32_t flags){char clean[VFS_PATH_MAX];if(path_normalize(clean,path)<0)return VFS_FD_INVALID;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=open_slot(kernel_fds,clean,flags);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_close(int fd){if(fd<0||fd>=(int)VFS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=close_slot(&kernel_fds[fd]);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_read(int fd,void *buffer,uint32_t length){if(fd<0||fd>=(int)VFS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=read_slot(&kernel_fds[fd],buffer,length);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_write(int fd,const void *buffer,uint32_t length){if(fd<0||fd>=(int)VFS_FD_MAX)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=write_slot(&kernel_fds[fd],buffer,length);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_remove(const char*path){char clean[VFS_PATH_MAX];if(path_normalize(clean,path)<0)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);int r=ramfs_data(clean)?ramfs_remove(clean):(diskfs_available()&&diskfs_exists(clean)?diskfs_remove(clean):-1);spinlock_irqrestore_release(&vfs_lock,irq);return r;}
int vfs_stat_path(const char*path,struct vfs_stat*st){char clean[VFS_PATH_MAX];if(!st||path_normalize(clean,path)<0)return-1;uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);if(ramfs_data(clean)){st->size=ramfs_size(clean);st->backend=VFS_BACKEND_RAMFS;st->flags=0;spinlock_irqrestore_release(&vfs_lock,irq);return 0;}if(diskfs_available()&&diskfs_exists(clean)){st->size=diskfs_size(clean);st->backend=VFS_BACKEND_DISKFS;st->flags=0;spinlock_irqrestore_release(&vfs_lock,irq);return 0;}spinlock_irqrestore_release(&vfs_lock,irq);return-1;}
const char*vfs_name(uint32_t index){uint32_t n=ramfs_count();if(index<n)return ramfs_name(index);return diskfs_name(index-n);}
uint32_t vfs_count(void){uint32_t irq=spinlock_irqsave_acquire(&vfs_lock);uint32_t n=ramfs_count()+(diskfs_available()?diskfs_count():0u);spinlock_irqrestore_release(&vfs_lock,irq);return n;}
