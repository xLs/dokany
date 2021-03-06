/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2018 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

  http://dokan-dev.github.io

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "../../dokan/dokan.h"
#include "../../dokan/fileinfo.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <winbase.h>
#include <vector>
#include <list>
#include <string>
#include <algorithm>

//#define WIN10_ENABLE_LONG_PATH
#ifdef WIN10_ENABLE_LONG_PATH
//dirty but should be enough
#define DOKAN_MAX_PATH 32768
#else
#define DOKAN_MAX_PATH MAX_PATH
#endif // DEBUG

#define MAX_ROOT_DIRECTORIES  3
//static WCHAR RootDirectories[MAX_ROOT_DIRECTORIES][DOKAN_MAX_PATH] = {L"C:", 0, 0};


static WCHAR RootDirectory[DOKAN_MAX_PATH] = L"C:";
static WCHAR MountPoint[DOKAN_MAX_PATH] = L"M:\\";
static WCHAR UNCName[DOKAN_MAX_PATH] = L"";
static size_t CurrentRootDirectory = 0;



struct ReplicationHandle
	{
		ReplicationHandle(const std::wstring &path_, bool isMaster = false) 
		{ 
			outOfSync = false; master = isMaster; handle = 0; lastStatus = 0; path = path_; 
		}
		
		ReplicationHandle(const ReplicationHandle&) = default;
		
		operator HANDLE() { return handle;}

		void MakeFilePath(PWCHAR filePath, ULONG numberOfElements, LPCWSTR FileName) 
		{
			wcsncpy_s(filePath, numberOfElements, path.c_str(), wcslen(path.c_str()));
			size_t unclen = wcslen(UNCName);
			if (unclen > 0 && _wcsnicmp(FileName, UNCName, unclen) == 0) 
			{
				if (_wcsnicmp(FileName + unclen, L".", 1) != 0) 
				{
					wcsncat_s(filePath, numberOfElements, FileName + unclen,
					wcslen(FileName) - unclen);
				}
			} 
			else 
			{
				wcsncat_s(filePath, numberOfElements, FileName, wcslen(FileName));
			}
		}

		void MakeFilePath(LPCWSTR FileName) 
		{
			if(resolved_path.size() == 0)
			{
				resolved_path.resize(DOKAN_MAX_PATH);
				wcsncpy_s(&resolved_path[0], DOKAN_MAX_PATH, path.c_str(), wcslen(path.c_str()));
				size_t unclen = wcslen(UNCName);

				if (unclen > 0 && _wcsnicmp(FileName, UNCName, unclen) == 0) 
				{
					if (_wcsnicmp(FileName + unclen, L".", 1) != 0) 
					{
						wcsncat_s(&resolved_path[0], DOKAN_MAX_PATH, FileName + unclen, wcslen(FileName) - unclen);
					}
				} 
				else 
				{
					wcsncat_s(&resolved_path[0], DOKAN_MAX_PATH, FileName, wcslen(FileName));
				}
			}
		}
		bool outOfSync;
		bool master;
		HANDLE handle;
		std::wstring path;
		std::wstring resolved_path;
		NTSTATUS lastStatus;
	};


static NTSTATUS DOKAN_CALLBACK ReplicatedGetFileInformation( ReplicationHandle* rep_handle, LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation, PDOKAN_FILE_INFO DokanFileInfo) ;
static NTSTATUS DOKAN_CALLBACK ReplicatedSetFileTime( ReplicationHandle* rep_handle, LPCWSTR FileName, CONST FILETIME *CreationTime, CONST FILETIME *LastAccessTime, CONST FILETIME *LastWriteTime, PDOKAN_FILE_INFO DokanFileInfo) ;

struct ReplicatedDirectory
{
	ReplicatedDirectory(std::wstring path_, bool master_ = false) : path(path_) , master(master_)
	{
	}
	ReplicatedDirectory(const ReplicatedDirectory&) = default;
	
	bool master;
	std::wstring path;
};


static std::vector<ReplicatedDirectory> RootDirectories;

class ReplicatedFile
{
public:

	

private:
	ReplicatedFile() 
	{ 
		_master = _handles.end();
		SynchronizeHandles(); 
	}

	~ReplicatedFile() 	{   }
public:

	void Release()
	{
		delete this;
	}

	static ReplicatedFile * Create()
	{
		return new ReplicatedFile();
	}

	
	
	std::list<ReplicationHandle>::iterator begin()
	{
		return _handles.begin();
	}

	std::list<ReplicationHandle>::iterator end()
	{
		return _handles.end();
	}
		
	void SynchronizeHandles()
	{
		std::vector<ReplicationHandle> removedHandles;
		if(RootDirectories.size() != _handles.size())
		{
			auto rh = _handles.begin();
			while(rh != _handles.end()) // remove no longer paths
			{
				bool found = false;
				for(auto dir : RootDirectories )
				{
					if(rh->path.compare(dir.path) == 0){ found = true; break; }
				}
				if(!found)
				{
					_handles.erase(rh++);
				}
				else 
					++rh;
			}
		}

		for(auto dir : RootDirectories )
		{
			bool found = false;
			for(auto rh : _handles)
			{
				if(rh.path.compare(dir.path) == 0){ found = true; break; }
			}
			if(!found) 
				_handles.push_back(ReplicationHandle(dir.path, dir.master));
		}
		
		UpdateMaster();
	}

	BOOL IntegrityCheck(LPCWSTR FileName, PDOKAN_FILE_INFO dokan_file_info)
	{
		return TRUE;
		if(wcslen(FileName) <2) return TRUE;
		auto latestHandle = GetLatestModified(FileName,dokan_file_info);
		if(latestHandle == _handles.end()) 
			return TRUE; // All ok :) kindof

		auto rh = _handles.begin();
		while(rh != _handles.end()) // remove no longer paths
		{
			if(rh != latestHandle && rh->outOfSync)
			{
				::CopyFile( latestHandle->resolved_path.c_str(), rh->resolved_path.c_str(), FALSE);
				printf("Copy out of sync file\n");
			}
			++rh;
		}
		SynchronizeFT(FileName,dokan_file_info);
		return TRUE;
	}

	// returns latest modified handle, if all are equal, end iterator is returned;
	std::list<ReplicationHandle>::iterator GetLatestModified(LPCWSTR FileName, PDOKAN_FILE_INFO dokan_file_info)
	{
		auto rh = _handles.begin();
		
		std::vector<BY_HANDLE_FILE_INFORMATION> fi;
		FILETIME ft = {0};
		auto latestHandle = _handles.end();
		fi.resize(_handles.size());
		auto fit = fi.begin();
		while(rh != _handles.end()) // remove no longer paths
		{
			FILETIME* ft2 = &fit->ftLastWriteTime;
			ReplicatedGetFileInformation(&*rh ,FileName,&*fit++,dokan_file_info);
			if(rh == _handles.begin())
			{
				latestHandle = rh;
				ft = *ft2;
			}
			else
			{
				int c = CompareFileTime(&ft, ft2);
				if( c != 0) 
				{
					if(c < 0)
					{
						latestHandle->outOfSync = true; // previous is older
						latestHandle = rh;
						ft = *ft2;
					}else
					{
						rh->outOfSync = true; // current is older
						printf("File out of sync\n");
					}
				}
			}
			++rh;
		}
		return latestHandle; // returns end() iterator if all are the same
	}

	void SynchronizeFT(LPCWSTR FileName, PDOKAN_FILE_INFO dokan_file_info)
	{
		return;
		if(_master != _handles.end())
		{
			BY_HANDLE_FILE_INFORMATION fi = {0};
			ReplicationHandle* h = &*_master;
			auto mstat = _master->lastStatus; // lets not overwrite status
			ReplicatedGetFileInformation(h ,FileName,&fi,dokan_file_info);
			_master->lastStatus = mstat;

			auto rh = _handles.begin();
			while(rh != _handles.end()) // remove no longer paths
			{
				if(rh != _master)
				{
					auto stat = rh->lastStatus; // lets not overwrite status
					ReplicatedSetFileTime(&*rh,FileName, &fi.ftCreationTime, &fi.ftLastAccessTime, &fi.ftLastWriteTime, dokan_file_info);
					rh->lastStatus = stat;
					rh->outOfSync = false;
				}
				++rh;
			}
		}
	}
	void UpdateMaster()
	{
		std::list<ReplicationHandle>::iterator master = _handles.end();
		bool found = false;
		auto rh = _handles.begin();
		while(rh != _handles.end()) // remove no longer paths
		{
			if(rh->master == true) { master = rh; found = true; break; }
			++rh;
		}
		if(!found) master = _handles.begin(); // can be end()
		if(master != _master)
		{
			// Master Changed
			_master = master;
			// TODO: check diffs, who is latest greatest GOD
		}
	}
	
private:
	bool MasterFile;
	std::list<ReplicationHandle> _handles;
	std::list<ReplicationHandle>::iterator _master;
};



BOOL g_UseStdErr;
BOOL g_DebugMode;
BOOL g_HasSeSecurityPrivilege;
BOOL g_ImpersonateCallerUser;

