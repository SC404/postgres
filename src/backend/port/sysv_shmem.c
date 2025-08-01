/*-------------------------------------------------------------------------
 *
 * sysv_shmem.c
 *	  Implement shared memory using SysV facilities
 *
 * These routines used to be a fairly thin layer on top of SysV shared
 * memory functionality.  With the addition of anonymous-shmem logic,
 * they're a bit fatter now.  We still require a SysV shmem block to
 * exist, though, because mmap'd shmem provides no way to find out how
 * many processes are attached, which we need for interlocking purposes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/sysv_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "portability/mem.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/pidfile.h"


/*
 * As of PostgreSQL 9.3, we normally allocate only a very small amount of
 * System V shared memory, and only for the purposes of providing an
 * interlock to protect the data directory.  The real shared memory block
 * is allocated using mmap().  This works around the problem that many
 * systems have very low limits on the amount of System V shared memory
 * that can be allocated.  Even a limit of a few megabytes will be enough
 * to run many copies of PostgreSQL without needing to adjust system settings.
 *
 * We assume that no one will attempt to run PostgreSQL 9.3 or later on
 * systems that are ancient enough that anonymous shared memory is not
 * supported, such as pre-2.4 versions of Linux.  If that turns out to be
 * false, we might need to add compile and/or run-time tests here and do this
 * only if the running kernel supports it.
 *
 * However, we must always disable this logic in the EXEC_BACKEND case, and
 * fall back to the old method of allocating the entire segment using System V
 * shared memory, because there's no way to attach an anonymous mmap'd segment
 * to a process after exec().  Since EXEC_BACKEND is intended only for
 * developer use, this shouldn't be a big problem.  Because of this, we do
 * not worry about supporting anonymous shmem in the EXEC_BACKEND cases below.
 *
 * As of PostgreSQL 12, we regained the ability to use a large System V shared
 * memory region even in non-EXEC_BACKEND builds, if shared_memory_type is set
 * to sysv (though this is not the default).
 */


 typedef key_t IpcMemoryKey;		/* shared memory key passed to shmget_for_oh(2) */
 typedef int IpcMemoryId;		/* shared memory ID returned by shmget_for_oh(2) */

/*
 * How does a given IpcMemoryId relate to this PostgreSQL process?
 *
 * One could recycle unattached segments of different data directories if we
 * distinguished that case from other SHMSTATE_FOREIGN cases.  Doing so would
 * cause us to visit less of the key space, making us less likely to detect a
 * SHMSTATE_ATTACHED key.  It would also complicate the concurrency analysis,
 * in that postmasters of different data directories could simultaneously
 * attempt to recycle a given key.  We'll waste keys longer in some cases, but
 * avoiding the problems of the alternative justifies that loss.
 */
typedef enum
{
	SHMSTATE_ANALYSIS_FAILURE,	/* unexpected failure to analyze the ID */
	SHMSTATE_ATTACHED,			/* pertinent to DataDir, has attached PIDs */
	SHMSTATE_ENOENT,			/* no segment of that ID */
	SHMSTATE_FOREIGN,			/* exists, but not pertinent to DataDir */
	SHMSTATE_UNATTACHED,		/* pertinent to DataDir, no attached PIDs */
} IpcMemoryState;


unsigned long UsedShmemSegID = 0;
int UsedShmemID = 0;
void	   *UsedShmemSegAddr = NULL;

static Size AnonymousShmemSize;
static void *AnonymousShmem = NULL;

static void *InternalIpcMemoryCreate(IpcMemoryKey memKey, Size size);
static void IpcMemoryDetach(int status, Datum shmaddr);
static void IpcMemoryDelete(int status, Datum shmId);
static IpcMemoryState PGSharedMemoryAttach(IpcMemoryId shmId,
										   void *attachAt,
										   PGShmemHeader **addr);
/**
 * @brief Creates or opens a POSIX shared memory object.
 * 
 * @param key The key for the shared memory segment.
 * @param size The size of the shared memory segment.
 * @param shmflg The flags for shared memory creation and access.
 * @return int The shared memory ID, or -1 on failure.
 */
static int shmget_for_oh(key_t key, size_t size, int shmflg)
{
    char name[32] = {0};
    snprintf(name, sizeof(name), "/%d", key); 

    int oflag = O_RDWR; 
    mode_t mode = shmflg & 0777; 

    if (shmflg & IPC_CREAT) {
        oflag |= O_CREAT;
        if (shmflg & IPC_EXCL) {
            oflag |= O_EXCL;
        }
    }
	
    // 打开或创建共享内存对象
    int shm_id = shm_open(name, oflag, mode);
	
    if (shm_id == -1) {
		perror("shm_open");
		printf("get out shmopen -1\r\n");
        return -1;
    }

    // 如果设置了 IPC_CREAT 或共享内存对象已经存在，设置共享内存对象的大小
    if (ftruncate(shm_id, size) < 0) {  
        perror("ftruncate");  
        printf("shmget_for_oh failed! ftruncate error.\n");  
        close(shm_id);  
        if (oflag & O_CREAT) {  
            if (shm_unlink(name) < 0) {  
                perror("shm_unlink");  
            }  
        }  
        return -1;  
    }  

    UsedShmemID = shm_id;

    return shm_id;
}

