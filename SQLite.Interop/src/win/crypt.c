/********************************************************
 * ADO.NET 2.0 Data Provider for SQLite Version 3.X
 * Written by Robert Simpson (robert@blackcastlesoft.com)
 *
 * Released to the public domain, use at your own risk!
 ********************************************************/

#ifndef SQLITE_OMIT_DISKIO
#ifdef SQLITE_HAS_CODEC

#include <windows.h>
#include <wincrypt.h>

/* Extra padding before and after the cryptographic buffer */
#define CRYPT_OFFSET 8

typedef struct _CRYPTBLOCK
{
  Pager    *    pPager;       /* Pager this cryptblock belongs to */
  HCRYPTKEY     hReadKey;     /* Key used to read from the database and write to the journal */
  HCRYPTKEY     hWriteKey;    /* Key used to write to the database */
  DWORD         dwPageSize;   /* Size of pages */
  LPVOID        pvCrypt;      /* A buffer for encrypting/decrypting */
  DWORD         dwBlockSize;  /* Block size of the encryption algorithm (in bytes) */
  LPVOID        pvIV;         /* A buffer for storing the IV of the encryption algorithm mode */
} CRYPTBLOCK, *LPCRYPTBLOCK;

HCRYPTPROV g_hProvider = 0; /* Global instance of the cryptographic provider */

#define SQLITECRYPTERROR_PROVIDER "Cryptographic provider not available"

/* Needed for re-keying */
static void * sqlite3pager_get_codecarg(Pager *pPager)
{
  return (pPager->xCodec) ? pPager->pCodec: NULL;
}

void sqlite3_activate_see(const char *info)
{
}

/* Create a cryptographic context.  Use the enhanced provider because it is available on
** most platforms
*/
static BOOL InitializeProvider()
{
    BOOL result = FALSE;
    MUTEX_LOGIC( sqlite3_mutex *pMaster = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER); )
    __try
    {
        sqlite3_mutex_enter(pMaster);

        if (g_hProvider)
        {
            return TRUE;
        }
        result = CryptAcquireContext(&g_hProvider, NULL, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    }
    __finally
    {
        sqlite3_mutex_leave(pMaster);
    }
    return result;
}

/* Create or update a cryptographic context for a pager.
** This function will automatically determine if the encryption algorithm requires
** extra padding, and if it does, will create a temp buffer big enough to provide
** space to hold it.
*/
static LPCRYPTBLOCK CreateCryptBlock(HCRYPTKEY hKey, Pager *pager, int pageSize, LPCRYPTBLOCK pExisting)
{
  LPCRYPTBLOCK pBlock;

  if (!pExisting) /* Creating a new cryptblock */
  {
    pBlock = (LPCRYPTBLOCK)sqlite3_malloc(sizeof(CRYPTBLOCK));
    if (!pBlock) return NULL;

    ZeroMemory(pBlock, sizeof(CRYPTBLOCK));
    pBlock->hReadKey = hKey;
    pBlock->hWriteKey = hKey;
  }
  else /* Updating an existing cryptblock */
  {
    pBlock = pExisting;
  }

  if (pageSize == -1)
    pageSize = pager->pageSize;

  pBlock->pPager = pager;
  pBlock->dwPageSize = (DWORD)pageSize;

  /* Existing cryptblocks may have a buffer, if so, delete it */
  if (pBlock->pvCrypt)
  {
    sqlite3_free(pBlock->pvCrypt);
    pBlock->pvCrypt = NULL;
  }

  pBlock->pvCrypt = sqlite3_malloc(pBlock->dwPageSize);
  if (!pBlock->pvCrypt)
  {
    /* We created a new block in here, so free it.  Otherwise leave the original intact */
    if (pBlock != pExisting)
      sqlite3_free(pBlock);

    return NULL;
  }

  {
      DWORD dwGutter;
      DWORD dwBlockSize;
      CryptGetKeyParam(hKey, KP_BLOCKLEN, (BYTE*)&dwBlockSize, &dwGutter, 0);
      dwBlockSize /= 8;
      if (dwBlockSize != pBlock->dwBlockSize)
      {
          pBlock->dwBlockSize = dwBlockSize;
          if (pBlock->pvIV)
          {
              sqlite3_free(pBlock->pvIV);
              pBlock->pvIV = 0;
          }
          pBlock->pvIV = sqlite3_malloc(pBlock->dwBlockSize);
          if (!pBlock->pvIV)
          {
              if (pBlock != pExisting)
              {
                  sqlite3_free(pBlock->pvCrypt);
                  sqlite3_free(pBlock);
              }
              return NULL;
          }
      }
  }
  
  return pBlock;
}