#ifdef _DEBUG
static void DbgPrint(LPCWSTR format, ...) {
  if (g_DebugMode) {
    const WCHAR *outputString;
    WCHAR *buffer = NULL;
    size_t length;
    va_list argp;

    va_start(argp, format);
    length = _vscwprintf(format, argp) + 1;
    buffer = (WCHAR*)_malloca(length * sizeof(WCHAR));
    if (buffer) {
      vswprintf_s(buffer, length, format, argp);
      outputString = buffer;
    } else {
      outputString = format;
    }
    if (g_UseStdErr)
      fputws(outputString, stderr);
    else
      OutputDebugStringW(outputString);
    if (buffer)
      _freea(buffer);
    va_end(argp);
    if (g_UseStdErr)
      fflush(stderr);
  }
}
#else
static void DbgPrint(LPCWSTR format, ...) { }
#endif


static void GetFilePath(PWCHAR filePath, ULONG numberOfElements, LPCWSTR FileName) {
  wcsncpy_s(filePath, numberOfElements, RootDirectories[0].path.c_str(), wcslen(RootDirectories[0].path.c_str()));
  size_t unclen = wcslen(UNCName);
  if (unclen > 0 && _wcsnicmp(FileName, UNCName, unclen) == 0) {
    if (_wcsnicmp(FileName + unclen, L".", 1) != 0) {
      wcsncat_s(filePath, numberOfElements, FileName + unclen,
                wcslen(FileName) - unclen);
    }
  } else {
    wcsncat_s(filePath, numberOfElements, FileName, wcslen(FileName));
  }
}

static void PrintUserName(PDOKAN_FILE_INFO DokanFileInfo) {
  HANDLE handle;
  UCHAR buffer[1024];
  DWORD returnLength;
  WCHAR accountName[256];
  WCHAR domainName[256];
  DWORD accountLength = sizeof(accountName) / sizeof(WCHAR);
  DWORD domainLength = sizeof(domainName) / sizeof(WCHAR);
  PTOKEN_USER tokenUser;
  SID_NAME_USE snu;

  if (!g_DebugMode)
    return;

  handle = DokanOpenRequestorToken(DokanFileInfo);
  if (handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"  DokanOpenRequestorToken failed\n");
    return;
  }

  if (!GetTokenInformation(handle, TokenUser, buffer, sizeof(buffer),
                           &returnLength)) {
    DbgPrint(L"  GetTokenInformaiton failed: %d\n", GetLastError());
    CloseHandle(handle);
    return;
  }

  CloseHandle(handle);

  tokenUser = (PTOKEN_USER)buffer;
  if (!LookupAccountSid(NULL, tokenUser->User.Sid, accountName, &accountLength,
                        domainName, &domainLength, &snu)) {
    DbgPrint(L"  LookupAccountSid failed: %d\n", GetLastError());
    return;
  }

  DbgPrint(L"  AccountName: %s, DomainName: %s\n", accountName, domainName);
}

static BOOL AddSeSecurityNamePrivilege() {
  HANDLE token = 0;
  DbgPrint(
           L"## Attempting to add SE_SECURITY_NAME privilege to process token ##\n");
  DWORD err;
  LUID luid;
  if (!LookupPrivilegeValue(0, SE_SECURITY_NAME, &luid)) {
    err = GetLastError();
    if (err != ERROR_SUCCESS) {
      DbgPrint(L"  failed: Unable to lookup privilege value. error = %u\n",
               err);
      return FALSE;
    }
  }

  LUID_AND_ATTRIBUTES attr;
  attr.Attributes = SE_PRIVILEGE_ENABLED;
  attr.Luid = luid;

  TOKEN_PRIVILEGES priv;
  priv.PrivilegeCount = 1;
  priv.Privileges[0] = attr;

  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    err = GetLastError();
    if (err != ERROR_SUCCESS) {
      DbgPrint(L"  failed: Unable obtain process token. error = %u\n", err);
      return FALSE;
    }
  }

  TOKEN_PRIVILEGES oldPriv;
  DWORD retSize;
  AdjustTokenPrivileges(token, FALSE, &priv, sizeof(TOKEN_PRIVILEGES), &oldPriv,
                        &retSize);
  err = GetLastError();
  if (err != ERROR_SUCCESS) {
    DbgPrint(L"  failed: Unable to adjust token privileges: %u\n", err);
    CloseHandle(token);
    return FALSE;
  }

  BOOL privAlreadyPresent = FALSE;
  for (unsigned int i = 0; i < oldPriv.PrivilegeCount; i++) {
    if (oldPriv.Privileges[i].Luid.HighPart == luid.HighPart &&
        oldPriv.Privileges[i].Luid.LowPart == luid.LowPart) {
      privAlreadyPresent = TRUE;
      break;
    }
  }
  DbgPrint(privAlreadyPresent ? L"  success: privilege already present\n"
             : L"  success: privilege added\n");
  if (token)
    CloseHandle(token);
  return TRUE;
}

