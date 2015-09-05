/*
** Windows-specific run-time environment implementation for LSM.
*/

#include <Windows.h>
#include "lsmInt.h"

typedef struct SharedMemorySegment SharedMemorySegment;
struct SharedMemorySegment {
  void *pSegment;                     /* Pointer to the mapped memory */
  int  nSize;
  SharedMemorySegment *next;
};
typedef int(*SharedMemorySegmentFilterFunction)(SharedMemorySegment *, void *);

struct SharedMemoryFile {
  SharedMemorySegment *pSegRoot;      /* Pointer to the root node of memory mapped segments */
  LARGE_INTEGER nSizeSharedMemory;    /* Size of shared memory file */
  HANDLE hFile;                       /* Handle to shared memory file */
};
typedef struct SharedMemoryFile SharedMemoryFile;

/*
** An open file is an instance of the following object
*/
typedef struct WindowsFile WindowsFile;
struct WindowsFile {
  lsm_env *pEnv;                  /* The run-time environment */
  wchar_t *zName;                 /* Full path to file */
  HANDLE hFile;                   /* Handle to the file */
  HANDLE hFileLockMutex;          /* Mutex for file locking */
  BOOL bReadOnly;                 /* Database in readonly mode */
  LPVOID pMap;                    /* Pointer to the memory mapped address */
  LARGE_INTEGER nMap;             /* Number of bytes  */
  SharedMemoryFile *pShm;         /* Pointer to shared memory file */
};

static SharedMemorySegment *sharedMemorySegmentListAppend(SharedMemorySegment *root, SharedMemorySegment *v) {
  if (root == NULL) {
    return v;
  }

  SharedMemorySegment *p = root;
  while (p->next != NULL) {
    p = p->next;
  }

  v->next = NULL;
  p->next = v;
  return root;
}

static SharedMemorySegment *sharedMemorySegmentListFind(SharedMemorySegment *root, SharedMemorySegmentFilterFunction f, void *params) {
  SharedMemorySegment *p = root;
  while (p != NULL && f(p, params) != 0) {
    p = p->next;
  }

  return p;
}

static SharedMemorySegment *sharedMemorySegmentListDelete(SharedMemorySegment *root, SharedMemorySegment *v, int *removed) {
  *removed = 0;
  if (root == NULL) {
    return NULL;
  }

  if (root == v) {
    *removed = 1;
    return root->next;
  }

  SharedMemorySegment *p = root;
  while (p != NULL && p->next != v) {
    p = p->next;
  }

  if (p->next == NULL) {
    return root;
  }

  *removed = 1;
  p->next = p->next->next;
  return root;
}

static LPWSTR lsmWindowsUtf8ToUnicode(lsm_env *env, const char *zFilename) {
  int nChar;
  LPWSTR zWideFilename;

  // Determine length
  nChar = MultiByteToWideChar(CP_UTF8, 0, zFilename, -1, NULL, 0);
  if (nChar == 0) {
    return NULL;
  }

  zWideFilename = lsm_malloc(env, nChar*sizeof(zWideFilename[0]));
  if (zWideFilename == NULL) {
    return NULL;
  }
  memset(zWideFilename, 0, nChar*sizeof(zWideFilename[0]));
  nChar = MultiByteToWideChar(CP_UTF8, 0, zFilename, -1, zWideFilename, nChar);

  if (nChar == 0) {
    lsm_free(env, zWideFilename);
    return NULL;
  }

  return zWideFilename;
}

static char *lsmWindowsUnicodeToUtf8(lsm_env *env, LPCWSTR zWideFilename) {
  int nByte;
  char *zFilename;

  nByte = WideCharToMultiByte(CP_UTF8, 0, zWideFilename, -1, 0, 0, 0, 0);
  if (nByte == 0) {
    return NULL;
  }

  zFilename = lsm_malloc(env, nByte);
  if (zFilename == NULL) {
    return NULL;
  }

  memset(zFilename, 0, nByte);
  nByte = WideCharToMultiByte(CP_UTF8, 0, zWideFilename, -1, zFilename, nByte, 0, 0);
  if (nByte == 0) {
    lsm_free(env, zFilename);
    return NULL;
  }

  return zFilename;
}