/* Destroy a cryptographic context and any buffers and keys allocated therein */
static void sqlite3CodecFree(LPVOID pv)
{
  LPCRYPTBLOCK pBlock = (LPCRYPTBLOCK)pv;
  /* Destroy the read key if there is one */
  if (pBlock->hReadKey)
  {
    CryptDestroyKey(pBlock->hReadKey);
  }

  /* If there's a writekey and its not equal to the readkey, destroy it */
  if (pBlock->hWriteKey && pBlock->hWriteKey != pBlock->hReadKey)
  {
    CryptDestroyKey(pBlock->hWriteKey);
  }
  pBlock->hReadKey = 0;
  pBlock->hWriteKey = 0;

  /* If there's extra buffer space allocated, free it as well */
  if (pBlock->pvCrypt)
  {
    sqlite3_free(pBlock->pvCrypt);
    pBlock->pvCrypt = 0;
  }
  if (pBlock->pvIV)
  {
      sqlite3_free(pBlock->pvIV);
      pBlock->pvIV = 0;
  }

  /* All done with this cryptblock */
  sqlite3_free(pBlock);
}

void sqlite3CodecSizeChange(void *pArg, int pageSize, int reservedSize)
{
  LPCRYPTBLOCK pBlock = (LPCRYPTBLOCK)pArg;

  if (pBlock->dwPageSize != pageSize)
  {
    CreateCryptBlock(pBlock->hReadKey, pBlock->pPager, pageSize, pBlock);
    /* If this fails, pvCrypt will be NULL, and the next time sqlite3Codec() is called, it will result in an error */
  }
}