#define MirrorCheckFlag(val, flag)                                             \
  if (val & flag) {                                                            \
    DbgPrint(L"\t" L#flag L"\n");                                              \
  }

  static NTSTATUS DOKAN_CALLBACK
ReplicatedCreateFile(ReplicationHandle* rep_handle, LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
                 ACCESS_MASK DesiredAccess, ULONG FileAttributes,
                 ULONG ShareAccess, ULONG CreateDisposition,
                 ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo, bool& file_opened) {
  
  
  DWORD fileAttr;
  NTSTATUS status = STATUS_SUCCESS;
  DWORD creationDisposition;
  DWORD fileAttributesAndFlags;
  DWORD error = 0;
  SECURITY_ATTRIBUTES securityAttrib;
  ACCESS_MASK genericDesiredAccess;
  // userTokenHandle is for Impersonate Caller User Option
  HANDLE userTokenHandle = INVALID_HANDLE_VALUE;
  file_opened = false;
  securityAttrib.nLength = sizeof(securityAttrib);
  securityAttrib.lpSecurityDescriptor =
    SecurityContext->AccessState.SecurityDescriptor;
  securityAttrib.bInheritHandle = FALSE;

  DokanMapKernelToUserCreateFileFlags(
      DesiredAccess, FileAttributes, CreateOptions, CreateDisposition,
	  &genericDesiredAccess, &fileAttributesAndFlags, &creationDisposition);

  
  rep_handle->MakeFilePath(FileName);
  DbgPrint(L"CreateFile : %s\n", rep_handle->resolved_path.c_str());

  PrintUserName(DokanFileInfo);

  /*
  if (ShareMode == 0 && AccessMode & FILE_WRITE_DATA)
          ShareMode = FILE_SHARE_WRITE;
  else if (ShareMode == 0)
          ShareMode = FILE_SHARE_READ;
  */

  DbgPrint(L"\tShareMode = 0x%x\n", ShareAccess);

  MirrorCheckFlag(ShareAccess, FILE_SHARE_READ);
  MirrorCheckFlag(ShareAccess, FILE_SHARE_WRITE);
  MirrorCheckFlag(ShareAccess, FILE_SHARE_DELETE);

  DbgPrint(L"\tDesiredAccess = 0x%x\n", DesiredAccess);

  MirrorCheckFlag(DesiredAccess, GENERIC_READ);
  MirrorCheckFlag(DesiredAccess, GENERIC_WRITE);
  MirrorCheckFlag(DesiredAccess, GENERIC_EXECUTE);

  MirrorCheckFlag(DesiredAccess, DELETE);
  MirrorCheckFlag(DesiredAccess, FILE_READ_DATA);
  MirrorCheckFlag(DesiredAccess, FILE_READ_ATTRIBUTES);
  MirrorCheckFlag(DesiredAccess, FILE_READ_EA);
  MirrorCheckFlag(DesiredAccess, READ_CONTROL);
  MirrorCheckFlag(DesiredAccess, FILE_WRITE_DATA);
  MirrorCheckFlag(DesiredAccess, FILE_WRITE_ATTRIBUTES);
  MirrorCheckFlag(DesiredAccess, FILE_WRITE_EA);
  MirrorCheckFlag(DesiredAccess, FILE_APPEND_DATA);
  MirrorCheckFlag(DesiredAccess, WRITE_DAC);
  MirrorCheckFlag(DesiredAccess, WRITE_OWNER);
  MirrorCheckFlag(DesiredAccess, SYNCHRONIZE);
  MirrorCheckFlag(DesiredAccess, FILE_EXECUTE);
  MirrorCheckFlag(DesiredAccess, STANDARD_RIGHTS_READ);
  MirrorCheckFlag(DesiredAccess, STANDARD_RIGHTS_WRITE);
  MirrorCheckFlag(DesiredAccess, STANDARD_RIGHTS_EXECUTE);

  // When filePath is a directory, needs to change the flag so that the file can
  // be opened.
  fileAttr = GetFileAttributes(rep_handle->resolved_path.c_str());

  if (fileAttr != INVALID_FILE_ATTRIBUTES
    && fileAttr & FILE_ATTRIBUTE_DIRECTORY) {
      if (!(CreateOptions & FILE_NON_DIRECTORY_FILE)) {
        DokanFileInfo->IsDirectory = TRUE;
        // Needed by FindFirstFile to list files in it
        // TODO: use ReOpenFile in MirrorFindFiles to set share read temporary
        ShareAccess |= FILE_SHARE_READ;
      } else { // FILE_NON_DIRECTORY_FILE - Cannot open a dir as a file
        DbgPrint(L"\tCannot open a dir as a file\n");
        return STATUS_FILE_IS_A_DIRECTORY;
      }
  }

  DbgPrint(L"\tFlagsAndAttributes = 0x%x\n", fileAttributesAndFlags);

  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_ARCHIVE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_COMPRESSED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_DEVICE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_DIRECTORY);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_ENCRYPTED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_HIDDEN);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_INTEGRITY_STREAM);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_NORMAL);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_NO_SCRUB_DATA);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_OFFLINE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_READONLY);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_REPARSE_POINT);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_SPARSE_FILE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_SYSTEM);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_TEMPORARY);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_VIRTUAL);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_WRITE_THROUGH);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_OVERLAPPED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_NO_BUFFERING);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_RANDOM_ACCESS);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_SEQUENTIAL_SCAN);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_DELETE_ON_CLOSE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_BACKUP_SEMANTICS);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_POSIX_SEMANTICS);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_OPEN_REPARSE_POINT);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_OPEN_NO_RECALL);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_ANONYMOUS);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_IDENTIFICATION);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_IMPERSONATION);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_DELEGATION);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_CONTEXT_TRACKING);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_EFFECTIVE_ONLY);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_SQOS_PRESENT);

  if (creationDisposition == CREATE_NEW) {
    DbgPrint(L"\tCREATE_NEW\n");
  } else if (creationDisposition == OPEN_ALWAYS) {
    DbgPrint(L"\tOPEN_ALWAYS\n");
  } else if (creationDisposition == CREATE_ALWAYS) {
    DbgPrint(L"\tCREATE_ALWAYS\n");
  } else if (creationDisposition == OPEN_EXISTING) {
    DbgPrint(L"\tOPEN_EXISTING\n");
  } else if (creationDisposition == TRUNCATE_EXISTING) {
    DbgPrint(L"\tTRUNCATE_EXISTING\n");
  } else {
    DbgPrint(L"\tUNKNOWN creationDisposition!\n");
  }

  if (g_ImpersonateCallerUser) {
    userTokenHandle = DokanOpenRequestorToken(DokanFileInfo);

    if (userTokenHandle == INVALID_HANDLE_VALUE) {
      DbgPrint(L"  DokanOpenRequestorToken failed\n");
      // Should we return some error?
    }
  }

  if (DokanFileInfo->IsDirectory) {
    // It is a create directory request

    if (creationDisposition == CREATE_NEW ||
        creationDisposition == OPEN_ALWAYS) {

      if (g_ImpersonateCallerUser && userTokenHandle != INVALID_HANDLE_VALUE) {
        // if g_ImpersonateCallerUser option is on, call the ImpersonateLoggedOnUser function.
        if (!ImpersonateLoggedOnUser(userTokenHandle)) {
          // handle the error if failed to impersonate
          DbgPrint(L"\tImpersonateLoggedOnUser failed.\n");
        }
      }

      //We create folder
      if (!CreateDirectory(rep_handle->resolved_path.c_str(), &securityAttrib)) {
        error = GetLastError();
        // Fail to create folder for OPEN_ALWAYS is not an error
        if (error != ERROR_ALREADY_EXISTS ||
            creationDisposition == CREATE_NEW) {
          DbgPrint(L"\terror code = %d\n\n", error);
          status = DokanNtStatusFromWin32(error);
        }
      }

      if (g_ImpersonateCallerUser && userTokenHandle != INVALID_HANDLE_VALUE) {
        // Clean Up operation for impersonate
        DWORD lastError = GetLastError();
        if (status != STATUS_SUCCESS) //Keep the handle open for CreateFile
          CloseHandle(userTokenHandle);
        RevertToSelf();
        SetLastError(lastError);
      }
    }

    if (status == STATUS_SUCCESS) {

      //Check first if we're trying to open a file as a directory.
      if (fileAttr != INVALID_FILE_ATTRIBUTES &&
          !(fileAttr & FILE_ATTRIBUTE_DIRECTORY) &&
          (CreateOptions & FILE_DIRECTORY_FILE)) {
        return STATUS_NOT_A_DIRECTORY;
      }

      if (g_ImpersonateCallerUser && userTokenHandle != INVALID_HANDLE_VALUE) {
        // if g_ImpersonateCallerUser option is on, call the ImpersonateLoggedOnUser function.
        if (!ImpersonateLoggedOnUser(userTokenHandle)) {
          // handle the error if failed to impersonate
          DbgPrint(L"\tImpersonateLoggedOnUser failed.\n");
        }
      }

      // FILE_FLAG_BACKUP_SEMANTICS is required for opening directory handles
      HANDLE handle =
        CreateFile(rep_handle->resolved_path.c_str(), genericDesiredAccess, ShareAccess,
                   &securityAttrib, OPEN_EXISTING,
                   fileAttributesAndFlags | FILE_FLAG_BACKUP_SEMANTICS, NULL);

      if (g_ImpersonateCallerUser && userTokenHandle != INVALID_HANDLE_VALUE) {
        // Clean Up operation for impersonate
        DWORD lastError = GetLastError();
        CloseHandle(userTokenHandle);
        RevertToSelf();
        SetLastError(lastError);
      }

      if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        DbgPrint(L"\terror code = %d\n\n", error);

        status = DokanNtStatusFromWin32(error);
      } else {
        rep_handle->handle = handle; // save the file handle in Context
		
        // Open succeed but we need to inform the driver
        // that the dir open and not created by returning STATUS_OBJECT_NAME_COLLISION
        if (creationDisposition == OPEN_ALWAYS &&
            fileAttr != INVALID_FILE_ATTRIBUTES)
          return STATUS_OBJECT_NAME_COLLISION;
      }
    }
  } else {
    // It is a create file request

    // Cannot overwrite a hidden or system file if flag not set
    if (fileAttr != INVALID_FILE_ATTRIBUTES &&
        ((!(fileAttributesAndFlags & FILE_ATTRIBUTE_HIDDEN) &&
          (fileAttr & FILE_ATTRIBUTE_HIDDEN)) ||
         (!(fileAttributesAndFlags & FILE_ATTRIBUTE_SYSTEM) &&
          (fileAttr & FILE_ATTRIBUTE_SYSTEM))) &&
        (creationDisposition == TRUNCATE_EXISTING ||
         creationDisposition == CREATE_ALWAYS))
      return STATUS_ACCESS_DENIED;

    // Cannot delete a read only file
    if ((fileAttr != INVALID_FILE_ATTRIBUTES &&
         (fileAttr & FILE_ATTRIBUTE_READONLY) ||
         (fileAttributesAndFlags & FILE_ATTRIBUTE_READONLY)) &&
        (fileAttributesAndFlags & FILE_FLAG_DELETE_ON_CLOSE))
      return STATUS_CANNOT_DELETE;

    // Truncate should always be used with write access
    if (creationDisposition == TRUNCATE_EXISTING)
      genericDesiredAccess |= GENERIC_WRITE;

    if (g_ImpersonateCallerUser && userTokenHandle != INVALID_HANDLE_VALUE) {
      // if g_ImpersonateCallerUser option is on, call the ImpersonateLoggedOnUser function.
      if (!ImpersonateLoggedOnUser(userTokenHandle)) {
        // handle the error if failed to impersonate
        DbgPrint(L"\tImpersonateLoggedOnUser failed.\n");
      }
    }

    HANDLE handle = CreateFile(
                        rep_handle->resolved_path.c_str(),
        genericDesiredAccess, // GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
                        ShareAccess,
                        &securityAttrib, // security attribute
                        creationDisposition,
                        fileAttributesAndFlags, // |FILE_FLAG_NO_BUFFERING,
                        NULL);                  // template file handle

    if (g_ImpersonateCallerUser && userTokenHandle != INVALID_HANDLE_VALUE) {
      // Clean Up operation for impersonate
      DWORD lastError = GetLastError();
      CloseHandle(userTokenHandle);
      RevertToSelf();
      SetLastError(lastError);
    }

    if (handle == INVALID_HANDLE_VALUE) {
      error = GetLastError();
      DbgPrint(L"\terror code = %d\n\n", error);

      status = DokanNtStatusFromWin32(error);
    } else {

      //Need to update FileAttributes with previous when Overwrite file
      if (fileAttr != INVALID_FILE_ATTRIBUTES &&
          creationDisposition == TRUNCATE_EXISTING) {
        SetFileAttributes(rep_handle->resolved_path.c_str(), fileAttributesAndFlags | fileAttr);
      }

      rep_handle->handle = handle; // save the file handle in Context
	  
      if (creationDisposition == OPEN_ALWAYS ||
          creationDisposition == CREATE_ALWAYS) {
        error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
          DbgPrint(L"\tOpen an already existing file\n");
          // Open succeed but we need to inform the driver
          // that the file open and not created by returning STATUS_OBJECT_NAME_COLLISION
		  file_opened = true;
          status = STATUS_OBJECT_NAME_COLLISION;
        }
      }
    }
  }

  DbgPrint(L"\n");
  return status;
}


static NTSTATUS DOKAN_CALLBACK
MirrorCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
                 ACCESS_MASK DesiredAccess, ULONG FileAttributes,
                 ULONG ShareAccess, ULONG CreateDisposition,
                 ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo) {
  

	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}
	bool file_was_opened = false;
	bool file_was_created = false; // not needed really
	NTSTATUS lastStatus = 0;
	for(auto& i: *replicated)
	{
		bool opened = false;
		i.lastStatus = ReplicatedCreateFile(&i, FileName, SecurityContext, DesiredAccess, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, DokanFileInfo, opened);
		lastStatus = i.lastStatus;
		if(opened) file_was_opened = opened;
		if(!opened) file_was_created = opened;
	}
	if(wcslen(FileName) > 1)
	{
		
		{
			replicated->IntegrityCheck(FileName,DokanFileInfo);
		}
		replicated->SynchronizeFT(FileName, DokanFileInfo);
	}
	// TODO: which error
	return lastStatus;
}


