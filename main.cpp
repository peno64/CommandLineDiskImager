/**********************************************************************
 *  This program is free software; you can redistribute it and/or     *
 *  modify it under the terms of the GNU General Public License       *
 *  as published by the Free Software Foundation; either version 2    *
 *  of the License, or (at your option) any later version.            *
 *                                                                    *
 *  This program is distributed in the hope that it will be useful,   *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the     *
 *  GNU General Public License for more details.                      *
 *                                                                    *
 *  You should have received a copy of the GNU General Public License *
 *  along with this program; if not, see http://gnu.org/licenses/
 *  ---                                                               *
 *  Copyright (C) 2009, Justin Davis <tuxdavis@gmail.com>             *
 *  Copyright (C) 2009-2014 ImageWriter developers                    *
 *                          https://launchpad.net/~image-writer-devs  *
 *  Copyright (C) 2016, David Ferguson <fergusondavid6@gmail.com>     *
 **********************************************************************/

#ifndef WINVER
#define WINVER 0x0601
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <winioctl.h>
#include <dbt.h>
#include <shlobj.h>
#include <iostream>
#include <sstream>

#include "main.h"

DWORD getDeviceID(HANDLE hVolume)
{
	VOLUME_DISK_EXTENTS sd;
	DWORD bytesreturned;
	if (!DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &sd, sizeof(sd), &bytesreturned, NULL))
	{
		return -1;
	}
	return sd.Extents[0].DiskNumber;
}

int error;

