/* Read-only game RomFS, backed by the player's fully IVFC-verified image. */
#include "runtime.h"
#include "totk_ui_lang.h"
#include "game_data.h"
#include "game_version.h"
#include "fs_host.h"
#include "romfs_path.h"
#include "romfs_scan.h"
#include "mod_ips.h"   /* mod_exefs_dir */
#include "host_os.h"   /* SHA-256: CommonCrypto on Darwin */
#include "totk_drs.h"
#if !defined(__APPLE__)
#define st_mtimespec st_mtim   /* Linux spells the nanosecond modification time st_mtim */
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "romfs_proof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536
/* The image's size and SHA-256 and the index's SHA-256 from the player's verified extraction: romfs-config.txt in the game
   data folder (game_data.h; scripts/prepare_romfs_backend.py on a PC, the app's setup on the device), read when the RomFS
   is opened. A pinned build (TOTK_PINNED, Android 1.4.3) accepts only the pinned values, so the player's file cannot point
   the runtime at another image. */
#if TOTK_PINNED
#include "totk_pins.h"
#endif
static uint64_t romfs_image_size;static const char *romfs_image_sha256,*romfs_index_sha256;
#define ROMFS_IMAGE_SIZE romfs_image_size
#define ROMFS_IMAGE_SHA256 romfs_image_sha256
#define ROMFS_INDEX_SHA256 romfs_index_sha256
static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count,strings_offset;
static int image_fd=-1;
/* Split image: romfs.bin.000, .001, ... of equal size (the last one shorter), read in place. The phone gets the 17 GB
   image this way: devicectl drops the connection on a single file of that size, and joining the parts on the device
   would need the space twice. image_fd is then the first part; src_pread routes image offsets to their part. */
#define MAX_CHUNKS 64
static int chunk_fds[MAX_CHUNKS];
static unsigned nchunks;
static uint64_t chunk_size;
static ssize_t src_pread(int fd,void *buf,size_t n,off_t off) {
 if(!nchunks||fd!=image_fd)return pread(fd,buf,n,off);
 uint64_t c=(uint64_t)off/chunk_size,o=(uint64_t)off%chunk_size;if(c>=nchunks)return 0;
 if(n>chunk_size-o)n=(size_t)(chunk_size-o);
 return pread(chunk_fds[c],buf,n,(off_t)o);
}
static const uint8_t *image_map;
static char mount_name[32];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t read_calls,read_bytes,read_ns,read_prepare_ns,read_report_ns;
static uint64_t handles[NH]; /* file index + 1; 0 is free */
/* Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
   path, the overlay's size and SHA-256 and the original's SHA-256; a missing path, a size or hash mismatch or a changed
   original stops the start. The directories, in this order: the build's patches (scripts/make_romfs_patches.py;
   TOTK_ROMFS_PATCHES, default extracted/<version>/romfs-patches, 0 = none), the true-2K HeapDef overlay
   (scripts/make_output_res_overlay.py) and the player's mods (TOTK_ROMFS_MODS, which the launcher sets to
   <data>/mods/<version> and the Android app to the first mods/<version> it finds, frontend_android.c; no manifest there =
   none, unless it holds a mod manager's romfs/ folder, which mods_load scans instead). Each file can be replaced once. An overlay file is opened when the
   game opens it, so a mod can replace thousands of files (a dub's voice streams) without holding a descriptor each; the
   file's identity (device, inode, size, modification time) is kept from the start check, and an open that finds another
   file there stops the game instead of serving bytes nobody checked. scripts/make_mod_manifest.py writes a mod's manifest.
   A line whose original SHA-256 is "-" adds a file the RomFS lacks (issue #48: The Second Quest adds 874): it is numbered
   after the RomFS's own files (file_count + k, k in path order, added[]), opens and reads like a replacement, and its
   folders exist for runtime_romfs_entry_type. The RomFS has no directory listing here, so nothing else needs to know. */
typedef struct { uint64_t index,size; char *path; dev_t dev; ino_t ino; struct timespec mtime; char *name; const uint8_t *memory; } Overlay;
static Overlay *overlays; /* sorted by RomFS index after overlay_init */
static size_t noverlay,overlay_cap;
#define ADDED UINT64_MAX   /* an added file's index until overlay_init numbers it */
static char **added;       /* RomFS paths of the added files, sorted; added[k] is index file_count + k */
static size_t nadded;
static uint32_t handle_ovl[NH]; /* overlay slot + 1 of an open handle, 0 = original */
static int handle_fd[NH];       /* the handle's open overlay file, when handle_ovl is set */
typedef struct { uint64_t offset,size;uint32_t name,len; } Entry;
_Static_assert(sizeof(Entry)==24,"index entry layout");
static Entry entry(uint64_t i) {Entry e;memcpy(&e,index_bytes+24+i*24,24);return e;}
static const char *entry_name(Entry e) {return (char *)index_bytes+strings_offset+e.name;}
static void need(int c,const char *m) {if(!c)runtime_fail(m);}
static void sha_fd(int fd,const char *expected,uint64_t total,const char *stage) {
 host_sha256_ctx c;host_sha256_init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(off_t off=0;;){ssize_t n=src_pread(fd,buffer,8<<20,off);need(n>=0,"read hash input");if(!n)break;host_sha256_update(&c,buffer,(size_t)n);off+=n;
  if(stage&&((uint64_t)off%(256u<<20)<(8u<<20)||!n))runtime_boot_progress(stage,(uint64_t)off,total);}
 uint8_t d[32];char hex[65];host_sha256_final(&c,d);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);need(!strcmp(hex,expected),"RomFS integrity mismatch");
}
/* A successful full hash may be reused only while the same regular file has
   unchanged filesystem identity and modification metadata. This is a startup
   shortcut, not a fresh integrity proof; TOTK_ROMFS_VERIFY=full forces one.
   The record (romfs_proof.h) is shared with the Android setup, which writes it after verifying the image it imported. */