#pragma warning(push)
#pragma warning(disable : 4305)



static void DOKAN_CALLBACK ReplicatedCloseFile(ReplicationHandle* rep_handle, LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
  

  if (DokanFileInfo->Context) {
    DbgPrint(L"CloseFile: %s\n", rep_handle->resolved_path.c_str());
    DbgPrint(L"\terror : not cleanuped file\n\n");
    CloseHandle(rep_handle->handle);
	rep_handle->handle = 0;
	rep_handle->lastStatus = 0;
  
  } else {
    DbgPrint(L"Close: %s\n\n", rep_handle->resolved_path.c_str());
  }

}

static void DOKAN_CALLBACK MirrorCloseFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		return;
	}

	for(auto& i: *replicated)
	{
		ReplicatedCloseFile(&i, FileName, DokanFileInfo);
	}
	replicated->SynchronizeFT(FileName, DokanFileInfo);
	replicated->Release();
	DokanFileInfo->Context = 0;

}


static void DOKAN_CALLBACK ReplicatedCleanup(ReplicationHandle* rep_handle, LPCWSTR FileName,
                                         PDOKAN_FILE_INFO DokanFileInfo) {
  

  if (DokanFileInfo->Context) 
  {
    DbgPrint(L"Cleanup: %s\n\n", rep_handle->resolved_path.c_str());
    CloseHandle((HANDLE)(rep_handle->handle));
    rep_handle->handle = 0;
  } else {
    DbgPrint(L"Cleanup: %s\n\tinvalid handle\n\n", rep_handle->resolved_path.c_str());
  }

  if (DokanFileInfo->DeleteOnClose) 
  {
    // Should already be deleted by CloseHandle
    // if open with FILE_FLAG_DELETE_ON_CLOSE
    DbgPrint(L"\tDeleteOnClose\n");
    if (DokanFileInfo->IsDirectory) {
      DbgPrint(L"  DeleteDirectory ");
      if (!RemoveDirectory(rep_handle->resolved_path.c_str())) {
        DbgPrint(L"error code = %d\n\n", GetLastError());
      } else {
        DbgPrint(L"success\n\n");
      }
    } else {
      DbgPrint(L"  DeleteFile ");
      if (DeleteFile(rep_handle->resolved_path.c_str()) == 0) {
        DbgPrint(L" error code = %d\n\n", GetLastError());
      } else {
        DbgPrint(L"success\n\n");
      }
    }
  }
}


static void DOKAN_CALLBACK MirrorCleanup(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		return;
	}

	for(auto& i: *replicated)
	{
		ReplicatedCleanup(&i, FileName, DokanFileInfo);
	}

	replicated->Release();
	DokanFileInfo->Context = 0;

}



static NTSTATUS DOKAN_CALLBACK ReplicatedReadFile(ReplicationHandle* rep_handle, LPCWSTR FileName, LPVOID Buffer,
                                              DWORD BufferLength,
                                              LPDWORD ReadLength,
                                              LONGLONG Offset,
                                              PDOKAN_FILE_INFO DokanFileInfo) 
{
  UNREFERENCED_PARAMETER(DokanFileInfo);
  
  HANDLE handle = rep_handle->handle;
  ULONG offset = (ULONG)Offset;
  BOOL opened = FALSE;

  

  DbgPrint(L"ReadFile : %s\n", rep_handle->resolved_path.c_str());

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle, cleanuped?\n");
    handle = CreateFile(rep_handle->resolved_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      DbgPrint(L"\tCreateFile error : %d\n\n", error);
      return DokanNtStatusFromWin32(error);
    }
    opened = TRUE;
  }

  LARGE_INTEGER distanceToMove;
  distanceToMove.QuadPart = Offset;
  if (!SetFilePointerEx(handle, distanceToMove, NULL, FILE_BEGIN)) {
    DWORD error = GetLastError();
    DbgPrint(L"\tseek error, offset = %d\n\n", offset);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);
  }

  if (!ReadFile(handle, Buffer, BufferLength, ReadLength, NULL)) {
    DWORD error = GetLastError();
    DbgPrint(L"\tread error = %u, buffer length = %d, read length = %d\n\n",
             error, BufferLength, *ReadLength);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);

  } else {
    DbgPrint(L"\tByte to read: %d, Byte read %d, offset %d\n\n", BufferLength,
             *ReadLength, offset);
  }

  if (opened)
    CloseHandle(handle);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorReadFile(LPCWSTR FileName, LPVOID Buffer, DWORD BufferLength, LPDWORD ReadLength, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedReadFile(&i, FileName, Buffer, BufferLength, ReadLength, Offset, DokanFileInfo);
	}
	
	replicated->SynchronizeFT(FileName, DokanFileInfo);
	// TODO: which error
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK ReplicatedWriteFile(ReplicationHandle* rep_handle, LPCWSTR FileName, LPCVOID Buffer,
                                               DWORD NumberOfBytesToWrite,
                                               LPDWORD NumberOfBytesWritten,
                                               LONGLONG Offset,
                                               PDOKAN_FILE_INFO DokanFileInfo) {
  
  HANDLE handle = rep_handle->handle;
  BOOL opened = FALSE;

  

  DbgPrint(L"WriteFile : %s, offset %I64d, length %d\n", rep_handle->resolved_path.c_str(), Offset,
           NumberOfBytesToWrite);

  // reopen the file
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle, cleanuped?\n");
    handle = CreateFile(rep_handle->resolved_path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      DbgPrint(L"\tCreateFile error : %d\n\n", error);
      return DokanNtStatusFromWin32(error);
    }
    opened = TRUE;
  }

  UINT64 fileSize = 0;
  DWORD fileSizeLow = 0;
  DWORD fileSizeHigh = 0;
  fileSizeLow = GetFileSize(handle, &fileSizeHigh);
  if (fileSizeLow == INVALID_FILE_SIZE) {
    DWORD error = GetLastError();
    DbgPrint(L"\tcan not get a file size error = %d\n", error);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);
  }

  fileSize = ((UINT64)fileSizeHigh << 32) | fileSizeLow;

  LARGE_INTEGER distanceToMove;
  if (DokanFileInfo->WriteToEndOfFile) {
    LARGE_INTEGER z;
    z.QuadPart = 0;
    if (!SetFilePointerEx(handle, z, NULL, FILE_END)) {
      DWORD error = GetLastError();
      DbgPrint(L"\tseek error, offset = EOF, error = %d\n", error);
      if (opened)
        CloseHandle(handle);
      return DokanNtStatusFromWin32(error);
    }
  } else {
    // Paging IO cannot write after allocate file size.
    if (DokanFileInfo->PagingIo) {
      if ((UINT64)Offset >= fileSize) {
        *NumberOfBytesWritten = 0;
        if (opened)
          CloseHandle(handle);
        return STATUS_SUCCESS;
      }

      if (((UINT64)Offset + NumberOfBytesToWrite) > fileSize) {
        UINT64 bytes = fileSize - Offset;
        if (bytes >> 32) {
          NumberOfBytesToWrite = (DWORD)(bytes & 0xFFFFFFFFUL);
        } else {
          NumberOfBytesToWrite = (DWORD)bytes;
        }
      }
    }

    if ((UINT64)Offset > fileSize) {
      // In the mirror sample helperZeroFileData is not necessary. NTFS will
      // zero a hole.
      // But if user's file system is different from NTFS( or other Windows's
      // file systems ) then  users will have to zero the hole themselves.
    }

    distanceToMove.QuadPart = Offset;
    if (!SetFilePointerEx(handle, distanceToMove, NULL, FILE_BEGIN)) {
      DWORD error = GetLastError();
      DbgPrint(L"\tseek error, offset = %I64d, error = %d\n", Offset, error);
      if (opened)
        CloseHandle(handle);
      return DokanNtStatusFromWin32(error);
    }
  }

  if (!WriteFile(handle, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten,
                 NULL)) {
    DWORD error = GetLastError();
    DbgPrint(L"\twrite error = %u, buffer length = %d, write length = %d\n",
             error, NumberOfBytesToWrite, *NumberOfBytesWritten);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);

  } else {
    DbgPrint(L"\twrite %d, offset %I64d\n\n", *NumberOfBytesWritten, Offset);
  }

  // close the file when it is reopened
  if (opened)
    CloseHandle(handle);

  return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK MirrorWriteFile(LPCWSTR FileName, LPCVOID Buffer, DWORD NumberOfBytesToWrite, LPDWORD NumberOfBytesWritten, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedWriteFile(&i, FileName, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten, Offset, DokanFileInfo);
	}
	
	replicated->SynchronizeFT(FileName, DokanFileInfo);

	// TODO: which error
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedFlushFileBuffers(ReplicationHandle* rep_handle, LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
	
	HANDLE handle = rep_handle->handle;

	

	DbgPrint(L"FlushFileBuffers : %s\n", rep_handle->resolved_path.c_str());

	if (!handle || handle == INVALID_HANDLE_VALUE) 
	{
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_SUCCESS;
	}

	if (FlushFileBuffers(handle)) 
	{
		return STATUS_SUCCESS;
	} 
	else 
	{
		DWORD error = GetLastError();
		DbgPrint(L"\tflush error code = %d\n", error);
		return DokanNtStatusFromWin32(error);
	}
}