static int lsmWindowsSharedMemoryFileName(wchar_t *originalName, wchar_t *target, int targetBufferSize) {
  const wchar_t *pPostfix = L"-shm";
  const int nPostfixSize = 5;
  int originalLength = wcslen(originalName);
  if (target == NULL) {
    return originalLength + nPostfixSize;
  }

  if (targetBufferSize < originalLength + nPostfixSize) {
    return 0;
  }

  wcscpy_s(target, targetBufferSize, originalName);
  wcscat_s(target, targetBufferSize, pPostfix);

  return originalLength + nPostfixSize;
}

static int lsmWindowsOsOpen(
  lsm_env *pEnv,
  const char *zFile,
  int flags,
  lsm_file **ppFile
  ) {
  int rc = LSM_OK;
  WindowsFile *p;

  p = lsm_malloc(pEnv, sizeof(WindowsFile));

  if (p == NULL) {
    rc = LSM_NOMEM_BKPT;
  }
  else {
    size_t nFileName = strlen(zFile);
    int bReadonly = (flags & LSM_OPEN_READONLY);
    DWORD oflags = (bReadonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE));
    DWORD crflags = (bReadonly ? OPEN_EXISTING : OPEN_ALWAYS);
    DWORD err;

    memset(p, 0, sizeof(WindowsFile));
    p->pEnv = pEnv;
    p->pShm = lsm_malloc(pEnv, sizeof(SharedMemoryFile));
    memset(p->pShm, 0, sizeof(SharedMemoryFile));

    p->bReadOnly = bReadonly ? TRUE : FALSE;
    p->zName = lsmWindowsUtf8ToUnicode(pEnv, zFile);

    if (p->zName == NULL) {
      return LSM_NOMEM_BKPT;
    }

    p->hFile = CreateFile2(
      p->zName,
      oflags,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      crflags,
      NULL);
    if (p->hFile == INVALID_HANDLE_VALUE) {
      lsm_free(pEnv, p);
      p = NULL;
      err = GetLastError();
      if (err == ERROR_FILE_NOT_FOUND) {
        rc = lsmErrorBkpt(LSM_IOERR_NOENT);
      }
      else {
        rc = LSM_IOERR_BKPT;
      }
    }
  }

  if (rc == LSM_OK) {
    size_t nameChars = strlen(zFile) + 1;
    wchar_t *nameCopy = lsm_malloc(pEnv, nameChars * sizeof(wchar_t));
    if (nameCopy == NULL) {
      CloseHandle(p->hFile);
      lsm_free(pEnv, p->zName);
      lsm_free(pEnv, p);
      return LSM_NOMEM_BKPT;
    }

    memset(nameCopy, 0, nameChars * sizeof(wchar_t));
    wcscpy_s(nameCopy, nameChars, p->zName);
    while (1) {
      wchar_t * slash = wcschr(nameCopy, L'\\');
      wchar_t * colon = wcschr(nameCopy, L':');
      
      if (slash != NULL) {
        *slash = L'_';
      }

      if (colon != NULL) {
        *colon = L'_';
      }

      if (slash == NULL && colon == NULL) {
        break;
      }
    }

    p->hFileLockMutex = CreateMutex(NULL, 0, nameCopy);
    lsm_free(pEnv, nameCopy);

    if (p->hFileLockMutex == NULL) {
      CloseHandle(p->hFile);
      lsm_free(pEnv, p->zName);
      lsm_free(pEnv, p);
      return LSM_IOERR_BKPT;
    }
  }

  *ppFile = (lsm_file *)p;
  return rc;
}

static int lsmWindowsOsWrite(
  lsm_file *pFile,                /* File to write to */
  lsm_i64 iOff,                   /* Offset to write to */
  void *pData,                    /* Write data from this buffer */
  int nData                       /* Bytes of data to write */
  ) {
  int rc = LSM_OK;
  WindowsFile *p = (WindowsFile *)pFile;
  LARGE_INTEGER liSize, oliSize;
  BOOL success;
  liSize.QuadPart = iOff;

  success = SetFilePointerEx(
    p->hFile,
    liSize,
    &oliSize,
    FILE_BEGIN);

  if (success == TRUE && liSize.QuadPart == iOff) {
    DWORD nWritten;
    success = WriteFile(
      p->hFile,
      pData,
      nData,
      &nWritten,
      NULL);
    if (success == FALSE || nWritten != nData) {
      rc = LSM_IOERR_BKPT;
    }
  }
  else {
    rc = LSM_IOERR_BKPT;
  }

  return rc;
}