static RomfsProof proof_for(const struct stat *s) {return romfs_proof_for(s,ROMFS_IMAGE_SHA256);}
static int proof_equal(const RomfsProof *a,const RomfsProof *b) {
 return !memcmp(a->magic,b->magic,sizeof a->magic)&&!memcmp(a->sha256,b->sha256,sizeof a->sha256)
  &&a->dev==b->dev&&a->ino==b->ino&&a->size==b->size
  &&a->mtime_sec==b->mtime_sec&&a->mtime_nsec==b->mtime_nsec
  &&a->ctime_sec==b->ctime_sec&&a->ctime_nsec==b->ctime_nsec
  &&a->birth_sec==b->birth_sec&&a->birth_nsec==b->birth_nsec;
}
/* Split image: one record per part (romfs_proof_part_for), all of which must be unchanged for the earlier full hash to be
   reused. */
static void chunk_proofs(RomfsProof *out) {
 for(unsigned c=0;c<nchunks;c++){struct stat st;need(fstat(chunk_fds[c],&st)==0&&S_ISREG(st.st_mode),"RomFS part stat");out[c]=romfs_proof_part_for(&st,ROMFS_IMAGE_SHA256);}
}
/* tmp + fsync + rename of one record file in the proof folder; best effort (without it the next start hashes again). */
static void record_write(const char *dir,const char *name,const void *data,size_t bytes) {
 char path[4096],tmp[4200];if(snprintf(path,sizeof(path),"%s/%s",dir,name)>=(int)sizeof(path))return;
 snprintf(tmp,sizeof(tmp),"%s.%ld.tmp",path,(long)getpid());
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,data,bytes);int ok=n==(ssize_t)bytes&&fsync(out)==0;close(out);if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
/* The sampled-content sidecar (romfs_proof.h): part i is chunk_fds[i], or the one image file. */
static int sample_read(void *user,int part,uint64_t off,void *buf,size_t n) {
 (void)user;int fd=nchunks?chunk_fds[part]:image_fd;uint8_t *p=buf;
 while(n){ssize_t r=pread(fd,p,n,(off_t)off);if(r<0&&errno==EINTR)continue;if(r<=0)return -1;p+=r;n-=(size_t)r;off+=(uint64_t)r;}
 return 0;
}
static void sample_digest(const void *data,size_t n,uint8_t out[32]) {host_sha256_ctx c;host_sha256_init(&c);host_sha256_update(&c,data,n);host_sha256_final(&c,out);}
/* 1 when every file kept its recorded size and mtime and the sidecar's sampled bytes still match: the same files after a
   remount changed their device and inode numbers. */