/**
 * @brief Attaches the shared memory segment to the address space of the calling process.
 * 
 * @param shm_id The shared memory ID.
 * @param shm_addr The desired address where to attach the shared memory.
 * @param shmflg The flags for attaching the shared memory.
 * @return void* The address of the attached shared memory, or (void*)-1 on failure.
 */
static void *shmat_for_oh(int shm_id, const void *shm_addr, int shmflg)
{

    struct stat buf;
    if (fstat(shm_id, &buf) < 0) {
        perror("fstat");
        printf("shmat_for_oh out! fstat failed!\n");
        return (void *) -1;
    }

    void *ptr = mmap((void *)shm_addr, buf.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_id, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        printf("shmat_for_oh out! mmap failed!\n");
        return (void *) -1;
    }

    return ptr;
}

/**
 * @brief Performs the control operations on the shared memory segment.
 * 
 * @param shm_id The shared memory ID.
 * @param command The command to be executed.
 * @param buffer The buffer for the IPC_STAT command.
 * @return int 0 on success, -1 on failure.
 */
static int shmctl_for_oh(int shm_id, int command, struct shmid_ds *buffer)
{

    if (command == IPC_STAT) {
        if (buffer == NULL) {
            printf("shmctl_for_oh out! buffer null!\n");
            return -1;
        }
        struct stat buf;
        if (fstat(shm_id, &buf) < 0) {
            perror("fstat");
            printf("shmctl_for_oh out! fstat failed!\n");
            return -1;
        }
        buffer->shm_perm.uid = buf.st_uid;
        buffer->shm_perm.gid = buf.st_gid;
        buffer->shm_perm.mode = buf.st_mode;
        buffer->shm_nattch = buf.st_nlink;
        printf("shmctl_for_oh out! stat\n");
        return 0;
    } else if (command == IPC_RMID) {
        char name[32] = {0};
        snprintf(name, sizeof(name), "/%d",UsedShmemSegID); // POSIX shared memory names should start with '/'
        int ret = shm_unlink(name);
        if (ret == -1) {
            return -1;
        }
        return ret;
    }
    return -1; // Return -1 for unsupported commands
}

/**
 * @brief Detaches the shared memory segment from the address space of the calling process.
 * 
 * @param shmaddr The address of the attached shared memory.
 * @return int 0 on success, -1 on failure.
 */
static int shmdt_for_oh(const void *shmaddr)
{

    if (shmaddr == NULL) {
        printf("shmdt_for_oh out! shmaddr null!\n");
        return -1;
    }
    struct stat buf;
    if (fstat(UsedShmemID, &buf) < 0) {
        perror("fstat");
        printf("shmdt_for_oh out! fstat failed!\n");
        return -1;
    }
    int ret = munmap((void *)shmaddr, buf.st_size);
    if (ret == -1) {
        perror("munmap");
        printf("shmdt_for_oh out! munmap failed!\n");
        return -1;
    }

    return ret;
}
/*
 *	InternalIpcMemoryCreate(memKey, size)
 *
 * Attempt to create a new shared memory segment with the specified key.
 * Will fail (return NULL) if such a segment already exists.  If successful,
 * attach the segment to the current process and return its attached address.
 * On success, callbacks are registered with on_shmem_exit to detach and
 * delete the segment when on_shmem_exit is called.
 *
 * If we fail with a failure code other than collision-with-existing-segment,
 * print out an error and abort.  Other types of errors are not recoverable.
 */