static int windowsSetFileSizeTo(HANDLE hFile, lsm_i64 nSize) {
  int rc = LSM_OK;                /* Return code */
  LARGE_INTEGER liSize, oliSize;
  BOOL success;

  if (GetFileSizeEx(hFile, &liSize) && liSize.QuadPart >= nSize) {
    return LSM_OK;
  }

  liSize.QuadPart = nSize;

  if (hFile == NULL || hFile == INVALID_HANDLE_VALUE) {
    return LSM_IOERR_BKPT;
  }

  success = SetFilePointerEx(
    hFile,
    liSize,
    &oliSize,
    FILE_BEGIN);

  if (success == TRUE && oliSize.QuadPart == nSize) {
    success = SetEndOfFile(hFile);
  }
  else {
    rc = LSM_IOERR_BKPT;
  }

  if (success != TRUE) {
    rc = LSM_IOERR_BKPT;
  }

  return rc;
}

static int lsmWindowsOsTruncate(
  lsm_file *pFile,                /* File to write to */
  lsm_i64 nSize                   /* Size to truncate file to */
  ) {
  WindowsFile *p = (WindowsFile *)pFile;
  return windowsSetFileSizeTo(p->hFile, nSize);
}

static int lsmWindowsOsRead(
  lsm_file *pFile,                /* File to read from */
  lsm_i64 iOff,                   /* Offset to read from */
  void *pData,                    /* Read data into this buffer */
  int nData                       /* Bytes of data to read */
  ) {
  int rc = LSM_OK;
  WindowsFile *p = (WindowsFile *)pFile;
  LARGE_INTEGER liSize, oliSize;
  BOOL success;

  liSize.QuadPart = iOff;
  success = SetFilePointerEx(
    p->hFile,
    liSize,
    &oliSize,
    FILE_BEGIN);
  if (success == TRUE && oliSize.QuadPart == iOff) {
    int bData;
    success = ReadFile(p->hFile, pData, nData, &bData, NULL);
    if (success == FALSE) {
      rc = LSM_IOERR_BKPT;
    }
    else if (bData < nData) {
      memset(&((u8 *)pData)[bData], 0, nData - bData);
    }
  }
  else {
    rc = LSM_IOERR_BKPT;
  }

  return rc;
}

static int lsmWindowsOsSync(lsm_file *pFile) {
  int rc = LSM_OK;

#ifndef LSM_NO_SYNC
  WindowsFile *p = (WindowsFile *)pFile;

  if (FlushFileBuffers(p->hFile) != TRUE) {
    rc = LSM_IOERR_BKPT;
  }
#else
  (void)pFile;
#endif

  return rc;
}

static int lsmWindowsOsSectorSize(lsm_file *pFile) {
  WindowsFile *p = (WindowsFile *)pFile;
  FILE_STORAGE_INFO fileInfo;
  BOOL success;
  success = GetFileInformationByHandleEx(
    p->hFile,
    FileStorageInfo,
    &fileInfo,
    sizeof(FILE_STORAGE_INFO));

  if (success == FALSE) {
    return 512;
  }

  return fileInfo.PhysicalBytesPerSectorForAtomicity;
}