extern "C" {
	extern void initgzip();

	extern int dounzip();
	extern int dozip();

	extern void initprogress();
	extern void progress(double percent, char *selapsedseconds, char *sestimatedseconds);
	int(*write_buffer_func)(int fd, char *buf, unsigned cnt);
	int(*read_buffer_func)(int ifd, char *buf, unsigned size);

#if TESTING
	extern int gzip();
	extern int gunzip();

	extern int read_buffer_file(int ifd, char *buf, unsigned size);
	extern int write_buffer_file(int fd, char *buf, unsigned cnt);

	int(*write_buffer_disk)(int fd, char *buf, unsigned cnt);
	int(*read_buffer_disk)(int ifd, char *buf, unsigned size);
#else
	int read_buffer(int ifd, char *buf, unsigned size)
	{
		return read_buffer_func(ifd, buf, size);
	}

	int read_buffer_file(int ifd, char *buf, unsigned size)
	{
		DWORD numberOfBytesRead;

		ReadFile(hFile, buf, size, &numberOfBytesRead, NULL);

		return numberOfBytesRead;
	}

	int read_buffer_disk(int ifd, char *buf, unsigned size)
	{
		unsigned count = 0;

		while (size > 0)
		{
			if (sectorData != NULL && sectorDataPos < sectorDataLen * sectorsize)
			{
				unsigned n = size;
				if (sectorstep * sectorsize - sectorDataPos < n)
					n = sectorstep * sectorsize - sectorDataPos;
				memcpy(buf, sectorData + sectorDataPos, n);
				size -= n;
				buf += n;
				sectorDataPos += n;
				count += n;
			}

			if (size > 0)
			{
				if (sectorData != NULL)
				{
					delete[] sectorData;
					sectorData = NULL;
				}
				if (sector >= numsectors)
					break;
				unsigned long long nsectors = (numsectors - sector >= sectorstep) ? sectorstep : (numsectors - sector);

				for (int i = 0; (i < 3) && sectorData == NULL; i++)
					sectorData = readSectorDataFromHandle(hRawDisk, sector, nsectors, sectorsize);

				if (sectorData == NULL)
				{
					printf("\r\nerror whilst reading\n");
					error = 10;
					errno = EIO;
					return -1;
				}
				sector += nsectors;
				sectorDataPos = 0;
				sectorDataLen = nsectors;
			}
		}

		if (sector % 100 == 0)
		{
			char elapsedseconds[20], estimatedseconds[20];

			printf("\r%d", sector);
			printf("/");
			printf("%d", numsectors);
			printf(" (%d%%)", (sector * 100) / numsectors);

			progress(((double)sector) / ((double)numsectors), elapsedseconds, estimatedseconds);
			printf(" %s/%s     \b\b\b\b\b", elapsedseconds, estimatedseconds);

			setbuf(stdout, NULL);
		}

		return count;
	}

	int write_buffer(int fd, char *buf, unsigned cnt)
	{
		return write_buffer_func(fd, buf, cnt);
	}

	int write_buffer_file(int fd, char *buf, unsigned cnt)
	{
		DWORD n;

		if (!WriteFile(hFile, buf, cnt, &n, NULL))
			n = -1;
		return n;
	}

	int write_buffer_disk(int fd, char *buf, unsigned cnt)
	{
		int written = cnt;
		unsigned n;

		if (error != 0)
			return -1;

		if (sectorData == NULL)
			sectorData = new char[sectorsize * sectorstep];

		do
		{
			if (buf != NULL && cnt > 0)
			{
				unsigned n = cnt;
				if (n > sectorsize * sectorstep - sectorDataPos)
					n = sectorsize * sectorstep - sectorDataPos;
				memcpy(sectorData + sectorDataPos, buf, n);
				sectorDataPos += n;
				buf += n;
				cnt -= n;
			}
			if (sectorDataPos > 0 && (sectorDataPos == sectorsize * sectorstep || buf == NULL))
			{
				char ok = 0;
				for (int i = 0; (i < 3) && !ok; i++)
					if (writeSectorDataToHandle(hRawDisk, sectorData, sector, sectorDataPos / sectorsize, sectorsize))
						ok = 1;
				if (!ok)
				{
					printf("\r\nerror whilst writing\n");
					errno = EIO;
					error = 11;
					return -1;
				}
				sector += sectorDataPos / sectorsize;
				sectorDataPos = 0;

				if (sector % 100 == 0)
				{
					char elapsedseconds[20], estimatedseconds[20];

					printf("\r%d", sector);
					printf("/");
					printf("%d", numsectors);
					printf(" (%d%%)", (sector * 100) / numsectors);

					progress(((double)sector) / ((double)numsectors), elapsedseconds, estimatedseconds);
					printf(" %s/%s     \b\b\b\b\b", elapsedseconds, estimatedseconds);

					setbuf(stdout, NULL);
				}
			}
		} while (cnt > 0);

		return written;
	}
#endif
}

void DiskSize(char *disk)
{
	unsigned long long availablesectors;

	int volumeID = toupper(*disk) - 'A';

	hVolume = getHandleOnVolume(volumeID, GENERIC_READ);

	DWORD deviceID = getDeviceID(hVolume);

	hRawDisk = getHandleOnDevice(deviceID, GENERIC_READ);

	availablesectors = getNumberOfSectors(hRawDisk, &sectorsize);

	printf("Available sectors on disk %s = %lld, sectorsize = %lld, size = %lld MB\n", disk, availablesectors, sectorsize, availablesectors * sectorsize / 1024 / 1024);
}

void Usage(char *argv[])
{
	printf("Usage:\n");
	printf(" %s /s <driveletter>", argv[0]);
	printf("   Print size parameters of drive\n");
	printf(" %s <driveletter> filename\n", argv[0]);
	printf("   Copy drive data to filename\n");
	printf(" %s filename <driveletter> [/f]\n", argv[0]);
	printf("   Copy filename to drive. /f forces to copy even if file contains more data than disk\n");
}