static NTSTATUS DOKAN_CALLBACK MirrorFlushFileBuffers(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
 ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedFlushFileBuffers(&i, FileName, DokanFileInfo);
	}

	// TODO: which error
	return STATUS_SUCCESS;
}



static NTSTATUS DOKAN_CALLBACK ReplicatedGetFileInformation( ReplicationHandle* rep_handle, LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation, PDOKAN_FILE_INFO DokanFileInfo) 
{
  UNREFERENCED_PARAMETER(DokanFileInfo);
  
  HANDLE handle = rep_handle->handle;
  BOOL opened = FALSE;

  

  DbgPrint(L"GetFileInfo : %s\n", rep_handle->resolved_path.c_str());

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle, cleanuped?\n");
    handle = CreateFile(rep_handle->resolved_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      DbgPrint(L"\tCreateFile error : %d\n\n", error);
      return DokanNtStatusFromWin32(error);
    }
    opened = TRUE;
  }

  if (!GetFileInformationByHandle(handle, HandleFileInformation)) {
    DbgPrint(L"\terror code = %d\n", GetLastError());

    // FileName is a root directory
    // in this case, FindFirstFile can't get directory information
    if (wcslen(FileName) == 1) {
      DbgPrint(L"  root dir\n");
      HandleFileInformation->dwFileAttributes = GetFileAttributes(rep_handle->resolved_path.c_str());

    } else {
      WIN32_FIND_DATAW find;
      ZeroMemory(&find, sizeof(WIN32_FIND_DATAW));
      HANDLE findHandle = FindFirstFile(rep_handle->resolved_path.c_str(), &find);
      if (findHandle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        DbgPrint(L"\tFindFirstFile error code = %d\n\n", error);
        if (opened)
          CloseHandle(handle);
        return DokanNtStatusFromWin32(error);
      }
      HandleFileInformation->dwFileAttributes = find.dwFileAttributes;
      HandleFileInformation->ftCreationTime = find.ftCreationTime;
      HandleFileInformation->ftLastAccessTime = find.ftLastAccessTime;
      HandleFileInformation->ftLastWriteTime = find.ftLastWriteTime;
      HandleFileInformation->nFileSizeHigh = find.nFileSizeHigh;
      HandleFileInformation->nFileSizeLow = find.nFileSizeLow;
      DbgPrint(L"\tFindFiles OK, file size = %d\n", find.nFileSizeLow);
      FindClose(findHandle);
    }
  } else {
    DbgPrint(L"\tGetFileInformationByHandle success, file size = %d\n",
             HandleFileInformation->nFileSizeLow);
  }

  DbgPrint(L"FILE ATTRIBUTE  = %d\n", HandleFileInformation->dwFileAttributes);

  if (opened)
    CloseHandle(handle);

  return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK MirrorGetFileInformation( LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation,PDOKAN_FILE_INFO DokanFileInfo) 
{
ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}
	NTSTATUS someStatus = STATUS_SUCCESS;
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedGetFileInformation(&i, FileName, HandleFileInformation, DokanFileInfo);
		someStatus = i.lastStatus;
	}

	return someStatus;
}