static int lsmWindowsOsRemap(
  lsm_file *pFile,
  lsm_i64 iMin,
  void **ppOut,
  lsm_i64 *pnOut
  ) {
  const int nBitExtensionMask = ((2 << 20) - 1);
  WindowsFile *p = (WindowsFile *)pFile;
  LARGE_INTEGER nSz;
  HANDLE hMMFile;

  *ppOut = NULL;
  *pnOut = 0;

  // Unmap previous view
  if (p->pMap != NULL) {
    UnmapViewOfFile(p->pMap);
    p->nMap.QuadPart = 0;
    p->pMap = NULL;
  }

  if (GetFileSizeEx(p->hFile, &nSz) != TRUE) {
    return LSM_IOERR_BKPT;
  }

  if (iMin <= 0) {
    return LSM_IOERR_BKPT;
  }

  // Extend the file in 1MB boundaries 
  if (nSz.QuadPart < iMin) {
    int extendSize = (iMin & (~nBitExtensionMask)),
        err = LSM_OK;
    if ((iMin & nBitExtensionMask) > 0) {
      extendSize = extendSize + nBitExtensionMask + 1;
    }

    err = windowsSetFileSizeTo(p->hFile, extendSize);
    if (err != LSM_OK) {
      return err;
    }

    nSz.QuadPart = extendSize;
  }

  hMMFile = CreateFileMapping(
    p->hFile,
    NULL /* lpFileMappingAttributes */,
    (p->bReadOnly ? PAGE_READONLY : PAGE_READWRITE),
    nSz.HighPart,
    nSz.LowPart,
    NULL /* lpName */);

  if (hMMFile == NULL) {
    return LSM_IOERR_BKPT;
  }

  p->nMap.QuadPart = nSz.QuadPart;
  p->pMap = MapViewOfFile(
    hMMFile,
    p->bReadOnly ? FILE_MAP_READ : FILE_MAP_WRITE,
    0 /* dwFileOffsetHigh */,
    0 /* dwFileOffsetLow */,
    p->nMap.LowPart /* dwNumberOfBytesToMap */);

  CloseHandle(hMMFile);
  if (p->pMap == NULL) {
    return LSM_IOERR_BKPT;
  }

  *ppOut = p->pMap;
  *pnOut = p->nMap.QuadPart;

  return LSM_OK;
}

static int lsmWindowsOsFullpath(
  lsm_env *pEnv,
  const char *zName,
  char *zOut,
  int *pnOut
  ) {
  wchar_t *pWcPath = lsmWindowsUtf8ToUnicode(pEnv, zName);
  wchar_t *pWcFullPath;
  char *pFullPath;
  DWORD nFullPathCopiedSize, nFullPathBufferSize;
  size_t nWcConverted;

  if (pWcPath == NULL) {
    return LSM_NOMEM_BKPT;
  }

  nFullPathBufferSize = MAX_PATH;
  do {
    pWcFullPath = (wchar_t *)lsm_malloc(pEnv, nFullPathBufferSize * sizeof(wchar_t));

    if (pWcFullPath == NULL) {
      lsm_free(pEnv, pWcPath);
      return LSM_NOMEM_BKPT;
    }

    nFullPathCopiedSize = GetFullPathName(pWcPath, nFullPathBufferSize, pWcFullPath, NULL);

    if (nFullPathCopiedSize == 0) {
      lsm_free(pEnv, pWcPath);
      lsm_free(pEnv, pWcFullPath);
      return LSM_IOERR_BKPT;
    }

    // Buffer is not able to hold the complete path, expand the path
    if (nFullPathCopiedSize == nFullPathBufferSize) {
      lsm_free(pEnv, pWcFullPath);
      nFullPathBufferSize += MAX_PATH;
      continue;
    }

  } while (nFullPathCopiedSize >= nFullPathBufferSize);

  lsm_free(pEnv, pWcPath);
  pWcFullPath[nFullPathCopiedSize] = 0;
  pFullPath = lsmWindowsUnicodeToUtf8(pEnv, pWcFullPath);

  if (pFullPath == NULL) {
    lsm_free(pEnv, pWcFullPath);
    return LSM_NOMEM_BKPT;
  }

  nWcConverted = strlen(pFullPath);
  if (zOut != NULL) {
    memset(zOut, 0, *pnOut);
    strcpy_s(zOut, *pnOut, pFullPath);
  }
  lsm_free(pEnv, pFullPath);

  *pnOut = (int)nWcConverted + 1;
  return LSM_OK;
}

static int lsmWindowsOsFileid(
  lsm_file *pFile,
  void *pBuf,
  int *pnBuf
  ) {
  WindowsFile *p = (WindowsFile *)pFile;
  int nSize = *pnBuf;
  *pnBuf = sizeof(HANDLE);

  if (pBuf != NULL) {
    memcpy_s(pnBuf, nSize, &p->hFile, sizeof(HANDLE));
  }

  return LSM_OK;
}

static int lsmWindowsOsUnlink(lsm_env *pEnv, const char *zFile) {
  if (DeleteFileA(zFile) == FALSE) {
    return LSM_IOERR_BKPT;
  }

  return LSM_OK;
}