int main(int argc, char *argv[])
{
	bool force = FALSE;

#if TESTING
	//gzip();
	gunzip();

	return 0;
#endif

	if (argc == 1)
	{
		Usage(argv);
		return 0;
	}

	if ((argc == 3) && (strcmp(argv[1], "/s") == 0) && (strlen(argv[2]) == 1))
	{
		DiskSize(argv[2]);
		return 0;
	}

	for (int i = 1; i < argc; i++)
	{
		bool remove = FALSE;

		if (_stricmp(argv[i], "/f") == 0)
			force = remove = TRUE;

		if (remove)
		{
			memcpy(argv + i, argv + i + 1, (argc - i - 1) * sizeof(*argv));
			argc--;
			i--;
		}
	}

	if (argc != 3)
	{
		printf("Wrong number of arguments\n");
		return 1; //not enough arguments
	}

	bool read = true;
	if (strlen(argv[2]) == 1)
		read = false;

	const char ltr = read ? argv[1][0] : argv[2][0];
	const char *imagefile = read ? argv[2] : argv[1];

	status = STATUS_IDLE;
	hVolume = INVALID_HANDLE_VALUE;
	hFile = INVALID_HANDLE_VALUE;
	hRawDisk = INVALID_HANDLE_VALUE;
	sectorData = NULL;
	sectorsize = 0ul;
	sectorDataPos = 0ul;
	sectorDataLen = 0ul;
	sector = 0ul;
	error = 0;

	if (strcmp(imagefile, "") != 0)
	{
		int ret = 0;

		if (!read && INVALID_FILE_ATTRIBUTES == GetFileAttributes(imagefile) && GetLastError() == ERROR_FILE_NOT_FOUND)
		{
			printf(".img not found\n");
			return 2; // .img not found
		}
		else
		{
			if (read && INVALID_FILE_ATTRIBUTES != GetFileAttributes(imagefile))
			{
				char buf[80];

				printf("Are you sure you want to read from disk %c and overwrite file %s ? ", ltr, imagefile);
				fgets(buf, sizeof(buf), stdin);
				if (buf[0] != '1' && buf[0] != 'y' && buf[0] != 'Y')
					return(-1);
			}

			if (!read)
			{
				char buf[80];

				printf("Are you sure you want write to disk %c and overwrite all its content ? ", ltr);
				fgets(buf, sizeof(buf), stdin);
				if (buf[0] != '1' && buf[0] != 'y' && buf[0] != 'Y')
					return(-1);
			}

			status = STATUS_WRITING;

			unsigned long long i, availablesectors;
			int volumeID = ltr - 'A';
			//        int deviceID0 = getDriveNumberFromLetter(ltr);
			//        if( deviceID0 == -1 )
			//        {
						//printf("device not found\n");
						//return 3; // device not found
			//        }

			hVolume = getHandleOnVolume(volumeID, read ? GENERIC_READ : GENERIC_WRITE);
			if (hVolume == INVALID_HANDLE_VALUE)
			{
				status = STATUS_IDLE;
				printf("invalid handle value for volume\n");
				return 4; // invalid handle value for volume
			}
			DWORD deviceID = getDeviceID(hVolume);
			if (deviceID == -1)
			{
				printf("device not found\n");
				return 3; // device not found
			}
			if (!getLockOnVolume(hVolume))
			{
				CloseHandle(hVolume);
				status = STATUS_IDLE;
				hVolume = INVALID_HANDLE_VALUE;
				printf("can't get lock on volume\n");
				return 5; // can't get lock on volume
			}
			if (!unmountVolume(hVolume))
			{
				removeLockOnVolume(hVolume);
				CloseHandle(hVolume);
				status = STATUS_IDLE;
				hVolume = INVALID_HANDLE_VALUE;
				printf("can't unmount volume\n");
				return 6; // can't unmount volume
			}
			hFile = getHandleOnFile(imagefile, read ? GENERIC_WRITE : GENERIC_READ);
			if (hFile == INVALID_HANDLE_VALUE)
			{
				removeLockOnVolume(hVolume);
				CloseHandle(hVolume);
				status = STATUS_IDLE;
				hVolume = INVALID_HANDLE_VALUE;
				printf("invalid handle value for file\n");
				return 7; // invalid handle value for file
			}
			hRawDisk = getHandleOnDevice(deviceID, read ? GENERIC_READ : GENERIC_WRITE);
			if (hRawDisk == INVALID_HANDLE_VALUE)
			{
				removeLockOnVolume(hVolume);
				CloseHandle(hFile);
				CloseHandle(hVolume);
				status = STATUS_IDLE;
				hVolume = INVALID_HANDLE_VALUE;
				hFile = INVALID_HANDLE_VALUE;
				printf("invalid handle value for disk\n");
				return 8; // invalid handle value for disk
			}
			availablesectors = getNumberOfSectors(hRawDisk, &sectorsize);
			numsectors = read ? availablesectors : getFileSizeInSectors(hFile, sectorsize);
			if (numsectors > availablesectors && !force)
			{
				printf("Not enough space on volume\n");
				return 9; // not enough space on volume
			}

			sector = 0;

			initprogress();

			if (read)
			{
				read_buffer_func = read_buffer_disk;
				write_buffer_func = write_buffer_file;
			}
			else
			{
				read_buffer_func = read_buffer_file;
				write_buffer_func = write_buffer_disk;
			}

#if 0
			char total_string[32];
			sprintf(total_string, "%d", numsectors);

			for (i = 0ul; i < numsectors && status == STATUS_WRITING; i += sectorstep)
			{
				if (i % 100 == 0)
				{
					char i_string[32];
					sprintf(i_string, "\r%d", i);
					printf(i_string);
					printf("/");
					printf(total_string);
					printf(" (%d%%)", (i * 100) / numsectors);
					setbuf(stdout, NULL);
				}

				sectorData = readSectorDataFromHandle(read ? hRawDisk : hFile, i, (numsectors - i >= sectorstep) ? sectorstep : (numsectors - i), sectorsize);
				if (sectorData == NULL)
				{
					status = STATUS_IDLE;
					printf("sector data is null\n");
					ret = 10; // sector data is null
				}
				else if (!writeSectorDataToHandle(read ? hFile : hRawDisk, sectorData, i, (numsectors - i >= sectorstep) ? sectorstep : (numsectors - i), sectorsize))
				{
					status = STATUS_IDLE;
					printf("error whilst writing\n");
					ret = 11; // error whilst writing
				}
				if (sectorData != NULL)
				{
					delete[] sectorData;
					sectorData = NULL;
				}
			}

#else
			if ((_stricmp(imagefile + strlen(imagefile) - 3, ".gz") == 0) ||
				(_stricmp(imagefile + strlen(imagefile) - 4, ".zip") == 0))
			{
				initgzip();

				if (read)
				{
					dozip();
					ret = error;
				}
				else
				{
					numsectors = availablesectors;

					if (dounzip())
					{
						write_buffer_disk(0, NULL, 0);
						ret = error;
					}
					else
						ret = 101;
				}
			}
			else
			{
				char *data = new char[sectorsize * sectorstep];

				for (i = 0ul; i < numsectors && status == STATUS_WRITING; i += sectorstep)
				{
					read_buffer_func(0, data, ((numsectors - i >= sectorstep) ? sectorstep : (numsectors - i)) * sectorsize);
					if (error != 0)
					{
						status = STATUS_IDLE;
						printf("\r\nsector data is null\n");
						ret = 10; // sector data is null
					}
					else
					{
						write_buffer_func(0, data, ((numsectors - i >= sectorstep) ? sectorstep : (numsectors - i)) * sectorsize);
						if (error != 0)
						{
							status = STATUS_IDLE;
							printf("\r\nerror whilst writing\n");
							ret = 11; // error whilst writing
						}
					}
				}
			}
#endif

			if (sectorData != NULL)
			{
				delete[] sectorData;
				sectorData = NULL;
			}

			removeLockOnVolume(hVolume);
			CloseHandle(hRawDisk);
			CloseHandle(hFile);
			CloseHandle(hVolume);
			hRawDisk = INVALID_HANDLE_VALUE;
			hFile = INVALID_HANDLE_VALUE;
			hVolume = INVALID_HANDLE_VALUE;
		}

		if (ret == 0)
			printf("\r\nDone\n");
		return ret; // success
	}
	else
	{
		printf("not enough arguments\n");
		return 1; // not enough arguments
	}
	status = STATUS_IDLE;
	printf("\r\nerror whilst writing\n");
	return 11; // error whilst writing
}

