#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "file_sink.h"
#include "mp4_builder.h"

#define WRITE_STATISTICS_THRESHHOLD 100

int mFd = -1;
UINT mBufSize;
UINT mDataSize;
UINT mWriteCnt;
U64  mWriteDataSize;
U64  mWriteAvgSpeed;
U64  mHistMinSpeed;
U64  mHistMaxSpeed;
U64  mWriteTime;

U8 *mpCustomBuf;
U8 *mpDataWriteAddr;
U8 *mpDataSendAddr;

bool mIODirectSet;
bool mEnableDirectIO;

int SetFileFlag (int flag, bool enable);
 
int Mp4_CreateFile(const char *pFileName)
{
	if (!(mFd < 0)) 
	{
	  	Mp4_CloseFile();
	}

	if (pFileName == NULL) 
	{
	  	return ME_ERROR;
	}
	if(mp4_CheckSDExist())
	{
		#if DISK_MANAGER
		//if ( (mFd = Mux_open(pFileName)) < 0 )
		{
			return ME_ERROR;
		}
		#else

		if ((mFd = open (pFileName, O_WRONLY | O_CREAT | O_TRUNC,
		                 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0)           
		{     
		  	return ME_ERROR;
		}
		#endif

		mDataSize = 0;
		mpDataWriteAddr = mpCustomBuf;
		mpDataSendAddr = mpCustomBuf;

		return ME_OK;
	}
	return ME_ERROR;
}

int Mp4_CloseFile()
{
	if (mFd < 0) 
	{
	  	return ME_BAD_PARAM;
	}

	Mp4_FlushFile ();
	#if DISK_MANAGER
	//if (Mux_close (mFd,NULL) != 0) 
	#else
	if (close (mFd) != 0) 
	#endif	
	{
		mFd = -1;

		switch (errno) 
		{
			case EBADF: return ME_BAD_PARAM;
			case EIO:   return ME_IO_ERROR;
			case EINTR: return ME_BUSY;
			default:   return ME_ERROR;
		}
	}

	mFd = -1;
	mIODirectSet = false;

	freeSPSPPS();//++

	return ME_OK;
}

INT Mp4_Write(const void *buf, UINT nbyte)
{
	UINT    remainSize = nbyte;
	INT       retval = 0;
	UINT     writeCnt = 0;
	struct timeval start = {0, 0};
	struct timeval   end = {0, 0};

	if (gettimeofday(&start, NULL) < 0) 
	{    
	}
	do 
	{	
		if(!mp4_CheckSDExist())
		{
			return -1;
		}
		#if DISK_MANAGER
		//retval = Mux_write(mFd, buf, remainSize);
		#else
		retval = write(mFd, buf, remainSize);
		#endif
		if (retval > 0) 
		{
		  remainSize -= retval;
		} 
		else if(retval < 0)
		{
			return ME_ERROR;
		}
		else if ((errno != EAGAIN) && (errno != EINTR)) 
		{
		  break;
		} 
	} while ((++ writeCnt < 5) && (remainSize > 0));

	++ mWriteCnt;
	mWriteDataSize += (nbyte - remainSize);

	if (remainSize < nbyte) 
	{
		if (gettimeofday(&end, NULL) < 0) 
		{      
		} 
		else 
		{
			U64 timediff = ((end.tv_sec * 1000000 + end.tv_usec) -
			  (start.tv_sec * 1000000 + start.tv_usec));
			mWriteTime += timediff;

			if (timediff >= 200000) 
			{
			}
			if (mWriteCnt >= WRITE_STATISTICS_THRESHHOLD) 
			{
				mWriteAvgSpeed = mWriteDataSize * 1000000 / mWriteTime / 1024;
				mHistMinSpeed  = (mWriteAvgSpeed < mHistMinSpeed)  ?
				    mWriteAvgSpeed : mHistMinSpeed;
				mHistMaxSpeed  = (mWriteAvgSpeed > mHistMaxSpeed)  ?
				    mWriteAvgSpeed : mHistMaxSpeed;

				mWriteDataSize = 0;
				mWriteCnt = 0;
				mWriteTime = 0;
			}
		}
	}

	return (retval < 0) ? retval : (nbyte - remainSize);
}

int Mp4_WriteFile (const void *buf, UINT len)
{
	UINT remainData = len;
	U8*  dataAddr   = (U8 *)buf;

	if (mFd < 0) 
	{
	  	return ME_ERROR;
	}

	if (len == 0) 
	{
	  	return ME_OK;
	}

	if (NULL == mpCustomBuf) 
	{ 
	  	int ret = -1;
	  	while (remainData > 0) 
	  	{
	  		if(mp4_CheckSDExist())
			{
			    if ((ret = Mp4_Write(dataAddr, remainData)) > 0) 
				{
			      	remainData -= ret;
			      	dataAddr += ret;
			    } 
				else 
				{ 
			      	break;
			    }
  			}
			else
			{
				printf("sd card does not exite!!!!!!\n");
				return ME_ERROR;
			}
	  	}
	} 
	else 
	{ // Use IO Buffer
		UINT bufferSize = 0;
		UINT dataSize   = 0;
		while (remainData > 0) 
		{	
			
			if(!mp4_CheckSDExist())
				return ME_ERROR;
			bufferSize = mBufSize + mpCustomBuf - mpDataWriteAddr;
			dataSize = (remainData <= bufferSize ? remainData : bufferSize);

			memcpy(mpDataWriteAddr, dataAddr, dataSize);
			dataAddr      += dataSize;
			mDataSize += dataSize;
			remainData    -= dataSize;
			mpDataWriteAddr += dataSize;
			if ((U32)(mpDataWriteAddr - mpCustomBuf) >= mBufSize) 
			{
				mpDataWriteAddr = mpCustomBuf;
			}
			
			while (mDataSize >= IO_TRANSFER_BLOCK_SIZE) 
			{
				if(mp4_CheckSDExist())
				{
					int writeRet = Mp4_Write(mpDataSendAddr, IO_TRANSFER_BLOCK_SIZE);
					if (writeRet == IO_TRANSFER_BLOCK_SIZE) 
					{
						mDataSize -= IO_TRANSFER_BLOCK_SIZE;
						mpDataSendAddr += IO_TRANSFER_BLOCK_SIZE;
						if ((U32)(mpDataSendAddr - mpCustomBuf) >= mBufSize) 
						{
						  	mpDataSendAddr = mpCustomBuf;
						}
					} 
					else 
					{
					   	return ME_ERROR;
					}
				}
				else
				{
					printf("sd card not exite ........\n");
					if(mp4_CheckSDExist())
					return ME_ERROR;
				}
			}
	  	}
	}

	return ((remainData == 0) ? ME_OK : ME_ERROR);
}

int Mp4_FlushFile ()
{
	ERR err = ME_ERROR;
	if ((mDataSize > 0) && mpDataSendAddr && mpCustomBuf) 
	{
		if (((mDataSize % 512) != 0) && mIODirectSet && mEnableDirectIO) 
		{
			mIODirectSet = !(ME_OK == SetFileFlag((int)O_DIRECT, false));
		}
		
		while (mDataSize > 0) 
		{
			UINT dataRemain = mDataSize;
			int ret = -1;
			if (mpDataWriteAddr > mpDataSendAddr) 
			{
				dataRemain = (mpDataWriteAddr - mpDataSendAddr); //mDataSize
			} 
			else if (mpDataWriteAddr < mpDataSendAddr) 
			{
				dataRemain = (mpCustomBuf + mBufSize - mpDataSendAddr);
			} 
			else 
			{
				break;
			}
			if(mp4_CheckSDExist())
			{
				if ((ret = Mp4_Write(mpDataSendAddr, dataRemain)) != (int)mDataSize) 
				{

					err = ME_ERROR;          
				} 
				else 
				{
					mDataSize -= dataRemain;
					mpDataSendAddr += dataRemain;
					if ((U32)(mpDataSendAddr - mpCustomBuf) >= mBufSize) 
					{
						mpDataSendAddr = mpCustomBuf;
					}
					err = ME_OK;
				}
			}
			else
			{
				return ME_ERROR;
			}
		}
		
		if (mEnableDirectIO &&mpCustomBuf &&((mBufSize % 512) == 0) &&!mIODirectSet) 
		{
			mIODirectSet = (ME_OK == SetFileFlag((int)O_DIRECT, true));
		}
	} 
	else 
	{
		err = ME_OK;
	}
	mDataSize = 0;
	mpDataWriteAddr = mpCustomBuf;
	mpDataSendAddr  = mpCustomBuf;

	return err;
}

int Mp4_Setbuf (UINT size)
{
	if (mFd < 0) 
	{   
	  	return ME_ERROR;
	}

	if (mpCustomBuf && ((0 == size) || (size != mBufSize))) 
	{
	//   delete[] mpCustomBuf;
		free(mpCustomBuf);
		mpCustomBuf = NULL;
		mpDataWriteAddr = NULL;
		mpDataSendAddr = NULL;
		mDataSize = 0;
		mBufSize = 0;
	}

	if (size == 0) 
	{
	  	return ME_OK;
	}

	if (!mpCustomBuf && (size > 0) && ((size % IO_TRANSFER_BLOCK_SIZE) == 0)) 
	{
		//   mpCustomBuf = new U8[size];
		mpCustomBuf = (U8*)malloc(size*sizeof(U8));
		mpDataWriteAddr = mpCustomBuf;
		mpDataSendAddr = mpCustomBuf;
		mDataSize = 0;
		mBufSize = (mpCustomBuf ? size : 0);
	} 
	else if ((size % IO_TRANSFER_BLOCK_SIZE) != 0) 
	{
	}

	if (mEnableDirectIO && mpCustomBuf &&((size % 512) == 0) && !mIODirectSet) 
	{
	  	mIODirectSet = (ME_OK == SetFileFlag(((int)O_DIRECT), true));
	}

	return (mpCustomBuf ? ME_OK : ME_ERROR);
}

int SetFileFlag (int flag, bool enable)
{
	#if  (!DISK_MANAGER)
	int fileFlags = 0;
	if (mFd < 0) 
	{
	  	return ME_ERROR;
	}
	
	if (flag & ~(O_APPEND | O_ASYNC | O_NONBLOCK | O_DIRECT | O_NOATIME)) 
	{     
	  	return ME_ERROR;
	}
	
	fileFlags = fcntl(mFd, F_GETFL);
	
	if (enable) 
	{
	  	fileFlags |= flag;
	}
	else 
	{
	  	fileFlags &= ~flag;
	}
	
	if (fcntl(mFd, F_SETFL, fileFlags) < 0) 
	{
	  	return ME_ERROR;
	}
	#endif
	return ME_OK;
}


int Mp4_SeekFile(file_off_t offset, UINT whence)
{
	if (mFd < 0) 
	{
	  	return ME_ERROR;
	}

	if (Mp4_FlushFile () != ME_OK) 
	{
	  	return ME_ERROR;
	}
	#if DISK_MANAGER
	if (Mux_lseek (mFd, offset, whence) < 0) 
	#else
	if (lseek (mFd, offset, whence) < 0) 
	#endif		
	{
	  	return ME_IO_ERROR;
	}

	return ME_OK;
}