int lsmWindowsOsLock(lsm_file *pFile, int iLock, int eType) {
  WindowsFile *p = (WindowsFile *)pFile;
  OVERLAPPED sOverlapped;
  DWORD lockFlags = LOCKFILE_FAIL_IMMEDIATELY;
  int lockRetries = 0;
  assert(iLock > 0 && iLock <= 32);

  memset(&sOverlapped, 0, sizeof(OVERLAPPED));
  sOverlapped.Offset = 4096 - iLock;
  sOverlapped.OffsetHigh = 0;

  WaitForSingleObject(p->hFileLockMutex, INFINITE);
  if (eType == LSM_LOCK_UNLOCK) {
    if (UnlockFileEx(p->hFile, 0, 1, 0, &sOverlapped)) {
      ReleaseMutex(p->hFileLockMutex);
      return LSM_OK;
    }

    ReleaseMutex(p->hFileLockMutex);
    return LSM_IOERR_BKPT;
  }

  lockFlags = lockFlags | (eType == LSM_LOCK_EXCL ? LOCKFILE_EXCLUSIVE_LOCK : 0);

  // Try to acquire lock
  if (LockFileEx(
        p->hFile,
        lockFlags,
        0 /* reserved */,
        1 /* low */,
        0 /* high */,
        &sOverlapped)) 
  {
    ReleaseMutex(p->hFileLockMutex);
    return LSM_OK;
  }
  
  ReleaseMutex(p->hFileLockMutex);
  return LSM_BUSY;
}

int lsmWindowsOsTestLock(lsm_file *pFile, int iLock, int nLock, int eType) {
  WindowsFile *p = (WindowsFile *)pFile;
  OVERLAPPED sOverlapped, unlockOverlapped;

  assert(iLock > 0 && iLock <= 32);

  memset(&sOverlapped, 0, sizeof(OVERLAPPED));
  memset(&unlockOverlapped, 0, sizeof(OVERLAPPED));
  sOverlapped.Offset = 4096 - iLock;
  unlockOverlapped.Offset = 4096 - iLock;
  sOverlapped.OffsetHigh = 0;
  unlockOverlapped.OffsetHigh = 0;

  WaitForSingleObject(p->hFileLockMutex, INFINITE);
  if (LockFileEx(
        p->hFile,
        LOCKFILE_FAIL_IMMEDIATELY,
        0 /* reserved*/,
        nLock /* low */,
        0 /* high */,
        &sOverlapped)) {
    UnlockFileEx(p->hFile, 0, nLock, 0, &unlockOverlapped);
    ReleaseMutex(p->hFileLockMutex);
    return LSM_OK;
  }

  ReleaseMutex(p->hFileLockMutex);
  return LSM_BUSY;
}

static int windowsOpenShmFile(WindowsFile *p) {
  int shFileNameLength = lsmWindowsSharedMemoryFileName(p->zName, NULL, 0);
  wchar_t *shFileName = lsm_malloc(p->pEnv, (shFileNameLength + 1) * sizeof(wchar_t));
  
  memset(shFileName, 0, (shFileNameLength + 1) * sizeof(wchar_t));
  if (shFileNameLength != lsmWindowsSharedMemoryFileName(p->zName, shFileName, shFileNameLength)) {
    return LSM_IOERR_BKPT;
  }

  assert(p->pShm->hFile == INVALID_HANDLE_VALUE || p->pShm->hFile == NULL);

  // Open file if not exist
  p->pShm->hFile = CreateFile2(
    shFileName,
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    OPEN_ALWAYS,
    NULL);

  lsm_free(p->pEnv, shFileName);
  if (p->pShm->hFile == INVALID_HANDLE_VALUE) {
    return LSM_IOERR_BKPT;
  }

  return LSM_OK;
}