int getDriveNumberFromLetter(char lookingforname)
{
	unsigned long driveMask = GetLogicalDrives();
	int i = 0;
	ULONG pID;

	while (driveMask != 0)
	{
		if (driveMask & 1)
		{
			// the "A" in drivename will get incremented by the # of bits
			// we've shifted
			char drivename[] = "\\\\.\\A:\\";
			drivename[4] += i;

			if (checkDriveType(drivename, &pID))
			{
				if (lookingforname == drivename[4])
				{
					return pID;
				}
			}
		}
		driveMask >>= 1;
		++i;
	}
	return -1;
}

HANDLE getHandleOnFile(const char *filelocation, DWORD access)
{
	HANDLE hFile;
	hFile = CreateFileA(filelocation, access, (access == GENERIC_READ) ? FILE_SHARE_READ : 0, NULL, (access == GENERIC_READ) ? OPEN_EXISTING : CREATE_ALWAYS, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		//printf("error - not able to get handle on image file");
	}
	return hFile;
}

HANDLE getHandleOnDevice(int device, DWORD access)
{
	HANDLE hDevice;
	char devicename[20];
	sprintf(devicename, "\\\\.\\PhysicalDrive%d", device);

	hDevice = CreateFile(devicename, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hDevice == INVALID_HANDLE_VALUE)
	{
		//printf("not able to get handle on device\n");
	}
	return hDevice;
}