static int samples_vouch(const char *dir,const RomfsProof *saved,const RomfsProof *want,unsigned n) {
 uint64_t sizes[MAX_CHUNKS];
 for(unsigned i=0;i<n;i++){if(!romfs_proof_same_content_times(&saved[i],&want[i]))return 0;sizes[i]=want[i].size;}
 char path[4096];RomfsSamples s;if(snprintf(path,sizeof(path),"%s/" ROMFS_SAMPLES_FILE,dir)>=(int)sizeof(path))return 0;
 int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return 0;
 int ok=read(fd,&s,sizeof(s))==(ssize_t)sizeof(s);close(fd);if(!ok)return 0;
 uint8_t *scratch=malloc(ROMFS_SAMPLE_SCRATCH);need(scratch!=NULL,"RomFS sample buffer");
 ok=romfs_samples_match(&s,(int)n,sizes,sample_read,NULL,sample_digest,scratch);free(scratch);
 return ok;
}
/* After a full hash: a new salt (so the sampled places are not predictable), the samples of the files just verified. */
static void samples_write(const char *dir,const RomfsProof *want,unsigned n) {
 uint8_t salt[32];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);int ok=fd>=0&&read(fd,salt,32)==32;if(fd>=0)close(fd);if(!ok)return;
 uint64_t sizes[MAX_CHUNKS];for(unsigned i=0;i<n;i++)sizes[i]=want[i].size;
 RomfsSamples s;uint8_t *scratch=malloc(ROMFS_SAMPLE_SCRATCH);need(scratch!=NULL,"RomFS sample buffer");
 ok=!romfs_samples_make(&s,salt,(int)n,sizes,sample_read,NULL,sample_digest,scratch);free(scratch);
 if(ok)record_write(dir,ROMFS_SAMPLES_FILE,&s,sizeof(s));
}
static void verify_chunks(const char *dir) {
 RomfsProof want[MAX_CHUNKS],saved[MAX_CHUNKS],after[MAX_CHUNKS];size_t bytes=nchunks*sizeof(RomfsProof);chunk_proofs(want);
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;   /* iOS: directories devicectl creates belong to root */
 char path[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-parts-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&(size_t)ps.st_size==bytes&&read(proof_fd,saved,bytes)==(ssize_t)bytes;close(proof_fd);
  if(ok&&!force&&!memcmp(saved,want,bytes)){fprintf(stderr,"[totk] RomFS parts unchanged; reused full SHA256 verification record (%u parts)\n",nchunks);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}
  if(ok&&!force&&samples_vouch(dir,saved,want,nchunks)){
   fprintf(stderr,"[totk] RomFS parts have new device/inode numbers (storage remounted); size, mtime and sampled content unchanged: reused full SHA256 verification record (%u parts)\n",nchunks);
   record_write(dir,".romfs-verified-parts-v1",want,bytes);
   runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 over %u parts (%llu bytes)%s\n",nchunks,(unsigned long long)ROMFS_IMAGE_SIZE,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,ROMFS_IMAGE_SIZE);sha_fd(image_fd,ROMFS_IMAGE_SHA256,ROMFS_IMAGE_SIZE,TOTK_UI("校验游戏资源","Verifying game data"));
 chunk_proofs(after);need(!memcmp(after,want,bytes),"RomFS changed during verification");
 record_write(dir,".romfs-verified-parts-v1",want,bytes);samples_write(dir,want,nchunks);
}
static void open_chunks(const char *dir) {
 uint64_t total=0;
 for(;nchunks<MAX_CHUNKS;nchunks++){char path[4096];need(snprintf(path,sizeof(path),"%s/romfs.bin.%03u",dir,nchunks)<(int)sizeof(path),"RomFS part path");
  int fd=open(path,O_RDONLY);if(fd<0)break;struct stat st;need(fstat(fd,&st)==0&&st.st_size>0,"RomFS part stat");
  if(!nchunks)chunk_size=(uint64_t)st.st_size;else need(chunk_fds[nchunks-1]>=0&&total==(uint64_t)nchunks*chunk_size,"RomFS parts must have equal sizes except the last");
  chunk_fds[nchunks]=fd;total+=(uint64_t)st.st_size;}
 need(nchunks>0,"RomFS image missing (romfs.bin or romfs.bin.000...)");
 need(total==ROMFS_IMAGE_SIZE,"RomFS parts do not add up to the image size");
 image_fd=chunk_fds[0];
 fprintf(stderr,"[totk] RomFS image in %u parts of %llu bytes\n",nchunks,(unsigned long long)chunk_size);
}
static void verify_image(int fd,const char *dir) {
 if(nchunks){verify_chunks(dir);return;}
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;
 struct stat before,after;need(fstat(fd,&before)==0&&S_ISREG(before.st_mode),"RomFS image stat");
 RomfsProof want=proof_for(&before),saved;
 char path[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&ps.st_size==sizeof(saved)&&read(proof_fd,&saved,sizeof(saved))==sizeof(saved);close(proof_fd);
  if(ok&&!force&&proof_equal(&saved,&want)){fprintf(stderr,"[totk] RomFS unchanged; reused full SHA256 verification record (%llu bytes)\n",(unsigned long long)want.size);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),want.size,want.size);return;}
  if(ok&&!force&&image_fd==fd&&samples_vouch(dir,&saved,&want,1)){
   fprintf(stderr,"[totk] RomFS has new device/inode numbers (storage remounted); size, mtime and sampled content unchanged: reused full SHA256 verification record (%llu bytes)\n",(unsigned long long)want.size);
   record_write(dir,".romfs-verified-v1",&want,sizeof(want));
   runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),want.size,want.size);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 (%llu bytes)%s\n",(unsigned long long)want.size,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,want.size);sha_fd(fd,ROMFS_IMAGE_SHA256,want.size,TOTK_UI("校验游戏资源","Verifying game data"));
 need(fstat(fd,&after)==0,"RomFS image stat after hash");
 RomfsProof finished=proof_for(&after);need(proof_equal(&want,&finished),"RomFS changed during verification");
 record_write(dir,".romfs-verified-v1",&want,sizeof(want));
 if(image_fd==fd)samples_write(dir,&want,1);
}
static void sha_range(int fd,uint64_t offset,uint64_t size,const char *expected,const char *what) {
 size_t cap=size<(8u<<20)?(size_t)size:(8u<<20);host_sha256_ctx c;host_sha256_init(&c);uint8_t *buffer=malloc(cap?cap:1);need(buffer!=NULL,"hash buffer");
 for(uint64_t done=0;done<size;){size_t chunk=size-done>cap?cap:(size_t)(size-done);ssize_t n=src_pread(fd,buffer,chunk,(off_t)(offset+done));need(n>0,"read hash input");host_sha256_update(&c,buffer,(size_t)n);done+=(uint64_t)n;}
 uint8_t d[32];char hex[65];host_sha256_final(&c,d);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);
 if(strcmp(hex,expected)){fprintf(stderr,"[totk] RomFS overlay: %s SHA-256 %s, manifest says %s\n",what,hex,expected);runtime_fail("RomFS overlay integrity mismatch");}
}
static uint64_t lower(const char *name);
static int path_ok(const char *p);
static void overlay_load(const char *dir,int required) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"RomFS overlay path");FILE *m=fopen(path,"r");
 if(!m&&required){fprintf(stderr,"[totk] RomFS overlay: %s missing; build it with scripts/make_output_res_overlay.py <w>x<h>\n",path);runtime_fail("true-2K HeapDef overlay missing (make romfs-patches-out OUTPUT_RES=<w>x<h>)");}
 if(!m){fprintf(stderr,"[totk] RomFS overlay: none (%s)\n",path);return;}
 char line[8192];size_t first=noverlay;uint64_t hashed=0,t0=runtime_now_ns();
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;char name[4096],sha[65],orig[65];unsigned long long size=0;
  need(sscanf(line,"%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]",name,&size,sha,orig)==4&&strlen(sha)==64&&(strlen(orig)==64||!strcmp(orig,"-")),"RomFS overlay manifest line");
  need(path_ok(name),"RomFS overlay path");uint64_t i=lower(name);int known=i<file_count&&!strcmp(entry_name(entry(i)),name),add=!strcmp(orig,"-");
  if(!known&&!add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay names a file the RomFS lacks");}
  if(known&&add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay adds a file the RomFS already has");}
  Entry e={0};if(known){e=entry(i);sha_range(image_fd,e.offset,e.size,orig,"original");}else i=ADDED;
  char file[8200];need(snprintf(file,sizeof(file),"%s/%s",dir,name)<(int)sizeof(file),"RomFS overlay file path");int fd=open(file,O_RDONLY|O_CLOEXEC);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"RomFS overlay file missing or wrong size");sha_range(fd,0,size,sha,name);close(fd);
  hashed+=e.size+size;
  if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
  char *copy=strdup(file),*rel=strdup(name);need(copy&&rel,"RomFS overlay path copy");overlays[noverlay++]=(Overlay){i,size,copy,st.st_dev,st.st_ino,st.st_mtimespec,rel,NULL};
  if(noverlay-first<=16)fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) %s\n",name,size,add?"is added":"replaces the original");
 }
 fclose(m);
 if(noverlay-first>16)fprintf(stderr,"[totk] RomFS overlay: %zu files from %s (%zu more not listed)\n",noverlay-first,dir,noverlay-first-16);
 if(noverlay>first)fprintf(stderr,"[totk] RomFS overlay: %s checked, %zu files, %.1f MB hashed in %.2f s\n",dir,noverlay-first,hashed/1048576.0,(runtime_now_ns()-t0)/1e9);
}
/* A mods folder that has no manifest.tsv but holds a romfs/ folder is a mod manager's merged output (TKMM writes romfs/
   and exefs/ side by side; copy both into <data>/mods/<version>). Every file under romfs/ is served under its RomFS path:
   it replaces the original of that path, or is added when the RomFS has none. Nothing is hashed, so thousands of files
   cost one lstat each, and there is no original hash to compare: the manager merged against the player's own dump. The
   open-time identity check is the one a manifest's files get. Desktop leftovers (dot entries, desktop.ini, Thumbs.db),
   links and names no RomFS path can have are left out; the walk never leaves the folder (romfs_scan.h). A file one of
   this build's own overlays already replaces (the true-2K HeapDef, the quick-menu timing) stops the start, as two
   manifests would: either copy alone would drop what the other needs (the mod's heap sizes, the build's), so the log
   names the file and both ways out. Paths are case-sensitive: a file whose path differs from the RomFS's only in case
   counts as added and the game never opens it, so the first added paths are logged. */