static void *
InternalIpcMemoryCreate(IpcMemoryKey memKey, Size size)
{
	IpcMemoryId shmid;
	void	   *requestedAddress = NULL;
	void	   *memAddress;

	/*
	 * Normally we just pass requestedAddress = NULL to shmat_for_oh(), allowing the
	 * system to choose where the segment gets mapped.  But in an EXEC_BACKEND
	 * build, it's possible for whatever is chosen in the postmaster to not
	 * work for backends, due to variations in address space layout.  As a
	 * rather klugy workaround, allow the user to specify the address to use
	 * via setting the environment variable PG_SHMEM_ADDR.  (If this were of
	 * interest for anything except debugging, we'd probably create a cleaner
	 * and better-documented way to set it, such as a GUC.)
	 */
#ifdef EXEC_BACKEND
	{
		char	   *pg_shmem_addr = getenv("PG_SHMEM_ADDR");

		if (pg_shmem_addr)
			requestedAddress = (void *) strtoul(pg_shmem_addr, NULL, 0);
		else
		{
#if defined(__darwin__) && SIZEOF_VOID_P == 8
			/*
			 * Provide a default value that is believed to avoid problems with
			 * ASLR on the current macOS release.
			 */
			requestedAddress = (void *) 0x80000000000;
#endif
		}
	}
#endif

	shmid = shmget_for_oh(memKey, size, IPC_CREAT | IPC_EXCL | IPCProtection);

	if (shmid < 0)
	{
		int			shmget_errno = errno;

		/*
		 * Fail quietly if error indicates a collision with existing segment.
		 * One would expect EEXIST, given that we said IPC_EXCL, but perhaps
		 * we could get a permission violation instead?  Also, EIDRM might
		 * occur if an old seg is slated for destruction but not gone yet.
		 */
		if (shmget_errno == EEXIST || shmget_errno == EACCES
#ifdef EIDRM
			|| shmget_errno == EIDRM
#endif
			)
			return NULL;

		/*
		 * Some BSD-derived kernels are known to return EINVAL, not EEXIST, if
		 * there is an existing segment but it's smaller than "size" (this is
		 * a result of poorly-thought-out ordering of error tests). To
		 * distinguish between collision and invalid size in such cases, we
		 * make a second try with size = 0.  These kernels do not test size
		 * against SHMMIN in the preexisting-segment case, so we will not get
		 * EINVAL a second time if there is such a segment.
		 */
		if (shmget_errno == EINVAL)
		{
			shmid = shmget_for_oh(memKey, 0, IPC_CREAT | IPC_EXCL | IPCProtection);

			if (shmid < 0)
			{
				/* As above, fail quietly if we verify a collision */
				if (errno == EEXIST || errno == EACCES
#ifdef EIDRM
					|| errno == EIDRM
#endif
					)
					return NULL;
				/* Otherwise, fall through to report the original error */
			}
			else
			{
				/*
				 * On most platforms we cannot get here because SHMMIN is
				 * greater than zero.  However, if we do succeed in creating a
				 * zero-size segment, free it and then fall through to report
				 * the original error.
				 */
				if (shmctl_for_oh(shmid, IPC_RMID, NULL) < 0)
					elog(LOG, "shmctl_for_oh(%d, %d, 0) failed: %m",
						 (int) shmid, IPC_RMID);
			}
		}

		/*
		 * Else complain and abort.
		 *
		 * Note: at this point EINVAL should mean that either SHMMIN or SHMMAX
		 * is violated.  SHMALL violation might be reported as either ENOMEM
		 * (BSDen) or ENOSPC (Linux); the Single Unix Spec fails to say which
		 * it should be.  SHMMNI violation is ENOSPC, per spec.  Just plain
		 * not-enough-RAM is ENOMEM.
		 */
		errno = shmget_errno;
		ereport(FATAL,
				(errmsg("could not create shared memory segment: %m"),
				 errdetail("Failed system call was shmget_for_oh(key=%lu, size=%zu, 0%o).",
						   (unsigned long) memKey, size,
						   IPC_CREAT | IPC_EXCL | IPCProtection),
				 (shmget_errno == EINVAL) ?
				 errhint("This error usually means that PostgreSQL's request for a shared memory "
						 "segment exceeded your kernel's SHMMAX parameter, or possibly that "
						 "it is less than "
						 "your kernel's SHMMIN parameter.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.") : 0,
				 (shmget_errno == ENOMEM) ?
				 errhint("This error usually means that PostgreSQL's request for a shared "
						 "memory segment exceeded your kernel's SHMALL parameter.  You might need "
						 "to reconfigure the kernel with larger SHMALL.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.") : 0,
				 (shmget_errno == ENOSPC) ?
				 errhint("This error does *not* mean that you have run out of disk space.  "
						 "It occurs either if all available shared memory IDs have been taken, "
						 "in which case you need to raise the SHMMNI parameter in your kernel, "
						 "or because the system's overall limit for shared memory has been "
						 "reached.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.") : 0));
	}

	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(IpcMemoryDelete, Int32GetDatum(shmid));

	/* OK, should be able to attach to the segment */
	memAddress = shmat_for_oh(shmid, requestedAddress, PG_SHMAT_FLAGS);

	if (memAddress == (void *) -1)
		elog(FATAL, "shmat_for_oh(id=%d, addr=%p, flags=0x%x) failed: %m",
			 shmid, requestedAddress, PG_SHMAT_FLAGS);

	/* Register on-exit routine to detach new segment before deleting */
	on_shmem_exit(IpcMemoryDetach, PointerGetDatum(memAddress));

	/*
	 * Store shmem key and ID in data directory lockfile.  Format to try to
	 * keep it the same length always (trailing junk in the lockfile won't
	 * hurt, but might confuse humans).
	 */
	{
		char		line[64];

		sprintf(line, "%9lu %9lu",
				(unsigned long) memKey, (unsigned long) shmid);
		AddToDataDirLockFile(LOCK_FILE_LINE_SHMEM_KEY, line);
	}

	return memAddress;
}

