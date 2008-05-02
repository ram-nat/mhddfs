/*
	mhddfs - Multi HDD [FUSE] File System
	Copyright (C) 2008 Dmitry E. Oboukhov <dimka@avanto.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#define _XOPEN_SOURCE 500
#include <fuse.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>

#include "parse_options.h"
#include "tools.h"

#include "debug.h"

// getattr
static int mhdd_stat(const char *file_name, struct stat *buf)
{
  mhdd_debug(MHDD_MSG, "mhdd_stat: %s\n", file_name);
  char *path=find_path(file_name);
  if (path)
  {
    int ret=lstat(path, buf);
    free(path);
    if (ret==-1) return -errno;
    return 0;
  }
  errno=ENOENT;
  return -errno;
}

//statvfs
static int mhdd_statfs(const char *path, struct statvfs *buf)
{
  int i;
  struct statvfs * stats;

  mhdd_debug(MHDD_MSG, "mhdd_statfs: %s\n", path);

  stats=calloc(mhdd.cdirs, sizeof(struct statvfs));

  for (i=0; i<mhdd.cdirs; i++)
  {
    int ret=statvfs(mhdd.dirs[i], stats+i);
    if (ret!=0)
    {
      free(stats);
      return -errno;
    }
  }

  unsigned long 
    min_block=stats[0].f_bsize,
    min_frame=stats[0].f_frsize;

  for (i=1; i<mhdd.cdirs; i++)
  {
    if (min_block>stats[i].f_bsize) min_block=stats[i].f_bsize;
    if (min_frame>stats[i].f_frsize) min_frame=stats[i].f_frsize;
  }
  if (!min_block) min_block=512;
  if (!min_frame) min_frame=512;

  for (i=0; i<mhdd.cdirs; i++)
  {
    if (stats[i].f_bsize>min_block)
    {
      stats[i].f_bfree    *=  stats[i].f_bsize/min_block;
      stats[i].f_bavail   *=  stats[i].f_bsize/min_block;
      stats[i].f_bsize    =   min_block;
    }
    if (stats[i].f_frsize>min_frame)
    {
      stats[i].f_blocks   *=  stats[i].f_frsize/min_frame;
      stats[i].f_frsize   =   min_frame;
    }
  }

  memcpy(buf, stats, sizeof(struct statvfs));
  
  for (i=1; i<mhdd.cdirs; i++)
  {
    if (buf->f_namemax<stats[i].f_namemax)
    {
      buf->f_namemax=stats[i].f_namemax;
    }
    buf->f_ffree  +=  stats[i].f_ffree;
    buf->f_files  +=  stats[i].f_files;
    buf->f_favail +=  stats[i].f_favail;
    buf->f_bavail +=  stats[i].f_bavail;
    buf->f_bfree  +=  stats[i].f_bfree;
    buf->f_blocks +=  stats[i].f_blocks;
  }
  free(stats);

  return 0;
}

static int mhdd_readdir(
  const char *dirname,
  void *buf,
  fuse_fill_dir_t filler,
  off_t offset,
  struct fuse_file_info * fi)
{
  int i, j, found;
  
  mhdd_debug(MHDD_MSG, "mhdd_readdir: %s\n", dirname);
  char **dirs=(char **)calloc(mhdd.cdirs+1, sizeof(char *));
  
  struct dir_item
  {
    char            * name;
    struct stat     * st;
    struct dir_item * next, * prev;
  } *dir=0, *item;


  struct stat st;

  // find all dirs
  for(i=j=found=0; i<mhdd.cdirs; i++)
  {
    char *path=create_path(mhdd.dirs[i], dirname);
    if (stat(path, &st)==0)
    {
      found++;
      if (S_ISDIR(st.st_mode))
      {
        dirs[j]=path;
        j++;
        continue;
      }
    }
    free(path);
  }

  // dirs not found
  if (dirs[0]==0)
  {
    errno=ENOENT;
    if (found) errno=ENOTDIR;
    free(dirs);
    return -errno;
  }

  // read directories
  for (i=0; dirs[i]; i++)
  {
    DIR * dh=opendir(dirs[i]);
    if (!dh) continue;
    struct dirent *de;
    
    while((de=readdir(dh)))
    {
      // find dups
      struct dir_item *prev;
      for(prev=dir; prev; prev=prev->next)
        if (strcmp(prev->name, de->d_name)==0) break;
      if (prev) continue;

      // add item
      char *object_name=create_path(dirs[i], de->d_name);
      struct dir_item *new_item=calloc(1, sizeof(struct dir_item));
      
      new_item->name=calloc(strlen(de->d_name)+1, sizeof(char));
      strcpy(new_item->name, de->d_name);
      new_item->st=calloc(1, sizeof(struct stat));
      lstat(object_name, new_item->st);

      if (dir) { dir->prev=new_item; new_item->next=dir; }
      dir=new_item;
      free(object_name);
    }
    
    closedir(dh);
  }

  // fill list
  for(item=dir; item; item=item->next)
  {
    if (filler(buf, item->name, item->st, 0)) break;
  }

  // free memory
  while(dir)
  {
    free(dir->name);
    free(dir->st);
    if (dir->next) 
    { 
      dir=dir->next; 
      free(dir->prev); 
      continue;
    }
    free(dir); dir=0;
  }
  for (i=0; dirs[i]; i++) free(dirs[i]);
  free(dirs);
  return 0;
}

// readlink
static int mhdd_readlink(const char *path, char *buf, size_t size)
{
  mhdd_debug(MHDD_MSG, "mhdd_readlink: %s, size=%d\n", path, size);

  char *link=find_path(path);
  if (link)
  {
    memset(buf, 0, size);
    int res=readlink(link, buf, size);
    free(link);
    if (res>=0) return 0;
  }
  return -1;
}

#define CREATE_FUNCTION 0
#define OPEN_FUNCION    1
// create or open
static int mhdd_internal_open(const char *file,
  mode_t mode, struct fuse_file_info *fi, int what)
{
  mhdd_debug(MHDD_INFO, 
    "mhdd_internal_open: %s, flags=0x%X\n", file, fi->flags);
  int dir_id, fd;

  char *path=find_path(file);

  if (path)
  {
    if (what==CREATE_FUNCTION) fd=open(path, fi->flags, mode);
    else fd=open(path, fi->flags);
    if (fd==-1) { free(path); return -errno; }
    struct files_info *add=add_file_list(file, path, fi->flags, fd);
    fi->fh=add->id;
    free(path);
    return 0;
  }
  
  dir_id=get_free_dir();
  create_parent_dirs(dir_id, file);
  path=create_path(mhdd.dirs[dir_id], file);

  if (what==CREATE_FUNCTION) fd=open(path, fi->flags, mode);
  else fd=open(path, fi->flags);

  if (fd==-1)
  {
    free(path);
    return -errno;
  }

  if (getuid()==0) 
  {
    struct fuse_context * fcontext = fuse_get_context();
    if (fchown(fd, fcontext->uid, fcontext->gid)!=0)
    {
      mhdd_debug(MHDD_INFO, "mhdd_internal_open: error: "
          "can not set owner %d:%d to %s: %s\n",
          (int)fcontext->uid, (int)fcontext->gid, path, strerror(errno));
    }
  }
  struct files_info *add=add_file_list(file, path, fi->flags, fd);
  fi->fh=add->id;
  free(path);
  return 0;
}

// create
static int mhdd_create(const char *file, 
  mode_t mode, struct fuse_file_info *fi)
{
  mhdd_debug(MHDD_MSG, "mhdd_create: %s, mode=%X\n", file, fi->flags);
  int res=mhdd_internal_open(file, mode, fi, CREATE_FUNCTION);
  if (res!=0)
    mhdd_debug(MHDD_INFO, "mhdd_create: error: %s\n", strerror(-res));
  return res;
}

// open
static int mhdd_fileopen(const char *file, struct fuse_file_info *fi)
{
  mhdd_debug(MHDD_MSG, "mhdd_fileopen: %s, flags=%04X\n", file, fi->flags);
  int res=mhdd_internal_open(file, 0, fi, OPEN_FUNCION);
  if (res!=0)
    mhdd_debug(MHDD_INFO, "mhdd_fileopen: error: %s\n", strerror(-res));
  return res;
}

// close
static int mhdd_release(const char *path, struct fuse_file_info *fi)
{
  mhdd_debug(MHDD_MSG, "mhdd_release: %s, handle=%lld\n", path, fi->fh);

  lock_files();
  struct files_info *del=get_info_by_id(fi->fh);
  if (!del)
  {
    mhdd_debug(MHDD_INFO, "mhdd_release: unknown file number: %llu\n", fi->fh);
    errno=EBADF;
    unlock_files();
    return -errno;
  }

  int fh=del->fh;
  unlock_files();

  close(fh);
  del_file_list(del);
  return 0;
}

// read
static int mhdd_read(const char *path, char *buf, size_t count, off_t offset,
         struct fuse_file_info *fi)
{
  ssize_t res;
  lock_files();
  struct files_info *info=get_info_by_id(fi->fh);
  mhdd_debug(MHDD_INFO, "mhdd_read: %s, handle=%lld\n", path, fi->fh);

  if (!info)
  {
    unlock_files();
    errno=EBADF;
    return -errno;
  }

  pthread_mutex_lock(&info->lock);
  unlock_files();

  res=pread(info->fh, buf, count, offset);
  pthread_mutex_unlock(&info->lock);
  if (res==-1) return -errno;
  return res;
}

// write
static int mhdd_write(const char *path, const char *buf, size_t count,
  off_t offset, struct fuse_file_info *fi)
{
  ssize_t res;
  lock_files();
  struct files_info *info=get_info_by_id(fi->fh);
  mhdd_debug(MHDD_INFO, "mhdd_write: %s, handle=%lld\n", path, fi->fh);

  if (!info)
  {
    unlock_files();
    errno=EBADF;
    return -errno;
  }
  
  pthread_mutex_lock(&info->lock);
  unlock_files();

  res=pwrite(info->fh, buf, count, offset);
  if (res==-1)
  {
    // end free space
    if (errno==ENOSPC)
    {
      if (move_file(info, offset+count)==0) 
      {
        res=pwrite(info->fh, buf, count, offset);
        pthread_mutex_unlock(&info->lock);
        if (res==-1) 
        {
          mhdd_debug(MHDD_DEBUG, "mhdd_write: error restart write: %s\n",
            strerror(errno));
          return -errno;
        }
        return res;
      }
      errno=ENOSPC;
    }
    pthread_mutex_unlock(&info->lock);
    return -errno;
  }
  pthread_mutex_unlock(&info->lock);
  return res;
}

// truncate
static int mhdd_truncate(const char *path, off_t size)
{
  char *file=find_path(path);
  mhdd_debug(MHDD_MSG, "mhdd_truncate: %s\n", path);
  if (file)
  {
    int res=truncate(file, size);
    free(file);
    if (res==-1) return -errno;
    return 0;
  }
  errno=ENOENT;
  return -errno;
}

// ftrucate
static int mhdd_ftruncate(const char *path, off_t size, 
  struct fuse_file_info *fi)
{
  int res;
  lock_files();
  struct files_info *info=get_info_by_id(fi->fh);
  mhdd_debug(MHDD_MSG, "mhdd_ftruncate: %s, handle=%lld\n", path, fi->fh);

  if (!info)
  {
    unlock_files();
    errno=EBADF;
    return -errno;
  }

  int fh=info->fh;
  unlock_files();
  res=ftruncate(fh, size);
  if (res==-1) return -errno;
  return 0;
}

// access
static int mhdd_access(const char *path, int mask)
{
  mhdd_debug(MHDD_MSG, "mhdd_access: %s mode=%04X\n", path, mask);
  char *file=find_path(path);
  if (file)
  {
    int res=access(file, mask);
    free(file);
    if (res==-1) return -errno;
    return 0;
  }
  
  errno=ENOENT;
  return -errno;
}

// mkdir
static int mhdd_mkdir(const char * path, mode_t mode)
{
  mhdd_debug(MHDD_MSG, "mhdd_mkdir: %s mode=%04X\n", path, mode);

  char *parent=get_parent_path(path);
  if (!parent) { errno=EFAULT; return -errno; }

  int i, new_dir_id, dir_id;
  new_dir_id=find_path_id(parent);
  if (new_dir_id==-1) { free(parent); errno=EFAULT; return -errno; }

  for (dir_id=-1, i=0; i<3; i++)
  {
    if (new_dir_id!=dir_id && new_dir_id!=-1)
    {
      char *dir_path=create_path(mhdd.dirs[new_dir_id], path);
      create_parent_dirs(new_dir_id, path);
      int res=mkdir(dir_path, mode);
      if (res==0)
      {
        if (getuid()==0)
        {
          struct fuse_context * fcontext = fuse_get_context();
          chown(dir_path, fcontext->uid, fcontext->gid);
        }
        free(dir_path);
        free(parent);
        return 0;
      }
      free(dir_path);
      if (errno!=ENOSPC) { free(parent); return -errno; }
    }
    dir_id=new_dir_id;
    switch(i)
    {
      case 0: new_dir_id=get_free_dir_by_path(parent); break;
      case 1: new_dir_id=get_free_dir(); break;
    }
  }
  free(parent);
  return -errno;
}

// rmdir
static int mhdd_rmdir(const char * path)
{
  mhdd_debug(MHDD_MSG, "mhdd_rmdir: %s\n", path);
  char *dir;
  while((dir=find_path(path)))
  {
    int res=rmdir(dir);
    free(dir);
    if (res==-1) return -errno;
  }
  return 0;
}

// unlink
static int mhdd_unlink(const char *path)
{
  mhdd_debug(MHDD_MSG, "mhdd_unlink: %s\n", path);
  char *file=find_path(path);
  if (!file)
  {
    errno=ENOENT;
    return -errno;
  }
  int res=unlink(file);
  free(file);
  if (res==-1) return -errno;
  return 0;
}

// rename
static int mhdd_rename(const char *from, const char *to)
{
  mhdd_debug(MHDD_MSG, "mhdd_rename: from=%s to=%s\n", from, to);
  int from_dir_id=find_path_id(from);

  if (from_dir_id==-1)
  {
    errno=ENOENT;
    return -errno;
  }

  char * to_parent=get_parent_path(to);
  if (to_parent) 
  {
    char * to_find_parent=find_path(to_parent);
    free(to_parent);
    
    if (!to_find_parent)
    {
      errno=ENOENT;
      return -errno;
    }
    free(to_find_parent);
    // to-parent exists
    // from exists
    create_parent_dirs(from_dir_id, to);
    char *obj_to    = create_path(mhdd.dirs[from_dir_id], to);
    char *obj_from  = create_path(mhdd.dirs[from_dir_id], from);

    int res=rename(obj_from, obj_to);
    free(obj_to);
    free(obj_from);

    if (res==-1) return -errno;

    if (find_path_id(from)==-1) return 0;
    // rename directories
    return mhdd_rename(from, to);
  }
  else
  {
    errno=ENOENT;
    return -errno;
  }
  return 0;
}

// .utimens
static int mhdd_utimens(const char *path, const struct timespec ts[2])
{
  mhdd_debug(MHDD_MSG, "mhdd_utimens: %s\n", path);
  int i, res, flag_found;

  for (i=flag_found=0; i<mhdd.cdirs; i++)
  {
    char *object=create_path(mhdd.dirs[i], path);
    struct stat st;
    if (lstat(object, &st)!=0) { free(object); continue; }
    
    flag_found=1;
    struct timeval tv[2];

    tv[0].tv_sec = ts[0].tv_sec;
    tv[0].tv_usec = ts[0].tv_nsec / 1000;
    tv[1].tv_sec = ts[1].tv_sec;
    tv[1].tv_usec = ts[1].tv_nsec / 1000;

    res = utimes(object, tv);
    free(object);
    if (res == -1) return -errno;
  }
  if (flag_found) return 0;
  errno=ENOENT;
  return -errno;
}

// .chmod
static int mhdd_chmod(const char *path, mode_t mode)
{
  mhdd_debug(MHDD_MSG, "mhdd_chmod: mode=0x%03X %s\n", mode, path);
  int i, res, flag_found;

  for (i=flag_found=0; i<mhdd.cdirs; i++)
  {
    char *object=create_path(mhdd.dirs[i], path);
    struct stat st;
    if (lstat(object, &st)!=0) { free(object); continue; }
    
    flag_found=1;
    res=chmod(object, mode);
    free(object);
    if (res == -1) return -errno;
  }
  if (flag_found) return 0;
  errno=ENOENT;
  return -errno;
}

// chown
static int mhdd_chown(const char *path, uid_t uid, gid_t gid)
{
  mhdd_debug(MHDD_MSG, "mhdd_chown: pid=0x%03X gid=%03X %s\n", uid, gid, path);
  int i, res, flag_found;

  for (i=flag_found=0; i<mhdd.cdirs; i++)
  {
    char *object=create_path(mhdd.dirs[i], path);
    struct stat st;
    if (lstat(object, &st)!=0) { free(object); continue; }
    
    flag_found=1;
    res=lchown(object, uid, gid);
    free(object);
    if (res == -1) return -errno;
  }
  if (flag_found) return 0;
  errno=ENOENT;
  return -errno;
}

// symlink
static int mhdd_symlink(const char *from, const char *to)
{
  mhdd_debug(MHDD_MSG, "mhdd_symlink: from=%s to=%s\n", from, to);
  int i, res;
  char *parent=get_parent_path(to);
  if (!parent) 
  {
    errno=ENOENT;
    return -errno;
  }

  int dir_id=find_path_id(parent);
  free(parent);
  
  if (dir_id==-1)
  {
    errno=ENOENT;
    return -errno;
  }

  for (i=0; i<2; i++)
  {
    if (i)
    {
      dir_id=get_free_dir();
      create_parent_dirs(dir_id, to);
    }

    char *path_to=create_path(mhdd.dirs[dir_id], to);

    res=symlink(from, path_to);
    free(path_to);
    if (res==0) return 0;
    if (errno!=ENOSPC) return -errno;
  }
  return -errno;
}

// mknod
static int mhdd_mknod(const char *path, mode_t mode, dev_t rdev)
{
  mhdd_debug(MHDD_MSG, "mhdd_mknod: path=%s mode=%X\n", path, mode);
  int res, i;
  char *nod;

  char *parent=get_parent_path(path);
  if (!parent)
  {
    errno=ENOENT;
    return -errno;
  }

  int dir_id=find_path_id(parent);
  free(parent);

  if (dir_id==-1)
  {
    errno=ENOENT;
    return -errno;
  }

  for (i=0; i<2; i++)
  {
    if (i)
    {
      dir_id=get_free_dir();
      create_parent_dirs(dir_id, path);
    }
    nod=create_path(mhdd.dirs[dir_id], path);
    
    if (S_ISREG(mode)) 
    {
      res = open(nod, O_CREAT | O_EXCL | O_WRONLY, mode);
      if (res >= 0) res = close(res);
    } else if (S_ISFIFO(mode)) res = mkfifo(nod, mode);
    else  res = mknod(nod, mode, rdev);
    if (res!=-1)
    {
      if (getuid()==0)
      {
        struct fuse_context * fcontext = fuse_get_context();
        chown(nod, fcontext->uid, fcontext->gid);
      }
      free(nod);
      return 0;
    }
    free(nod);
    if (errno!=ENOSPC) return -errno;
  }
  return -errno;
}

//fsync
static int mhdd_fsync(const char *path, int isdatasync, 
  struct fuse_file_info *fi)
{
  mhdd_debug(MHDD_MSG, "mhdd_fsync: path=%s handle=%llu\n", path, fi->fh);
  lock_files();
  struct files_info *info=get_info_by_id(fi->fh);
  int res;
  if (!info)
  {
    errno=EBADF;
    unlock_files();
    return -errno;
  }

  int fh=info->fh;
  unlock_files();

#ifdef HAVE_FDATASYNC
  if (isdatasync)
    res = fdatasync(fh);
  else
#endif
    res = fsync(fh);
  if (res == -1) return -errno;
  return 0;
}

// functions links
static struct fuse_operations mhdd_oper = 
{
  .getattr    = mhdd_stat,
  .statfs     = mhdd_statfs,
  .readdir    = mhdd_readdir,
  .readlink   = mhdd_readlink,
  .open       = mhdd_fileopen,
  .release    = mhdd_release,
  .read       = mhdd_read,
  .write      = mhdd_write,
  .create     = mhdd_create,
  .truncate   = mhdd_truncate,
  .ftruncate  = mhdd_ftruncate,
  .access     = mhdd_access,
  .mkdir      = mhdd_mkdir,
  .rmdir      = mhdd_rmdir,
  .unlink     = mhdd_unlink,
  .rename     = mhdd_rename,
  .utimens    = mhdd_utimens,
  .chmod      = mhdd_chmod,
  .chown      = mhdd_chown,
  .symlink    = mhdd_symlink,
  .mknod      = mhdd_mknod,
  .fsync      = mhdd_fsync,
};


// start
int main(int argc, char *argv[])
{
  struct fuse_args *args=parse_options(argc, argv);
  mhdd_tools_init();
  return fuse_main(args->argc, args->argv, &mhdd_oper, 0);
}