typedef struct { size_t first,replaced,added;uint64_t bytes; } ModScan;
static int scan_visit(void *user,const char *rel,const char *file,const struct stat *st) {
 ModScan *s=user;uint64_t i=lower(rel);int known=i<file_count&&!strcmp(entry_name(entry(i)),rel);
 if(known)for(size_t k=0;k<s->first;k++)if(overlays[k].index==i){
  fprintf(stderr,"[totk] RomFS mods: %s replaces %s, which this build already replaces with its own %s. Remove that file from the mods folder, or turn off the option that needs the build's copy (a 2560x1440 output or a scene above 1600x900 for the HeapDef; TOTK_ROMFS_PATCHES=0 for the build's patches)\n",file,rel,overlays[k].path);
  runtime_fail("RomFS mods: the merge replaces a file this build replaces itself (see the log)");}
 if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
 char *copy=strdup(file),*name=strdup(rel);need(copy&&name,"RomFS overlay path copy");
 overlays[noverlay++]=(Overlay){known?i:ADDED,(uint64_t)st->st_size,copy,st->st_dev,st->st_ino,st->st_mtimespec,name,NULL};
 if(known)s->replaced++;else if(s->added++<8)fprintf(stderr,"[totk] RomFS mods: %s is added (the RomFS has no file of that path)\n",rel);
 s->bytes+=(uint64_t)st->st_size;return 0;
}
/* exefs/ beside romfs/: code mods. IPS patches of main are applied by the game patches (mod_ips.h). Code modules
   (subsdk files, exlaunch mods) are not loaded: the loader maps the three rewritten modules only, so naming what is
   there explains a mod whose code half is missing. */
static void mods_exefs_note(const char *mods) {
 char p[4200],names[256]="";if(!mod_exefs_dir(mods,p,sizeof(p)))return;DIR *d=opendir(p);if(!d)return;
 unsigned count=0;size_t used=0;struct dirent *e;
 while((e=readdir(d))){size_t len=strlen(e->d_name);
  if(romfs_scan_junk(e->d_name)||!strcmp(e->d_name,"main.npdm")||(len>4&&!strcasecmp(e->d_name+len-4,".ips")))continue;
  if(count++<6){int n=snprintf(names+used,sizeof(names)-used,"%s%.40s",used?", ":"",e->d_name);if(n>0&&(size_t)n<sizeof(names)-used)used+=(size_t)n;}}
 closedir(d);
 if(count)fprintf(stderr,"[totk] RomFS mods: %s holds %u code mod file%s (%s%s) that this port does not load: RomFS files and IPS patches of main are used, so a mod that needs a code module may misbehave\n",p,count,count==1?"":"s",names,count>6?", ...":"");
}
/* 1 when dir holds a manifest.tsv, which is then loaded and decides everything: a manifest that is there but cannot be
   read or parsed stops the start and never falls back to scanning, and one without lines means no mod. */
static int mods_manifest(const char *dir) {
 char p[4200];struct stat st;need(snprintf(p,sizeof(p),"%s/manifest.tsv",dir)<(int)sizeof(p),"RomFS mods path");
 if(lstat(p,&st))return 0;
 if(access(p,R_OK)){fprintf(stderr,"[totk] RomFS mods: %s cannot be read: %s\n",p,strerror(errno));runtime_fail("RomFS mods: manifest.tsv is there but cannot be read (see the log)");}
 overlay_load(dir,0);return 1;
}
/* The mods folder, in this order: its manifest.tsv (loose files under their RomFS paths); the manifest.tsv inside its
   romfs/ folder (a merge someone wrote a manifest for: paths relative to romfs/); else romfs/ is scanned. */
static void mods_load(const char *mods) {
 char p[4200];struct stat st;
 if(mods_manifest(mods)){mods_exefs_note(mods);return;}
 need(snprintf(p,sizeof(p),"%s/romfs",mods)<(int)sizeof(p),"RomFS mods path");
 if(lstat(p,&st)){overlay_load(mods,0);return;}   /* neither a manifest nor romfs/: logs "none" */
 if(!S_ISDIR(st.st_mode)){fprintf(stderr,"[totk] RomFS mods: %s is not a folder (a link is not followed); copy the merged romfs folder itself there\n",p);runtime_fail("RomFS mods: romfs is not a folder (see the log)");}
 if(mods_manifest(p)){mods_exefs_note(mods);return;}
 ModScan s={.first=noverlay};RomfsScanCounts c;uint64_t t0=runtime_now_ns();
 if(romfs_scan_tree(p,scan_visit,&s,&c)){fprintf(stderr,"[totk] RomFS mods: reading %s failed: %s\n",p,strerror(errno));runtime_fail("RomFS mods folder could not be read (see the log)");}
 if(s.added>8)fprintf(stderr,"[totk] RomFS mods: %zu more added files not listed\n",s.added-8);
 fprintf(stderr,"[totk] RomFS mods: %s scanned (no manifest.tsv): %zu files replace originals, %zu added; skipped %lu desktop leftovers, %lu links or special files, %lu invalid names; %.1f MB in %.2f s\n",
  p,s.replaced,s.added,c.junk,c.special,c.invalid,s.bytes/1048576.0,(runtime_now_ns()-t0)/1e9);
 mods_exefs_note(mods);
}
static int overlay_order(const void *a,const void *b) {uint64_t x=((const Overlay *)a)->index,y=((const Overlay *)b)->index;return (x>y)-(x<y);}
static int added_order(const void *a,const void *b) {const Overlay *x=*(Overlay *const *)a,*y=*(Overlay *const *)b;return strcmp(x->name,y->name);}
/* Number the added files in path order after the RomFS's own (file_count + k); two overlays adding one path stop the start. */
static void added_init(void) {
 Overlay **list=NULL;for(size_t k=0;k<noverlay;k++)if(overlays[k].index==ADDED){list=realloc(list,(nadded+1)*sizeof(*list));need(list!=NULL,"RomFS added files");list[nadded++]=&overlays[k];}
 if(!nadded)return;
 qsort(list,nadded,sizeof(*list),added_order);added=malloc(nadded*sizeof(*added));need(added!=NULL,"RomFS added files");
 for(size_t k=0;k<nadded;k++){
  if(k&&!strcmp(list[k]->name,list[k-1]->name)){fprintf(stderr,"[totk] RomFS overlay: %s and %s both add %s\n",list[k-1]->path,list[k]->path,list[k]->name);runtime_fail("RomFS overlay: two overlays add the same file (see the log; remove it from one of the manifests)");}
  list[k]->index=file_count+k;added[k]=list[k]->name;}
 free(list);fprintf(stderr,"[totk] RomFS overlay: %zu files added to the RomFS\n",nadded);
}
/* The index of RomFS path p (an added file: file_count + k), or UINT64_MAX. */
static uint64_t file_index(const char *p) {
 uint64_t i=lower(p);if(i<file_count&&!strcmp(p,entry_name(entry(i))))return i;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],p)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strcmp(added[lo],p)?file_count+lo:UINT64_MAX;
}
/* 1 when a RomFS file or an added one lies under folder (path ending in '/'). */
static int folder_exists(const char *folder) {
 size_t n=strlen(folder);uint64_t i=lower(folder);if(i<file_count&&!strncmp(entry_name(entry(i)),folder,n))return 1;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],folder)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strncmp(added[lo],folder,n);
}
/* The overlay slot + 1 that replaces RomFS file i, or 0. */
static uint32_t overlay_slot(uint64_t i) {size_t lo=0,hi=noverlay;while(lo<hi){size_t m=lo+(hi-lo)/2;if(overlays[m].index<i)lo=m+1;else hi=m;}return lo<noverlay&&overlays[lo].index==i?(uint32_t)(lo+1):0;}
/* DRS is a final, in-memory whole-pack overlay. The template is unmodified
   player data decoded once by scripts/prepare_drs_template.py, not a replacement
   manifest. No template is read or required unless TOTK_DRS_MIN is set. */