/****************************************************************************/
/*	IpcMemoryDetach(status, shmaddr)	removes a shared memory segment		*/
/*										from process' address space			*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDetach(int status, Datum shmaddr)
{
	/* Detach System V shared memory block. */
	if (shmdt_for_oh((void *) DatumGetPointer(shmaddr)) < 0)
		elog(LOG, "shmdt_for_oh(%p) failed: %m", DatumGetPointer(shmaddr));
}

/****************************************************************************/
/*	IpcMemoryDelete(status, shmId)		deletes a shared memory segment		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDelete(int status, Datum shmId)
{
	if (shmctl_for_oh(DatumGetInt32(shmId), IPC_RMID, NULL) < 0)
		elog(LOG, "shmctl_for_oh(%d, %d, 0) failed: %m",
			 DatumGetInt32(shmId), IPC_RMID);
}

/*
 * PGSharedMemoryIsInUse
 *
 * Is a previously-existing shmem segment still existing and in use?
 *
 * The point of this exercise is to detect the case where a prior postmaster
 * crashed, but it left child backends that are still running.  Therefore
 * we only care about shmem segments that are associated with the intended
 * DataDir.  This is an important consideration since accidental matches of
 * shmem segment IDs are reasonably common.
 */
bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
	PGShmemHeader *memAddress;
	IpcMemoryState state;

	state = PGSharedMemoryAttach((IpcMemoryId) id2, NULL, &memAddress);
	if (memAddress && shmdt_for_oh(memAddress) < 0)
		elog(LOG, "shmdt_for_oh(%p) failed: %m", memAddress);
	switch (state)
	{
		case SHMSTATE_ENOENT:
		case SHMSTATE_FOREIGN:
		case SHMSTATE_UNATTACHED:
			return false;
		case SHMSTATE_ANALYSIS_FAILURE:
		case SHMSTATE_ATTACHED:
			return true;
	}
	return true;
}

/*
 * Test for a segment with id shmId; see comment at IpcMemoryState.
 *
 * If the segment exists, we'll attempt to attach to it, using attachAt
 * if that's not NULL (but it's best to pass NULL if possible).
 *
 * *addr is set to the segment memory address if we attached to it, else NULL.
 */
static IpcMemoryState
PGSharedMemoryAttach(IpcMemoryId shmId,
					 void *attachAt,
					 PGShmemHeader **addr)
{
	struct shmid_ds shmStat;
	struct stat statbuf;
	PGShmemHeader *hdr;

	*addr = NULL;

	/*
	 * First, try to stat the shm segment ID, to see if it exists at all.
	 */
	if (shmctl_for_oh(shmId, IPC_STAT, &shmStat) < 0)
	{
		/*
		 * EINVAL actually has multiple possible causes documented in the
		 * shmctl_for_oh man page, but we assume it must mean the segment no longer
		 * exists.
		 */
		if (errno == EINVAL)
			return SHMSTATE_ENOENT;

		/*
		 * EACCES implies we have no read permission, which means it is not a
		 * Postgres shmem segment (or at least, not one that is relevant to
		 * our data directory).
		 */
		if (errno == EACCES)
			return SHMSTATE_FOREIGN;

		/*
		 * Some Linux kernel versions (in fact, all of them as of July 2007)
		 * sometimes return EIDRM when EINVAL is correct.  The Linux kernel
		 * actually does not have any internal state that would justify
		 * returning EIDRM, so we can get away with assuming that EIDRM is
		 * equivalent to EINVAL on that platform.
		 */
#ifdef HAVE_LINUX_EIDRM_BUG
		if (errno == EIDRM)
			return SHMSTATE_ENOENT;
#endif

		/*
		 * Otherwise, we had better assume that the segment is in use.  The
		 * only likely case is (non-Linux, assumed spec-compliant) EIDRM,
		 * which implies that the segment has been IPC_RMID'd but there are
		 * still processes attached to it.
		 */
		return SHMSTATE_ANALYSIS_FAILURE;
	}

	/*
	 * Try to attach to the segment and see if it matches our data directory.
	 * This avoids any risk of duplicate-shmem-key conflicts on machines that
	 * are running several postmasters under the same userid.
	 *
	 * (When we're called from PGSharedMemoryCreate, this stat call is
	 * duplicative; but since this isn't a high-traffic case it's not worth
	 * trying to optimize.)
	 */
	if (stat(DataDir, &statbuf) < 0)
		return SHMSTATE_ANALYSIS_FAILURE;	/* can't stat; be conservative */

	hdr = (PGShmemHeader *) shmat_for_oh(shmId, attachAt, PG_SHMAT_FLAGS);
	if (hdr == (PGShmemHeader *) -1)
	{
		/*
		 * Attachment failed.  The cases we're interested in are the same as
		 * for the shmctl_for_oh() call above.  In particular, note that the owning
		 * postmaster could have terminated and removed the segment between
		 * shmctl_for_oh() and shmat_for_oh().
		 *
		 * If attachAt isn't NULL, it's possible that EINVAL reflects a
		 * problem with that address not a vanished segment, so it's best to
		 * pass NULL when probing for conflicting segments.
		 */
		if (errno == EINVAL)
			return SHMSTATE_ENOENT; /* segment disappeared */
		if (errno == EACCES)
			return SHMSTATE_FOREIGN;	/* must be non-Postgres */
#ifdef HAVE_LINUX_EIDRM_BUG
		if (errno == EIDRM)
			return SHMSTATE_ENOENT; /* segment disappeared */
#endif
		/* Otherwise, be conservative. */
		return SHMSTATE_ANALYSIS_FAILURE;
	}
	*addr = hdr;

	if (hdr->magic != PGShmemMagic ||
		hdr->device != statbuf.st_dev ||
		hdr->inode != statbuf.st_ino)
	{
		/*
		 * It's either not a Postgres segment, or not one for my data
		 * directory.
		 */
		return SHMSTATE_FOREIGN;
	}

	/*
	 * It does match our data directory, so now test whether any processes are
	 * still attached to it.  (We are, now, but the shm_nattch result is from
	 * before we attached to it.)
	 */
	return shmStat.shm_nattch == 0 ? SHMSTATE_UNATTACHED : SHMSTATE_ATTACHED;
}

