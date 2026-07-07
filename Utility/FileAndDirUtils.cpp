/*
** Filename:  FileAndDirUtils.cpp
**
** Purpose:
**   Implements CreateFullPath (declared in FileAndDirUtils.h): walks a path
**   string component by component, calling _mkdir on each directory prefix
**   in turn so every missing intermediate directory gets created, in order.
*/

/* Includes */
#include <stdio.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include "Error.h"
#include "Utility.h"

/* Functions */
static bool createPath(char* pPathToCreate);   /* forward declaration -- defined after CreateFullPath below, sorted by name among this file's helpers */

/*
** Function: CreateFullPath
** @brief    Creates every missing directory component of pszFullPath, in
**           order, so the full path exists afterward.
** @details  Scans pszFullPath one character at a time, copying it into
**           tempName. Each time a path separator (or the terminating NUL)
**           is reached, tempName up to that point is a complete directory
**           prefix, so createPath is called on it -- meaning a path like
**           "C:\a\b\c" triggers _mkdir on "C:\a", then "C:\a\b", then
**           "C:\a\b\c" in turn. A drive letter prefix ("C:\") is copied
**           verbatim up front, since it is never itself a directory to create.
** @param    pszFullPath - the full path whose directory components should exist
** @return   true if the full path exists (or was created) afterward; false on failure.
*/
bool CreateFullPath(const char* pszFullPath)
{
    bool    result                             = true;                 /* overall success/failure of the whole path creation         */
    char    tempName[MAX_FULL_PATH_NAME + 1];                          /* path built up one component at a time                      */
    size_t  nameLen                            = strlen(pszFullPath);  /* length of the input path                                   */
    int     currIdx                            = 0;                    /* current scan position within pszFullPath                   */
    bool    hadADirName                        = false;                /* true once tempName holds a real (non-empty) path component */

    memset(tempName, 0, sizeof(tempName));

    /* Reject up front rather than overflowing tempName partway through the scan. */
    if (nameLen > MAX_FULL_PATH_NAME)
    {
        result = false;
        Error(UTIL_RC_Path_Too_Long, "The path '%s' is larger than allowed (%zu)\n", pszFullPath, (size_t)MAX_FULL_PATH_NAME);
    }
    else
    {
        /* A drive letter prefix ("C:" or "C:\") is copied verbatim and
        ** skipped over -- it is never itself a directory to create.
        */
        if (nameLen >= 2 && pszFullPath[1] == ':')
        {
            currIdx = 2;
            tempName[0] = pszFullPath[0];
            tempName[1] = pszFullPath[1];
            if (nameLen >= 3 && (pszFullPath[2] == '\\' || pszFullPath[2] == '/'))
            {
                currIdx = 3;
                tempName[2] = '\\';
            }
        }
    }

    for (; currIdx <= nameLen && result; currIdx++)
    {
        switch (pszFullPath[currIdx])
        {
            /* End of string or a path separator both mark the end of a
            ** directory component -- create everything gathered so far.
            */
            case '\0':
            case '\\':
            case '/':
                if (hadADirName)
                    result = createPath(tempName);
                tempName[currIdx] = '\\';
                break;
            default:
                tempName[currIdx] = pszFullPath[currIdx];
                hadADirName = true;
        }
    }

    return result;
}

/*
** Function: createPath
** @brief    Creates a single directory, treating "already exists" as success.
** @param    pPathToCreate - the directory path to create
** @return   true on success or if the directory already existed; false otherwise.
*/
static bool createPath(char* pPathToCreate)
{
    int  mkDirResult = _mkdir(pPathToCreate);
    bool result      = true;

    /* EEXIST means another component (or a previous run) already created
    ** this directory -- that is success, not failure, for our purposes.
    */
    if (mkDirResult != 0 && errno != EEXIST)
    {
        Error(UTIL_RC_Could_Not_Create_Directory, "The directory '%s' cannot be created (%d)", pPathToCreate, errno);
        result = false;
    }

    return result;
}