/* Encrypt/Decrypt functionality, called by pager.c */
void * sqlite3Codec(void *pArg, void *data, Pgno nPageNum, int nMode)
{
  LPCRYPTBLOCK pBlock = (LPCRYPTBLOCK)pArg;
  DWORD dwEffectivePageSize;
  DWORD dwBlockSize;
  HCRYPTKEY hCurrentKey;

  if (!pBlock) return data;
  if (pBlock->pvCrypt == NULL) return NULL; /* This only happens if CreateCryptBlock() failed to make scratch space */

  dwBlockSize = pBlock->dwBlockSize;
  dwEffectivePageSize = pBlock->dwPageSize - dwBlockSize;

  switch(nMode)
  {
  case 0: /* Undo a "case 7" journal file encryption */
  case 2: /* Reload a page */
  case 3: /* Load a page */
    if (!pBlock->hReadKey) break;

    // Obtain a local key for actual decryption
    if (!CryptDuplicateKey(pBlock->hReadKey, NULL, 0, &hCurrentKey))
        break;

    __try
    {
        // Read IV from the reserved space at the end of the page
        CopyMemory(pBlock->pvIV, ((LPBYTE)data) + dwEffectivePageSize, dwBlockSize);
        if (!CryptSetKeyParam(hCurrentKey, KP_IV, (BYTE*)pBlock->pvIV, 0))
            break;

        // Decrypt!
        {
            DWORD dwBufferSize = dwEffectivePageSize;
            CryptDecrypt(hCurrentKey, 0, FALSE, 0, (LPBYTE)data, &dwBufferSize);
        }
    }
    __finally
    {
        // Destroy the local decryption key
        CryptDestroyKey(hCurrentKey);
    }

    break;
  case 6: /* Encrypt a page for the main database file */
    if (!pBlock->hWriteKey) break;
  
    // Obtain a local key for actual encryption
    if (!CryptDuplicateKey(pBlock->hWriteKey, NULL, 0, &hCurrentKey))
        break;

    __try
    {
        if (!CryptGenRandom(g_hProvider, dwBlockSize, (LPBYTE)pBlock->pvIV))
            break;
        if (!CryptSetKeyParam(hCurrentKey, KP_IV, (LPBYTE)pBlock->pvIV, 0))
            break;

        // Copy page data into buffer
        CopyMemory((LPBYTE)pBlock->pvCrypt, data, pBlock->dwPageSize);
        data = (LPBYTE)pBlock->pvCrypt;

        // Copy IV to reserved space at the end of the page
        CopyMemory(((LPBYTE)data) + dwEffectivePageSize, pBlock->pvIV, dwBlockSize);

        // Encrypt effective page data (except IV)
        {
            DWORD dwBufferSize = dwEffectivePageSize;
            CryptEncrypt(hCurrentKey, 0, FALSE, 0, ((LPBYTE)data), &dwBufferSize, dwBufferSize);
        }
    }
    __finally
    {
        // Destroy local encryption key
        CryptDestroyKey(hCurrentKey);
    }

    break;
  case 7: /* Encrypt a page for the journal file */
    /* Under normal circumstances, the readkey is the same as the writekey.  However,
    when the database is being rekeyed, the readkey is not the same as the writekey.
    The rollback journal must be written using the original key for the
    database file because it is, by nature, a rollback journal.
    Therefore, for case 7, when the rollback is being written, always encrypt using
    the database's readkey, which is guaranteed to be the same key that was used to
    read the original data.
    */
    if (!pBlock->hReadKey) break;

    // Obtain a local key for actual encryption
    if (!CryptDuplicateKey(pBlock->hReadKey, NULL, 0, &hCurrentKey))
        break;

    __try
    {
        // Generate a new random IV for the page
        if (!CryptGenRandom(g_hProvider, dwBlockSize, (LPBYTE)pBlock->pvIV))
            break;
        if (!CryptSetKeyParam(hCurrentKey, KP_IV, (LPBYTE)pBlock->pvIV, 0))
            break;

        // Copy page data into buffer
        CopyMemory((LPBYTE)pBlock->pvCrypt, data, pBlock->dwPageSize);
        data = (LPBYTE)pBlock->pvCrypt;

        // Copy IV to reserved space at the end of the page
        CopyMemory(((LPBYTE)data) + dwEffectivePageSize, (LPBYTE)pBlock->pvIV, dwBlockSize);

        // Encrypt!
        {
            DWORD dwBufferSize = dwEffectivePageSize;
            CryptEncrypt(hCurrentKey, 0, FALSE, 0, ((LPBYTE)data), &dwBufferSize, dwBufferSize);
        }
    }
    __finally
    {
        // Destroy local encryption key
        CryptDestroyKey(hCurrentKey);
    }

    break;
  }

  return data;
}

/* Derive an encryption key from a user-supplied buffer */
static DWORD DeriveKey(const void *pKey, int nKey)
{
    HCRYPTHASH hHash = 0;
    HCRYPTKEY  hKey = (HCRYPTKEY) NULL;

    if (!pKey || !nKey) return 0;

    if (!InitializeProvider())
    {
        return MAXDWORD;
    }

    {
        MUTEX_LOGIC( sqlite3_mutex *pMaster = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER); )
        sqlite3_mutex_enter(pMaster);
        __try
        {
            if (!CryptCreateHash(g_hProvider, CALG_SHA_512, 0, 0, &hHash))
                return MAXDWORD;
            __try
            {
                if (!CryptHashData(hHash, (LPBYTE)pKey, nKey, 0))
                    return MAXDWORD;
                if (!CryptDeriveKey(g_hProvider, CALG_AES_256, hHash, 0, &hKey))
                {
                    return MAXDWORD;
                }
                else
                {
                    DWORD dwMode = CRYPT_MODE_CBC;
                    if (!CryptSetKeyParam(hKey, KP_MODE, (BYTE*)&dwMode, 0))
                    {
                        CryptDestroyKey(hKey);
                        return MAXDWORD;
                    }
                }
            
            }
            __finally
            {
                CryptDestroyHash(hHash);
            }

        }
        __finally
        {
            sqlite3_mutex_leave(pMaster);
        }
    }

    return hKey;
}