/*
 * Identify the huge page size to use, and compute the related mmap flags.
 *
 * Some Linux kernel versions have a bug causing mmap() to fail on requests
 * that are not a multiple of the hugepage size.  Versions without that bug
 * instead silently round the request up to the next hugepage multiple ---
 * and then munmap() fails when we give it a size different from that.
 * So we have to round our request up to a multiple of the actual hugepage
 * size to avoid trouble.
 *
 * Doing the round-up ourselves also lets us make use of the extra memory,
 * rather than just wasting it.  Currently, we just increase the available
 * space recorded in the shmem header, which will make the extra usable for
 * purposes such as additional locktable entries.  Someday, for very large
 * hugepage sizes, we might want to think about more invasive strategies,
 * such as increasing shared_buffers to absorb the extra space.
 *
 * Returns the (real, assumed or config provided) page size into
 * *hugepagesize, and the hugepage-related mmap flags to use into
 * *mmap_flags if requested by the caller.  If huge pages are not supported,
 * *hugepagesize and *mmap_flags are set to 0.
 */
void
GetHugePageSize(Size *hugepagesize, int *mmap_flags)
{
#ifdef MAP_HUGETLB

	Size		default_hugepagesize = 0;
	Size		hugepagesize_local = 0;
	int			mmap_flags_local = 0;

	/*
	 * System-dependent code to find out the default huge page size.
	 *
	 * On Linux, read /proc/meminfo looking for a line like "Hugepagesize:
	 * nnnn kB".  Ignore any failures, falling back to the preset default.
	 */
#ifdef __linux__

	{
		FILE	   *fp = AllocateFile("/proc/meminfo", "r");
		char		buf[128];
		unsigned int sz;
		char		ch;

		if (fp)
		{
			while (fgets(buf, sizeof(buf), fp))
			{
				if (sscanf(buf, "Hugepagesize: %u %c", &sz, &ch) == 2)
				{
					if (ch == 'k')
					{
						default_hugepagesize = sz * (Size) 1024;
						break;
					}
					/* We could accept other units besides kB, if needed */
				}
			}
			FreeFile(fp);
		}
	}
#endif							/* __linux__ */

	if (huge_page_size != 0)
	{
		/* If huge page size is requested explicitly, use that. */
		hugepagesize_local = (Size) huge_page_size * 1024;
	}
	else if (default_hugepagesize != 0)
	{
		/* Otherwise use the system default, if we have it. */
		hugepagesize_local = default_hugepagesize;
	}
	else
	{
		/*
		 * If we fail to find out the system's default huge page size, or no
		 * huge page size is requested explicitly, assume it is 2MB. This will
		 * work fine when the actual size is less.  If it's more, we might get
		 * mmap() or munmap() failures due to unaligned requests; but at this
		 * writing, there are no reports of any non-Linux systems being picky
		 * about that.
		 */
		hugepagesize_local = 2 * 1024 * 1024;
	}

	mmap_flags_local = MAP_HUGETLB;

	/*
	 * On recent enough Linux, also include the explicit page size, if
	 * necessary.
	 */
#if defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT)
	if (hugepagesize_local != default_hugepagesize)
	{
		int			shift = pg_ceil_log2_64(hugepagesize_local);

		mmap_flags_local |= (shift & MAP_HUGE_MASK) << MAP_HUGE_SHIFT;
	}
#endif

	/* assign the results found */
	if (mmap_flags)
		*mmap_flags = mmap_flags_local;
	if (hugepagesize)
		*hugepagesize = hugepagesize_local;

#else

	if (hugepagesize)
		*hugepagesize = 0;
	if (mmap_flags)
		*mmap_flags = 0;

#endif							/* MAP_HUGETLB */
}