static void drs_init(const char *romfs_dir) {
 const char *value=getenv("TOTK_DRS_MIN");if(!value)return;
#if !defined(TOTK_GAME_V143)
 (void)romfs_dir;runtime_fail("TOTK_DRS_MIN requires the authenticated 1.4.3 RomFS");
#else
 uint32_t word;need(totk_drs_scale(value,&word),"TOTK_DRS_MIN must be finite, positive and strictly below native 0.8 (including after float rounding)");
 uint64_t i=lower(TOTK_DRS_PACK);need(i<file_count&&!strcmp(entry_name(entry(i)),TOTK_DRS_PACK),"DRS original Bootup missing");
 for(size_t k=0;k<noverlay;k++)if(overlays[k].index==i){
  fprintf(stderr,"[totk] TOTK_DRS_MIN conflicts with Bootup replacement %s; remove that replacement or unset TOTK_DRS_MIN. Other RomFS overlays are supported.\n",overlays[k].path);
  runtime_fail("DRS refuses a different Bootup overlay; no replacement is silently ignored");}
 Entry e=entry(i);need(e.size==TOTK_DRS_PACK_SIZE,"DRS original Bootup size mismatch");
 sha_range(image_fd,e.offset,e.size,TOTK_DRS_PACK_SHA,"DRS original Bootup");
 const char *template=getenv("TOTK_DRS_TEMPLATE");char path[4096];
 if(!template){need(snprintf(path,sizeof(path),"%s/bootup-drs-v143.sarc",romfs_dir)<(int)sizeof(path),"DRS template path");template=path;}
 int fd=open(template,O_RDONLY|O_CLOEXEC);struct stat st;
 if(fd<0){fprintf(stderr,"[totk] DRS template %s missing; generate from your own original game data with:\n  python3.14 scripts/prepare_drs_template.py --romfs-dir '%s' --output '%s'\n",template,romfs_dir,template);runtime_fail("TOTK_DRS_MIN template missing (see generation command above)");}
 need(fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size==TOTK_DRS_TEMPLATE_SIZE,"DRS template must be the original 1795492-byte Bootup SARC");
 uint8_t *sarc=malloc(TOTK_DRS_TEMPLATE_SIZE);need(sarc!=NULL,"DRS template allocation");
 for(size_t done=0;done<TOTK_DRS_TEMPLATE_SIZE;){ssize_t n=pread(fd,sarc+done,TOTK_DRS_TEMPLATE_SIZE-done,(off_t)done);if(n<0&&errno==EINTR)continue;need(n>0,"DRS template read");done+=(size_t)n;}
 close(fd);uint8_t *packed=NULL;size_t size=0;
 const char *error=totk_drs_transform(sarc,TOTK_DRS_TEMPLATE_SIZE,word,host_sha256_hex,&packed,&size);free(sarc);if(error)runtime_fail(error);
 if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
 char *copy=strdup(template),*name=strdup(TOTK_DRS_PACK);need(copy&&name,"DRS overlay path copy");
 overlays[noverlay++]=(Overlay){.index=i,.size=size,.path=copy,.name=name,.memory=packed};
 float scale;memcpy(&scale,&word,sizeof(scale));
 fprintf(stderr,"[totk] DRS handheld minimum 0.8 -> %.9g applied once in memory; .9 step/other ladders/thresholds preserved, 292 members, %u decompressed bytes, %zu packed bytes, dictionary ID 3; original/template/member SHA-256 checked\n",(double)scale,TOTK_DRS_TEMPLATE_SIZE,size);
#endif
}
/* TOTK_ROMFS_PATCHES (default extracted/<version>/romfs-patches; 0 = none); with a true-2K output bigger than 1920x1080
   also <TOTK_ROMFS_PATCHES_OUT, default extracted/<version>/romfs-patches-out>/<w>x<h> (the HeapDef sizes,
   scripts/make_output_res_overlay.py); then the player's mods in TOTK_ROMFS_MODS (0 = none). */