int lsmWindowsOsShmMap(lsm_file *pFile, int chunkIndex, int chunkSize, void **ppShm) {
  WindowsFile *p = (WindowsFile *)pFile;
  LARGE_INTEGER chunkOffset = { 0 };
  HANDLE hMMFile;
  *ppShm = 0;
  assert(chunkSize == LSM_SHM_CHUNK_SIZE);

  chunkOffset.QuadPart = chunkIndex * LSM_SHM_CHUNK_SIZE;
  // Open file if it's not open
  if (p->pShm->hFile == NULL || p->pShm->hFile == INVALID_HANDLE_VALUE) {
    int err = windowsOpenShmFile(p);
    if (err != LSM_OK) {
      return err;
    }
  }

  // If file size lesser than chunk index resize file
  if ((chunkOffset.QuadPart + chunkSize) >= p->pShm->nSizeSharedMemory.QuadPart) {
    LARGE_INTEGER newSize = { 0 };

    // Try to double up the size of existing memory
    newSize.QuadPart = p->pShm->nSizeSharedMemory.QuadPart * 2;
    if (newSize.QuadPart < (chunkOffset.QuadPart + chunkSize)) {
      newSize.QuadPart = chunkOffset.QuadPart + chunkSize;
    }

    int err = windowsSetFileSizeTo(p->pShm->hFile, newSize.QuadPart);
    if (err != LSM_OK) {
      return LSM_IOERR_BKPT;
    }

    p->pShm->nSizeSharedMemory = newSize;
  }

  // Create record in linked list of mapped memory
  SharedMemorySegment *seg = (SharedMemorySegment *)lsm_malloc(p->pEnv, sizeof(SharedMemorySegment));
  if (seg == NULL) {
    return LSM_NOMEM_BKPT;
  }

  memset(seg, 0, sizeof(SharedMemorySegment));
  seg->nSize = chunkSize;

  // Create file mapping object and return if it fails
  hMMFile = CreateFileMapping(
    p->pShm->hFile,
    NULL,
    PAGE_READWRITE,
    p->pShm->nSizeSharedMemory.HighPart,
    p->pShm->nSizeSharedMemory.LowPart,
    NULL);

  if (hMMFile == NULL) {
    lsm_free(p->pEnv, seg);
    return LSM_IOERR_BKPT;
  }

  seg->pSegment = MapViewOfFile(
    hMMFile,
    FILE_MAP_WRITE,
    chunkOffset.HighPart,
    chunkOffset.LowPart,
    chunkSize);

  CloseHandle(hMMFile);
  if (seg->pSegment == NULL) {
    DWORD err = GetLastError();
    lsm_free(p->pEnv, seg);
    return LSM_NOMEM_BKPT;
  }

  p->pShm->pSegRoot = sharedMemorySegmentListAppend(p->pShm->pSegRoot, seg);
  *ppShm = seg->pSegment;
  return LSM_OK;
}

void lsmWindowsOsShmBarrier(void) {
}

int lsmWindowsOsShmUnmap(lsm_file *pFile, int bDelete) {
  WindowsFile *p = (WindowsFile *)pFile;
  int shmFileNameSize = lsmWindowsSharedMemoryFileName(p->zName, NULL, 0);
  wchar_t *shmFileName = NULL;

  while (p->pShm->pSegRoot != NULL) {
    SharedMemorySegment *tmp = p->pShm->pSegRoot;
    int removed = 0;
    p->pShm->pSegRoot = sharedMemorySegmentListDelete(p->pShm->pSegRoot, p->pShm->pSegRoot, &removed);

    UnmapViewOfFile(tmp->pSegment);

    lsm_free(p->pEnv, tmp);
  }
  
  if (p->pShm->hFile != NULL && p->pShm->hFile != INVALID_HANDLE_VALUE) {
    CloseHandle(p->pShm->hFile);
    p->pShm->hFile = NULL;
  }

  p->pShm->nSizeSharedMemory.QuadPart = 0;
  if (bDelete) {
    int ret = LSM_OK;
    shmFileName = lsm_malloc(p->pEnv, (shmFileNameSize + 1) * sizeof(wchar_t));
    if (shmFileName == NULL) {
      return LSM_NOMEM_BKPT;
    }

    lsmWindowsSharedMemoryFileName(p->zName, shmFileName, shmFileNameSize);
    ret = DeleteFile(shmFileName) ? LSM_OK : LSM_IOERR_BKPT;
    lsm_free(p->pEnv, shmFileName);
    
    return ret;
  }
  
  return LSM_OK;
}