/*
 * GUC check_hook for huge_page_size
 */
bool
check_huge_page_size(int *newval, void **extra, GucSource source)
{
#if !(defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT))
	/* Recent enough Linux only, for now.  See GetHugePageSize(). */
	if (*newval != 0)
	{
		GUC_check_errdetail("\"huge_page_size\" must be 0 on this platform.");
		return false;
	}
#endif
	return true;
}

/*
 * Creates an anonymous mmap()ed shared memory segment.
 *
 * Pass the requested size in *size.  This function will modify *size to the
 * actual size of the allocation, if it ends up allocating a segment that is
 * larger than requested.
 */
static void *
CreateAnonymousSegment(Size *size)
{
	Size		allocsize = *size;
	void	   *ptr = MAP_FAILED;
	int			mmap_errno = 0;

#ifndef MAP_HUGETLB
	/* PGSharedMemoryCreate should have dealt with this case */
	Assert(huge_pages != HUGE_PAGES_ON);
#else
	if (huge_pages == HUGE_PAGES_ON || huge_pages == HUGE_PAGES_TRY)
	{
		/*
		 * Round up the request size to a suitable large value.
		 */
		Size		hugepagesize;
		int			mmap_flags;

		GetHugePageSize(&hugepagesize, &mmap_flags);

		if (allocsize % hugepagesize != 0)
			allocsize += hugepagesize - (allocsize % hugepagesize);

		ptr = mmap(NULL, allocsize, PROT_READ | PROT_WRITE,
				   PG_MMAP_FLAGS | mmap_flags, -1, 0);
		mmap_errno = errno;
		if (huge_pages == HUGE_PAGES_TRY && ptr == MAP_FAILED)
			elog(DEBUG1, "mmap(%zu) with MAP_HUGETLB failed, huge pages disabled: %m",
				 allocsize);
	}
#endif

	/*
	 * Report whether huge pages are in use.  This needs to be tracked before
	 * the second mmap() call if attempting to use huge pages failed
	 * previously.
	 */
	SetConfigOption("huge_pages_status", (ptr == MAP_FAILED) ? "off" : "on",
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	if (ptr == MAP_FAILED && huge_pages != HUGE_PAGES_ON)
	{
		/*
		 * Use the original size, not the rounded-up value, when falling back
		 * to non-huge pages.
		 */
		allocsize = *size;
		ptr = mmap(NULL, allocsize, PROT_READ | PROT_WRITE,
				   PG_MMAP_FLAGS, -1, 0);
		mmap_errno = errno;
	}

	if (ptr == MAP_FAILED)
	{
		errno = mmap_errno;
		ereport(FATAL,
				(errmsg("could not map anonymous shared memory: %m"),
				 (mmap_errno == ENOMEM) ?
				 errhint("This error usually means that PostgreSQL's request "
						 "for a shared memory segment exceeded available memory, "
						 "swap space, or huge pages. To reduce the request size "
						 "(currently %zu bytes), reduce PostgreSQL's shared "
						 "memory usage, perhaps by reducing \"shared_buffers\" or "
						 "\"max_connections\".",
						 allocsize) : 0));
	}

	*size = allocsize;
	return ptr;
}

/*
 * AnonymousShmemDetach --- detach from an anonymous mmap'd block
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
AnonymousShmemDetach(int status, Datum arg)
{
	/* Release anonymous shared memory block, if any. */
	if (AnonymousShmem != NULL)
	{
		if (munmap(AnonymousShmem, AnonymousShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 AnonymousShmem, AnonymousShmemSize);
		AnonymousShmem = NULL;
	}
}

/*
 * PGSharedMemoryCreate
 *
 * Create a shared memory segment of the given size and initialize its
 * standard header.  Also, register an on_shmem_exit callback to release
 * the storage.
 *
 * Dead Postgres segments pertinent to this DataDir are recycled if found, but
 * we do not fail upon collision with foreign shmem segments.  The idea here
 * is to detect and re-use keys that may have been assigned by a crashed
 * postmaster or backend.
 */
PGShmemHeader *
PGSharedMemoryCreate(Size size,
					 PGShmemHeader **shim)
{
	IpcMemoryKey NextShmemSegID;
	void	   *memAddress;
	PGShmemHeader *hdr;
	struct stat statbuf;
	Size		sysvsize;

	/*
	 * We use the data directory's ID info (inode and device numbers) to
	 * positively identify shmem segments associated with this data dir, and
	 * also as seeds for searching for a free shmem key.
	 */
	if (stat(DataDir, &statbuf) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not stat data directory \"%s\": %m",
						DataDir)));

	/* Complain if hugepages demanded but we can't possibly support them */