static NTSTATUS DOKAN_CALLBACK ReplicatedFindFiles( ReplicationHandle* rep_handle, LPCWSTR FileName, PFillFindData FillFindData,  PDOKAN_FILE_INFO DokanFileInfo) 
{

	
  WCHAR filePath[DOKAN_MAX_PATH];
  size_t fileLen;
  HANDLE hFind;
  WIN32_FIND_DATAW findData;
  DWORD error;
  int count = 0;

    rep_handle->resolved_path.copy(filePath,DOKAN_MAX_PATH);

  DbgPrint(L"FindFiles : %s\n", filePath);

  fileLen = wcslen(filePath);
  if (filePath[fileLen - 1] != L'\\') {
    filePath[fileLen++] = L'\\';
  }
  filePath[fileLen] = L'*';
  filePath[fileLen + 1] = L'\0';

  hFind = FindFirstFile(filePath, &findData);

  if (hFind == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    DbgPrint(L"\tinvalid file handle. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  // Root folder does not have . and .. folder - we remove them
  BOOLEAN rootFolder = (wcscmp(FileName, L"\\") == 0);
  do {
    if (!rootFolder || (wcscmp(findData.cFileName, L".") != 0 &&
                        wcscmp(findData.cFileName, L"..") != 0))
      FillFindData(&findData, DokanFileInfo);
    count++;
  } while (FindNextFile(hFind, &findData) != 0);

  error = GetLastError();
  FindClose(hFind);

  if (error != ERROR_NO_MORE_FILES) {
    DbgPrint(L"\tFindNextFile error. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tFindFiles return %d entries in %s\n\n", count, filePath);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorFindFiles(LPCWSTR FileName, PFillFindData FillFindData, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	for(auto& i: *replicated)
	{
		if(i.master && i.handle)
		{
			i.lastStatus = ReplicatedFindFiles(&i, FileName, FillFindData, DokanFileInfo);
			break;
		}
	}

	// TODO: which error
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedDeleteFile(ReplicationHandle* rep_handle, LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
  
  HANDLE handle = rep_handle->handle;

  
  DbgPrint(L"DeleteFile %s - %d\n", rep_handle->resolved_path.c_str(), DokanFileInfo->DeleteOnClose);

  DWORD dwAttrib = GetFileAttributes(rep_handle->resolved_path.c_str());

  if (dwAttrib != INVALID_FILE_ATTRIBUTES &&
      (dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
    return STATUS_ACCESS_DENIED;

  if (handle && handle != INVALID_HANDLE_VALUE) {
    FILE_DISPOSITION_INFO fdi;
    fdi.DeleteFile = DokanFileInfo->DeleteOnClose;
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &fdi,
                                    sizeof(FILE_DISPOSITION_INFO)))
      return DokanNtStatusFromWin32(GetLastError());
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorDeleteFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
 ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedDeleteFile(&i, FileName, DokanFileInfo);
	}

	// TODO: which error, if any
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK ReplicatedDeleteDirectory(ReplicationHandle* rep_handle, LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
  WCHAR filePath[DOKAN_MAX_PATH];
  
  HANDLE hFind;
  WIN32_FIND_DATAW findData;
  size_t fileLen;

  ZeroMemory(filePath, sizeof(filePath));
  
  rep_handle->resolved_path.copy(filePath,DOKAN_MAX_PATH);

  DbgPrint(L"DeleteDirectory %s - %d\n", filePath,
           DokanFileInfo->DeleteOnClose);

  if (!DokanFileInfo->DeleteOnClose)
    //Dokan notify that the file is requested not to be deleted.
    return STATUS_SUCCESS;

  fileLen = wcslen(filePath);
  if (filePath[fileLen - 1] != L'\\') {
    filePath[fileLen++] = L'\\';
  }
  filePath[fileLen] = L'*';
  filePath[fileLen + 1] = L'\0';

  hFind = FindFirstFile(filePath, &findData);

  if (hFind == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  do {
    if (wcscmp(findData.cFileName, L"..") != 0 &&
        wcscmp(findData.cFileName, L".") != 0) {
      FindClose(hFind);
      DbgPrint(L"\tDirectory is not empty: %s\n", findData.cFileName);
      return STATUS_DIRECTORY_NOT_EMPTY;
    }
  } while (FindNextFile(hFind, &findData) != 0);

  DWORD error = GetLastError();

  FindClose(hFind);

  if (error != ERROR_NO_MORE_FILES) {
    DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorDeleteDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedDeleteDirectory(&i, FileName, DokanFileInfo);
	}

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedMoveFile(ReplicationHandle* rep_handle, LPCWSTR FileName, /* existing file name*/ LPCWSTR NewFileName, BOOL ReplaceIfExisting, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
  WCHAR filePath[DOKAN_MAX_PATH];
  WCHAR newFilePath[DOKAN_MAX_PATH];
  HANDLE handle;
  DWORD bufferSize;
  BOOL result;
  size_t newFilePathLen;

  PFILE_RENAME_INFO renameInfo = NULL;
    
  rep_handle->MakeFilePath(filePath, DOKAN_MAX_PATH, FileName);
  rep_handle->MakeFilePath(newFilePath, DOKAN_MAX_PATH, NewFileName);

  DbgPrint(L"MoveFile %s -> %s\n\n", filePath, newFilePath);
  handle = rep_handle->handle;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  newFilePathLen = wcslen(newFilePath);

  // the PFILE_RENAME_INFO struct has space for one WCHAR for the name at
  // the end, so that
  // accounts for the null terminator

  bufferSize = (DWORD)(sizeof(FILE_RENAME_INFO) +
                       newFilePathLen * sizeof(newFilePath[0]));

  renameInfo = (PFILE_RENAME_INFO)malloc(bufferSize);
  if (!renameInfo) {
    return STATUS_BUFFER_OVERFLOW;
  }
  ZeroMemory(renameInfo, bufferSize);

  renameInfo->ReplaceIfExists =
    ReplaceIfExisting
      ? TRUE
          : FALSE; // some warning about converting BOOL to BOOLEAN
  renameInfo->RootDirectory = NULL; // hope it is never needed, shouldn't be
  renameInfo->FileNameLength =
    (DWORD)newFilePathLen *
    sizeof(newFilePath[0]); // they want length in bytes

  wcscpy_s(renameInfo->FileName, newFilePathLen + 1, newFilePath);

  result = SetFileInformationByHandle(handle, FileRenameInfo, renameInfo,
                                      bufferSize);

  free(renameInfo);

  if (result) {
	  rep_handle->resolved_path = newFilePath;
    return STATUS_SUCCESS;
  } else {
    DWORD error = GetLastError();
    DbgPrint(L"\tMoveFile error = %u\n", error);
    return DokanNtStatusFromWin32(error);
  }
}


static NTSTATUS DOKAN_CALLBACK MirrorMoveFile(LPCWSTR FileName, /* existing file name */ LPCWSTR NewFileName, BOOL ReplaceIfExisting, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedMoveFile(&i, FileName, NewFileName, ReplaceIfExisting, DokanFileInfo);
	}

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedLockFile(ReplicationHandle* rep_handle, LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
  WCHAR filePath[DOKAN_MAX_PATH];
  HANDLE handle;
  LARGE_INTEGER offset;
  LARGE_INTEGER length;

  rep_handle->MakeFilePath(filePath, DOKAN_MAX_PATH, FileName);

  DbgPrint(L"LockFile %s\n", filePath);

  handle = rep_handle->handle;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  length.QuadPart = Length;
  offset.QuadPart = ByteOffset;

  if (!LockFile(handle, offset.LowPart, offset.HighPart, length.LowPart,
                length.HighPart)) {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tsuccess\n\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorLockFile(LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedLockFile(&i, FileName, ByteOffset, Length, DokanFileInfo);
	}

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedSetEndOfFile( ReplicationHandle* rep_handle, LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
  WCHAR filePath[DOKAN_MAX_PATH];
  HANDLE handle;
  LARGE_INTEGER offset;

  rep_handle->MakeFilePath(filePath, DOKAN_MAX_PATH, FileName);

  DbgPrint(L"SetEndOfFile %s, %I64d\n", filePath, ByteOffset);

  handle = rep_handle->handle;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  offset.QuadPart = ByteOffset;
  if (!SetFilePointerEx(handle, offset, NULL, FILE_BEGIN)) {
    DWORD error = GetLastError();
    DbgPrint(L"\tSetFilePointer error: %d, offset = %I64d\n\n", error,
             ByteOffset);
    return DokanNtStatusFromWin32(error);
  }

  if (!SetEndOfFile(handle)) {
    DWORD error = GetLastError();
    DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetEndOfFile( LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO DokanFileInfo) 
{
ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedSetEndOfFile(&i, FileName, ByteOffset,  DokanFileInfo);
	}
	
	replicated->SynchronizeFT(FileName, DokanFileInfo);

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedSetAllocationSize( ReplicationHandle* rep_handle, LPCWSTR FileName, LONGLONG AllocSize, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
  
  HANDLE handle;
  LARGE_INTEGER fileSize;



  DbgPrint(L"SetAllocationSize %s, %I64d\n", rep_handle->resolved_path.c_str(), AllocSize);

  handle = rep_handle->handle;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  if (GetFileSizeEx(handle, &fileSize)) {
    if (AllocSize < fileSize.QuadPart) {
      fileSize.QuadPart = AllocSize;
      if (!SetFilePointerEx(handle, fileSize, NULL, FILE_BEGIN)) {
        DWORD error = GetLastError();
        DbgPrint(L"\tSetAllocationSize: SetFilePointer eror: %d, "
                 L"offset = %I64d\n\n",
                 error, AllocSize);
        return DokanNtStatusFromWin32(error);
      }
      if (!SetEndOfFile(handle)) {
        DWORD error = GetLastError();
        DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);
        return DokanNtStatusFromWin32(error);
      }
    }
  } else {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetAllocationSize( LPCWSTR FileName, LONGLONG AllocSize, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedSetAllocationSize(&i, FileName, AllocSize, DokanFileInfo);
	}

	replicated->SynchronizeFT(FileName, DokanFileInfo);

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedSetFileAttributes( ReplicationHandle* rep_handle, LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO DokanFileInfo) 
{
  UNREFERENCED_PARAMETER(DokanFileInfo);

  

  
  DbgPrint(L"SetFileAttributes %s 0x%x\n", rep_handle->resolved_path.c_str(), FileAttributes);

  if (FileAttributes != 0) {
    if (!SetFileAttributes(rep_handle->resolved_path.c_str(), FileAttributes)) {
      DWORD error = GetLastError();
      DbgPrint(L"\terror code = %d\n\n", error);
      return DokanNtStatusFromWin32(error);
    }
  } else {
    // case FileAttributes == 0 :
    // MS-FSCC 2.6 File Attributes : There is no file attribute with the value 0x00000000
    // because a value of 0x00000000 in the FileAttributes field means that the file attributes for this file MUST NOT be changed when setting basic information for the file
    DbgPrint(L"Set 0 to FileAttributes means MUST NOT be changed. Didn't call "
             L"SetFileAttributes function. \n");
  }

  DbgPrint(L"\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetFileAttributes( LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO DokanFileInfo) 
{
ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus = ReplicatedSetFileAttributes(&i, FileName, FileAttributes, DokanFileInfo);
	}

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedSetFileTime( ReplicationHandle* rep_handle, LPCWSTR FileName, CONST FILETIME *CreationTime, CONST FILETIME *LastAccessTime, CONST FILETIME *LastWriteTime, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
  HANDLE handle;
  DbgPrint(L"SetFileTime %s\n", rep_handle->resolved_path.c_str());

  handle = rep_handle->handle;

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  if (!SetFileTime(handle, CreationTime, LastAccessTime, LastWriteTime)) {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetFileTime(LPCWSTR FileName, CONST FILETIME *CreationTime, CONST FILETIME *LastAccessTime, CONST FILETIME *LastWriteTime,  PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus  = ReplicatedSetFileTime( &i, FileName, CreationTime, LastAccessTime,  LastWriteTime, DokanFileInfo);
	}
  // TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedUnlockFile(ReplicationHandle* rep_handle,  LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
	UNREFERENCED_PARAMETER(rep_handle);
  WCHAR filePath[DOKAN_MAX_PATH];
  HANDLE handle;
  LARGE_INTEGER length;
  LARGE_INTEGER offset;

  GetFilePath(filePath, DOKAN_MAX_PATH, FileName);

  DbgPrint(L"UnlockFile %s\n", filePath);

  handle = (HANDLE)DokanFileInfo->Context;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  length.QuadPart = Length;
  offset.QuadPart = ByteOffset;

  if (!UnlockFile(handle, offset.LowPart, offset.HighPart, length.LowPart,
                  length.HighPart)) {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tsuccess\n\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorUnlockFile(LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus  = ReplicatedUnlockFile( &i, FileName,  ByteOffset,  Length,  DokanFileInfo);
	}
  // TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedGetFileSecurity(ReplicationHandle* rep_handle,  LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG BufferLength, PULONG LengthNeeded, PDOKAN_FILE_INFO DokanFileInfo) 
{
  
  BOOLEAN requestingSaclInfo;

  UNREFERENCED_PARAMETER(DokanFileInfo);

	

  DbgPrint(L"GetFileSecurity %s\n", rep_handle->resolved_path.c_str());

  MirrorCheckFlag(*SecurityInformation, FILE_SHARE_READ);
  MirrorCheckFlag(*SecurityInformation, OWNER_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, GROUP_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, DACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, SACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, LABEL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, ATTRIBUTE_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, SCOPE_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation,
    PROCESS_TRUST_LABEL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, BACKUP_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, PROTECTED_DACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, PROTECTED_SACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, UNPROTECTED_DACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, UNPROTECTED_SACL_SECURITY_INFORMATION);

  requestingSaclInfo = ((*SecurityInformation & SACL_SECURITY_INFORMATION) ||
                        (*SecurityInformation & BACKUP_SECURITY_INFORMATION));

  if (!g_HasSeSecurityPrivilege) {
    *SecurityInformation &= ~SACL_SECURITY_INFORMATION;
    *SecurityInformation &= ~BACKUP_SECURITY_INFORMATION;
  }

  DbgPrint(L"  Opening new handle with READ_CONTROL access\n");
  HANDLE handle = CreateFile(
      rep_handle->resolved_path.c_str(),
      READ_CONTROL | ((requestingSaclInfo && g_HasSeSecurityPrivilege)
                          ? ACCESS_SYSTEM_SECURITY
                          : 0),
      FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
      NULL, // security attribute
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, // |FILE_FLAG_NO_BUFFERING,
      NULL);

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    int error = GetLastError();
    return DokanNtStatusFromWin32(error);
  }

  if (!GetUserObjectSecurity(handle, SecurityInformation, SecurityDescriptor,
                             BufferLength, LengthNeeded)) {
    int error = GetLastError();
    if (error == ERROR_INSUFFICIENT_BUFFER) {
      DbgPrint(L"  GetUserObjectSecurity error: ERROR_INSUFFICIENT_BUFFER\n");
      CloseHandle(handle);
      return STATUS_BUFFER_OVERFLOW;
    } else {
      DbgPrint(L"  GetUserObjectSecurity error: %d\n", error);
      CloseHandle(handle);
      return DokanNtStatusFromWin32(error);
    }
  }

  // Ensure the Security Descriptor Length is set
  DWORD securityDescriptorLength =
    GetSecurityDescriptorLength(SecurityDescriptor);
  DbgPrint(L"  GetUserObjectSecurity return true,  *LengthNeeded = "
           L"securityDescriptorLength \n");
  *LengthNeeded = securityDescriptorLength;

  CloseHandle(handle);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetFileSecurity( LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG BufferLength, PULONG LengthNeeded, PDOKAN_FILE_INFO DokanFileInfo) 
{
ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus  = ReplicatedGetFileSecurity( &i, FileName,  SecurityInformation, SecurityDescriptor, BufferLength, LengthNeeded, DokanFileInfo);
	}
  // TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK ReplicatedSetFileSecurity( ReplicationHandle* rep_handle, LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG SecurityDescriptorLength, PDOKAN_FILE_INFO DokanFileInfo) 
{
	UNREFERENCED_PARAMETER(DokanFileInfo);
  HANDLE handle;
  

  UNREFERENCED_PARAMETER(SecurityDescriptorLength);

  

  DbgPrint(L"SetFileSecurity %s\n", rep_handle->resolved_path.c_str());

  handle = rep_handle->handle;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  if (!SetUserObjectSecurity(handle, SecurityInformation, SecurityDescriptor)) {
    int error = GetLastError();
    DbgPrint(L"  SetUserObjectSecurity error: %d\n", error);
    return DokanNtStatusFromWin32(error);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetVolumeInformation(
  LPWSTR VolumeNameBuffer, DWORD VolumeNameSize, LPDWORD VolumeSerialNumber,
  LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
  LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize,
  PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  WCHAR volumeRoot[4];
  DWORD fsFlags = 0;

  wcscpy_s(VolumeNameBuffer, VolumeNameSize, L"NVRAM");

  if (VolumeSerialNumber)
    *VolumeSerialNumber = 0x19831116;
  if (MaximumComponentLength)
    *MaximumComponentLength = 255;
  if (FileSystemFlags)
    *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                       FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK |
                       FILE_PERSISTENT_ACLS | FILE_NAMED_STREAMS;

  volumeRoot[0] = RootDirectories[0].path[0];
  volumeRoot[1] = ':';
  volumeRoot[2] = '\\';
  volumeRoot[3] = '\0';

  if (GetVolumeInformation(volumeRoot, NULL, 0, NULL, MaximumComponentLength,
                           &fsFlags, FileSystemNameBuffer,
                           FileSystemNameSize)) {

    if (FileSystemFlags)
      *FileSystemFlags &= fsFlags;

    if (MaximumComponentLength) {
      DbgPrint(L"GetVolumeInformation: max component length %u\n",
               *MaximumComponentLength);
    }
    if (FileSystemNameBuffer) {
      DbgPrint(L"GetVolumeInformation: file system name %s\n",
               FileSystemNameBuffer);
    }
    if (FileSystemFlags) {
      DbgPrint(L"GetVolumeInformation: got file system flags 0x%08x,"
               L" returning 0x%08x\n",
               fsFlags, *FileSystemFlags);
    }
  } else {

    DbgPrint(L"GetVolumeInformation: unable to query underlying fs,"
             L" using defaults.  Last error = %u\n",
             GetLastError());

    // File system name could be anything up to 10 characters.
    // But Windows check few feature availability based on file system name.
    // For this, it is recommended to set NTFS or FAT here.
    wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"NTFS");
  }

  return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK MirrorSetFileSecurity( LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG SecurityDescriptorLength, PDOKAN_FILE_INFO DokanFileInfo) 
{
	ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		i.lastStatus  = ReplicatedSetFileSecurity( &i, FileName, SecurityInformation, SecurityDescriptor, SecurityDescriptorLength, DokanFileInfo);
	}
  // TODO: which error, if any
	return STATUS_SUCCESS;
}

/*
//Uncomment for personalize disk space
static NTSTATUS DOKAN_CALLBACK MirrorDokanGetDiskFreeSpace(
    PULONGLONG FreeBytesAvailable, PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes, PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  *FreeBytesAvailable = (ULONGLONG)(512 * 1024 * 1024);
  *TotalNumberOfBytes = 9223372036854775807;
  *TotalNumberOfFreeBytes = 9223372036854775807;

  return STATUS_SUCCESS;
}
*/

/**
 * Avoid #include <winternl.h> which as conflict with FILE_INFORMATION_CLASS
 * definition.
 * This only for MirrorFindStreams. Link with ntdll.lib still required.
 *
 * Not needed if you're not using NtQueryInformationFile!
 *
 * BEGIN
 */
#pragma warning(push)
#pragma warning(disable : 4201)
typedef struct _IO_STATUS_BLOCK {
  union {
    NTSTATUS Status;
    PVOID Pointer;
  } DUMMYUNIONNAME;

  ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
#pragma warning(pop)

NTSYSCALLAPI NTSTATUS NTAPI NtQueryInformationFile(
    _In_ HANDLE FileHandle, _Out_ PIO_STATUS_BLOCK IoStatusBlock,
    _Out_writes_bytes_(Length) PVOID FileInformation, _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS FileInformationClass);
/**
 * END
 */

NTSTATUS DOKAN_CALLBACK
ReplicatedFindStreams(ReplicationHandle* rep_handle, LPCWSTR FileName, PFillFindStreamData FillFindStreamData,
                  PDOKAN_FILE_INFO DokanFileInfo) {
  
  HANDLE hFind;
  WIN32_FIND_STREAM_DATA findData;
  DWORD error;
  int count = 0;

  

  DbgPrint(L"FindStreams :%s\n", rep_handle->resolved_path.c_str());

  hFind = FindFirstStreamW(rep_handle->resolved_path.c_str(), FindStreamInfoStandard, &findData, 0);

  if (hFind == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    DbgPrint(L"\tinvalid file handle. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  FillFindStreamData(&findData, DokanFileInfo);
  count++;

  while (FindNextStreamW(hFind, &findData) != 0) {
    FillFindStreamData(&findData, DokanFileInfo);
    count++;
  }

  error = GetLastError();
  FindClose(hFind);

  if (error != ERROR_HANDLE_EOF) {
    DbgPrint(L"\tFindNextStreamW error. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tFindStreams return %d entries in %s\n\n", count, rep_handle->resolved_path.c_str());

  return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK MirrorFindStreams(LPCWSTR FileName, PFillFindStreamData FillFindStreamData, PDOKAN_FILE_INFO DokanFileInfo) 
{
  ReplicatedFile* replicated = (ReplicatedFile*) DokanFileInfo->Context;
	if(DokanFileInfo->Context == 0)
	{
		replicated = ReplicatedFile::Create();
		DokanFileInfo->Context = (ULONG64) replicated;
	}

	
	for(auto& i: *replicated)
	{
		NTSTATUS succeeded = ReplicatedFindStreams( &i, FileName,  FillFindStreamData, DokanFileInfo);
		succeeded = succeeded;
	}

	// TODO: which error, if any
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorMounted(PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  DbgPrint(L"Mounted\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorUnmounted(PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  DbgPrint(L"Unmounted\n");
  return STATUS_SUCCESS;
}

#pragma warning(pop)

BOOL WINAPI CtrlHandler(DWORD dwCtrlType) {
  switch (dwCtrlType) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    DokanRemoveMountPoint(MountPoint);
    return TRUE;
  default:
    return FALSE;
  }
}

void ShowUsage() {
  // clang-format off
  fprintf(stderr, "mirror.exe\n"
          "  /r RootDirectory (ex. /r c:\\test)\t\t Directory source to mirror.\n"
		  "  /m RootDirectory (ex. /r c:\\test)\t\t Directory source to mirror.\n"
          "  /l MountPoint (ex. /l m)\t\t\t Mount point. Can be M:\\ (drive letter) or empty NTFS folder C:\\mount\\dokan .\n"
          "  /t ThreadCount (ex. /t 5)\t\t\t Number of threads to be used internally by Dokan library.\n\t\t\t\t\t\t More threads will handle more event at the same time.\n"
          "  /d (enable debug output)\t\t\t Enable debug output to an attached debugger.\n"
          "  /s (use stderr for output)\t\t\t Enable debug output to stderr.\n"
          "  /n (use network drive)\t\t\t Show device as network device.\n"
          "  /q (use removable drive)\t\t\t Show device as removable media.\n"
          "  /w (write-protect drive)\t\t\t Read only filesystem.\n"
          "  /o (use mount manager)\t\t\t Register device to Windows mount manager.\n\t\t\t\t\t\t This enables advanced Windows features like recycle bin and more...\n"
          "  /c (mount for current session only)\t\t Device only visible for current user session.\n"
          "  /u (UNC provider name ex. \\localhost\\myfs)\t UNC name used for network volume.\n"
          "  /p (Impersonate Caller User)\t\t\t Impersonate Caller User when getting the handle in CreateFile for operations.\n\t\t\t\t\t\t This option requires administrator right to work properly.\n"
          "  /a Allocation unit size (ex. /a 512)\t\t Allocation Unit Size of the volume. This will behave on the disk file size.\n"
          "  /k Sector size (ex. /k 512)\t\t\t Sector Size of the volume. This will behave on the disk file size.\n"
          "  /f User mode Lock\t\t\t\t Enable Lockfile/Unlockfile operations. Otherwise Dokan will take care of it.\n"
          "  /i (Timeout in Milliseconds ex. /i 30000)\t Timeout until a running operation is aborted and the device is unmounted.\n\n"
          "Examples:\n"
          "\tmirror.exe /r C:\\Users /l M:\t\t\t# Mirror C:\\Users as RootDirectory into a drive of letter M:\\.\n"
          "\tmirror.exe /r C:\\Users /l C:\\mount\\dokan\t# Mirror C:\\Users as RootDirectory into NTFS folder C:\\mount\\dokan.\n"
          "\tmirror.exe /r C:\\Users /l M: /n /u \\myfs\\myfs1\t# Mirror C:\\Users as RootDirectory into a network drive M:\\. with UNC \\\\myfs\\myfs1\n\n"
          "Unmount the drive with CTRL + C in the console or alternatively via \"dokanctl /u MountPoint\".\n");
  // clang-format on
}

int __cdecl wmain(ULONG argc, PWCHAR argv[]) {
  int status;
  ULONG command;
  PDOKAN_OPERATIONS dokanOperations =
    (PDOKAN_OPERATIONS)malloc(sizeof(DOKAN_OPERATIONS));
  if (dokanOperations == NULL) {
    return EXIT_FAILURE;
  }
  PDOKAN_OPTIONS dokanOptions = (PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));
  if (dokanOptions == NULL) {
    free(dokanOperations);
    return EXIT_FAILURE;
  }

  if (argc < 3) {
    ShowUsage();
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
  }

  g_DebugMode = FALSE;
  g_UseStdErr = FALSE;

  ZeroMemory(dokanOptions, sizeof(DOKAN_OPTIONS));
  dokanOptions->Version = DOKAN_VERSION;
  dokanOptions->ThreadCount = 0; // use default
  
  CurrentRootDirectory = 0;
  
  for (command = 1; command < argc; command++) {
	bool master = false;
    switch (towlower(argv[command][1])) {
	case L'm':
		master = true;
    case L'r':
      command++;
	  RootDirectories.push_back(ReplicatedDirectory(argv[command],master));
      DbgPrint(L"RootDirectory: %ls\n", RootDirectories.back().path.c_str());
      ++CurrentRootDirectory;
	  if (CurrentRootDirectory >= MAX_ROOT_DIRECTORIES)
	  {
        fwprintf(
            stderr,
            L"Cannot define more than : %d root directories (defined %s)\n",
            MAX_ROOT_DIRECTORIES, argv[command]);
        free(dokanOperations);
        free(dokanOptions);
        return EXIT_FAILURE;
	  }
      break;
    case L'l':
      command++;
      wcscpy_s(MountPoint, sizeof(MountPoint) / sizeof(WCHAR), argv[command]);
      dokanOptions->MountPoint = MountPoint;
      break;
    case L't':
      command++;
      dokanOptions->ThreadCount = (USHORT)_wtoi(argv[command]);
      break;
    case L'd':
      g_DebugMode = TRUE;
      break;
    case L's':
      g_UseStdErr = TRUE;
      break;
    case L'n':
      dokanOptions->Options |= DOKAN_OPTION_NETWORK;
      break;
    case L'q':
      dokanOptions->Options |= DOKAN_OPTION_REMOVABLE;
      break;
    case L'w':
      dokanOptions->Options |= DOKAN_OPTION_WRITE_PROTECT;
      break;
    case L'o':
      dokanOptions->Options |= DOKAN_OPTION_MOUNT_MANAGER;
      break;
    case L'c':
      dokanOptions->Options |= DOKAN_OPTION_CURRENT_SESSION;
      break;
    case L'f':
      dokanOptions->Options |= DOKAN_OPTION_FILELOCK_USER_MODE;
      break;
    case L'u':
      command++;
      wcscpy_s(UNCName, sizeof(UNCName) / sizeof(WCHAR), argv[command]);
      dokanOptions->UNCName = UNCName;
      DbgPrint(L"UNC Name: %ls\n", UNCName);
      break;
    case L'p':
      g_ImpersonateCallerUser = TRUE;
      break;
    case L'i':
      command++;
      dokanOptions->Timeout = (ULONG)_wtol(argv[command]);
      break;
    case L'a':
      command++;
      dokanOptions->AllocationUnitSize = (ULONG)_wtol(argv[command]);
      break;
    case L'k':
      command++;
      dokanOptions->SectorSize = (ULONG)_wtol(argv[command]);
      break;
    default:
      fwprintf(stderr, L"unknown command: %s\n", argv[command]);
      free(dokanOperations);
      free(dokanOptions);
      return EXIT_FAILURE;
    }
  }

  CurrentRootDirectory = 0;

  if (wcscmp(UNCName, L"") != 0 &&
      !(dokanOptions->Options & DOKAN_OPTION_NETWORK)) {
    fwprintf(
             stderr,
             L"  Warning: UNC provider name should be set on network drive only.\n");
  }

  if (dokanOptions->Options & DOKAN_OPTION_NETWORK &&
      dokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) {
    fwprintf(stderr, L"Mount manager cannot be used on network drive.\n");
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
  }

  if (!(dokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) &&
      wcscmp(MountPoint, L"") == 0) {
    fwprintf(stderr, L"Mount Point required.\n");
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
  }

  if ((dokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) &&
      (dokanOptions->Options & DOKAN_OPTION_CURRENT_SESSION)) {
    fwprintf(stderr,
             L"Mount Manager always mount the drive for all user sessions.\n");
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
  }

  if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
    fwprintf(stderr, L"Control Handler is not set.\n");
  }

  // Add security name privilege. Required here to handle GetFileSecurity
  // properly.
  g_HasSeSecurityPrivilege = AddSeSecurityNamePrivilege();
  if (!g_HasSeSecurityPrivilege) {
    fwprintf(stderr, L"Failed to add security privilege to process\n");
    fwprintf(stderr,
             L"\t=> GetFileSecurity/SetFileSecurity may not work properly\n");
    fwprintf(stderr, L"\t=> Please restart mirror sample with administrator "
             L"rights to fix it\n");
  }

  if (g_ImpersonateCallerUser && !g_HasSeSecurityPrivilege) {
    fwprintf(stderr, L"Impersonate Caller User requires administrator right to "
             L"work properly\n");
    fwprintf(stderr, L"\t=> Other users may not use the drive properly\n");
    fwprintf(stderr, L"\t=> Please restart mirror sample with administrator "
             L"rights to fix it\n");
  }

  if (g_DebugMode) {
    dokanOptions->Options |= DOKAN_OPTION_DEBUG;
  }
  if (g_UseStdErr) {
    dokanOptions->Options |= DOKAN_OPTION_STDERR;
  }

  dokanOptions->Options |= DOKAN_OPTION_ALT_STREAM;

  ZeroMemory(dokanOperations, sizeof(DOKAN_OPERATIONS));
  dokanOperations->ZwCreateFile = MirrorCreateFile;
  dokanOperations->Cleanup = MirrorCleanup;
  dokanOperations->CloseFile = MirrorCloseFile;
  dokanOperations->ReadFile = MirrorReadFile;
  dokanOperations->WriteFile = MirrorWriteFile;
  dokanOperations->FlushFileBuffers = MirrorFlushFileBuffers;
  dokanOperations->GetFileInformation = MirrorGetFileInformation;
  dokanOperations->FindFiles = MirrorFindFiles;
  dokanOperations->FindFilesWithPattern = NULL;
  dokanOperations->SetFileAttributes = MirrorSetFileAttributes;
  dokanOperations->SetFileTime = MirrorSetFileTime;
  dokanOperations->DeleteFile = MirrorDeleteFile;
  dokanOperations->DeleteDirectory = MirrorDeleteDirectory;
  dokanOperations->MoveFile = MirrorMoveFile;
  dokanOperations->SetEndOfFile = MirrorSetEndOfFile;
  dokanOperations->SetAllocationSize = MirrorSetAllocationSize;
  dokanOperations->LockFile = MirrorLockFile;
  dokanOperations->UnlockFile = MirrorUnlockFile;
  dokanOperations->GetFileSecurity = MirrorGetFileSecurity;
  dokanOperations->SetFileSecurity = MirrorSetFileSecurity;
  dokanOperations->GetDiskFreeSpace = NULL; // MirrorDokanGetDiskFreeSpace;
  dokanOperations->GetVolumeInformation = MirrorGetVolumeInformation;
  dokanOperations->Unmounted = MirrorUnmounted;
  dokanOperations->FindStreams = MirrorFindStreams;
  dokanOperations->Mounted = MirrorMounted;

  status = DokanMain(dokanOptions, dokanOperations);
  switch (status) {
  case DOKAN_SUCCESS:
    fprintf(stderr, "Success\n");
    break;
  case DOKAN_ERROR:
    fprintf(stderr, "Error\n");
    break;
  case DOKAN_DRIVE_LETTER_ERROR:
    fprintf(stderr, "Bad Drive letter\n");
    break;
  case DOKAN_DRIVER_INSTALL_ERROR:
    fprintf(stderr, "Can't install driver\n");
    break;
  case DOKAN_START_ERROR:
    fprintf(stderr, "Driver something wrong\n");
    break;
  case DOKAN_MOUNT_ERROR:
    fprintf(stderr, "Can't assign a drive letter\n");
    break;
  case DOKAN_MOUNT_POINT_ERROR:
    fprintf(stderr, "Mount point error\n");
    break;
  case DOKAN_VERSION_ERROR:
    fprintf(stderr, "Version error\n");
    break;
  default:
    fprintf(stderr, "Unknown error: %d\n", status);
    break;
  }

  free(dokanOptions);
  free(dokanOperations);
  return EXIT_SUCCESS;
}