static void overlay_init(const char *romfs_dir) {
 const char *dir=getenv("TOTK_ROMFS_PATCHES");if(!dir||strcmp(dir,"0"))overlay_load(dir?dir:"extracted/" TOTK_GAME_DIR "/romfs-patches",0);
 /* A scene bigger than 1600x900 (TOTK_RENDER_RES, 1.4.3) grows the same heaps: its overlay is <w>x<h>-scene-<sw>x<sh>. */
 unsigned w=1920,h=1080,sw,sh;if(runtime_output_res_extra_bytes()){runtime_output_res(&w,&h);const char *out=getenv("TOTK_ROMFS_PATCHES_OUT");char d[4096],scene[64]="";
  if(runtime_render_res(&sw,&sh)&&(double)sw*sh>1600.0*900.0)snprintf(scene,sizeof(scene),"-scene-%ux%u",sw,sh);
  need(snprintf(d,sizeof(d),"%s/%ux%u%s",out?out:"extracted/" TOTK_GAME_DIR "/romfs-patches-out",w,h,scene)<(int)sizeof(d),"RomFS output overlay path");
  /* build_app.py makes the 2560x1440 canvas and, for 1.4.3, the 1920x1080 / 2560x1440 scenes on either canvas; other sizes are one command. */
  char m[4200];snprintf(m,sizeof(m),"%s/manifest.tsv",d);
  if(access(m,R_OK)){fprintf(stderr,"[totk] RomFS overlay %s missing: make it with\n  .venv/bin/python scripts/make_output_res_overlay.py %ux%u%s%s --version " TOTK_GAME_DIR "%s%s\n",d,w,h,*scene?" --scene ":"",*scene?scene+7:"",out?" --output-root ":"",out?out:"");
   runtime_fail("HeapDef overlay for this output / scene size missing (see the line above)");}
  overlay_load(d,1);}
 const char *mods=getenv("TOTK_ROMFS_MODS");if(mods&&*mods&&strcmp(mods,"0"))mods_load(mods);
 drs_init(romfs_dir);
 added_init();
 if(noverlay)qsort(overlays,noverlay,sizeof(*overlays),overlay_order);
 for(size_t k=1;k<noverlay;k++)if(overlays[k].index==overlays[k-1].index){
  fprintf(stderr,"[totk] RomFS overlay: %s and %s replace the same file %s\n",overlays[k-1].path,overlays[k].path,overlays[k].name);
  runtime_fail("RomFS overlay: two overlays replace the same file (see the log; remove it from one of the manifests)");}
}
void runtime_fs_init(void) {
 if(image_fd>=0)return;
 romfs_image_size=runtime_game_number("romfs-config.txt","ROMFS_IMAGE_SIZE");romfs_image_sha256=runtime_game_sha256("romfs-config.txt","ROMFS_IMAGE_SHA256");
 romfs_index_sha256=runtime_game_sha256("romfs-config.txt","ROMFS_INDEX_SHA256");
#if TOTK_PINNED
 if(romfs_image_size!=TOTK_PIN_ROMFS_SIZE||strcmp(romfs_image_sha256,TOTK_PIN_ROMFS_SHA256)||strcmp(romfs_index_sha256,TOTK_PIN_ROMFS_INDEX_SHA256))
  runtime_fail("romfs-config.txt differs from the pinned 1.4.3 RomFS (run the app's setup again)");
#endif
 const char *dir=getenv("TOTK_ROMFS");if(!dir)dir="extracted/" TOTK_GAME_DIR "/romfs";char path[4096];
 need(snprintf(path,sizeof(path),"%s/runtime-index.bin",dir)<(int)sizeof(path),"RomFS path");int fd=open(path,O_RDONLY);need(fd>=0,"RomFS index missing");
 struct stat st;need(fstat(fd,&st)==0&&st.st_size>=24&&st.st_size<128*1024*1024,"RomFS index size");runtime_boot_progress(TOTK_UI("读取资源索引","Reading the data index"),0,0);sha_fd(fd,ROMFS_INDEX_SHA256,0,NULL);
 index_size=(size_t)st.st_size;index_bytes=mmap(NULL,index_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);need(index_bytes!=MAP_FAILED,"map RomFS index");
 need(!memcmp(index_bytes,"TOTKRFS1",8),"RomFS index format");memcpy(&file_count,index_bytes+8,8);memcpy(&strings_offset,index_bytes+16,8);
 need(file_count<1000000&&strings_offset==24+file_count*24&&strings_offset<index_size,"RomFS index layout");
 const char *previous=NULL;
 for(uint64_t i=0;i<file_count;i++){Entry e=entry(i);need(e.offset<=ROMFS_IMAGE_SIZE&&e.size<=ROMFS_IMAGE_SIZE-e.offset,"RomFS file bounds");need(e.name<index_size-strings_offset&&e.len>1&&e.len<=index_size-strings_offset-e.name,"RomFS name bounds");const char *name=entry_name(e);need(name[e.len-1]==0&&strlen(name)==e.len-1&&(!previous||strcmp(previous,name)<0),"RomFS sorted paths");previous=name;}
 need(snprintf(path,sizeof(path),"%s/romfs.bin",dir)<(int)sizeof(path),"RomFS image path");image_fd=open(path,O_RDONLY);
 if(image_fd<0)open_chunks(dir);
 else need(fstat(image_fd,&st)==0&&(uint64_t)st.st_size==ROMFS_IMAGE_SIZE,"RomFS image size");
 verify_image(image_fd,dir);
 if(!nchunks&&getenv("TOTK_ROMFS_MMAP")&&strcmp(getenv("TOTK_ROMFS_MMAP"),"0")){
  void *mapped=mmap(NULL,(size_t)ROMFS_IMAGE_SIZE,PROT_READ,MAP_PRIVATE,image_fd,0);
  if(mapped!=MAP_FAILED){image_map=mapped;fprintf(stderr,"[totk] RomFS mmap read path enabled\n");}
  else fprintf(stderr,"[totk] RomFS mmap failed (%s); using pread\n",strerror(errno));
 }
 runtime_boot_status(TOTK_UI("准备游戏资源","Preparing game data"),TOTK_UI("检查分辨率补丁并挂载只读游戏资源","Checking the resolution patch and mounting the read-only game data"));
 overlay_init(dir);
 fprintf(stderr,"[totk] native read-only RomFS ready: %llu files\n",(unsigned long long)file_count);
}
static int path_ok(const char *p) {return romfs_path_valid_relative(p);}
static const char *rom_path(const char *path) {
 return romfs_path_relative(mount_name,path);
}
static uint64_t lower(const char *name) {uint64_t lo=0,hi=file_count;while(lo<hi){uint64_t m=lo+(hi-lo)/2;if(strcmp(entry_name(entry(m)),name)<0)lo=m+1;else hi=m;}return lo;}
static unsigned mount_rom(const char *name,void *cache,uint64_t size) {
 (void)cache;(void)size;runtime_fs_init();if(!name||!path_ok(name)||strlen(name)>=sizeof(mount_name)||strchr(name,'/')||strchr(name,':'))return INVALID;
 if(*mount_name&&strcmp(name,mount_name))runtime_fail("second RomFS mount not implemented");strcpy(mount_name,name);return 0;
}
static unsigned cache_size(uint64_t *out) {if(!out)return INVALID;*out=0;return 0;}
/* TOTK_FS_LOG=1: log every RomFS open with the time since start (shows how far the game gets without a picture). */
static void fs_log(const char *what,const char *path){static int on=-1;static uint64_t t0;if(on<0){on=getenv("TOTK_FS_LOG")!=NULL;t0=runtime_now_ns();}if(on)fprintf(stderr,"[totk-fs] %8.3f %s %s\n",(runtime_now_ns()-t0)/1e9,what,path);}
void runtime_fs_log(const char *what,const char *path){fs_log(what,path);}
unsigned runtime_romfs_open(uint64_t *out,const char *path,int mode) {
 fs_log("rom",path?path:"(null)");
 if(!out||mode!=1)return UNSUPPORTED;const char *p=rom_path(path);if(!p)return NOT_FOUND;uint64_t i=file_index(p);if(i==UINT64_MAX)return NOT_FOUND;
 uint32_t ov=overlay_slot(i);int fd=-1;
 if(ov&&!overlays[ov-1].memory){const Overlay *o=&overlays[ov-1];fd=open(o->path,O_RDONLY|O_CLOEXEC);struct stat st;
  if(fd<0||fstat(fd,&st)!=0||(uint64_t)st.st_size!=o->size||st.st_dev!=o->dev||st.st_ino!=o->ino||st.st_mtimespec.tv_sec!=o->mtime.tv_sec||st.st_mtimespec.tv_nsec!=o->mtime.tv_nsec){
   fprintf(stderr,"[totk] RomFS overlay: %s is missing or changed since the start check\n",o->path);runtime_fail("RomFS overlay file changed while the game ran (restart the game)");}}
 pthread_mutex_lock(&lock);for(unsigned j=1;j<NH;j++)if(!handles[j]){handles[j]=i+1;handle_ovl[j]=ov;handle_fd[j]=fd;*out=0x100000000ULL+j;pthread_mutex_unlock(&lock);return 0;}pthread_mutex_unlock(&lock);runtime_fail("file handle table full");return INVALID;
}
static int handle_entry(uint64_t h,Entry *e) {if(h<=0x100000000ULL||h>=0x100000000ULL+NH)return 0;pthread_mutex_lock(&lock);uint64_t i=handles[h-0x100000000ULL];pthread_mutex_unlock(&lock);if(!i)return 0;*e=i-1<file_count?entry(i-1):(Entry){0};return 1;}   /* an added file's bytes are all in its overlay */
/* Where an open handle's bytes come from: an immutable memory overlay, an
   identity-checked overlay file, or the original's range of the image. */