#if !defined(MAP_HUGETLB)
	if (huge_pages == HUGE_PAGES_ON)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("huge pages not supported on this platform")));
#endif

	/* For now, we don't support huge pages in SysV memory */
	if (huge_pages == HUGE_PAGES_ON && shared_memory_type != SHMEM_TYPE_MMAP)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("huge pages not supported with the current \"shared_memory_type\" setting")));

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	if (shared_memory_type == SHMEM_TYPE_MMAP)
	{
		AnonymousShmem = CreateAnonymousSegment(&size);
		AnonymousShmemSize = size;

		/* Register on-exit routine to unmap the anonymous segment */
		on_shmem_exit(AnonymousShmemDetach, (Datum) 0);

		/* Now we need only allocate a minimal-sized SysV shmem block. */
		sysvsize = sizeof(PGShmemHeader);
	}
	else
	{
		sysvsize = size;

		/* huge pages are only available with mmap */
		SetConfigOption("huge_pages_status", "off",
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	}

	/*
	 * Loop till we find a free IPC key.  Trust CreateDataDirLockFile() to
	 * ensure no more than one postmaster per data directory can enter this
	 * loop simultaneously.  (CreateDataDirLockFile() does not entirely ensure
	 * that, but prefer fixing it over coping here.)
	 */
	NextShmemSegID = statbuf.st_ino;

	for (;;)
	{
		IpcMemoryId shmid;
		PGShmemHeader *oldhdr;
		IpcMemoryState state;

		/* Try to create new segment */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, sysvsize);
		if (memAddress)
			break;				/* successful create and attach */

		/* Check shared memory and possibly remove and recreate */

		/*
		 * shmget_for_oh() failure is typically EACCES, hence SHMSTATE_FOREIGN.
		 * ENOENT, a narrow possibility, implies SHMSTATE_ENOENT, but one can
		 * safely treat SHMSTATE_ENOENT like SHMSTATE_FOREIGN.
		 */
		shmid = shmget_for_oh(NextShmemSegID, sizeof(PGShmemHeader), 0);
		if (shmid < 0)
		{
			oldhdr = NULL;
			state = SHMSTATE_FOREIGN;
		}
		else
			state = PGSharedMemoryAttach(shmid, NULL, &oldhdr);

		switch (state)
		{
			case SHMSTATE_ANALYSIS_FAILURE:
			case SHMSTATE_ATTACHED:
				ereport(FATAL,
						(errcode(ERRCODE_LOCK_FILE_EXISTS),
						 errmsg("pre-existing shared memory block (key %lu, ID %lu) is still in use",
								(unsigned long) NextShmemSegID,
								(unsigned long) shmid),
						 errhint("Terminate any old server processes associated with data directory \"%s\".",
								 DataDir)));
				break;
			case SHMSTATE_ENOENT:

				/*
				 * To our surprise, some other process deleted since our last
				 * InternalIpcMemoryCreate().  Moments earlier, we would have
				 * seen SHMSTATE_FOREIGN.  Try that same ID again.
				 */
				elog(LOG,
					 "shared memory block (key %lu, ID %lu) deleted during startup",
					 (unsigned long) NextShmemSegID,
					 (unsigned long) shmid);
				break;
			case SHMSTATE_FOREIGN:
				NextShmemSegID++;
				break;
			case SHMSTATE_UNATTACHED:

				/*
				 * The segment pertains to DataDir, and every process that had
				 * used it has died or detached.  Zap it, if possible, and any
				 * associated dynamic shared memory segments, as well.  This
				 * shouldn't fail, but if it does, assume the segment belongs
				 * to someone else after all, and try the next candidate.
				 * Otherwise, try again to create the segment.  That may fail
				 * if some other process creates the same shmem key before we
				 * do, in which case we'll try the next key.
				 */
				if (oldhdr->dsm_control != 0)
					dsm_cleanup_using_control_segment(oldhdr->dsm_control);
				if (shmctl_for_oh(shmid, IPC_RMID, NULL) < 0)
					NextShmemSegID++;
				break;
		}

		if (oldhdr && shmdt_for_oh(oldhdr) < 0)
			elog(LOG, "shmdt_for_oh(%p) failed: %m", oldhdr);
	}

	/* Initialize new segment. */
	hdr = (PGShmemHeader *) memAddress;
	hdr->creatorPID = getpid();
	hdr->magic = PGShmemMagic;
	hdr->dsm_control = 0;

	/* Fill in the data directory ID info, too */
	hdr->device = statbuf.st_dev;
	hdr->inode = statbuf.st_ino;

	/*
	 * Initialize space allocation status for segment.
	 */
	hdr->totalsize = size;
	hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));
	*shim = hdr;

	/* Save info for possible future use */
	UsedShmemSegAddr = memAddress;
	UsedShmemSegID = (unsigned long) NextShmemSegID;

	/*
	 * If AnonymousShmem is NULL here, then we're not using anonymous shared
	 * memory, and should return a pointer to the System V shared memory
	 * block. Otherwise, the System V shared memory block is only a shim, and
	 * we must return a pointer to the real block.
	 */
	if (AnonymousShmem == NULL)
		return hdr;
	memcpy(AnonymousShmem, hdr, sizeof(PGShmemHeader));
	return (PGShmemHeader *) AnonymousShmem;
}