static int lsmWindowsOsClose(lsm_file *pFile) {
  WindowsFile *p = (WindowsFile *)pFile;
  lsmWindowsOsShmUnmap(pFile, 0);

  if (p->pMap != NULL) {
    UnmapViewOfFile(p->pMap);
    p->pMap = NULL;
  }

  if (p->hFile != NULL && p->hFile != INVALID_HANDLE_VALUE) {
    CloseHandle(p->hFile);
    p->hFile = NULL;
  }

  if (p->pShm != NULL) {
    lsm_free(p->pEnv, p->pShm);
    p->pShm = NULL;
  }

  lsm_free(p->pEnv, p);
  return LSM_OK;
}


static int lsmWindowsOsSleep(lsm_env *pEnv, int us) {
  HANDLE hSleep = CreateEventEx(NULL, NULL, CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE);
  WaitForSingleObjectEx(hSleep, (us + 999) / 1000, FALSE);
  CloseHandle(hSleep);
  return LSM_OK;
}

/****************************************************************************
** Memory allocation routines.
*/
#define BLOCK_HDR_SIZE ROUND8( sizeof(sqlite4_size_t) )

static void *lsmWindowsOsMalloc(lsm_env *pEnv, int N) {
  unsigned char * m;
  N += BLOCK_HDR_SIZE;
  m = (unsigned char *)malloc(N);
  *((sqlite4_size_t*)m) = N;
  return m + BLOCK_HDR_SIZE;
}

static void lsmWindowsOsFree(lsm_env *pEnv, void *p) {
  if (p) {
    free(((unsigned char *)p) - BLOCK_HDR_SIZE);
  }
}

static void *lsmWindowsOsRealloc(lsm_env *pEnv, void *p, int N) {
  unsigned char * m = (unsigned char *)p;
  if (1 > N) {
    lsmWindowsOsFree(pEnv, p);
    return NULL;
  }
  else if (NULL == p) {
    return lsmWindowsOsMalloc(pEnv, N);
  }
  else {
    void * re = NULL;
    m -= BLOCK_HDR_SIZE;
    re = realloc(m, N + BLOCK_HDR_SIZE);
    if (re) {
      m = (unsigned char *)re;
      *((sqlite4_size_t*)m) = N;
      return m + BLOCK_HDR_SIZE;
    }
    else {
      return NULL;
    }
  }
}

static sqlite4_size_t lsmWindowsOsMSize(lsm_env *pEnv, void *p) {
  unsigned char * m = (unsigned char *)p;
  return *((sqlite4_size_t*)(m - BLOCK_HDR_SIZE));
}
#undef BLOCK_HDR_SIZE

#define INVALID_LSM_ENV (lsm_env *)-1

typedef struct WindowsThreadMutex WindowsThreadMutex;
struct WindowsThreadMutex {
  lsm_env *pEnv;
  DWORD nOwner;
  CRITICAL_SECTION criticalSection;
};

static WindowsThreadMutex csGlobal = { 0 };
static WindowsThreadMutex csHeap = { 0 };

static int lsmWindowsOsMutexStatic(
  lsm_env *pEnv,
  int mutexType,
  lsm_mutex **ppStatic
  ) {

  assert(mutexType == LSM_MUTEX_GLOBAL || mutexType == LSM_MUTEX_HEAP);
  assert(LSM_MUTEX_GLOBAL == 1 && LSM_MUTEX_HEAP == 2);
  if (mutexType == 1) {
    *ppStatic = (lsm_mutex *)&csGlobal;
  }
  else {
    *ppStatic = (lsm_mutex *)&csHeap;
  }

  return LSM_OK;
}

static int lsmWindowsOsMutexNew(lsm_env *pEnv, lsm_mutex **ppNew) {
  WindowsThreadMutex *m = lsm_malloc(pEnv, sizeof(WindowsThreadMutex));
  if (m == NULL) {
    return LSM_NOMEM_BKPT;
  }

  memset(m, 0, sizeof(WindowsThreadMutex));
  m->pEnv = pEnv;
  InitializeCriticalSectionEx(&m->criticalSection, 0, 0);

  *ppNew = (lsm_mutex *)m;
  return LSM_OK;
}