/* Called by sqlite and sqlite3_key_interop to attach a key to a database. */
int sqlite3CodecAttach(sqlite3 *db, int nDb, const void *pKey, int nKeyLen)
{
  HCRYPTKEY hKey = 0;

  /* No key specified, could mean either use the main db's encryption or no encryption */
  if (!pKey || !nKeyLen)
  {
    if (!nDb)
    {
      return SQLITE_OK; /* Main database, no key specified so not encrypted */
    }
    else /* Attached database, use the main database's key */
    {
      /* Get the encryption block for the main database and attempt to duplicate the key
      ** for use by the attached database
      */
      Pager *p = sqlite3BtreePager(db->aDb[0].pBt);
      LPCRYPTBLOCK pBlock = (LPCRYPTBLOCK)sqlite3pager_get_codecarg(p);

      if (!pBlock) return SQLITE_OK; /* Main database is not encrypted so neither will be any attached database */
      if (!pBlock->hReadKey) return SQLITE_OK; /* Not encrypted */

      if (!CryptDuplicateKey(pBlock->hReadKey, NULL, 0, &hKey))
        return SQLITE_ERROR; /* Unable to duplicate the key */
    }
  }
  else /* User-supplied passphrase, so create a cryptographic key out of it */
  {
    hKey = DeriveKey(pKey, nKeyLen);
    if (hKey == MAXDWORD)
    {
#if SQLITE_VERSION_NUMBER >= 3008007
        sqlite3ErrorWithMsg(db, SQLITE_ERROR, SQLITECRYPTERROR_PROVIDER);
#else
        sqlite3Error(db, SQLITE_ERROR, SQLITECRYPTERROR_PROVIDER);
#endif
        return SQLITE_ERROR;
    }
  }

  /* Create a new encryption block and assign the codec to the new attached database */
  if (hKey)
  {
    Pager *p = sqlite3BtreePager(db->aDb[nDb].pBt);

    LPCRYPTBLOCK pBlock = CreateCryptBlock(hKey, p, -1, NULL);
    if (!pBlock)
    {
        CryptDestroyKey(hKey);
        return SQLITE_NOMEM;
    }

    __try
    {
        sqlite3_mutex_enter(db->mutex);
        if (p->nReserve != pBlock->dwBlockSize)
        {
            int rc = sqlite3BtreeSetPageSize(db->aDb[0].pBt, p->pageSize, pBlock->dwBlockSize, 0);
            if (rc != SQLITE_OK)
            {
#if SQLITE_VERSION_NUMBER >= 3008007
                sqlite3ErrorWithMsg(db, rc, "could not set reserved space! setpagesize failed");
#else
                sqlite3Error(db, rc, "could not set reserved space! setpagesize failed");
#endif
                sqlite3CodecFree(pBlock);
                return rc;
            }
        }
    }
    __finally
    {
        sqlite3_mutex_leave(db->mutex);
    }

    sqlite3PagerSetCodec(p, sqlite3Codec, sqlite3CodecSizeChange, sqlite3CodecFree, pBlock);
    return SQLITE_OK;
  }
  else
  {
#if SQLITE_VERSION_NUMBER >= 3008007
      sqlite3ErrorWithMsg(db, SQLITE_ERROR, "could not acquire key!");
#else
      sqlite3Error(db, SQLITE_ERROR, "could not acquire key!");
#endif
      return SQLITE_ERROR;
  }
}

/* Once a password has been supplied and a key created, we don't keep the
** original password for security purposes.  Therefore return NULL.
*/
void sqlite3CodecGetKey(sqlite3 *db, int nDb, void **ppKey, int *pnKeyLen)
{
  Btree *pbt = db->aDb[0].pBt;
  Pager *p = sqlite3BtreePager(pbt);
  LPCRYPTBLOCK pBlock = (LPCRYPTBLOCK)sqlite3pager_get_codecarg(p);

  if (ppKey) *ppKey = 0;
  if (pnKeyLen && pBlock) *pnKeyLen = 1;
}

/* We do not attach this key to the temp store, only the main database. */
SQLITE_API int sqlite3_key_v2(sqlite3 *db, const char *zDbName, const void *pKey, int nKey)
{
    return sqlite3CodecAttach(db, 0, pKey, nKey);
}

SQLITE_API int sqlite3_key(sqlite3 *db, const void *pKey, int nKey)
{
    return sqlite3_key_v2(db, 0, pKey, nKey);
}