#ifdef EXEC_BACKEND

/*
 * PGSharedMemoryReAttach
 *
 * This is called during startup of a postmaster child process to re-attach to
 * an already existing shared memory segment.  This is needed only in the
 * EXEC_BACKEND case; otherwise postmaster children inherit the shared memory
 * segment attachment via fork().
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.  The caller must have already restored them to the postmaster's
 * values.
 */
void
PGSharedMemoryReAttach(void)
{
	IpcMemoryId shmid;
	PGShmemHeader *hdr;
	IpcMemoryState state;
	void	   *origUsedShmemSegAddr = UsedShmemSegAddr;

	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

#ifdef __CYGWIN__
	/* cygipc (currently) appears to not detach on exec. */
	PGSharedMemoryDetach();
	UsedShmemSegAddr = origUsedShmemSegAddr;
#endif

	elog(DEBUG3, "attaching to %p", UsedShmemSegAddr);
	shmid = shmget_for_oh(UsedShmemSegID, sizeof(PGShmemHeader), 0);
	if (shmid < 0)
		state = SHMSTATE_FOREIGN;
	else
		state = PGSharedMemoryAttach(shmid, UsedShmemSegAddr, &hdr);
	if (state != SHMSTATE_ATTACHED)
		elog(FATAL, "could not reattach to shared memory (key=%d, addr=%p): %m",
			 (int) UsedShmemSegID, UsedShmemSegAddr);
	if (hdr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 hdr, origUsedShmemSegAddr);
	dsm_set_control_handle(hdr->dsm_control);

	UsedShmemSegAddr = hdr;		/* probably redundant */
}

/*
 * PGSharedMemoryNoReAttach
 *
 * This is called during startup of a postmaster child process when we choose
 * *not* to re-attach to the existing shared memory segment.  We must clean up
 * to leave things in the appropriate state.  This is not used in the non
 * EXEC_BACKEND case, either.
 *
 * The child process startup logic might or might not call PGSharedMemoryDetach
 * after this; make sure that it will be a no-op if called.
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.  The caller must have already restored them to the postmaster's
 * values.
 */
void
PGSharedMemoryNoReAttach(void)
{
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

#ifdef __CYGWIN__
	/* cygipc (currently) appears to not detach on exec. */
	PGSharedMemoryDetach();
#endif

	/* For cleanliness, reset UsedShmemSegAddr to show we're not attached. */
	UsedShmemSegAddr = NULL;
	/* And the same for UsedShmemSegID. */
	UsedShmemSegID = 0;
}

#endif							/* EXEC_BACKEND */

/*
 * PGSharedMemoryDetach
 *
 * Detach from the shared memory segment, if still attached.  This is not
 * intended to be called explicitly by the process that originally created the
 * segment (it will have on_shmem_exit callback(s) registered to do that).
 * Rather, this is for subprocesses that have inherited an attachment and want
 * to get rid of it.
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine, also AnonymousShmem and AnonymousShmemSize.
 */
void
PGSharedMemoryDetach(void)
{
	if (UsedShmemSegAddr != NULL)
	{
		if ((shmdt_for_oh(UsedShmemSegAddr) < 0)
#if defined(EXEC_BACKEND) && defined(__CYGWIN__)
		/* Work-around for cygipc exec bug */
			&& shmdt_for_oh(NULL) < 0
#endif
			)
			elog(LOG, "shmdt_for_oh(%p) failed: %m", UsedShmemSegAddr);
		UsedShmemSegAddr = NULL;
	}

	if (AnonymousShmem != NULL)
	{
		if (munmap(AnonymousShmem, AnonymousShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 AnonymousShmem, AnonymousShmemSize);
		AnonymousShmem = NULL;
	}
}