static void lsmWindowsOsMutexDel(lsm_mutex *p) {
  WindowsThreadMutex *m = (WindowsThreadMutex *)p;
  if (m->pEnv == INVALID_LSM_ENV) {
    return;
  }

  DeleteCriticalSection(&m->criticalSection);
  lsm_free(m->pEnv, m);
}

static void lsmWindowsOsMutexEnter(lsm_mutex *p) {
  WindowsThreadMutex *m = (WindowsThreadMutex *)p;

  EnterCriticalSection(&m->criticalSection);
  m->nOwner = GetCurrentThreadId();
}

static int lsmWindowsOsMutexTry(lsm_mutex *p) {
  WindowsThreadMutex *m = (WindowsThreadMutex *)p;
  BOOL entered = TryEnterCriticalSection(&m->criticalSection);
  if (entered) {
    m->nOwner = GetCurrentThreadId();
  }

  return entered? LSM_OK : LSM_BUSY;
}

static void lsmWindowsOsMutexLeave(lsm_mutex *p) {
  WindowsThreadMutex *m = (WindowsThreadMutex *)p;
  DWORD currentId = GetCurrentThreadId();
  if (m->nOwner == currentId) {
    m->nOwner = 0;
  }

  LeaveCriticalSection(&m->criticalSection);
}

static int lsmWindowsOsMutexHeld(lsm_mutex *p) {
  WindowsThreadMutex *m = (WindowsThreadMutex *)p;
  return m->nOwner == GetCurrentThreadId();
}

static int lsmWindowsOsMutexNotHeld(lsm_mutex *p) {
  WindowsThreadMutex *m = (WindowsThreadMutex *)p;
  return m->nOwner != GetCurrentThreadId();
}

lsm_env *lsm_default_env(void) {
  if (csGlobal.pEnv == NULL) {
    csGlobal.pEnv = INVALID_LSM_ENV;
    InitializeCriticalSection(&csGlobal.criticalSection);
  }

  if (csHeap.pEnv == NULL) {
    csHeap.pEnv = INVALID_LSM_ENV;
    InitializeCriticalSection(&csHeap.criticalSection);
  }

  static lsm_env windows_env = {
    sizeof(lsm_env),         /* nByte */
    1,                       /* iVersion */
    /***** file i/o ******************/
    0,                       /* pVfsCtx */
    lsmWindowsOsFullpath,      /* xFullpath */
    lsmWindowsOsOpen,          /* xOpen */
    lsmWindowsOsRead,          /* xRead */
    lsmWindowsOsWrite,         /* xWrite */
    lsmWindowsOsTruncate,      /* xTruncate */
    lsmWindowsOsSync,          /* xSync */
    lsmWindowsOsSectorSize,    /* xSectorSize */
    lsmWindowsOsRemap,         /* xRemap */
    lsmWindowsOsFileid,        /* xFileid */
    lsmWindowsOsClose,         /* xClose */
    lsmWindowsOsUnlink,        /* xUnlink */
    lsmWindowsOsLock,          /* xLock */
    lsmWindowsOsTestLock,      /* xTestLock */
    lsmWindowsOsShmMap,        /* xShmMap */
    lsmWindowsOsShmBarrier,    /* xShmBarrier */
    lsmWindowsOsShmUnmap,      /* xShmUnmap */
    /***** memory allocation *********/
    0,                         /* pMemCtx */
    lsmWindowsOsMalloc,        /* xMalloc */
    lsmWindowsOsRealloc,       /* xRealloc */
    lsmWindowsOsFree,          /* xFree */
    lsmWindowsOsMSize,         /* xSize */
    /***** mutexes *********************/
    0,                         /* pMutexCtx */
    lsmWindowsOsMutexStatic,   /* xMutexStatic */
    lsmWindowsOsMutexNew,      /* xMutexNew */
    lsmWindowsOsMutexDel,      /* xMutexDel */
    lsmWindowsOsMutexEnter,    /* xMutexEnter */
    lsmWindowsOsMutexTry,      /* xMutexTry */
    lsmWindowsOsMutexLeave,    /* xMutexLeave */
    lsmWindowsOsMutexHeld,     /* xMutexHeld */
    lsmWindowsOsMutexNotHeld,  /* xMutexNotHeld */
    /***** other *********************/
    lsmWindowsOsSleep,         /* xSleep */
  };
  return &windows_env;
}

#undef INVALID_LSM_ENV