HANDLE getHandleOnVolume(int volume, DWORD access)
{
	HANDLE hVolume;
	char volumename[] = "\\\\.\\A:";
	volumename[4] += volume;

	hVolume = CreateFile(volumename, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hVolume == INVALID_HANDLE_VALUE)
	{
		//printf("error - not able to get handle on volume");
	}
	return hVolume;
}

bool getLockOnVolume(HANDLE handle)
{
	DWORD bytesreturned;
	BOOL bResult;
	bResult = DeviceIoControl(handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesreturned, NULL);
	if (!bResult)
	{
		//printf("error - not able to lock volume");
	}
	return (bResult);
}

bool removeLockOnVolume(HANDLE handle)
{
	DWORD junk;
	BOOL bResult;
	bResult = DeviceIoControl(handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &junk, NULL);
	if (!bResult)
	{
		//printf("error - not able to unlock volume");
	}
	return (bResult);
}

bool unmountVolume(HANDLE handle)
{
	DWORD junk;
	BOOL bResult;
	bResult = DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &junk, NULL);
	if (!bResult)
	{
		//printf("error - not able to dismount volume");
	}
	return (bResult);
}

char *readSectorDataFromHandle(HANDLE handle, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize)
{
	unsigned long bytesread;
	char *data = new char[sectorsize * numsectors];
	LARGE_INTEGER li;
	li.QuadPart = startsector * sectorsize;
	SetFilePointer(handle, li.LowPart, &li.HighPart, FILE_BEGIN);
	if (!ReadFile(handle, data, sectorsize * numsectors, &bytesread, NULL))
	{
		//printf("error - not able to read data from handle");
		delete[] data;
		data = NULL;
	}
	if (data && bytesread < (sectorsize * numsectors))
	{
		memset(data + bytesread, 0, (sectorsize * numsectors) - bytesread);
	}
	return data;
}

