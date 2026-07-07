/*
** Filename:  FileAndDirUtils.h
**
** Purpose:
**   Declares CreateFullPath, a Windows equivalent of "mkdir -p": creates
**   every missing intermediate directory component of a path, one path
**   component at a time.
*/

#pragma once

/* Macros and Defines */
#define MAX_FULL_PATH_NAME 4000

/* Functions */

/*
** Function: CreateFullPath
** @brief    Creates every missing directory component of pszFullPath, in
**           order, so the full path exists afterward.
** @param    pszFullPath - the full path whose directory components should exist
** @return   true if the full path exists (or was created) afterward; false on failure.
*/
bool CreateFullPath(const char* pszFullPath);