static void handle_source(uint64_t h,const Entry *e,int *fd,uint64_t *base,uint64_t *size,const uint8_t **memory) {
 pthread_mutex_lock(&lock);uint32_t ov=handle_ovl[h-0x100000000ULL];int ofd=handle_fd[h-0x100000000ULL];pthread_mutex_unlock(&lock);
 if(ov){*fd=ofd;*base=0;*size=overlays[ov-1].size;*memory=overlays[ov-1].memory;}
 else{*fd=image_fd;*base=e->offset;*size=e->size;*memory=image_map?image_map+e->offset:NULL;}
}
unsigned runtime_romfs_read(uint64_t *out,uint64_t h,int64_t offset,void *buffer,uint64_t size) {
 Entry e;if(!out||offset<0||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,fsize;const uint8_t *memory;handle_source(h,&e,&fd,&base,&fsize,&memory);if((uint64_t)offset>fsize||(!buffer&&size))return INVALID;
 uint64_t n=size;if(n>fsize-(uint64_t)offset)n=fsize-(uint64_t)offset;uint64_t done=0;
 static _Atomic int profile_mode=-1;int profile=atomic_load(&profile_mode);if(profile<0){profile=getenv("TOTK_FS_TIMING")!=NULL;atomic_store(&profile_mode,profile);}uint64_t prep_start=profile?runtime_now_ns():0;
 dm_guest_write_prepare(buffer,(size_t)n);
 uint64_t t0=profile?runtime_now_ns():0;
 if(memory){if(n)memcpy(buffer,memory+(uint64_t)offset,(size_t)n);done=n;}
 else while(done<n){size_t chunk=(size_t)(n-done);if(chunk>8<<20)chunk=8<<20;ssize_t r=src_pread(fd,(uint8_t *)buffer+done,chunk,(off_t)(base+(uint64_t)offset+done));if(r<=0)runtime_fail("RomFS read failure");done+=(uint64_t)r;}
 if(profile){uint64_t now=runtime_now_ns();atomic_fetch_add(&read_calls,1);atomic_fetch_add(&read_bytes,done);atomic_fetch_add(&read_ns,now-t0);atomic_fetch_add(&read_prepare_ns,t0-prep_start);
  uint64_t last=atomic_load(&read_report_ns);
  if(now-last>1000000000ULL&&atomic_compare_exchange_strong(&read_report_ns,&last,now))
   fprintf(stderr,"[totk-fs] read window t=%.3f calls=%llu bytes=%llu wall_ms=%.3f prepare_ms=%.3f\n",now/1e9,
    (unsigned long long)atomic_exchange(&read_calls,0),(unsigned long long)atomic_exchange(&read_bytes,0),atomic_exchange(&read_ns,0)/1e6,atomic_exchange(&read_prepare_ns,0)/1e6);}
 *out=done;return 0;
}
unsigned runtime_romfs_size(int64_t *out,uint64_t h) {Entry e;if(!out||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,size;const uint8_t *memory;handle_source(h,&e,&fd,&base,&size,&memory);*out=(int64_t)size;return 0;}
void runtime_romfs_close(uint64_t h) {Entry e;if(!handle_entry(h,&e))runtime_fail("close invalid file handle");uint64_t j=h-0x100000000ULL;pthread_mutex_lock(&lock);int fd=handle_ovl[j]?handle_fd[j]:-1;handles[j]=0;handle_ovl[j]=0;handle_fd[j]=-1;pthread_mutex_unlock(&lock);if(fd>=0)close(fd);}
unsigned runtime_romfs_entry_type(int *out,const char *path) {
 if(!out)return INVALID;const char *p=rom_path(path);if(!p)return NOT_FOUND;if(file_index(p)!=UINT64_MAX){*out=1;return 0;}
 char dir[4096];if(snprintf(dir,sizeof(dir),"%s/",p)>=(int)sizeof(dir))return INVALID;if(folder_exists(dir)){*out=0;return 0;}return NOT_FOUND;
}
int runtime_romfs_owns(const char *path) {size_t n=strlen(mount_name);return n&&path&&!strncmp(path,mount_name,n)&&path[n]==':';}
int runtime_romfs_handle(uint64_t h) {return h>0x100000000ULL&&h<0x100000000ULL+NH;}
unsigned runtime_romfs_read_options(uint64_t *out,uint64_t h,int64_t off,void *buf,uint64_t size) {return runtime_romfs_read(out,h,off,buf,size);}
void *runtime_fs_override(const char *name) {
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn2fs8MountRomEPKcPvm",mount_rom},{"_ZN2nn2fs22QueryMountRomCacheSizeEPm",cache_size}};
 for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)if(!strcmp(name,map[i].name))return map[i].fn;return runtime_fs_host_override(name);
}
void runtime_fs_probe(void) {
 runtime_fs_init();need(mount_rom("rom",NULL,0)==0,"mount RomFS");unsigned count=0;uint8_t buffer[257];uint64_t sum=0;
 /* Audio stream paths include an extra slash after the mount root. Verify
    actual resource lookup and bytes, while still rejecting path traversal. */
 {
  const char *name="Sound/Resource/Stream/Dm_OP_0001.bwav";
  char path[256];uint8_t expected[257],actual[257];uint64_t h,n;
  snprintf(path,sizeof path,"rom:/%s",name);
  need(!runtime_romfs_open(&h,path,1),"open normal audio path");
  need(!runtime_romfs_read(&n,h,0,expected,sizeof expected)&&n==sizeof expected,"read normal audio path");runtime_romfs_close(h);
  for(int slashes=2;slashes<=3;slashes++){
   snprintf(path,sizeof path,slashes==2?"rom://%s":"rom:///%s",name);
   int type=-1;need(!runtime_romfs_entry_type(&type,path)&&type==1,"normalized audio entry type");
   need(!runtime_romfs_open(&h,path,1),"open normalized audio stream path");
   need(!runtime_romfs_read(&n,h,0,actual,sizeof actual)&&n==sizeof actual&&!memcmp(actual,expected,sizeof actual),"normalized audio stream bytes");runtime_romfs_close(h);
  }
  need(runtime_romfs_open(&h,"rom://../Sound/Resource/Stream/Dm_OP_0001.bwav",1)!=0,"reject traversal after repeated root separator");
 }
 for(uint64_t i=0;i<file_count;i+=file_count/64){Entry e=entry(i);uint32_t ov=overlay_slot(i);uint64_t expected_size=ov?overlays[ov-1].size:e.size;char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe path");uint64_t h=0,n=0;int64_t size;need(!runtime_romfs_open(&h,p,1)&&!runtime_romfs_size(&size,h)&&(uint64_t)size==expected_size,"probe open/size");need(!runtime_romfs_read(&n,h,0,buffer,sizeof(buffer)),"probe partial read");for(uint64_t j=0;j<n;j++)sum=sum*131+buffer[j];need(!runtime_romfs_read(&n,h,size,buffer,1)&&n==0,"probe EOF");runtime_romfs_close(h);count++;}
 for(size_t k=0;k<noverlay;k++){char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",overlays[k].name)<(int)sizeof(p),"probe overlay path");
  if(overlays[k].index>=file_count){int type=-1;char *slash=strchr(overlays[k].name,'/')?strrchr(p,'/'):NULL;
   if(slash){*slash=0;need(!runtime_romfs_entry_type(&type,p)&&type==0,"probe added file's folder");*slash='/';}
   need(!runtime_romfs_entry_type(&type,p)&&type==1,"probe added file type");}uint64_t oh=0,n=0;int64_t size=0;uint8_t a[64],b[64];
  need(!runtime_romfs_open(&oh,p,1)&&!runtime_romfs_size(&size,oh)&&(uint64_t)size==overlays[k].size,"probe overlay size");uint64_t tail=overlays[k].size<64?overlays[k].size:64;
  if(overlays[k].memory)memcpy(b,overlays[k].memory+size-(int64_t)tail,(size_t)tail);
  else{int fd=open(overlays[k].path,O_RDONLY|O_CLOEXEC);need(fd>=0&&pread(fd,b,tail,(off_t)(size-(int64_t)tail))==(ssize_t)tail,"probe overlay source");close(fd);}
  need(!runtime_romfs_read(&n,oh,size-(int64_t)tail,a,tail)&&n==tail&&!memcmp(a,b,tail),"probe overlay bytes");runtime_romfs_close(oh);}
 uint64_t h=0;need(runtime_romfs_open(&h,"rom:/../main",1)==NOT_FOUND,"reject traversal");need(runtime_romfs_open(&h,"rom:/absent",2)==UNSUPPORTED,"reject write open");
 printf("{\"status\":\"pass\",\"files\":%llu,\"sampled_files\":%u,\"overlays\":%zu,\"added\":%zu,\"read_digest\":\"%016llx\",\"scope\":\"read-only mount/open/size/read/EOF, verified overlays and added files; no game startup\"}\n",(unsigned long long)file_count,count,noverlay,nadded,(unsigned long long)sum);
}