bool writeSectorDataToHandle(HANDLE handle, char *data, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize)
{
	unsigned long byteswritten;
	BOOL bResult;
	LARGE_INTEGER li;
	li.QuadPart = startsector * sectorsize;
	SetFilePointer(handle, li.LowPart, &li.HighPart, FILE_BEGIN);
	bResult = WriteFile(handle, data, sectorsize * numsectors, &byteswritten, NULL);
	if (!bResult)
	{
		bResult = true;
		while (bResult && numsectors > 0)
		{
			li.QuadPart = startsector * sectorsize;
			SetFilePointer(handle, li.LowPart, &li.HighPart, FILE_BEGIN);
			bResult = WriteFile(handle, data, sectorsize, &byteswritten, NULL);
			data += sectorsize;
			numsectors--;
			startsector++;
		}

		//printf("error - not able to write data from handle");
	}
	return (bResult);
}

unsigned long long getNumberOfSectors(HANDLE handle, unsigned long long *sectorsize)
{
	DWORD junk;
	DISK_GEOMETRY_EX diskgeometry;
	BOOL bResult;
	bResult = DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &diskgeometry, sizeof(diskgeometry), &junk, NULL);
	if (!bResult)
	{
		return 12;
	}
	if (sectorsize != NULL)
	{
		*sectorsize = (unsigned long long)diskgeometry.Geometry.BytesPerSector;
	}
	return (unsigned long long)diskgeometry.DiskSize.QuadPart / (unsigned long long)diskgeometry.Geometry.BytesPerSector;
}

unsigned long long getFileSizeInSectors(HANDLE handle, unsigned long long sectorsize)
{
	unsigned long long retVal = 0;
	if (sectorsize) // avoid divide by 0
	{
		LARGE_INTEGER filesize;
		if (GetFileSizeEx(handle, &filesize) == 0)
		{
			//printf("error - not able to get image file size");
			retVal = 0;
		}
		else
		{
			retVal = ((unsigned long long)filesize.QuadPart / sectorsize) + (((unsigned long long)filesize.QuadPart % sectorsize) ? 1 : 0);
		}
	}
	return(retVal);
}

BOOL GetDisksProperty(HANDLE hDevice, PSTORAGE_DEVICE_DESCRIPTOR pDevDesc,
	DEVICE_NUMBER *devInfo)
{
	STORAGE_PROPERTY_QUERY Query; // input param for query
	DWORD dwOutBytes; // IOCTL output length
	BOOL bResult; // IOCTL return val
	BOOL retVal = true;
	DWORD cbBytesReturned;

	// specify the query type
	Query.PropertyId = StorageDeviceProperty;
	Query.QueryType = PropertyStandardQuery;

	// Query using IOCTL_STORAGE_QUERY_PROPERTY
	bResult = ::DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
		&Query, sizeof(STORAGE_PROPERTY_QUERY), pDevDesc,
		pDevDesc->Size, &dwOutBytes, (LPOVERLAPPED)NULL);
	if (bResult)
	{
		bResult = ::DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER,
			NULL, 0, devInfo, sizeof(DEVICE_NUMBER), &dwOutBytes,
			(LPOVERLAPPED)NULL);
		if (!bResult)
		{
			retVal = false;
			//printf("error - not able to get device number, is something accessing the device?");
			//setbuf(stdout, NULL);
		}
	}
	else
	{
		if (DeviceIoControl(hDevice, IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0, NULL, 0, &cbBytesReturned,
			(LPOVERLAPPED)NULL))
		{
			//printf("error - not able to get device properties, is something accessing the device?");
			//setbuf(stdout, NULL);
		}
		retVal = false;
	}

	return(retVal);
}