/* Changes the encryption key for an existing database. */
SQLITE_API int sqlite3_rekey_v2(sqlite3 *db, const char *zDbName, const void *pKey, int nKey)
{
    int rc;
    sqlite3_mutex_enter(db->mutex);

    rc = SQLITE_ERROR;
#if SQLITE_VERSION_NUMBER >= 3008007
    sqlite3ErrorWithMsg(db, rc, "rekey is not implemented");
#else
    sqlite3Error(db, rc, "rekey is not implemented");
#endif

    rc = sqlite3ApiExit(db, rc);
    sqlite3_mutex_leave(db->mutex);
    return rc;
    
  //Btree *pbt = db->aDb[0].pBt;
  //Pager *p = sqlite3BtreePager(pbt);
  //LPCRYPTBLOCK pBlock = (LPCRYPTBLOCK)sqlite3pager_get_codecarg(p);
  //HCRYPTKEY hKey = DeriveKey(pKey, nKey);
  //int rc = SQLITE_ERROR;

  //if (hKey == MAXDWORD)
  //{
  //  sqlite3Error(db, rc, SQLITECRYPTERROR_PROVIDER);
  //  return rc;
  //}

  //if (!pBlock && !hKey) return SQLITE_OK; /* Wasn't encrypted to begin with */

  ///* To rekey a database, we change the writekey for the pager.  The readkey remains
  //** the same
  //*/
  //if (!pBlock) /* Encrypt an unencrypted database */
  //{
  //  pBlock = CreateCryptBlock(hKey, p, -1, NULL);
  //  if (!pBlock)
  //  {
  //      CryptDestroyKey(hKey);
  //      return SQLITE_NOMEM;
  //  }

  //  pBlock->hReadKey = 0; /* Original database is not encrypted */
  //  sqlite3PagerSetCodec(p, sqlite3Codec, sqlite3CodecSizeChange, sqlite3CodecFree, pBlock);
  //}
  //else /* Change the writekey for an already-encrypted database */
  //{
  //  pBlock->hWriteKey = hKey;
  //}

  //sqlite3_mutex_enter(db->mutex);

  ///* Start a transaction */
  //rc = sqlite3BtreeBeginTrans(pbt, 1);

  //if (!rc)
  //{
  //  /* Rewrite all the pages in the database using the new encryption key */
  //  Pgno nPage;
  //  Pgno nSkip = PAGER_MJ_PGNO(p);
  //  DbPage *pPage;
  //  Pgno n;
  //  int count;

  //  sqlite3PagerPagecount(p, &count);
  //  nPage = (Pgno)count;

  //  for(n = 1; n <= nPage; n ++)
  //  {
  //    if (n == nSkip) continue;
  //    rc = sqlite3PagerGet(p, n, &pPage);
  //    if(!rc)
  //    {
  //      rc = sqlite3PagerWrite(pPage);
  //      sqlite3PagerUnref(pPage);
  //    }
  //  }
  //}

  ///* If we succeeded, try and commit the transaction */
  //if (!rc)
  //{
  //  rc = sqlite3BtreeCommit(pbt);
  //}

  //// If we failed, rollback */
  //if (rc)
  //{
  //  sqlite3BtreeRollback(pbt, SQLITE_OK);
  //}

  ///* If we succeeded, destroy any previous read key this database used
  //** and make the readkey equal to the writekey
  //*/
  //if (!rc)
  //{
  //  if (pBlock->hReadKey)
  //  {
  //    CryptDestroyKey(pBlock->hReadKey);
  //  }
  //  pBlock->hReadKey = pBlock->hWriteKey;
  //}
  ///* We failed.  Destroy the new writekey (if there was one) and revert it back to
  //** the original readkey
  //*/
  //else
  //{
  //  if (pBlock->hWriteKey)
  //  {
  //    CryptDestroyKey(pBlock->hWriteKey);
  //  }
  //  pBlock->hWriteKey = pBlock->hReadKey;
  //}

  ///* If the readkey and writekey are both empty, there's no need for a codec on this
  //** pager anymore.  Destroy the crypt block and remove the codec from the pager.
  //*/
  //if (!pBlock->hReadKey && !pBlock->hWriteKey)
  //{
  //  sqlite3PagerSetCodec(p, NULL, NULL, NULL, NULL);
  //}

  //sqlite3_mutex_leave(db->mutex);

  //return rc;
}

SQLITE_API int sqlite3_rekey(sqlite3 *db, const void *pKey, int nKey)
{
  return sqlite3_rekey_v2(db, 0, pKey, nKey);
}

#endif /* SQLITE_HAS_CODEC */
#endif /* SQLITE_OMIT_DISKIO */