bool slashify(char *str, char **slash, char **noSlash)
{
	bool retVal = false;
	int strLen = strlen(str);
	if (strLen > 0)
	{
		if (*(str + strLen - 1) == '\\')
		{
			// trailing slash exists
			if (((*slash = (char *)calloc((strLen + 1), sizeof(char))) != NULL) &&
				((*noSlash = (char *)calloc(strLen, sizeof(char))) != NULL))
			{
				strncpy(*slash, str, strLen);
				strncpy(*noSlash, *slash, (strLen - 1));
				retVal = true;
			}
		}
		else
		{
			// no trailing slash exists
			if (((*slash = (char *)calloc((strLen + 2), sizeof(char))) != NULL) &&
				((*noSlash = (char *)calloc((strLen + 1), sizeof(char))) != NULL))
			{
				strncpy(*noSlash, str, strLen);
				sprintf(*slash, "%s\\", *noSlash);
				retVal = true;
			}
		}
	}
	return(retVal);
}

bool GetMediaType(HANDLE hDevice)
{
	DISK_GEOMETRY diskGeo;
	DWORD cbBytesReturned;
	if (DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &diskGeo, sizeof(diskGeo), &cbBytesReturned, NULL))
	{
		if ((diskGeo.MediaType == FixedMedia) || (diskGeo.MediaType == RemovableMedia))
		{
			return true; // Not a floppy
		}
	}
	return false;
}

bool checkDriveType(char *name, ULONG *pid)
{
	HANDLE hDevice;
	PSTORAGE_DEVICE_DESCRIPTOR pDevDesc;
	DEVICE_NUMBER deviceInfo;
	bool retVal = false;
	char *nameWithSlash;
	char *nameNoSlash;
	int driveType;
	DWORD cbBytesReturned;

	// some calls require no tailing slash, some require a trailing slash...
	if (!(slashify(name, &nameWithSlash, &nameNoSlash)))
	{
		return(retVal);
	}

	driveType = GetDriveType(nameWithSlash);
	switch (driveType)
	{
	case DRIVE_REMOVABLE: // The media can be removed from the drive.
	case DRIVE_FIXED:     // The media cannot be removed from the drive.
		hDevice = CreateFile(nameNoSlash, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		if (hDevice == INVALID_HANDLE_VALUE)
		{
			//printf("error - not able to get handle on device");
			//setbuf(stdout, NULL);
		}
		else
		{
			int arrSz = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512 - 1;
			pDevDesc = (PSTORAGE_DEVICE_DESCRIPTOR)new BYTE[arrSz];
			pDevDesc->Size = arrSz;

			// get the device number if the drive is
			// removable or (fixed AND on the usb bus, SD, or MMC (undefined in XP/mingw))
			if (GetMediaType(hDevice) && GetDisksProperty(hDevice, pDevDesc, &deviceInfo) &&
				(((driveType == DRIVE_REMOVABLE) && (pDevDesc->BusType != BusTypeSata))
					|| ((driveType == DRIVE_FIXED) && ((pDevDesc->BusType == BusTypeUsb)
						|| (pDevDesc->BusType == BusTypeSd) || (pDevDesc->BusType == BusTypeMmc)))))
			{
				// ensure that the drive is actually accessible
				// multi-card hubs were reporting "removable" even when empty
				if (DeviceIoControl(hDevice, IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0, NULL, 0, &cbBytesReturned, (LPOVERLAPPED)NULL))
				{
					*pid = deviceInfo.DeviceNumber;
					retVal = true;
				}
				else
					// IOCTL_STORAGE_CHECK_VERIFY2 fails on some devices under XP/Vista, try the other (slower) method, just in case.
				{
					CloseHandle(hDevice);
					hDevice = CreateFile(nameNoSlash, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
					if (DeviceIoControl(hDevice, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &cbBytesReturned, (LPOVERLAPPED)NULL))
					{
						*pid = deviceInfo.DeviceNumber;
						retVal = true;
					}
				}
			}

			delete[] pDevDesc;
			CloseHandle(hDevice);
		}

		break;
	default:
		retVal = false;
	}

	// free the strings allocated by slashify
	free(nameWithSlash);
	free(nameNoSlash);

	return(retVal);
}
