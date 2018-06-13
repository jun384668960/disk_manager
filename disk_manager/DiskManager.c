
/******************************************************************************

                  版权所有 (C), 2012-2022, GOSCAM

 ******************************************************************************
  文 件 名   : DiskManager.c
  版 本 号   : 初稿
  作    者   : wusm
  生成日期   : 2016年10月24日
  最近修改   :
  功能描述   : 磁盘存储
  函数列表   :
  修改历史   :
  1.日    期   : 2016年10月24日
    作    者   : wusm
    修改内容   : 创建文件

******************************************************************************/

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <linux/unistd.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <mntent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <assert.h>
#include <dirent.h>
#include <sys/vfs.h> 
#include <sys/types.h>
#include <scsi/sg.h>
#include <pthread.h>

#include "DiskDefine.h"
#include "DiskManager.h"
#include "utils.h"
#include "lock_utils.h"
#include "utils_log.h"

#ifdef __cplusplus
extern "C"{
#endif

int fPart;						//磁盘句柄
int LockFlag;               	//文件锁
static int RemoveSdFlag = 0;			//移除sd卡标志
static int FormatSdFlag = 0;			//格式化sd卡
GosIndex *gMp4IndexList;     	//mp4文件索引
GosIndex *gJpegIndexList;		//jpeg文件索引
GosIndex *gAVIndexList;     	//裸文件索引
HeadIndex gHeadIndex;			//索引头
static __u32 oldStartTimeStap = 0; //用于判断循环覆盖是否成功
static __u32 oldEndTimeStap   = 0; //用于判断循环覆盖是否成功

int MaxWriteSize = 0;

#define FREE()    free(pFat);			\
 			      free(pZeroFat);  		\
			      free(pRootDir);  		\
			      free(pInfoSector);  	\
				  free(pGos_index);

//写分区表中某个簇
inline void Mark_FAT_Cluster (unsigned long cluster, unsigned long value, unsigned char* pFat)
{
	cluster %= (DEFAULT_SECTOR_SIZE/4);
	value &= 0xffffffff;//mine
	pFat[4 * cluster] = (unsigned char)(value & 0x000000ff);
	pFat[(4 * cluster) + 1] = (unsigned char) ((value & 0x0000ff00) >> 8);
	pFat[(4 * cluster) + 2] = (unsigned char) ((value & 0x00ff0000) >> 16);
	pFat[(4 * cluster) + 3] = (unsigned char) ((value & 0xff000000) >> 24);
}

unsigned char ChkSum (unsigned char *pFcbName)
{
    short FcbNameLen;
    unsigned char Sum;
    Sum = 0;
    for (FcbNameLen=11; FcbNameLen!=0; FcbNameLen--) 
	{ 
        Sum = ((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + *pFcbName++;
    }
	
    return (Sum);
}

void creatItem(char longNum,char *pFileName,char *de,unsigned char Chk)
{
	char *pname = pFileName;
	char *ptem = de;
	*ptem++ = longNum;
	int i;
	for(i = 0;i < 5;i++)
	{
		*ptem = *pname;
		ptem += 2;
		pname ++;
	}
	*ptem = 0x0F;
	ptem += 2;
	*ptem = Chk;
	ptem ++;
	for(i = 0;i < 6;i++)
	{
		*ptem = *pname;
		ptem += 2;
		pname ++;
	}
	ptem += 2;
	*ptem = *pname;
	ptem += 2;
	pname++;
	*ptem = *pname;
}

void CreateFileItem(msdos_dir_entry *de,char *pFileName,
		            unsigned long start, unsigned long filesize,
					unsigned char nClusterSize,char attribute)
{
	struct tm *ctime;
	time_t createtime;
	strcpy(de->name, pFileName);
	//0x20--文件,0x10--目录
	de->attr = attribute;		
	ctime = localtime(&createtime);
	de->time = CT_LE_W((unsigned short)((ctime->tm_sec >> 1) +
		               (ctime->tm_min << 5) + 
					   (ctime->tm_hour << 11)));
	de->date = CT_LE_W((unsigned short)(ctime->tm_mday +
		               ((ctime->tm_mon+1) << 5) +
					   ((ctime->tm_year-80) << 9)));
	de->ctime_ms = 0;
	de->ctime = de->time;
	de->cdate = de->date;
	de->adate = de->date;
	de->starthi = CT_LE_W(start>>16);
	de->start = CT_LE_W(start&0xffff);
	de->size = filesize;
	de->lcase = 0x18;	//mine 

	//start+=filesize/(nClusterSize*DEFAULT_SECTOR_SIZE);
	//if((filesize % (nClusterSize*DEFAULT_SECTOR_SIZE)) != 0)
	//	start+=1;
}

void CreateLongFileItem(long_msdos_dir_entry *de,unsigned char *shortname,
						char *pFileName,unsigned long start,
						unsigned long filesize,unsigned char nClusterSize,
						char attribute)
{
	#if LONG_MSDOS_DIR_SUPPORT
	//长目录每项最多携带13个字符
	long_msdos_dir_entry *de_msdos = de;
	memset(de_msdos,0,sizeof(long_msdos_dir_entry));
	
	//char *ptem = de->long_msdos_dir[1];
	//char *pname = pFileName;
	//char shortname[11] = "000001~1MP4";
    unsigned char Chk = ChkSum(shortname);
	//长目录每项最多携带13个字符

	//char *pFileName = "longname12345678901234567890.txt";
	creatItem(0x42,pFileName+13,de_msdos->long_msdos_dir[0],Chk);
	creatItem(0x01,pFileName,de_msdos->long_msdos_dir[1],Chk);
	
	CreateFileItem(&de_msdos->dir_entry,shortname,start,filesize,nClusterSize,attribute);
	#endif
	
}

//自定义文件锁
void Storage_Lock()
{
	while(LockFlag > 0)
	{
		continue;
	}
	LockFlag = 1;
}

void Storage_Unlock()
{
	LockFlag = 0;
}

int FormatjpegDir(int fpart)
{
	unsigned long lStartClu;
	unsigned long long offset;
	unsigned long long dirOffset;
	unsigned long gos_indexSize;
	
	//每个文件所占的簇数
	unsigned long lClustersofFile = JPEG_MAX_LENTH/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	if(JPEG_MAX_LENTH%(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)!=0)
	{
		lClustersofFile += 1;
	}
	//可写的文件数
	unsigned int lFileNum = gHeadIndex.lRootDirFileNum - 1;//PIC_PARTITION_SIZE/lClustersofFile;
	gHeadIndex.lJpegFileNum = lFileNum;
	
	//子目录所占的簇数
	unsigned long lClustersofRoot = (lFileNum+1)*LONG_DIR_ITEM_SIZE/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)+1;
	//子目录文件的大小(字节数)
	unsigned long lRootFileSize = lClustersofRoot*gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE;

	//添加子目录项
	struct msdos_dir_entry *pChildDir = NULL;
	if ((pChildDir = (struct msdos_dir_entry *)malloc(3 * sizeof(msdos_dir_entry))) == NULL)
	{
		printf("unable to allocate space in memory!!!\n");
		return -1;
	}
	memset(pChildDir, 0, 3 * sizeof(msdos_dir_entry));	
	offset = gHeadIndex.ChildItemEA;
	lStartClu = gHeadIndex.ChildStartCluster;
	CreateFileItem(&pChildDir[0],"IPC        ",lStartClu,0,gHeadIndex.ClusterSize,0x10);	
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(pChildDir), sizeof(msdos_dir_entry)) != sizeof(msdos_dir_entry))
	{
		free(pChildDir);
		printf("write ChildDir error!!!\n");
		return -1;
	}

	//添加子目录项的"."和".."
	char pname[11] = {0};
	sprintf(pname,"%c%c%c%c%c%c%c%c%c%c%c",46,32,32,32,32,32,32,32,32,32,32);
	CreateFileItem(&pChildDir[1],pname,lStartClu,0,gHeadIndex.ClusterSize,0x10);
	memset(pname,0,sizeof(pname));
	sprintf(pname,"%c%c%c%c%c%c%c%c%c%c%c",46,46,32,32,32,32,32,32,32,32,32);
	CreateFileItem(&pChildDir[2],pname,0,0,gHeadIndex.ClusterSize,0x10);
	offset = gHeadIndex.ChildStartSector * DEFAULT_SECTOR_SIZE;
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(pChildDir+1),2*sizeof(msdos_dir_entry)) != 2*sizeof(msdos_dir_entry))
	{
		free(pChildDir);
		printf("write ChildDir error!!!\n");
		return -1;
	}
	dirOffset = lseek64(fpart,0,SEEK_CUR);
	sync();
	free(pChildDir); 
	pChildDir = NULL;

	//创建子目录文件
	struct long_msdos_dir_entry* pDir = NULL; 
	lRootFileSize -= 2*sizeof(msdos_dir_entry);
	if ((pDir = (struct long_msdos_dir_entry *)malloc (lRootFileSize)) == NULL)
	{
		printf("unable to allocate space in memory!!!\n");
		return -1;
	}
	memset(pDir, 0, lRootFileSize);
	
	gos_indexSize = sizeof(GosIndex)*gHeadIndex.lJpegFileNum;
	struct GosIndex* pGos_index = NULL; 
	if ((pGos_index = (struct GosIndex *)malloc(gos_indexSize)) == NULL)
	{
		free(pDir);
		printf("unable to allocate space in memory!!!\n");
		return -1;
	}
	memset(pGos_index, 0, gos_indexSize);
	struct GosIndex *pIndex = &pGos_index[0];

	int j;
	int lAviCount = 0;
	char longName[25] = {0};
	int start = lStartClu+lClustersofRoot;
	while ( lAviCount < lFileNum )
	{
		char shortname[13];
		int num = lAviCount + gHeadIndex.lRootDirFileNum;
		for(j=0;j<6;j++)
		{
			shortname[5-j]='0'+num%10;
			num=num/10;
		}
		shortname[0] = 0xE5;
		strcpy(&shortname[6],"~1JPG");
		sprintf(longName,"%d%s",lAviCount + gHeadIndex.lRootDirFileNum,".jpg");
		
		CreateLongFileItem(&pDir[lAviCount],shortname,longName,start,JPEG_MAX_LENTH,gHeadIndex.ClusterSize,0x20);

		pIndex->startCluster = start;
		pIndex->CluSectorsNum = (start - 1) / 128 + gHeadIndex.FAT1StartSector;
		pIndex->CluSectorsEA = (start - 1) * 4 % 512;
		pIndex->fileInfo.fileIndex = lAviCount + gHeadIndex.lRootDirFileNum;
		pIndex->fileInfo.filestate = WRITE_OK;
		pIndex ++;
		
		start += JPEG_MAX_LENTH/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
		if((JPEG_MAX_LENTH % (gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)) != 0)
		start += 1;
		lAviCount++;
	}

	lseek64(fpart,dirOffset,SEEK_SET);
	if (write(fpart,(char *)(pDir),lRootFileSize) != (int)lRootFileSize)
	{
		free(pDir);
		free(pGos_index);
		printf("write ChildDir error!!!\n");
		return -1;
	}
	sync();
	free(pDir);
	pDir = NULL;

	//写子目录簇链
	offset = gHeadIndex.ChildClusterListEA+4;
	lseek64(fpart,offset,SEEK_SET);
	j = lStartClu;
	for(lAviCount = 0;lAviCount < lClustersofRoot-1;lAviCount++)
	{
		j++;
		write(fpart,&j, 4);
	}
	int flag = 0xFFFFFFFF;
	write(fpart,&flag, 4);
		
	pIndex = &pGos_index[0];
	int ii;
	int DataSectorsNum =  lClustersofRoot * gHeadIndex.ClusterSize + gHeadIndex.ChildStartSector;
	for(ii=0;ii<gHeadIndex.lJpegFileNum;ii++)
	{
		pIndex->DirSectorsNum = gHeadIndex.ChildStartSector +ii / (DEFAULT_SECTOR_SIZE / LONG_DIR_ITEM_SIZE);
		pIndex->DirSectorsEA= ii%(DEFAULT_SECTOR_SIZE / LONG_DIR_ITEM_SIZE)*LONG_DIR_ITEM_SIZE + 2*sizeof(msdos_dir_entry);
		pIndex->DataSectorsNum = DataSectorsNum;
		pIndex++;
		DataSectorsNum += lClustersofFile*gHeadIndex.ClusterSize;
	}
	
	//写gJpegIndexList索引
	offset = gHeadIndex.JpegStartEA;
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(pGos_index),gos_indexSize) != (int)gos_indexSize)
	{
		free(pGos_index);
		printf("write ChildDir error!!!\n");
		return -1;
	}
	dirOffset = lseek64(fpart,0,SEEK_CUR);
	sync();
	free(pGos_index);
	pGos_index = NULL;
	
	//更新索引标志
	gHeadIndex.FlagIndexHead = FLAG_INDEX_HEAD;
	offset = gHeadIndex.HeadStartSector * DEFAULT_SECTOR_SIZE;
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(&gHeadIndex), sizeof(HeadIndex)) != (int)sizeof(HeadIndex))
	{
		printf("write ChildDir error!!!\n");
		return -1;
	}
	sync();
	
	return 0;
}

int FormatParttion(int fpart, unsigned long filesize, unsigned long lAviNum,unsigned long eventlogSizeM /*, bool bigDiskFlag*/)
{
	//获取分区的物理信息，
	hd_geometry geometry;
	if ( ioctl(fpart, HDIO_GETGEO, &geometry) )
		return -1;

	//得到分区的大小
	LONGLONG cblocks;	// 单位是 512 字节
	
	//if ( true == bigDiskFlag )
	{
		//说明当前是超大容量磁盘
		LONG sz;	  
		LONGLONG b;
		int err;
	
		err = ioctl( fpart, BLKGETSIZE64, &b );
	
		printf("disk_size=%d errcode=%d\n",b , err);
	
		if ( err || b == 0 || b == sz ) 	   
			cblocks = sz;		 
		else			 
			cblocks = ( b >> 9 );	//总共扇区数

		printf("cblocks..000...111==%llu\n",cblocks);	
#if 1
		//减去 10M 的空间不分区
		cblocks -= 10*1024*2; // 1kB = 2个扇区 得到所有需要分区的扇区
#else
		cblocks = cblocks*9;
		cblocks = cblocks/10;
#endif
	}

	printf("cblocks..222!\n");	

	///获得文件的状态信息
	struct stat statbuf;
	if (fstat(fpart, &statbuf) < 0)
		return -1;
	if ( !S_ISBLK(statbuf.st_mode) )//如果文件是一个块设备，则S_ISBLK()返回真
	{
		statbuf.st_rdev = 0;///st_rdev;  
	}
	printf("cblocks..333! \n");	

	//初始化DBR中的参数，即引导扇区
	msdos_boot_sector mbs;	
	unsigned char szDummyBootJump[3] = {0};
	szDummyBootJump[0] = 0xeb;
	szDummyBootJump[1] = 0x3c;
	szDummyBootJump[2] = 0x90;
	memcpy(mbs.boot_jump,szDummyBootJump, 3);
	mbs.boot_jump[1] =((char *) &mbs.fat32.boot_code - (char *)&mbs) - 2; //58H
	strcpy ((char *)mbs.system_id, "MSDOS5.0"); 
	mbs.sector_size[0] = (char) ( DEFAULT_SECTOR_SIZE & 0x00ff );
	mbs.sector_size[1] = (char) ( (DEFAULT_SECTOR_SIZE & 0xff00) >> 8 );
	mbs.reserved = CT_LE_W(DBR_RESERVED_SECTORS);
	mbs.fats = (char)DEFAULT_FAT_NUM;	
	mbs.dir_entries[0] = (char)0;  
	mbs.dir_entries[1] = (char)0;  //fat32中根目录也当成文件来处理。fat16才使用该字段。fat32要为0
	mbs.media = (char)0xf8; 
	mbs.fat_length = CT_LE_W(0);   //fat32要求该字段为0
	mbs.secs_track = CT_LE_W(geometry.sectors);	
	mbs.heads = CT_LE_W(geometry.heads);
	mbs.hidden = CT_LE_L(0);
	
	mbs.fat32.flags = CT_LE_W(0);
	mbs.fat32.version[0] = 0;
	mbs.fat32.version[1] = 0;
	mbs.fat32.root_cluster = CT_LE_L(2);
	mbs.fat32.info_sector = CT_LE_W(1);
	int backupboot = (DBR_RESERVED_SECTORS >= 7) ? 6 :
	(DBR_RESERVED_SECTORS >= 2) ? DBR_RESERVED_SECTORS-1 : 0;
	mbs.fat32.backup_boot = CT_LE_W(backupboot);
	memset( &mbs.fat32.reserved2, 0, sizeof( mbs.fat32.reserved2 ) );
    
	//FAT32分区的扩展BPB字段  
	time_t CreateTime;
	time(&CreateTime);//可用于卷标，
	long lVolumeId = (long)CreateTime;	
	struct msdos_volume_info *vi =  &mbs.fat32.vi;
	vi->volume_id[0] = (unsigned char) (lVolumeId & 0x000000ff);
	vi->volume_id[1] = (unsigned char) ((lVolumeId & 0x0000ff00) >> 8);
	vi->volume_id[2] = (unsigned char) ((lVolumeId & 0x00ff0000) >> 16);
	vi->volume_id[3] = (unsigned char) (lVolumeId >> 24);
	memcpy( vi->volume_label, "vol", 11 );
	memcpy( vi->fs_type, MSDOS_FAT32_SIGN, 8 );

	char szDummyBootCode[BOOTCODE_FAT32_SIZE] = {0};
	szDummyBootCode[BOOTCODE_FAT32_SIZE-1] = 0;
	memcpy(mbs.fat32.boot_code, szDummyBootCode, BOOTCODE_FAT32_SIZE);
	mbs.boot_sign = CT_LE_W(BOOT_SIGN);///#define BOOT_SIGN 0xAA55

	//计算出参数cluster_size fat32_length的取值
	unsigned long long VolSize = cblocks*SECTORS_PER_BLOCK;//

	printf( "cblocks..444!\n"  );	

	//先给cluster_size赋一个粗值
	if ( VolSize <= 66600 )
	{
		printf("Volume too small for FAT32!\n" );
		return -1;
	}else if ( VolSize <= 532480 )
		mbs.cluster_size = 1;
	else if (VolSize <= 16777216)
		mbs.cluster_size = 8;
	else if (VolSize <= 33554432)
		mbs.cluster_size = 16;
	else if (VolSize <= 67108864)
		mbs.cluster_size = 32;
	else
		mbs.cluster_size = 64;

	//为cluster_size选择一个合适的值
	unsigned long  lTotalSectors = (unsigned long) cblocks; 

	printf( "cblocks==%ld  lTotalSectors=%d\n",cblocks ,lTotalSectors );
	
	unsigned long long lFatData = lTotalSectors - DBR_RESERVED_SECTORS;	 //剩余需要分区的扇区个数
	int maxclustsize = 128;	
	unsigned long lClust32; //记录数据区的簇数
	unsigned long lMaxClust32;
	unsigned long lFatLength32; //记录fat32表占的扇区数
	do 
	{
		lClust32 = (lFatData * DEFAULT_SECTOR_SIZE - DEFAULT_FAT_NUM*8) /
			((int) mbs.cluster_size * DEFAULT_SECTOR_SIZE + DEFAULT_FAT_NUM*4);
		lFatLength32 = ((lClust32+2) * 4 + DEFAULT_SECTOR_SIZE - 1) / DEFAULT_SECTOR_SIZE;

		lClust32 = (lFatData - DEFAULT_FAT_NUM*lFatLength32)/mbs.cluster_size;
		lMaxClust32 = (lFatLength32 * DEFAULT_SECTOR_SIZE) / 4;
		if (lMaxClust32 > MAX_CLUST_32)///#define MAX_CLUST_32	((1 << 28) - 16)
			lMaxClust32 = MAX_CLUST_32;

		if (lClust32 > lMaxClust32) 
		{
			lClust32 = 0;
		}
		
		if (lClust32 )///mine
			break;
		
		mbs.cluster_size <<= 1;
	} while (mbs.cluster_size && mbs.cluster_size <= maxclustsize);


	if ( !lClust32 )
	{
		printf( "Attempting to create a too large file system!\n" );
		return -1;
	}

	//到此处就为cluster_size选择了一个合适的值
	mbs.fat32.fat32_length = CT_LE_L( lFatLength32 );
	
	//填写参数sectors  total_sect
	if ( lTotalSectors >= 65536 )
	{
		mbs.sectors[0] = (char)0;
		mbs.sectors[1] = (char)0;
		mbs.total_sect = CT_LE_L( lTotalSectors );
	}
	else
	{
		mbs.sectors[0] = (char)( lTotalSectors & 0x00ff );
		mbs.sectors[1] = (char)((lTotalSectors & 0xff00) >> 8);
		mbs.total_sect = CT_LE_L(0);
	}
	unsigned long lStartDataofSector = (DBR_RESERVED_SECTORS + DEFAULT_FAT_NUM*lFatLength32);
	unsigned long lStartDataofBlock = (lStartDataofSector + SECTORS_PER_BLOCK - 1) / SECTORS_PER_BLOCK;
	if ( cblocks < lStartDataofBlock + 32 )	
	{
		printf( "Too few blocks for viable file system!!\n " );
		return -1;
	}
	
	//下面是填写fat32表和根目录文件。
	unsigned char* pFat = NULL; 
	unsigned char* pZeroFat = NULL;
	if ( ( pFat = ( unsigned char * )malloc(DEFAULT_SECTOR_SIZE) ) == NULL )
	{
		printf("nunable to allocate space for FAT image in memory!\n ");
		return -1;
	}
	memset(pFat, 0, DEFAULT_SECTOR_SIZE);
	
	if ((pZeroFat = (unsigned char *)malloc(DEFAULT_SECTOR_SIZE)) == NULL)
	{
		printf("unable to allocate space for FAT image in memory! \n");

		free(pFat);
		return -1;
	}
	memset(pZeroFat, 0, DEFAULT_SECTOR_SIZE);

	//填写fat表的第0，1簇
	Mark_FAT_Cluster(0, 0xffffffff,pFat);	
	pFat[0] = (unsigned char)mbs.media;	
	Mark_FAT_Cluster(1, 0xffffffff,pFat);

	//文件所占的簇数
	unsigned long lClustersofFile = filesize/(mbs.cluster_size*DEFAULT_SECTOR_SIZE);
	//整个FAT可写的文件数
	unsigned long lFileNum = (lFatLength32*DEFAULT_SECTOR_SIZE/4-2)/lClustersofFile;
	//根目录所占的簇数
	unsigned long lClustersofRoot = (lFileNum+1)*LONG_DIR_ITEM_SIZE/(mbs.cluster_size*DEFAULT_SECTOR_SIZE)+1;

	//下面填写根目录所在的簇，FAT32将根目录等同于普通的文件
	unsigned long nRootCluCount;
	for(nRootCluCount = 0; nRootCluCount < lClustersofRoot; nRootCluCount++)//lClustersofRoot一般为1
	{
		if(nRootCluCount == (lClustersofRoot - 1))
		{
			Mark_FAT_Cluster(nRootCluCount+2,0xffffffff,pFat);
		}
		else
		{
			Mark_FAT_Cluster(nRootCluCount+2,nRootCluCount+3,pFat);
		}
	}

	//根目录文件的大小(字节数)
	unsigned long lRootFileSize = lClustersofRoot * mbs.cluster_size * DEFAULT_SECTOR_SIZE;
	struct long_msdos_dir_entry* pRootDir = NULL; //指向根目录文件
	if ((pRootDir = (struct long_msdos_dir_entry *)malloc (lRootFileSize)) == NULL)
	{
		free(pFat);		
		free(pZeroFat);
		printf("unable to allocate space for root directory in memory\n");
		return -1;
	}
	memset(pRootDir, 0, lRootFileSize);

	//下面是写根目录文件
	#if 0
	//根目录的第0文件为：卷标
	////初始化第0个目录项(32个字节)//////////
	struct msdos_dir_entry *de = &pRootDir[0];  
	memcpy(de->name, "GOSCAM_WUSM", 11);
	de->attr = ATTR_VOLUME;//mine
	struct tm* ctime = localtime(&CreateTime);
	de->time = CT_LE_W( (unsigned short)((ctime->tm_sec >> 1) +
		                	(ctime->tm_min << 5) + (ctime->tm_hour << 11)));
	de->date = CT_LE_W( (unsigned short)(ctime->tm_mday +
				               ((ctime->tm_mon+1) << 5) +
				               ((ctime->tm_year-80) << 9)) );
	de->ctime_ms = 0;
	de->ctime = de->time;
	de->cdate = de->date;
	de->adate = de->date;
	de->starthi = CT_LE_W(0);
	de->start = CT_LE_W(0);
	de->size = CT_LE_L(0);
    #endif
	//分区上要有几个特定的文件
	unsigned long lStartClu = lClustersofRoot + 2;//数据区从二开始编簇号,数据区先放根目录，然后才放其它文件（在mbr中已经规定好了）

	//CreateFileItem(&pRootDir[0],"GosIndexDAT",lStartClu,10*1024*1024,mbs.cluster_size,0x20);
	char shortname[11] = {0};
	char longName[26] = {0};
	sprintf(shortname,"%s%s","gIndex","~1DAT");
	sprintf(longName,"%s","GosStorageManagerIndex.dat");
	
	CreateLongFileItem(&pRootDir[0],shortname,longName,lStartClu,10*1024*1024,mbs.cluster_size,0x20);
	//CreateLongFileItem(&pRootDir[2],shortname,longName,lStartClu,10*1024*1024,mbs.cluster_size,0x20);

   	//有实际簇的个数得到的视频文件数
   	lClust32 -= (10*1024*1024)/(mbs.cluster_size * DEFAULT_SECTOR_SIZE);  //去掉10M的空间不分配
    //unsigned long lActualAviNum = (lClust32 - (lStartClu -2) - PIC_PARTITION_SIZE)/lClustersofFile;
	#if CLOUD_STORAGE
	unsigned long clu = (FILE_MAX_LENTH) / (mbs.cluster_size * DEFAULT_SECTOR_SIZE);
	#else
	unsigned long clu = (MP4_MAX_LENTH + JPEG_MAX_LENTH) / (mbs.cluster_size * DEFAULT_SECTOR_SIZE);
	#endif
	unsigned long lActualAviNum = lClust32  / clu;

	printf("clu=%d lActualAviNum=%d lClust32=%d\n",clu,lActualAviNum,lClust32);
	      		
	gHeadIndex.FAT1StartSector = DBR_RESERVED_SECTORS;
	gHeadIndex.RootStartSector = gHeadIndex.FAT1StartSector + 2*lFatLength32;
	gHeadIndex.lRootDirFileNum = lActualAviNum;  
	gHeadIndex.ClusterSize = mbs.cluster_size;
	gHeadIndex.HeadStartSector = lClustersofRoot*gHeadIndex.ClusterSize+gHeadIndex.RootStartSector;
	
	//添加根目录项的"."和".."
	//char pname[11] = {0};
	//sprintf(pname,"%c%c%c%c%c%c%c%c%c%c%c",46,32,32,32,32,32,32,32,32,32,32);
	//CreateFileItem(&pRootDir[0],pname,0,0,gHeadIndex.ClusterSize,0x10);
	//memset(pname,0,sizeof(pname));
	//sprintf(pname,"%c%c%c%c%c%c%c%c%c%c%c",46,46,32,32,32,32,32,32,32,32,32);
	//CreateFileItem(&pRootDir[1],pname,0,0,gHeadIndex.ClusterSize,0x10);

	unsigned long all_gos_indexSize = sizeof(GosIndex)*gHeadIndex.lRootDirFileNum;
	struct GosIndex* pGos_index = NULL; 
	if ((pGos_index = (struct GosIndex *)malloc (all_gos_indexSize)) == NULL)
	{
		free(pFat);		
		free(pZeroFat);
		free(pRootDir);
		printf("unable to allocate space for root directory in memory\n");
		return -1;
	}
	memset(pGos_index, 0, all_gos_indexSize);	
	struct GosIndex *pIndex = &pGos_index[0];

	//循环写视频文件
	//预留一个文件索引用于交换文件或升级文件
	unsigned long lAviCount = 0;
	int j;
	while ( lAviCount < lActualAviNum - 1 )
	{
		char sz[13];
		int num = lAviCount + 1;
		for(j=0;j<6;j++)
		{
			sz[5-j]='0'+num%10;
			num=num/10;
		}
		sz[0] = 0xE5;
	#if CLOUD_STORAGE
		strcpy(&sz[6],"~1DAT");
		memset(longName,0,sizeof(longName));
		sprintf(longName,"%d%s",lAviCount + 1,".H264");
	#else
		strcpy(&sz[6],"~1MP4");
		memset(longName,0,sizeof(longName));
		sprintf(longName,"%d%s",lAviCount + 1,".mp4");
	#endif
		//CreateFileItem( &pRootDir[lAviCount+1],sz,lStartClu,filesize,mbs.cluster_size, 0x20);
		CreateLongFileItem(&pRootDir[lAviCount+1],sz,longName,lStartClu,filesize,mbs.cluster_size,0x20);

		lAviCount++;
	}

	//下面写文件信息
	unsigned char* pInfoSector = NULL;
	if ( ( pInfoSector = (unsigned char *)malloc(DEFAULT_SECTOR_SIZE)) == NULL )
	{	
		free(pFat);		
		free(pZeroFat);
		free(pRootDir);
		free(pGos_index);
		printf("Out of memory!\n");
		return -1;		
	}
	memset(pInfoSector, 0, DEFAULT_SECTOR_SIZE);
	
	struct fat32_fsinfo* info = (struct fat32_fsinfo *)(pInfoSector + 0x1e0);	
	
	pInfoSector[0] = 'R';
	pInfoSector[1] = 'R';
	pInfoSector[2] = 'a';
	pInfoSector[3] = 'A';	
	
	info->signature = CT_LE_L(0x61417272);
	
	info->free_clusters = CT_LE_L(lClust32-(lStartClu-2));//
	info->next_cluster = CT_LE_L(lStartClu);
	
	*(__u16 *)(pInfoSector + 0x1fe) = CT_LE_W(BOOT_SIGN);

	//到此处文件信息写完。
	lseek64(fpart,0,SEEK_SET);
	BYTE nIndex = 0;
	for( nIndex = 0; nIndex < DBR_RESERVED_SECTORS; nIndex++ )//写了32*512字节为0
	{
		if ( write( fpart, pZeroFat, DEFAULT_SECTOR_SIZE ) != DEFAULT_SECTOR_SIZE )
		{
			FREE();
			return -1;
		}
	}

    printf( "cblocks..555!\n "  );		

	//写dbr
	lseek64(fpart,0,SEEK_SET);
	int count ;
	if(write(fpart,(char *)&mbs,DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
	{
		FREE();
		return -1;
	}
	
	//写文件信息
	lseek64(fpart,CF_LE_W(mbs.fat32.info_sector)*DEFAULT_SECTOR_SIZE,SEEK_SET);  
	if(write(fpart,(char *)pInfoSector,DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
	{
		FREE();
		return -1;
	}

	printf( "cblocks..666! \n"  );		

	//写backup boot sector
	lseek64(fpart,CF_LE_W(mbs.fat32.backup_boot)*DEFAULT_SECTOR_SIZE,SEEK_SET);
	if(write(fpart,(char *)&mbs,DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
	{
		FREE();
		return -1;
	}

	//写FAT表中，在前面已经写了一部份了
	lseek64(fpart,DBR_RESERVED_SECTORS*DEFAULT_SECTOR_SIZE,SEEK_SET);
	int nFatCount; 
	int indexNum = 0;
	for (nFatCount = 1; nFatCount <= DEFAULT_FAT_NUM; nFatCount++ )
	{
		//对fat表是一个个扇区的大小写入硬盘中
		int nPageNum = lFatLength32/6;//用于显示格式的进度
		int nClusterCount  =0; 
		unsigned long cu = 0;//用于记录当前簇
		struct long_msdos_dir_entry *pFile = &pRootDir[0];
		unsigned long fristno = pFile->dir_entry.size / (4096 * 128);
		unsigned long fatno;
		for (fatno = 0; fatno < lFatLength32; fatno++)
		{
			unsigned char *pFatSector=pFat;
			if(fatno == 0)
			{ 
				pFatSector=pFat;
				cu=lClustersofRoot+2;
				nClusterCount = cu;
			}
			else
			{
				pFatSector=pZeroFat;
				memset(pZeroFat, 0, DEFAULT_SECTOR_SIZE);
				nClusterCount = 0;
			}
			int bNewFile = 1;
			unsigned long lEnd = 0;
			int fileCount = 0;
			while((nClusterCount <  DEFAULT_SECTOR_SIZE/4) && (pFile->dir_entry.size > 0))
			{
				if (bNewFile)
				{
					lEnd = pFile->dir_entry.starthi;
					lEnd <<= 16;
					lEnd |= pFile->dir_entry.start;
					lEnd += pFile->dir_entry.size/(mbs.cluster_size*DEFAULT_SECTOR_SIZE);
					if((pFile->dir_entry.size%(mbs.cluster_size*DEFAULT_SECTOR_SIZE)) == 0)
					{
						lEnd--;
					}
				}	
				if(cu < lEnd)//当前簇小于文件的簇数
				{
					
					if(pFile==&pRootDir[0])
					{
						Mark_FAT_Cluster(cu,cu+1,pFatSector);
					}
					else
					{
						Mark_FAT_Cluster(cu,0,pFatSector);
					}	
					bNewFile = -1;	
				}
				else
				{
					Mark_FAT_Cluster(cu,0xffffffff,pFatSector);
					if(nFatCount < DEFAULT_FAT_NUM)
					{
						int  datahi = pFile->dir_entry.starthi;
						pIndex->startCluster = (datahi << 16) | pFile->dir_entry.start;
						pIndex->CluSectorsNum = (pIndex->startCluster - 1) / 128 + gHeadIndex.FAT1StartSector;
						pIndex->CluSectorsEA = (pIndex->startCluster - 1) * 4 % 512;
						pIndex->fileInfo.fileIndex = indexNum;
						pIndex->fileInfo.filestate = WRITE_OK;
						indexNum++;
						if(fileCount < gHeadIndex.lRootDirFileNum)
							pIndex++;
						else
							pIndex = NULL;
						fileCount++;
					}
					pFile++;
					pFile->dir_entry.start = (cu+1) & 0xffff;
					pFile->dir_entry.starthi = ((cu+1) & 0xffff0000) >> 16;
					bNewFile = 1;
				}
				cu++;
				nClusterCount++;
			}
			
			if (write(fpart,(char *)pFatSector, DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
			{
				FREE();
				return -1;
			}
		}	
	}

	printf("cblocks..777! \n"  );	
	//写根目录文件
	if (write(fpart,(char *)pRootDir, lRootFileSize) != (int)lRootFileSize)
	{
		FREE();
		return -1;
	}
	
	struct GosIndex *ppIndex = &pGos_index[0];
	int ii;
	for(ii=0;ii<gHeadIndex.lRootDirFileNum;ii++)
	{
		ppIndex->DirSectorsNum = gHeadIndex.RootStartSector + (ii*LONG_DIR_ITEM_SIZE)/DEFAULT_SECTOR_SIZE;
		ppIndex->DirSectorsEA = (ii*LONG_DIR_ITEM_SIZE) % DEFAULT_SECTOR_SIZE;
		ppIndex->DataSectorsNum = (ppIndex->startCluster - 2) * gHeadIndex.ClusterSize + gHeadIndex.RootStartSector;
		ppIndex++;
	}

	//写gMp4IndexList索引
	__u64 offset = gHeadIndex.HeadStartSector *DEFAULT_SECTOR_SIZE+sizeof(gHeadIndex);
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(pGos_index), all_gos_indexSize) != (int)all_gos_indexSize)
	{
		FREE();
		return -1;
	}

	ppIndex = &pGos_index[0];
	for(lAviCount = 0;lAviCount < gHeadIndex.lRootDirFileNum - 1;lAviCount++)
	{
		ppIndex++;
	}	

	gHeadIndex.JpegStartEA  = lseek(fpart,0,SEEK_CUR);
	#if CLOUD_STORAGE
	gHeadIndex.ChildStartCluster = ppIndex->startCluster + FILE_MAX_LENTH/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	gHeadIndex.ChildStartSector = ppIndex->DataSectorsNum + FILE_MAX_LENTH/DEFAULT_SECTOR_SIZE;
	gHeadIndex.ChildClusterListEA = ppIndex->CluSectorsNum* DEFAULT_SECTOR_SIZE 
		+  ppIndex->CluSectorsEA + FILE_MAX_LENTH /(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)*4;
	#else
	gHeadIndex.ChildStartCluster = ppIndex->startCluster + MP4_MAX_LENTH /(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	gHeadIndex.ChildStartSector = ppIndex->DataSectorsNum + MP4_MAX_LENTH/DEFAULT_SECTOR_SIZE;
	gHeadIndex.ChildClusterListEA = ppIndex->CluSectorsNum* DEFAULT_SECTOR_SIZE 
		+  ppIndex->CluSectorsEA + MP4_MAX_LENTH /(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)*4;
	#endif
	gHeadIndex.ChildItemEA = ppIndex->DirSectorsNum*DEFAULT_SECTOR_SIZE + ppIndex->DirSectorsEA + LONG_DIR_ITEM_SIZE;

	FREE();
	sync();

	printf( "cblocks..8888 do_success!\n "  );
	#if CLOUD_STORAGE
	//更新索引标志
	gHeadIndex.FlagIndexHead = FLAG_INDEX_HEAD;
	gHeadIndex.CurrIndexPos = 0;
	offset = gHeadIndex.HeadStartSector * DEFAULT_SECTOR_SIZE;
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(&gHeadIndex), sizeof(HeadIndex)) != (int)sizeof(HeadIndex))
	{
		printf("write ChildDir error!!!\n");
		return -1;
	}
	sync();
	#else
	FormatjpegDir(fpart);
	#endif
	
	return 0;
}

//找到索引头
int Find_head_index(int fpart)
{
	hd_geometry geometry;
	LONGLONG cblocks;	// 单位是 512 字节
	LONG sz;	  
	LONGLONG b;
	__u8 cluster_size;
	if ( ioctl(fpart, HDIO_GETGEO, &geometry) )
		return -1;
	int err = ioctl( fpart, BLKGETSIZE64, &b );

	if ( err || b == 0 || b == sz ) 	   
		cblocks = sz;		 
	else			 
		cblocks = ( b >> 9 );	

	//减去 10M 的空间不分区
	cblocks -= 10*1024*2;

	//计算出参数cluster_size fat32_length的取值
	unsigned long long VolSize = cblocks*SECTORS_PER_BLOCK;//
	//先给cluster_size赋一个粗值
	if ( VolSize <= 66600 )
	{
		return -1;
	}else if ( VolSize <= 532480 )
		cluster_size = 1;
	else if (VolSize <= 16777216)
		cluster_size = 8;
	else if (VolSize <= 33554432)
		cluster_size = 16;
	else if (VolSize <= 67108864)
		cluster_size = 32;
	else
		cluster_size = 64;

	//为cluster_size选择一个合适的值
	unsigned long  lTotalSectors = (unsigned long) cblocks; 	
	unsigned long long lFatData = lTotalSectors - DBR_RESERVED_SECTORS;	
	int maxclustsize = 128;	
	unsigned long lClust32; //记录数据区的簇数
	unsigned long lMaxClust32;
	unsigned long lFatLength32; //记录fat32表占的扇区数
	do 
	{
		lClust32 = (lFatData * DEFAULT_SECTOR_SIZE - DEFAULT_FAT_NUM*8) /
			((int) cluster_size * DEFAULT_SECTOR_SIZE + DEFAULT_FAT_NUM*4);
		lFatLength32 = ((lClust32+2) * 4 + DEFAULT_SECTOR_SIZE - 1) / DEFAULT_SECTOR_SIZE;

		lClust32 = (lFatData - DEFAULT_FAT_NUM*lFatLength32)/cluster_size;
		lMaxClust32 = (lFatLength32 * DEFAULT_SECTOR_SIZE) / 4;
		if (lMaxClust32 > MAX_CLUST_32)///#define MAX_CLUST_32	((1 << 28) - 16)
			lMaxClust32 = MAX_CLUST_32;

		if (lClust32 > lMaxClust32) 
		{
			lClust32 = 0;
		}
		
		if (lClust32 )///mine
			break;
		
		cluster_size <<= 1;
	} while (cluster_size && cluster_size <= maxclustsize);
	
	int dir_sectors = DBR_RESERVED_SECTORS + lFatLength32 * 2;  
	__u16  clusterhi = 0;
	__u16  cluster = 0;

	lseek64(fpart,dir_sectors*DEFAULT_SECTOR_SIZE + LONG_DIR_ITEM_SIZE - 12,SEEK_SET); //找索引起始簇高16位
	read(fpart,&clusterhi,2);
	lseek64(fpart,dir_sectors*DEFAULT_SECTOR_SIZE + LONG_DIR_ITEM_SIZE - 6,SEEK_SET); //找索引起始簇低16位
	read(fpart,&cluster,2);

	int index_offset_sectors  = clusterhi;
	index_offset_sectors << 16;
	index_offset_sectors |= cluster;

	int index_offset = (index_offset_sectors - 2) * cluster_size + dir_sectors;
	
	//通过目录找到索引头的位置,读索引头
	lseek64(fpart,index_offset * DEFAULT_SECTOR_SIZE,SEEK_SET); 
	read(fpart,&gHeadIndex,sizeof(HeadIndex));	

	return index_offset;
}


GosIndex* Get_Oldest_file()
{
	int lAviCount;
	int IndexSum;
	GosIndex* pGos_indexList;
	GosIndex* Start_pGos_indexList;
	int StartTimeStamp = 0;
	lAviCount = 0;
	IndexSum = gHeadIndex.lRootDirFileNum;
	pGos_indexList = gAVIndexList;
	
	StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
	Start_pGos_indexList = pGos_indexList;
	while(StartTimeStamp <= 0) //找非零时间作为基准
	{
		pGos_indexList++;
		StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
		Start_pGos_indexList = pGos_indexList;
		if(pGos_indexList == &gAVIndexList[IndexSum-1])
			break;
	}
	
	pGos_indexList = gAVIndexList;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pGos_indexList->fileInfo.recordStartTimeStamp > 0 && pGos_indexList->fileInfo.recordStartTimeStamp < StartTimeStamp )
			{
				StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
				Start_pGos_indexList = pGos_indexList;
			}
		}
		pGos_indexList++;
	}

	if(StartTimeStamp == 0)
	{
		return NULL;
	}
	
	printf("oldest time is %d, fd = %d\n",StartTimeStamp,Start_pGos_indexList->fileInfo.fileIndex);
	return Start_pGos_indexList;	
}

GosIndex* Get_Oldest_Alarm_file()
{
	int lAviCount;
	int IndexSum;
	GosIndex* pGos_indexList;
	GosIndex* Start_pGos_indexList;
	int StartTimeStamp = 0;
	lAviCount = 0;
	IndexSum = gHeadIndex.lRootDirFileNum;
	pGos_indexList = gAVIndexList;
	
	StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
	Start_pGos_indexList = pGos_indexList;
	while(StartTimeStamp <= 0) //找非零时间作为基准
	{
		pGos_indexList++;
		StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
		Start_pGos_indexList = pGos_indexList;
		if(pGos_indexList == &gAVIndexList[IndexSum-1])
			break;
	}
	
	pGos_indexList = gAVIndexList;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pGos_indexList->fileInfo.recordStartTimeStamp > 0 && pGos_indexList->fileInfo.recordStartTimeStamp < StartTimeStamp)
			{
				StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
				Start_pGos_indexList = pGos_indexList;
			}
		}
		pGos_indexList++;
	}

	if(StartTimeStamp == 0 || Start_pGos_indexList == NULL)
	{
		return NULL;
	}
	
	lAviCount = 1;
	IndexSum = gHeadIndex.lRootDirFileNum;
	for(lAviCount;lAviCount < IndexSum;lAviCount++) //找到最早的报警录像文件
	{
		if(Start_pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(Start_pGos_indexList->fileInfo.recordStartTimeStamp >= StartTimeStamp 
				&& Start_pGos_indexList->fileInfo.alarmType > 0)
			{
				break;
			}
		}
		if(Start_pGos_indexList == &gAVIndexList[IndexSum-1])
			Start_pGos_indexList = &gAVIndexList[1];
		else
			Start_pGos_indexList++;
	}
	
	if(lAviCount == IndexSum) //没有报警录像文件
	{
		printf("there is no alarm file in this time!!!\n");
		return NULL;
	}	
	printf("oldest alarm file time is %d, fd = %d\n",StartTimeStamp,Start_pGos_indexList->fileInfo.fileIndex);
	return Start_pGos_indexList;
}


GosIndex* Get_Index_Form_fd(unsigned int fd)
{
	int lAviCount;
	int IndexSum;
	GosIndex* pGos_indexList;
	lAviCount = 0;
	IndexSum = gHeadIndex.lRootDirFileNum;
	pGos_indexList = gAVIndexList;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pGos_indexList->fileInfo.fileIndex == fd)
			{
				return pGos_indexList;
			}
		}
		pGos_indexList++;
	}

	return NULL;
	
}

//更新磁盘索引
int Storage_Write_gos_index(int fpart,enum RECORD_FILE_TYPE fileType)
{
	unsigned long all_gos_indexSize;

#if CLOUD_STORAGE
	if( NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
#endif
	//写索引头
	Storage_Lock();
	lseek64(fpart,gHeadIndex.HeadStartSector * DEFAULT_SECTOR_SIZE,SEEK_SET);
	if (write(fpart,(char *)(&gHeadIndex), sizeof(HeadIndex)) != (int)sizeof(HeadIndex))
	{
		return -1;
	}

    switch(fileType)
    {
        case RECORD_FILE_MP4: 
		{
			all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
			if (write(fpart,(char *)(gMp4IndexList), all_gos_indexSize) != (int)all_gos_indexSize)
			{
				return -1;
			}
			break;
		}
        case RECORD_FILE_JPG: 
		{
            all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lJpegFileNum;
			lseek64(fpart,gHeadIndex.JpegStartEA,SEEK_SET);
			if (write(fpart,(char *)(gJpegIndexList), all_gos_indexSize) != (int)all_gos_indexSize)
			{
				return -1;
			}	
            break;
		}
		case RECORD_FILE_H264:
		{
            all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
			if (write(fpart,(char *)(gAVIndexList), all_gos_indexSize) != (int)all_gos_indexSize)
			{
				return -1;
			}	
            break;
		}

        default: 
		{
			return -1;
		}            
    }
	Storage_Unlock();
	sync();

	return 0;
}

int Storage_Get_File_Size(const char *fileName)
{
	if(RemoveSdFlag == 1)
	{
		return -1;
	}

	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char zero;
	//char recordtype;
	char alarmtype;
	//int fileduration;
	char filetype[32] = {0};
	enum RECORD_FILE_TYPE FileType;
	struct GosIndex *pGos_indexList;

	//printf("file_name id %s\n",fileName);
	sscanf(fileName,"%04d%02d%02d%02d%02d%02d%c%c%04d%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,/*&fileduration,*/filetype);

	if(strncmp(filetype,pFileTypeString[RECORD_FILE_MP4],strlen(filetype))==0)
	{
		pGos_indexList = &gMp4IndexList[1];
		IndexSum = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_MP4;
		lAviCount = 1;	
	}
	else if(strncmp(filetype,pFileTypeString[RECORD_FILE_JPG],strlen(filetype))==0)
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_JPG;
	}
	else
	{
		return -1;
	}
	//printf("%d %d %d %d %d %d\n",tm_year,tm_mon,tm_mday,tm_hour,tm_min,tm_sec);
	unsigned int Checktime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;
	unsigned int Checkdate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			//printf("%d %d    %d %d\n",pGos_indexList->fileInfo.recordStartDate,pGos_indexList->fileInfo.recordStartTime,Checkdate&0xffff,Checktime&0xffff);
			if(pGos_indexList->fileInfo.recordStartDate == (Checkdate&0xffff)
			   && pGos_indexList->fileInfo.recordStartTime == (Checktime&0xffff))
			{
				//printf("get 111111111111111111111111\n");
				return pGos_indexList->fileInfo.fileSize;
			}
		}
		pGos_indexList++;
	}
	//printf("2222222222222222222222\n");
	return 0;
}

//删除文件
int DelectStorageFile(int Fileindex)
{
	int lAviCount;
	int IndexSum;
	int maxFileSize;
	__u64 fatOffset;
	enum RECORD_FILE_TYPE FileType;
	struct GosIndex *pGos_indexList = NULL;
	struct long_msdos_dir_entry dir_entry; 
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char longName[25] = {0};
	char shortname[11] = {0};
	unsigned long start;
	
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
	
	if(Fileindex < gHeadIndex.lRootDirFileNum)
	{
		pGos_indexList = gMp4IndexList;
		IndexSum = gHeadIndex.lRootDirFileNum;
		lAviCount = 0;
		maxFileSize = MP4_MAX_LENTH;
		FileType = RECORD_FILE_MP4;
	}
	else
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
		maxFileSize = JPEG_MAX_LENTH;
		FileType = RECORD_FILE_JPG;
	}
		
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.fileIndex == Fileindex
			&& pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			tm_hour = (pGos_indexList->fileInfo.recordStartTime >> 11) & 0x1F;
			tm_min  = (pGos_indexList->fileInfo.recordStartTime >> 5) & 0x3F;
			tm_sec  = pGos_indexList->fileInfo.recordStartTime & 0x1F;

			tm_year = (pGos_indexList->fileInfo.recordStartDate >> 9) & 0x7F;
			tm_mon  = (pGos_indexList->fileInfo.recordStartDate >> 5) & 0x0F;
			tm_mday  = pGos_indexList->fileInfo.recordStartDate & 0x1F;	

			sprintf(longName,"%04d%02d%02d%02d%02d%02d0%c%s",(tm_year+1980),tm_mon,tm_mday,
			tm_hour,tm_min,2*tm_sec,/*pGos_indexList->fileInfo.recordType+'a',*/
			pGos_indexList->fileInfo.alarmType+'a',
			/*pGos_indexList->fileInfo.recordDuration,*/pFileTypeString[FileType]);

			if(FileType == RECORD_FILE_MP4)
			{
				sprintf(shortname,"%c%05d%s",229,pGos_indexList->fileInfo.fileIndex,"~1MP4");
				start = pGos_indexList->startCluster;
				CreateLongFileItem(&dir_entry,shortname,longName,start,MP4_MAX_LENTH,gHeadIndex.ClusterSize,0x20);
			}
			else if(FileType == RECORD_FILE_JPG)
			{
				sprintf(shortname,"%c%05d%s",229,pGos_indexList->fileInfo.fileIndex,"~1JPG");
				start = pGos_indexList->startCluster;
				CreateLongFileItem(&dir_entry,shortname,longName,start,JPEG_MAX_LENTH,gHeadIndex.ClusterSize,0x20);
			}
			else
			{
				return -1;
			}
	
			dir_entry.dir_entry.time = pGos_indexList->fileInfo.recordStartTime;
			dir_entry.dir_entry.date = pGos_indexList->fileInfo.recordStartDate;
			dir_entry.dir_entry.ctime = dir_entry.dir_entry.time;
			dir_entry.dir_entry.cdate = dir_entry.dir_entry.date;
			dir_entry.dir_entry.adate = dir_entry.dir_entry.date;
			dir_entry.dir_entry.starthi = CT_LE_W(pGos_indexList->startCluster>>16);
			dir_entry.dir_entry.start = CT_LE_W(pGos_indexList->startCluster&0xffff);
			dir_entry.dir_entry.size = pGos_indexList->fileInfo.fileSize;

			Storage_Lock();
			fatOffset = pGos_indexList->DirSectorsNum * DEFAULT_SECTOR_SIZE + pGos_indexList->DirSectorsEA;
			lseek64(fPart,fatOffset,SEEK_SET);
			if(write(fPart,&dir_entry,sizeof(long_msdos_dir_entry)) != sizeof(long_msdos_dir_entry))
			{
				Storage_Unlock();
				return -1;
			}
			Storage_Unlock();		
			
			//删除时设置为空文件
			pGos_indexList->fileInfo.filestate = WRITE_OK;
			break;
		}
		pGos_indexList++;
	}
	if(lAviCount == IndexSum) //没有找到对应的扇区地址
	{
		return -1;
	}		
	
	Storage_Write_gos_index(fPart,FileType);
		
	LONG len = pGos_indexList->fileInfo.fileSize;
	int cuCount = len / (gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE); //这个文件占的簇数
	int allCount = maxFileSize/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	if(len % (gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE) != 0)
	{
		cuCount += 1;  //不整除,分配多一簇
	}
	fatOffset = pGos_indexList->CluSectorsNum * DEFAULT_SECTOR_SIZE + pGos_indexList->CluSectorsEA;
	
	int *pfat = (int *)malloc(allCount*sizeof(int));
	memset(pfat,0,allCount*sizeof(int));
	int *ptmp = &pfat[0];
	for(lAviCount = 0;lAviCount < allCount;lAviCount++)
	{
		if(lAviCount==0||lAviCount==cuCount)
		{
			*ptmp = 0XFFFFFFFF;
		}
		else
		{
			*ptmp = 0;
		}	
		ptmp++;
	}
	
	Storage_Lock();
	lseek64(fPart,fatOffset,SEEK_SET);
	write(fPart,(char *)pfat,allCount*sizeof(int));
	Storage_Unlock();
	free(pfat);
	pfat = NULL;
	sync();
	return 0;
}

#define WitchList() 								\
{													\
	if(FileType == RECORD_FILE_MP4)    				\
	{   			 								\
		pGos_indexList = &gMp4IndexList[1];			\
		lAviCount = 1;								\
	}          					 					\
	else if(FileType == RECORD_FILE_JPG)  			\
	{          										\
		pGos_indexList = gJpegIndexList;            \
		lAviCount = gHeadIndex.lRootDirFileNum;		\
	}   											\
	else if(FileType == RECORD_FILE_H264)			\
	{												\
		pGos_indexList = &gAVIndexList[1];          \
		lAviCount = 1;								\
	}												\
	else											\
	{												\
		return NULL;									\
	}												\
}

#define  GetListInit() 														\
{																			\
	if(file_type == 0) 														\
	{																		\
		pGos_indexList = gMp4IndexList;						    			\
		IndexSum = gHeadIndex.lRootDirFileNum;  							\
		lAviCount = 0;            											\
		FileType = RECORD_FILE_MP4;											\
	}         																\
	else if(file_type == 1)													\
	{																		\
		pGos_indexList = gJpegIndexList;  				        			\
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;  	\
		lAviCount = gHeadIndex.lRootDirFileNum;   							\
		FileType = RECORD_FILE_JPG;											\
	}																		\
}																			


/*
mkfs_vfat: 
1:格式化,预分配
0:按标志位判断是否预分配
*/
int Storage_Init(int mkfs_vfat)
{
	char szPartName[128] = {0};
	unsigned long lAviNumtemp = 0;
	unsigned long lLogSizeMtemp = 0;
	int index_offset = 0;
	char command[512] = {0};
	
	if(fPart > 0)
	{
		close(fPart);
		fPart = -1;
	}
	
	Storage_Unlock();
	
	//FormatSdFlag = 1;
	
	int i;
	for(i=1;i<=4;i++)
	{
		sprintf(szPartName,"%s%d","/dev/mmcblk0p",i);
		fPart = open( szPartName, O_RDWR | O_LARGEFILE); //以大文件的方式打开
		if(fPart < 0 )
		{
			printf("open szPartName=%s Failed!!!\n",szPartName);
		}	
		else
		{
			printf("open szPartName=%s OK!!!\n",szPartName);
			break;
		}
	}
	
	//if(i>4)
	//{
	//	return -1;
	//}

	//yym add
	if(fPart < 0 )
	{
		if(!access("/dev/mmcblk0", F_OK))
		{
			sprintf(szPartName,"%s","/dev/mmcblk0");
			fPart = open( szPartName, O_RDWR | O_LARGEFILE); 
		}
		else if(!access("/dev/mmcblk1", F_OK))
		{
			sprintf(szPartName,"%s","/dev/mmcblk1");
			fPart = open( szPartName, O_RDWR | O_LARGEFILE); 
		}

		if(fPart < 0 )
		{
			printf("open szPartName=%s Failed!!!\n",szPartName);
			return -1;
		}	
		else
		{
			printf("open szPartName=%s OK!!!\n",szPartName);
		}

	}
	
	
	/********************************读取索引************************************/
	index_offset = Find_head_index(fPart);
	//判断索引头标志，没有标志说明没有进行过预分配,开始预分配
	//mkfs_vfat为真时,表示格式化，进行预分配
	if(gHeadIndex.FlagIndexHead != FLAG_INDEX_HEAD || mkfs_vfat)
	{
		sprintf(command,"umount -fl %s",SDDirName);
		StoragepopenRead(command);
		//进行预分配	
		#if CLOUD_STORAGE
		int bRet = FormatParttion(fPart, FILE_MAX_LENTH, lAviNumtemp, lLogSizeMtemp );	
		#else
		int bRet = FormatParttion(fPart, MP4_MAX_LENTH, lAviNumtemp, lLogSizeMtemp );	
		#endif
		memset(command,0,sizeof(command));
		sprintf(command,"mount -t vfat %s %s",szPartName,SDDirName);
		StoragepopenRead(command);
	}

	//分配内存给磁盘索引
	#if CLOUD_STORAGE
	unsigned long all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
	if ((gAVIndexList = (struct GosIndex *)malloc (all_gos_indexSize)) == NULL)
	{
		printf("unable to allocate space for gAVIndexList in memory\n");
		return -1;
	}
	printf("all_gos_indexSize ------------------>  %d\n",all_gos_indexSize);
	memset(gAVIndexList, 0, all_gos_indexSize);
	read(fPart,&gAVIndexList[0],all_gos_indexSize);
	#else
	unsigned long all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
	if ((gMp4IndexList = (struct GosIndex *)malloc (all_gos_indexSize)) == NULL)
	{
		printf("unable to allocate space for gMp4IndexList in memory\n");
		return -1;
	}
	memset(gMp4IndexList, 0, all_gos_indexSize);
	read(fPart,&gMp4IndexList[0],all_gos_indexSize);
	
	all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lJpegFileNum;
	if ((gJpegIndexList= (struct GosIndex *)malloc (all_gos_indexSize)) == NULL)
	{
		free(gMp4IndexList);
		gMp4IndexList = NULL;
		printf("unable to allocate space for gJpegIndexList in memory\n");
		return -1;
	}
	memset(gJpegIndexList, 0, all_gos_indexSize);
	lseek64(fPart,gHeadIndex.JpegStartEA,SEEK_SET);
	read(fPart,&gJpegIndexList[0],all_gos_indexSize);
	#endif
	
	FormatSdFlag = 0;
	printf( "storage init do_success!!!\n "  );
	return 0;
}

int Storage_Close_All()
{
	FormatSdFlag = 1;

	if(fPart > 0)
	{
		close(fPart);
		fPart = -1;
	}
	
	setMaxWriteSize(0);
#if CLOUD_STORAGE
	free(gAVIndexList);	
	gAVIndexList = NULL;	
#else
	free(gMp4IndexList);   	
	free(gJpegIndexList); 	      	 
	gMp4IndexList = NULL; 	 
	gJpegIndexList = NULL;  
#endif
	memset(&gHeadIndex,0,sizeof(HeadIndex));
	Storage_Unlock();           //非安全退出状态下一定要解锁
	printf("release all storage memory!!!\n");
	return 0;
}

//寻找可写文件的索引号(文件句柄)
char* storage_Open(const char *fileName)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}

	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char zero;
	//char recordtype;
	char alarmtype;
	//int fileduration;
	char filetype[32] = {0};

	if(!CheckSdIsMount())
	{
		return NULL;
	}
#if CLOUD_STORAGE
	if(NULL == gAVIndexList || NULL == fileName || (!StorageCheckSDExist()))
	{
		return NULL;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || NULL == fileName || (!StorageCheckSDExist()))
	{
		return NULL;
	}
#endif

	setMaxWriteSize(0);


	struct GosIndex *pGos_indexList;
	enum RECORD_FILE_TYPE FileType;
	sscanf(fileName,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,filetype);

#if CLOUD_STORAGE
	pGos_indexList = &gAVIndexList[1];
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 1;
	FileType = RECORD_FILE_H264;
#else
	if(strncmp(filetype,pFileTypeString[RECORD_FILE_MP4],strlen(filetype))==0)
	{
		pGos_indexList = &gMp4IndexList[1];
		IndexSum = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_MP4;
		lAviCount = 1;	
	}
	else if(strncmp(filetype,pFileTypeString[RECORD_FILE_JPG],strlen(filetype))==0)
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_JPG;
	}
	else
	{
		return NULL;
	}
	
#endif

	if(gHeadIndex.CurrIndexPos == 0 || gHeadIndex.CurrIndexPos >=gHeadIndex.lRootDirFileNum-1)
	{
		pGos_indexList = &gAVIndexList[1];
	}
	else
	{
		pGos_indexList = &gAVIndexList[gHeadIndex.CurrIndexPos+1];
	}
	unsigned int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec/2;
	unsigned int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
	pGos_indexList->fileInfo.recordStartTime = ltime & 0xffff;
	pGos_indexList->fileInfo.recordStartDate = ldate & 0xffff;
	pGos_indexList->fileInfo.filestate = OCCUPATION;
	oldStartTimeStap = pGos_indexList->fileInfo.recordStartTimeStamp;
	oldEndTimeStap   = pGos_indexList->fileInfo.recordEndTimeStamp;
	printf("\n>>>>>>>>>>>>open DiskFd=%d\n",pGos_indexList->fileInfo.fileIndex);
	return (char*)pGos_indexList;
#if 0
#if 0
	unsigned int Checktime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;
	unsigned int Checkdate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 

	//先查找文件是否存在，存在则返回该文件描述符，不存在则返回未使用的描述符
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pGos_indexList->fileInfo.recordStartDate == (Checkdate&0xffff)
			   && pGos_indexList->fileInfo.recordStartTime == (Checktime&0xffff))
			{
				return (char*)pGos_indexList;
			}
		}
		pGos_indexList++;
	}
#endif
	if(NULL == gAVIndexList)
		return NULL;

	WitchList();
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(NULL == gAVIndexList)
			return NULL;
		
		if(pGos_indexList->fileInfo.fileIndex == 0)
		{
			printf("pGos_indexList->fileInfo.fileIndex ===== 0 !!!!!!!!!!!!!!!!!\n");
			continue;
		}
		
		if(pGos_indexList->fileInfo.filestate == WRITE_OK)
		{
			//文件信息
			//pGos_indexList->fileInfo.recordDuration = fileduration;
			pGos_indexList->fileInfo.alarmType = alarmtype - 'a';
			//pGos_indexList->fileInfo.fileType = FileType;
			//pGos_indexList->fileInfo.recordType = recordtype - 'a';
			//文件的创建时间 
			
			unsigned int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec/2;
			unsigned int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
			pGos_indexList->fileInfo.recordStartTime = ltime & 0xffff;
			pGos_indexList->fileInfo.recordStartDate = ldate & 0xffff;
			pGos_indexList->fileInfo.recordStartTimeStamp = GetTimeStamp(fileName);
			printf("fd = %d , recordStartTimeStamp = %d\n",pGos_indexList->fileInfo.fileIndex,pGos_indexList->fileInfo.recordStartTimeStamp);
			pGos_indexList->fileInfo.filestate = OCCUPATION;
			printf("\n>>>>>>>>>>>>open DiskFd=%d\n",pGos_indexList->fileInfo.fileIndex);
			return (char*)pGos_indexList;
		}
		pGos_indexList++;
	}
	
	if(NULL == gAVIndexList)
		return NULL;

	//当所有的句柄都被使用的时候，则需要找个最老的文件句柄拿出来使用(循环录像)
	#if 1
	pGos_indexList = Get_Oldest_file();

	if(pGos_indexList != NULL)
	{
		//文件信息
		pGos_indexList->fileInfo.alarmType = alarmtype - 'a';
		
		//文件的创建时间 
		int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;
		int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
		pGos_indexList->fileInfo.recordStartTime = ltime&0xffff;
		pGos_indexList->fileInfo.recordStartDate = ldate&0xffff;
		pGos_indexList->fileInfo.recordStartTimeStamp = GetTimeStamp(fileName);
		printf("recordStartTimeStamp = %d\n",pGos_indexList->fileInfo.recordStartTimeStamp);
		pGos_indexList->fileInfo.filestate = OCCUPATION;
		printf("\n>>>>>>>>>>>>open DiskFd=%d\n",pGos_indexList->fileInfo.fileIndex);
		return (char*)pGos_indexList;
	}
	#else
	WitchList();
	
	int counts;
	unsigned short int adate = 0xFFFF;
	unsigned short int atime = 0xFFFF;
	//先找到一个最小的日期，在找该日期的最小的时间
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(NULL == gAVIndexList)
			return NULL;
		if(pGos_indexList->fileInfo.recordStartDate < adate)
		{
			adate = pGos_indexList->fileInfo.recordStartDate;
		}
		pGos_indexList++;
	}
	
	if(NULL == gAVIndexList)
		return NULL;
	
	WitchList();
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(NULL == gAVIndexList)
			return NULL;
		
		if(pGos_indexList->fileInfo.recordStartDate == adate
			&&pGos_indexList->fileInfo.recordStartTime < atime)
		{

			atime = pGos_indexList->fileInfo.recordStartTime;
		}
		pGos_indexList++;
	}
	
	WitchList();
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(NULL == gAVIndexList)
			return NULL;
		if(pGos_indexList->fileInfo.recordStartDate == adate
			&& pGos_indexList->fileInfo.recordStartTime == atime)
		{
			//文件信息
			//pGos_indexList->fileInfo.recordDuration = fileduration;
			pGos_indexList->fileInfo.alarmType = alarmtype - 'a';
			//pGos_indexList->fileInfo.fileType = FileType;
			//pGos_indexList->fileInfo.recordType = recordtype - 'a';
			
			//文件的创建时间 
			int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;
			int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
			pGos_indexList->fileInfo.recordStartTime = ltime&0xffff;
			pGos_indexList->fileInfo.recordStartDate = ldate&0xffff;
			pGos_indexList->fileInfo.recordStartTimeStamp = GetTimeStamp(fileName);
			printf("recordStartTimeStamp = %d\n",pGos_indexList->fileInfo.recordStartTimeStamp);
			pGos_indexList->fileInfo.filestate = OCCUPATION;
			printf("\n>>>>>>>>>>>>open DiskFd=%d\n",pGos_indexList->fileInfo.fileIndex);
			return (char*)pGos_indexList;
		}
		pGos_indexList++;
	}
	#endif
	return NULL;
	#endif
}

//更新fat表,目录以及索引
int Storage_Close(char* Fileindex,char *fileName,int fpart)
{	
	if(RemoveSdFlag == 1)
	{
		return -1;
	}

	__u64 fatOffset;
	int maxFileSize = 0;
	int lAviCount;
	unsigned int nlen = 0;
	unsigned int fileoffset = 0;
	int IndexSum;
	enum RECORD_FILE_TYPE FileType;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char zero;
	//int fileduration;
	char filetype[32] = {0};
	//char recordtype;
	char alarmtype;
	if(Fileindex ==NULL)
	{
		return -1;
	}
	if(!CheckSdIsMount())
	{
		return -1;
	}
#if CLOUD_STORAGE
	if( NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
#endif
	
	setMaxWriteSize(0);
	
	struct GosIndex *pGos_indexList;

#if CLOUD_STORAGE
	pGos_indexList = gAVIndexList;
	maxFileSize = FILE_MAX_LENTH;
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 0;
	FileType = RECORD_FILE_H264;
#else
	if(Fileindex < gHeadIndex.lRootDirFileNum)
	{
		pGos_indexList = gMp4IndexList;
		maxFileSize = MP4_MAX_LENTH;
		IndexSum = gHeadIndex.lRootDirFileNum;
		lAviCount = 0;
		FileType = RECORD_FILE_MP4;
	}
	else
	{
		pGos_indexList = gJpegIndexList;
		maxFileSize = JPEG_MAX_LENTH;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_JPG;
	}
#endif

	/*for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.fileIndex == Fileindex)
		{
			pGos_indexList->fileInfo.FileFpart = 0;  //文件连续读写结束了
			pGos_indexList->DataSectorsEA = 0;
			break;
		}
		pGos_indexList++;
	}
	if(lAviCount == IndexSum) //没有找到对应的扇区地址
	{
		return -1;
	}	*/
	pGos_indexList = (GosIndex*)Fileindex;
	pGos_indexList->fileInfo.FileFpart = 0;  //文件连续读写结束了
	pGos_indexList->DataSectorsEA = 0;
	
	if(pGos_indexList->fileInfo.recordStartTimeStamp == oldStartTimeStap 
		|| pGos_indexList->fileInfo.recordEndTimeStamp == oldEndTimeStap
		|| pGos_indexList->fileInfo.recordStartTimeStamp <= 1514736000
		|| pGos_indexList->fileInfo.recordEndTimeStamp <= 1514736000) //时间小于2010年1月1日
	{
		printf("record file timestap error[%d,%d], return -1 !\n",pGos_indexList->fileInfo.recordStartTimeStamp
			,pGos_indexList->fileInfo.recordEndTimeStamp);
		pGos_indexList->fileInfo.filestate = NON_EMPTY_OK;
		return -1;
	}

	LONG len = pGos_indexList->fileInfo.fileSize;
	int cuCount = len / (gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE); //这个文件占的簇数
	int allCount = maxFileSize/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	if(len % (gHeadIndex.ClusterSize * DEFAULT_SECTOR_SIZE) != 0)
	{
		cuCount += 1;  //不整除,分配多一簇
	}
	if(maxFileSize % (gHeadIndex.ClusterSize * DEFAULT_SECTOR_SIZE) != 0)
	{
		allCount +=1;  //不整除,分配多一簇
	}
	if(cuCount > allCount)
	{
		return -1;   //文件长度大于预分配文件大小
	}
	fatOffset = pGos_indexList->CluSectorsNum * DEFAULT_SECTOR_SIZE + pGos_indexList->CluSectorsEA;

	int *pfat = NULL;
	if((pfat = (int *)malloc(allCount*sizeof(int))) == NULL)
	{
		return -1;
	}
	memset(pfat,0,allCount*sizeof(int));
	int *ptmp = &pfat[0];
	for(lAviCount = 0;lAviCount < allCount;lAviCount++)
	{
		if(lAviCount==0||lAviCount==cuCount)
		{
			*ptmp = 0XFFFFFFFF;
		}
		else
		{
			*ptmp = pGos_indexList->startCluster+lAviCount;
		}	
		ptmp++;
	}
	
	Storage_Lock();
	lseek64(fpart,fatOffset,SEEK_SET);
	if(write(fpart,(char *)pfat,allCount*sizeof(int)) != allCount*sizeof(int))
	{
		free(pfat);
		pfat = NULL;
		ptmp = NULL;
		Storage_Unlock();
		return  -1;
	}
	free(pfat);
	pfat = NULL;
	ptmp = NULL;
	Storage_Unlock();
	
	struct long_msdos_dir_entry dir_entry; 
	//int tm_year = 0,tm_mon = 0,tm_mday = 0;
	//int tm_hour = 0,tm_min = 0,tm_sec = 0;
	char longName[25] = {0};
	char shortname[11] = {0};
	unsigned long start;
	
	#if 0
	tm_hour = (pGos_indexList->fileInfo.recordStartTime >> 11) & 0x1F;
	tm_min  = (pGos_indexList->fileInfo.recordStartTime >> 5) & 0x3F;
	tm_sec  = pGos_indexList->fileInfo.recordStartTime & 0x1F;

	tm_year = (pGos_indexList->fileInfo.recordStartDate >> 9) & 0x7F;
	tm_mon  = (pGos_indexList->fileInfo.recordStartDate >> 5) & 0x0F;
	tm_mday  = pGos_indexList->fileInfo.recordStartDate & 0x1F;	

	//预防时间解析不对,用当前的系统时间
	struct tm *p;
	time_t timep;
	
	tzset();
	time(&timep);
	p = localtime(&timep);
	unsigned int Y,M,D,T,F,S = 0;

	Y 	= 1900+p->tm_year;
	M	= 1+p->tm_mon;
	D	= p->tm_mday;
	T	= p->tm_hour;
	F	= p->tm_min;
	S	= p->tm_sec;
	//时间不对,打补丁
	if((tm_year+1980) != Y || tm_mon != M || tm_mday != D)
	{
		return -1;
		
		unsigned int ltime = T * 2048 + F * 32 + S/2;
		unsigned int ldate = (Y - 1980) * 512 + M * 32 + D; 
		
		pGos_indexList->fileInfo.recordStartTime = ltime & 0xffff;
		pGos_indexList->fileInfo.recordStartDate = ldate & 0xffff;

		tm_hour = (pGos_indexList->fileInfo.recordStartTime >> 11) & 0x1F;
		tm_min  = (pGos_indexList->fileInfo.recordStartTime >> 5) & 0x3F;
		tm_sec  = pGos_indexList->fileInfo.recordStartTime & 0x1F;

		tm_year = (pGos_indexList->fileInfo.recordStartDate >> 9) & 0x7F;
		tm_mon  = (pGos_indexList->fileInfo.recordStartDate >> 5) & 0x0F;
		tm_mday  = pGos_indexList->fileInfo.recordStartDate & 0x1F;	

		sprintf(longName,"%04d%02d%02d%02d%02d%02d0%c%c%04d%s",(tm_year+1980),tm_mon,tm_mday,
		tm_hour,tm_min,2*tm_sec,pGos_indexList->fileInfo.recordType+'a',
		pGos_indexList->fileInfo.alarmType+'a',
		pGos_indexList->fileInfo.recordDuration,pFileTypeString[FileType]);
	}
	else
	{
		struct tm *ptime;
		time_t tsm;
		tsm = pGos_indexList->fileInfo.recordStartTimeStamp;
		ptime = gmtime(&tsm);
		
		sprintf(longName,"%04d%02d%02d%02d%02d%02d0%c%c%04d%s",(ptime->tm_year+1900),ptime->tm_mon+1,ptime->tm_mday,
		tm_hour,ptime->tm_min,ptime->tm_sec,pGos_indexList->fileInfo.recordType+'a',
		pGos_indexList->fileInfo.alarmType+'a',
		pGos_indexList->fileInfo.recordDuration,pFileTypeString[FileType]);
	}
	#endif

	if(fileName != NULL)
	{
		sscanf(fileName,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
			&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype/*,&fileduration*/,filetype);
	
		unsigned int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec/2;
		unsigned int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
		pGos_indexList->fileInfo.recordStartTime = ltime & 0xffff;
		pGos_indexList->fileInfo.recordStartDate = ldate & 0xffff;
		pGos_indexList->fileInfo.alarmType = alarmtype - 'a';

		sprintf(fileName,"%04d%02d%02d%02d%02d%02d0%c.H264",tm_year,tm_mon,tm_mday,tm_hour,tm_min,(tm_sec/2)*2,pGos_indexList->fileInfo.alarmType+'a');
		printf("[record_lib]close filename === %s\n",fileName);
		strcpy(longName,fileName);
	}

	#if 0
	sprintf(longName,"%04d%02d%02d%02d%02d%02d0%c%c%04d%s",tm_year,tm_mon,tm_mday,
	tm_hour,tm_min,tm_sec,pGos_indexList->fileInfo.recordType+'a',
	pGos_indexList->fileInfo.alarmType+'a',
	pGos_indexList->fileInfo.recordDuration,pFileTypeString[FileType]);
	#endif
	//pGos_indexList->fileInfo.recordEndTimeStamp = timep;
	//printf("fd: %d, start: %d, end : %d\n",pGos_indexList->fileInfo.fileIndex,pGos_indexList->fileInfo.recordStartTimeStamp,pGos_indexList->fileInfo.recordEndTimeStamp);
	//printf("longName ================ %s\n",longName);
#if CLOUD_STORAGE
	sprintf(shortname,"%06d%s",pGos_indexList->fileInfo.fileIndex,"~1DAT");
	start = pGos_indexList->startCluster;
	CreateLongFileItem(&dir_entry,shortname,longName,start,FILE_MAX_LENTH,gHeadIndex.ClusterSize,0x20);
#else
	if(pGos_indexList->fileInfo.fileType == RECORD_FILE_MP4)
	{
		sprintf(shortname,"%06d%s",pGos_indexList->fileInfo.fileIndex,"~1MP4");
		start = pGos_indexList->startCluster;
		CreateLongFileItem(&dir_entry,shortname,longName,start,MP4_MAX_LENTH,gHeadIndex.ClusterSize,0x20);
	}
	else if(pGos_indexList->fileInfo.fileType == RECORD_FILE_JPG)
	{
		sprintf(shortname,"%06d%s",pGos_indexList->fileInfo.fileIndex,"~1JPG");
		start = pGos_indexList->startCluster;
		CreateLongFileItem(&dir_entry,shortname,longName,start,JPEG_MAX_LENTH,gHeadIndex.ClusterSize,0x20);
	}
	else
	{
		return -1;
	}
#endif
	
	dir_entry.dir_entry.time = pGos_indexList->fileInfo.recordStartTime;
	dir_entry.dir_entry.date = pGos_indexList->fileInfo.recordStartDate;
	dir_entry.dir_entry.ctime = dir_entry.dir_entry.time;
	dir_entry.dir_entry.cdate = dir_entry.dir_entry.date;
	dir_entry.dir_entry.adate = dir_entry.dir_entry.date;
	dir_entry.dir_entry.starthi = CT_LE_W(pGos_indexList->startCluster>>16);
	dir_entry.dir_entry.start = CT_LE_W(pGos_indexList->startCluster&0xffff);
	dir_entry.dir_entry.size = pGos_indexList->fileInfo.fileSize;

	Storage_Lock();
	fatOffset = pGos_indexList->DirSectorsNum * DEFAULT_SECTOR_SIZE + pGos_indexList->DirSectorsEA;
	lseek64(fpart,fatOffset,SEEK_SET);
	if(write(fpart,&dir_entry,sizeof(long_msdos_dir_entry)) != sizeof(long_msdos_dir_entry))
	{
		Storage_Unlock();
		return -1;
	}
	Storage_Unlock();
	
	//更新索引
	pGos_indexList->fileInfo.filestate = NON_EMPTY_OK;
	gHeadIndex.CurrIndexPos = pGos_indexList->fileInfo.fileIndex;
#if 0
	if(gHeadIndex.mDateListCounts == 0)
	{
		gHeadIndex.mDateList[gHeadIndex.mDateListCounts].mDate 
			= pGos_indexList->fileInfo.recordStartDate;
		gHeadIndex.mDateList[gHeadIndex.mDateListCounts].mCounts ++;
		gHeadIndex.mDateListCounts ++;
	}
	else
	{
		for(lAviCount = 0;lAviCount < gHeadIndex.mDateListCounts;lAviCount++)
		{
			if(gHeadIndex.mDateList[lAviCount].mDate == pGos_indexList->fileInfo.recordStartDate)
			{
				gHeadIndex.mDateList[lAviCount].mCounts ++;
				break;
			}
		}
		if(lAviCount == gHeadIndex.mDateListCounts)
		{
			gHeadIndex.mDateList[gHeadIndex.mDateListCounts].mDate 
				= pGos_indexList->fileInfo.recordStartDate;
			gHeadIndex.mDateList[gHeadIndex.mDateListCounts].mCounts ++;
			gHeadIndex.mDateListCounts ++;
		}
	}
	

	struct DateList *pDateList = gDateList;
	if(gHeadIndex.DateListCount == 0)
	{
		pDateList->mDate = pGos_indexList->fileInfo.recordStartDate;
		pDateList->mCounts ++;
		gHeadIndex.DateListCount++;
	}
	else
	{
		for(lAviCount = 0;lAviCount < gHeadIndex.DateListCount;lAviCount++)
		{
			if(pDateList->mDate == pGos_indexList->fileInfo.recordStartDate)
			{
				pDateList->mCounts ++;
				break;
			}
			pDateList++;
		}
		if(lAviCount == gHeadIndex.DateListCount)
		{
			pDateList->mDate = pGos_indexList->fileInfo.recordStartDate;
			pDateList->mCounts ++;
			gHeadIndex.DateListCount++;	
		}
	}
	//循环录像时必须将之前的那天总数减一,新的日期加一
	int i;
	unsigned short int oldDate = 0;
	if(FileType == RECORD_FILE_MP4 && gMp4OldDate)
	{
		oldDate = gMp4OldDate;
	}
	else if(FileType == RECORD_FILE_JPG && gJpegOldDate)
	{
		oldDate = gJpegOldDate;
	}

	if(oldDate)
	{
		struct DateList *pDateList = gDateList;
		for(i = 0;i < gHeadIndex.DateListCount;i++)
		{
			if(pDateList->mDate == oldDate) 
			{
				pDateList->mCounts--;
				if(pDateList->mCounts < 1)
				{
					pDateList->mCounts = gDateList[gHeadIndex.DateListCount-1].mCounts;
					pDateList->mDate = gDateList[gHeadIndex.DateListCount-1].mDate;
					gHeadIndex.DateListCount--;
				}
				break;
			}
			pDateList++;
		}
	}
#endif 
	Storage_Write_gos_index(fpart,FileType);
	sync();
	
	return 0;
}

//读磁盘数据
int Storage_Read(char* Fileindex,int offset,void *data,int dataSize,int fpart)
{
	if(RemoveSdFlag == 1)
	{
		return -1;
	}

#if CLOUD_STORAGE
	if( NULL == gAVIndexList || Fileindex == NULL)
	{
		printf("error !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
		return -1;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList)
	{
		return -1;
	}
#endif
	struct GosIndex *pGos_indexList;
	int lAviCount;
	int IndexSum;
	int datalen;
	int retlen = 0;

#if CLOUD_STORAGE
	pGos_indexList = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 0;
#else
	if(Fileindex < gHeadIndex.lRootDirFileNum)
	{
		pGos_indexList = gMp4IndexList;
		IndexSum = gHeadIndex.lRootDirFileNum;
		lAviCount = 0;
	}
	else
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
	}
#endif
	
	/*for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.fileIndex == Fileindex)
		{
			break;
		}
		pGos_indexList++;
	}
	if(lAviCount == IndexSum) //没有找到对应的扇区地址
	{
		return -1;
	}*/
	pGos_indexList = (GosIndex*)Fileindex;
	//连续读
	#if 0
	if(pGos_indexList->fileInfo.FileFpart == 0)
	{
		pGos_indexList->DataSectorsEA = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;
	}
	pGos_indexList->fileInfo.FileFpart = 1;
	#endif
	if(offset >= pGos_indexList->fileInfo.fileSize)
	{
		printf("offset >= pGos_indexList->fileInfo.fileSize##########\n");
		return 0;
	}
	unsigned long long DataSectorsEA  = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE + offset;
	lseek64(fpart,DataSectorsEA,SEEK_SET); 
	
	datalen = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;
	unsigned long all_read_size = DataSectorsEA - datalen;
	unsigned long remain_size = pGos_indexList->fileInfo.fileSize - all_read_size;
	//printf("pGos_indexList->fileInfo.fileSize = %d, all_read_size = %d, remain_size = %d ,dataSize = %d\n",pGos_indexList->fileInfo.fileSize,
	//	all_read_size,remain_size,dataSize);
	if(remain_size == 0)
	{
		printf("file is read over.\n");
		return 0;
	}
	if(remain_size < (unsigned long)dataSize)
	{
		//文件读完
		retlen = read(fpart,(char *)data,remain_size);
		//pGos_indexList->DataSectorsEA = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;
	}
	else
	{
		if(all_read_size > pGos_indexList->fileInfo.fileSize)
		{
			//文件读完
			int len = pGos_indexList->fileInfo.fileSize - (DataSectorsEA - datalen - dataSize);
			retlen = read(fpart,(char *)data,len);
			//pGos_indexList->DataSectorsEA = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;		
		}
		else
		{
			retlen = read(fpart,(char *)data,dataSize);
			//pGos_indexList->DataSectorsEA += dataSize;
		}		
	}
	
	//pGos_indexList->DataSectorsEA += retlen;
	return retlen;
}

//写磁盘数据
int Storage_Write(char* Fileindex,const void *data,unsigned int dataSize,int fpart)
{
	if(RemoveSdFlag == 1)
	{
		return -1;
	}

	if(Fileindex == NULL)
	{
		printf("error!! Fileindex is null\n");
		return -1;
	}
	int lAviCount;
	unsigned int nlen = 0;
	unsigned int fileoffset = 0;
	int IndexSum;
	int MaxFileSize = 0;

	//根据索引值找到对应文件的磁盘位置,然后写入
	
	struct GosIndex *pGos_indexList;

#if CLOUD_STORAGE
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		printf("Storage_Write:line[%d] error gAVIndexList == NULL or sd remove\n",__LINE__);
		return -1;
	}
	pGos_indexList = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 0;
	MaxFileSize = FILE_MAX_LENTH;
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
	if(Fileindex < gHeadIndex.lRootDirFileNum)
	{
		pGos_indexList = gMp4IndexList;
		IndexSum = gHeadIndex.lRootDirFileNum;
		lAviCount = 0;
		MaxFileSize = MP4_MAX_LENTH;
	}
	else
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
		MaxFileSize = MP4_MAX_LENTH;
	}
#endif
	/*
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.fileIndex == Fileindex)
		{
			break;
		}
		pGos_indexList++;
	}
	if(lAviCount == IndexSum) //没有找到对应的扇区地址
	{
		return -1;
	}*/
	if(gAVIndexList == NULL)
	{
		printf("Storage_Write:line[%d] error gAVIndexList == NULL\n",__LINE__);
		return -1;
	}
	
	pGos_indexList = (GosIndex*)Fileindex;
	//连续写入
	if(pGos_indexList->fileInfo.FileFpart == 0)
	{
		pGos_indexList->fileInfo.fileSize = 0;
		pGos_indexList->DataSectorsEA = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;
	}
	pGos_indexList->fileInfo.FileFpart = 1;
	
	//文件的长度不能超过预分配给每个文件的大小 
	if((pGos_indexList->fileInfo.fileSize + dataSize) > MaxFileSize)
	{
		printf("Storage_Write:line[%d] error fileSize > maxfilesize\n",__LINE__);
		return -1;
	}

	if(gAVIndexList == NULL)
	{
		printf("Storage_Write:line[%d] error gAVIndexList == NULL\n",__LINE__);
		return -1;
	}

	//加锁
	Storage_Lock();
	//printf("pGos_indexList->DataSectorsEA=%llu\n",pGos_indexList->DataSectorsEA);
	lseek64(fpart,pGos_indexList->DataSectorsEA,SEEK_SET);
	nlen = write(fpart,(char *)data,dataSize);
	Storage_Unlock();

	
	if(gAVIndexList == NULL)
	{
		printf("Storage_Write:line[%d] error gAVIndexList == NULL\n",__LINE__);
		return -1;
	}

	if(nlen < 0)
	{
		printf("Storage_Write:line[%d] error nlen = %d\n",__LINE__,nlen);
		return nlen;
	}
	pGos_indexList->DataSectorsEA += nlen;

	unsigned long long beyondSize;
	beyondSize = pGos_indexList->DataSectorsEA - pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;
	if(beyondSize > pGos_indexList->fileInfo.fileSize)
	{
		pGos_indexList->fileInfo.fileSize += (beyondSize - pGos_indexList->fileInfo.fileSize); 
	}

	//printf("pGos_index->fileInfo.fileSize =%lu\n",pGos_indexList->fileInfo.fileSize);
	//sync();

	int max_len = MaxFileSize - 512 * 1024;
	if(max_len <= 0)
	{
		printf("Storage_Write:line[%d] error max_len <= 0\n",__LINE__);
		return -1;
	}
	if((pGos_indexList->fileInfo.fileSize + dataSize) > max_len)
	{
		setMaxWriteSize(1);
	}

	pGos_indexList = NULL;
	return nlen;
}

long long storage_Lseek(int Fileindex,unsigned int offset,unsigned int whence,int fpart)
{
	if(RemoveSdFlag == 1)
	{
		return -1;
	}

#if CLOUD_STORAGE
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
	{
		return -1;
	}
#endif
	
	int lAviCount;
	int IndexSum;
	unsigned long long DataEA;
	struct GosIndex *pGos_indexList;

#if CLOUD_STORAGE
	pGos_indexList = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 0;
#else
	if(Fileindex < gHeadIndex.lRootDirFileNum)
	{
		pGos_indexList = gMp4IndexList;
		IndexSum = gHeadIndex.lRootDirFileNum;
		lAviCount = 0;
	}
	else
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
	}
#endif

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.fileIndex == Fileindex)
		{
			break;
		}
		pGos_indexList++;
	}
	if(lAviCount == IndexSum) //没有找到对应的扇区地址
	{
		return -1;
	}	

    switch(whence)
    {
        case 0: 
		{
            DataEA = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE + offset;
            break;
		}
        case 1: 
		{
            DataEA = pGos_indexList->DataSectorsEA + offset;
            break;
		}
        case 2: 
		{
            DataEA = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE + 
				pGos_indexList->fileInfo.fileSize - offset;
            break;
		}
        default:
		{
			return -1;
		}          
   }

	pGos_indexList->DataSectorsEA = DataEA;
	//printf("offset64=%llu offset=%u\n",pGos_indexList->DataSectorsEA,offset);
	//文件偏移不能超过预分配给每个文件的大小
	if(offset > MP4_MAX_LENTH || offset > pGos_indexList->fileInfo.fileSize)
	{
		return -1;	
	}
	
	return lseek64(fpart,pGos_indexList->DataSectorsEA,whence);	
}

//获取某月录像事件列表 (即获取SD卡中该月有哪些天是有录像的)
char *sGetMonthEventList(char *sMonthEventList)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}

	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	char sList[32] = {0};
	struct GosIndex *pGos_indexList = NULL;

#if CLOUD_STORAGE
	if(NULL == gAVIndexList || NULL == sMonthEventList || (!StorageCheckSDExist()))
	{
		return NULL;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || NULL == sMonthEventList || (!StorageCheckSDExist()))
	{
		return NULL;
	}
#endif
	
	DateList mDateList[DateListMax];
	int mDateListCounts = 0;
	memset(&mDateList,0,sizeof(DateList)*DateListMax);


#if CLOUD_STORAGE
	lAviCount = 1;
	int i = 0;
	pGos_indexList = &gAVIndexList[1];
	IndexSum = gHeadIndex.lRootDirFileNum;	

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{

			if(mDateListCounts == 0)
			{
				mDateList[mDateListCounts].mDate = pGos_indexList->fileInfo.recordStartDate;
				mDateList[mDateListCounts].mCounts ++;
				mDateListCounts ++;
			}
			else
			{
				for(i = 0;i < mDateListCounts;i++)
				{
					if(mDateList[i].mDate == pGos_indexList->fileInfo.recordStartDate)
					{
						mDateList[i].mCounts ++;
						break;
					}
				}
				if(i == mDateListCounts)
				{
					mDateList[i].mDate = pGos_indexList->fileInfo.recordStartDate;
					mDateList[i].mCounts ++;
					mDateListCounts ++;
				}
			}
		}	
		pGos_indexList++;
	}
#else
	//遍历mp4	
	lAviCount = 1;
	int i = 0;
	pGos_indexList = &gMp4IndexList[1];
	IndexSum = gHeadIndex.lRootDirFileNum;	

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{

			if(mDateListCounts == 0)
			{
				mDateList[mDateListCounts].mDate = pGos_indexList->fileInfo.recordStartDate;
				mDateList[mDateListCounts].mCounts ++;
				mDateListCounts ++;
			}
			else
			{
				for(i = 0;i < mDateListCounts;i++)
				{
					if(mDateList[i].mDate == pGos_indexList->fileInfo.recordStartDate)
					{
						mDateList[i].mCounts ++;
						break;
					}
				}
				if(i == mDateListCounts)
				{
					mDateList[i].mDate = pGos_indexList->fileInfo.recordStartDate;
					mDateList[i].mCounts ++;
					mDateListCounts ++;
				}
			}
		}	
		pGos_indexList++;
	}

	//遍历jpeg
	pGos_indexList = &gJpegIndexList[0];
	IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
	lAviCount = gHeadIndex.lRootDirFileNum;

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{	
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{

			if(mDateListCounts == 0)
			{
				mDateList[mDateListCounts].mDate = pGos_indexList->fileInfo.recordStartDate;
				mDateList[mDateListCounts].mCounts ++;
				mDateListCounts ++;
			}
			else
			{
				for(i = 0;i < mDateListCounts;i++)
				{
					if(mDateList[i].mDate == pGos_indexList->fileInfo.recordStartDate)
					{
						mDateList[i].mCounts ++;
						break;
					}
				}
				if(i == mDateListCounts)
				{
					mDateList[i].mDate = pGos_indexList->fileInfo.recordStartDate;
					mDateList[i].mCounts ++;
					mDateListCounts ++;
				}
			}
		
		}	
		pGos_indexList++;
	}
#endif

	for(lAviCount = 0;lAviCount < mDateListCounts;lAviCount++)
	{
		tm_year = (mDateList[lAviCount].mDate >> 9) & 0x7F;
		tm_mon  = (mDateList[lAviCount].mDate >> 5) & 0x0F;
		tm_mday  = mDateList[lAviCount].mDate & 0x1F;

		memset(sList,0,sizeof(sList));
		sprintf(sList,"%04d%02d%02d%04hu|",(tm_year+1980),tm_mon,tm_mday,mDateList[lAviCount].mCounts);
		strcat(sMonthEventList,sList);
	}

	return sMonthEventList;
}

//获取某天录像事件列表
char *sGetDayEventList(const char *date, unsigned int file_type, char *sDayEventList, int NMaxLength, unsigned int *filecounts)
{
#if CLOUD_STORAGE
	if(NULL == date || NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		return NULL;
	}
#else
    if(NULL == date || NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
    {
        return NULL;
    }
#endif
	
	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	unsigned short int recordDate;
	char FileName[32] = {0};
	char Item[64] = {0};
	int  counts = 0;
	float fileSize;
	enum RECORD_FILE_TYPE FileType;
	struct GosIndex *pGos_indexList;
	
	sscanf(date,"%04d%02d%02d",&tm_year,&tm_mon,&tm_mday);
	recordDate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 

	//请求文件类型 (视频: 0 , 图片: 1)
#if CLOUD_STORAGE
	pGos_indexList = gAVIndexList; 
	IndexSum = gHeadIndex.lRootDirFileNum;	
	lAviCount = 0;	
	FileType = RECORD_FILE_H264; 
#else
	GetListInit();
#endif

	int i = 0;
	int *pList = (int *)malloc(10240*sizeof(int));
	memset(pList,0,10240*sizeof(int));
	int *ptmp = pList;
	
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.recordStartDate == recordDate
			&&pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{	
			*ptmp = pGos_indexList->fileInfo.recordStartTime;
			ptmp++;
			counts++;
		}
		pGos_indexList++;
	}

	QuickSort(pList,0,counts-1);

	ptmp = &pList[0];
	for(i = 0; i< counts; i++)
	{
	#if CLOUD_STORAGE
		pGos_indexList = gAVIndexList; 
		IndexSum = gHeadIndex.lRootDirFileNum;	
		lAviCount = 0;	
		FileType = RECORD_FILE_H264; 
	#else
		GetListInit();
	#endif
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(pGos_indexList->fileInfo.recordStartTime == *ptmp)
			{
				tm_hour = (pGos_indexList->fileInfo.recordStartTime >> 11) & 0x1F;
				tm_min  = (pGos_indexList->fileInfo.recordStartTime >> 5) & 0x3F;
				tm_sec  = pGos_indexList->fileInfo.recordStartTime & 0x1F;
				
				sprintf(FileName,"%s%02d%02d%02d0%c%s",date,
				tm_hour,tm_min,2*tm_sec,/*pGos_indexList->fileInfo.recordType+'a',*/
				pGos_indexList->fileInfo.alarmType+'a',
				/*pGos_indexList->fileInfo.recordDuration,*/pFileTypeString[FileType]);

				fileSize = (float)pGos_indexList->fileInfo.fileSize;

			#if !CLOUD_STORAGE
				if(pGos_indexList->fileInfo.fileType == RECORD_FILE_MP4)
				{
					sprintf(Item,"%s@%6.2f|",FileName,fileSize/(float)(1024*1024));
				}
				else if(pGos_indexList->fileInfo.fileType == RECORD_FILE_JPG)
				{
					sprintf(Item,"%s@%6.1f|",FileName,fileSize/(float)1024);
				}
			#else
				sprintf(Item,"%s@%6.2f|",FileName,fileSize/(float)(1024*1024));
			#endif
				strncat(sDayEventList,Item,strlen(Item));
				break;
			}
			pGos_indexList++;
		}		
		ptmp++;
	}

	free(pList);
	pList = NULL;
	ptmp = NULL;
	*filecounts = counts;
	NMaxLength = strlen(sDayEventList);	
	return sDayEventList;
}

/*
filename:指定的文件名(***.mp4)
sDayEventList:查找到的串
direction:查找方向,0:向上; 1:向下;
filecounts:查找数量
return:返回实际数量
*/
int sGetDayAssignTimeEventList(const char *filename, char *sDayEventList,int direction,unsigned int filecounts)
{
	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	unsigned short int recordDate;
	char FileName[32] = {0};
	char Item[64] = {0};
	int  counts = 0;
	float fileSize;
	enum RECORD_FILE_TYPE FileType;
	struct GosIndex *pGos_indexList;
	char zero;
	//char recordtype;
	char alarmtype;
	//int fileduration;
	char filetype[32] = {0};
	int rltCount = 0;
	unsigned int file_type;
	
#if CLOUD_STORAGE
	if(NULL == filename ||gAVIndexList == NULL || (!StorageCheckSDExist()))
	{
        return 0;
	}
	sscanf(filename,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,/*&fileduration,*/filetype);
#else
    if(NULL == filename || NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
    {
        return 0;
    }
	
	sscanf(filename,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,/*&fileduration,*/filetype);
#endif

	recordDate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
	int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;

#if CLOUD_STORAGE
	file_type = 0;
	
	pGos_indexList = gAVIndexList; 
	IndexSum = gHeadIndex.lRootDirFileNum;	
	lAviCount = 0;	
	FileType = RECORD_FILE_H264; 
#else
	//请求文件类型 (视频: 0 , 图片: 1)
	if(strncmp(filetype,pFileTypeString[RECORD_FILE_MP4],strlen(filetype))==0)
	{
		file_type = 0;
	}
	else if(strncmp(filetype,pFileTypeString[RECORD_FILE_JPG],strlen(filetype))==0)
	{
		file_type = 1;
	}
	else
	{
		return 0;
	}
	
	GetListInit();
#endif
	
	int i = 0;
	int *pList = (int *)malloc(10240*sizeof(int));
	int *pDirectionList = (int *)malloc(filecounts*sizeof(int));

	memset(pList,0,10240*sizeof(int));
	memset(pDirectionList,0,filecounts*sizeof(int));
	
	int *ptmp = pList;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.recordStartDate == recordDate
			&&pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{	
			*ptmp = pGos_indexList->fileInfo.recordStartTime;
			ptmp++;
			counts++;
		}
		pGos_indexList++;
	}
	
	QuickSort(pList,0,counts-1);
	
	ptmp = &pList[0];
	int *pdirection = &pDirectionList[0];

	if(direction == 0)
	{
		for(i = 0; i< counts; i++)
		{
			if(*ptmp == ltime)
				break;
			ptmp++;
			rltCount++;
		}
		if(rltCount>0&&rltCount<=filecounts)
		{
			ptmp--;
			for(i = 0; i< rltCount; i++)
			{
				*pdirection = *ptmp;
				pdirection++;
				ptmp--;
			}		
		}
		else if(rltCount>filecounts)
		{
			ptmp--;
			for(i = 0; i< rltCount; i++)
			{
				if(i==filecounts)
					break;
				*pdirection = *ptmp;
				pdirection++;
				ptmp--;
			}
			rltCount = filecounts;
		}
		else
		{
			free(pList);
			free(pDirectionList);
			return 0;
		}
	}
	else if(direction == 1)
	{
		for(i = 0; i< counts; i++)
		{
			if(*ptmp == ltime)
				break;
			ptmp++;
			rltCount++;
		}
		if((counts - rltCount -1) >= filecounts)
		{
			ptmp++;
			for(i = rltCount; i< counts; i++)
			{
				*pdirection = *ptmp;
				ptmp ++;
				pdirection ++;
				if((i - rltCount) == filecounts)
					break;
			}
			rltCount = filecounts;
		}
	    else if((counts - rltCount -1) < filecounts)
	    {
			ptmp++;
			for(i = rltCount; i< counts; i++)
			{
				*pdirection = *ptmp;
				ptmp ++;
				pdirection ++;
			}
			rltCount = i - rltCount-1;
		}
	}
	else
	{
		free(pList);
		free(pDirectionList);
		return 0;
	}

	QuickSort(pDirectionList,0,rltCount-1);
	
	ptmp = &pDirectionList[0];
	for(i = 0; i< rltCount; i++)
	{
	#if CLOUD_STORAGE
		pGos_indexList = gAVIndexList; 
		IndexSum = gHeadIndex.lRootDirFileNum;	
		lAviCount = 0;	
		FileType = RECORD_FILE_H264; 
	#else
		GetListInit();
	#endif
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(pGos_indexList->fileInfo.recordStartTime == *ptmp)
			{
				tm_hour = (pGos_indexList->fileInfo.recordStartTime >> 11) & 0x1F;
				tm_min  = (pGos_indexList->fileInfo.recordStartTime >> 5) & 0x3F;
				tm_sec  = pGos_indexList->fileInfo.recordStartTime & 0x1F;
				
				sprintf(FileName,"%04d%02d%02d%02d%02d%02d0%c%s",tm_year,tm_mon,tm_mday,
				tm_hour,tm_min,2*tm_sec,/*pGos_indexList->fileInfo.recordType+'a',*/
				pGos_indexList->fileInfo.alarmType+'a',
				/*pGos_indexList->fileInfo.recordDuration,*/pFileTypeString[FileType]);

				fileSize = (float)pGos_indexList->fileInfo.fileSize;

#if !CLOUD_STORAGE
				if(pGos_indexList->fileInfo.fileType == RECORD_FILE_MP4)
				{
					sprintf(Item,"%s@%6.2f|",FileName,fileSize/(float)(1024*1024));
				}
				else if(pGos_indexList->fileInfo.fileType == RECORD_FILE_JPG)
				{
					sprintf(Item,"%s@%6.1f|",FileName,fileSize/(float)1024);
				}
#else
				sprintf(Item,"%s@%6.1f|",FileName,fileSize/(float)(1024*1024));
#endif
				strncat(sDayEventList,Item,strlen(Item));
				break;
			}
			pGos_indexList++;
		}		
		ptmp++;
	}

	free(pList);
	free(pDirectionList);
	pList = NULL;
	pDirectionList = NULL;
	ptmp = NULL;
	pdirection = NULL;
	
	//*filecounts = counts;
	//NMaxLength = strlen(sDayEventList);	
	
	return rltCount;
}

//开始下载指定录像文件(获取录像文件路径)
char *sGetRecordFullName(const char *sFileName, char *sFullName)
{
	if(NULL == sFileName || NULL == gMp4IndexList || NULL == gJpegIndexList 
		|| NULL == sFullName || (!StorageCheckSDExist()))
	{
		return NULL;
	}
	
	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char zero;
	//char recordtype;
	char alarmtype;
	//int fileduration;
	char filetype[32] = {0};
	char sDir[256] = {0};
	enum RECORD_FILE_TYPE FileType;
	struct GosIndex *pGos_indexList;
	
	sscanf(sFileName,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,/*&fileduration,*/filetype);

	int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec/ 2;
	int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
	
	if(strncmp(filetype,pFileTypeString[RECORD_FILE_MP4],strlen(filetype))==0)
	{
		pGos_indexList = &gMp4IndexList[1];
		IndexSum = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_MP4;
		strcpy(sDir,SDDirName);
		lAviCount = 1;	
	}
	else if(strncmp(filetype,pFileTypeString[RECORD_FILE_JPG],strlen(filetype))==0)
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
		FileType = RECORD_FILE_JPG;
		strcpy(sDir,SDMOUNTPOINT);
	}
	else
	{
		return NULL;
	}
	
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.recordStartDate == (ldate&0xFFFF)
			&&pGos_indexList->fileInfo.recordStartTime == (ltime&0xFFFF))
		{
			#if LONG_MSDOS_DIR_SUPPORT	
				sprintf(sFullName,"%s/%s",sDir,sFileName);
				return sFullName;
			#else
			    sprintf(sFullName,"%s/%08d%s",sDir,pGos_indexList->fileInfo.fileIndex,pFileTypeString[FileType]);
				return sFullName;
			#endif
		}
		pGos_indexList++;
	}	

    return NULL;
}

int sDelRecord(const char *sFileName)
{
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || NULL == sFileName || (!StorageCheckSDExist()))
	{
		return -1;
	}
	
	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char zero;
	//char recordtype;
	char alarmtype;
	//int fileduration;
	char filetype[32] = {0};
	struct GosIndex *pGos_indexList;
	
	sscanf(sFileName,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,/*&fileduration,*/filetype);

	int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;
	int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
	
	if(strncmp(filetype,pFileTypeString[RECORD_FILE_MP4],strlen(filetype))==0)
	{
		pGos_indexList = &gMp4IndexList[1];
		IndexSum = gHeadIndex.lRootDirFileNum;
		lAviCount = 1;	
	}
	else if(strncmp(filetype,pFileTypeString[RECORD_FILE_JPG],strlen(filetype))==0)
	{
		pGos_indexList = gJpegIndexList;
		IndexSum = gHeadIndex.lJpegFileNum + gHeadIndex.lRootDirFileNum;
		lAviCount = gHeadIndex.lRootDirFileNum;
	}
	else
	{
		return -1;
	}
	
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.recordStartDate == (ldate&0xFFFF)
			&&pGos_indexList->fileInfo.recordStartTime== (ltime&0xFFFF))
		{
			return DelectStorageFile(pGos_indexList->fileInfo.fileIndex);	 
		}
		pGos_indexList++;
	}

	return -1;
}

int getMaxWriteSize()
{
	if(MaxWriteSize)
		return 1;
	else
		return 0;
}

int setMaxWriteSize(int prams)
{
	MaxWriteSize = prams;
}

//预防子目录变成不可写
void ChildDirSureSDCanWrite()
{
	//char fileName[64] = {0};
	//sprintf(fileName,"%d/wtest",SDMOUNTPOINT);
    //FILE *tmpFp = fopen(fileName,"wb"); 
    //if(!tmpFp)
    //{
    #if 0
    	if(gHeadIndex.FlagIndexHead != FLAG_INDEX_HEAD)
			return;
		//恢复子目录,写子目录簇链
		int lAviCount;
		unsigned int lStartClu = gHeadIndex.ChildStartCluster;
		unsigned int lFileNum = gHeadIndex.lJpegFileNum;
		unsigned long long offset = gHeadIndex.ChildClusterListEA+4;
		
		//子目录所占的簇数
		unsigned long lClustersofRoot = (lFileNum+1)*32/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)+1;
		Storage_Lock();
		lseek64(fPart,offset,SEEK_SET);
		
		for(lAviCount = 0;lAviCount < lClustersofRoot-1;lAviCount++)
		{
			lStartClu++;
			write(fPart,&lStartClu, 4);
		}
		int flag = 0xFFFFFFFF;
		write(fPart,&flag, 4);  
		Storage_Unlock();
		//printf("offset=%llu lFileNum=%d\n",offset,lFileNum);
		sync();
    //} 
   // else
   // {   
   //     fclose(tmpFp);
   //     tmpFp = NULL;
   //     usleep(100*1000);
	//	char command[64] = {0};
	//	sprintf(command,"rm -rf %s",fileName);
   //     system(command);
   // }  
   #endif
    return;
}

//可用容量
unsigned int GetDiskInfo_Usable()
{
	if(RemoveSdFlag == 1)
	{
		return 0;
	}
#if CLOUD_STORAGE
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		return 0;
	}
#else
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || (!StorageCheckSDExist()))
	{
		return 0;
	}
#endif
	int IndexSum;
	int RootDirFileUseCounts = 0;
	int JpegFileUseCounts = 0;
	struct GosIndex *pGos_indexList;

#if CLOUD_STORAGE
	int lAviCount = 1;
	pGos_indexList = gAVIndexList;
	for(lAviCount;lAviCount < gHeadIndex.lRootDirFileNum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			RootDirFileUseCounts++;
		}
		pGos_indexList++;
	}
	
	unsigned int UsableRootM = FILE_MAX_LENTH / (1024*1024);

	if(FILE_MAX_LENTH % (1024*1024) != 0)
	{
		UsableRootM += 1;
	}
	
	unsigned int Usable = (gHeadIndex.lRootDirFileNum - RootDirFileUseCounts - 1) * UsableRootM;
#else
	int lAviCount = 1;
	pGos_indexList = gMp4IndexList;
	for(lAviCount;lAviCount < gHeadIndex.lRootDirFileNum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			RootDirFileUseCounts++;
		}
		pGos_indexList++;
	}
	
	lAviCount = gHeadIndex.lRootDirFileNum;
	pGos_indexList = gJpegIndexList;
	for(lAviCount;lAviCount < gHeadIndex.lRootDirFileNum + gHeadIndex.lJpegFileNum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			JpegFileUseCounts++;
		}
		pGos_indexList++;
	}	
	pGos_indexList = NULL;
	
	unsigned int UsableRootM = MP4_MAX_LENTH / (1024*1024);

	if(MP4_MAX_LENTH % (1024*1024) != 0)
	{
		UsableRootM += 1;
	}

	unsigned int UsableJpegM = JPEG_MAX_LENTH / 1024;

	if(JPEG_MAX_LENTH % 1024 != 0)
	{
		UsableJpegM += 1;
	}

	UsableJpegM = (gHeadIndex.lJpegFileNum - JpegFileUseCounts) * UsableJpegM / 1024;
		
	if((gHeadIndex.lJpegFileNum - JpegFileUseCounts) * UsableJpegM % 1024 != 0)
	{
		UsableJpegM += 1;
	}
	unsigned int Usable = (gHeadIndex.lRootDirFileNum - RootDirFileUseCounts - 1) * UsableRootM \
		+ UsableJpegM;
	
#endif
	return Usable;  //M
}

char* Mux_open(const char *fileName)
{
	return storage_Open(fileName);
}

int Mux_close(char* Fileindex,char *fileName)
{
	return Storage_Close(Fileindex,fileName,fPart);
}

int Mux_write(char* Fileindex,const void *data,unsigned int dataSize)
{
	return 	Storage_Write(Fileindex,data,dataSize,fPart);
}

int Mux_read(char* Fileindex,int offset,void *data,unsigned int dataSize)
{
	return Storage_Read(Fileindex,offset,data,dataSize,fPart);
}

int Mux_Print_fd_time()
{
	GosIndex * tmpIndex = NULL;
	int lAviCount = 1;
	int IndexSum = 0;

	if(gAVIndexList == NULL || (!StorageCheckSDExist()))
	{
		return -1;
	}
	tmpIndex = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(tmpIndex->fileInfo.filestate == NON_EMPTY_OK)
		{
			printf("fd:%d  start: %d   end : %d  alarmType : %d\n",tmpIndex->fileInfo.fileIndex,tmpIndex->fileInfo.recordStartTimeStamp,
				tmpIndex->fileInfo.recordEndTimeStamp,tmpIndex->fileInfo.alarmType);
		}
		tmpIndex++;
	}
	return 0;
}

int Mux_Get_Sd_Remove_Flag()
{
	return RemoveSdFlag;		
}

int Mux_Get_Sd_Format_Flag()
{
	return FormatSdFlag;		
}

//fd==NULL 表示第一次调用，从头开始查，time_str生效
char* Mux_GetFdFromTime(char* lastFd,char *filename,unsigned int timestamp,unsigned int *time_lag,unsigned int *first_timestamp)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}
	GosIndex * GetIndex = NULL;
	GosIndex * FirstIndex = NULL;
	GosIndex * tmpIndex = NULL;
	GosIndex * nextIndex = NULL;
	//int ret_fd = 0;;

	if(gAVIndexList == NULL || filename == NULL || (!StorageCheckSDExist()))
	{
		return NULL;
	}

	if(lastFd == NULL)
	{	
		int lAviCount = 1;
		int IndexSum = 0;;
		IndexSum = gHeadIndex.lRootDirFileNum;
		FirstIndex = &gAVIndexList[1];
		
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(FirstIndex->fileInfo.filestate == NON_EMPTY_OK)
			{
				if(timestamp >= FirstIndex->fileInfo.recordStartTimeStamp && timestamp < FirstIndex->fileInfo.recordEndTimeStamp)
				{
					break;
				}
			}
			FirstIndex++;
		}
		
		if(lAviCount == IndexSum)
		{
			return NULL;
		}
		printf("got it ,fd: %d, start: %d, end: %d\n",FirstIndex->fileInfo.fileIndex,FirstIndex->fileInfo.recordStartTimeStamp,FirstIndex->fileInfo.recordEndTimeStamp);
		if(time_lag != NULL)
			*time_lag = FirstIndex->fileInfo.recordEndTimeStamp - FirstIndex->fileInfo.recordStartTimeStamp;
		if(first_timestamp != NULL)
			*first_timestamp = FirstIndex->fileInfo.recordStartTimeStamp;

		int tm_hour = (FirstIndex->fileInfo.recordStartTime >> 11) & 0x1F;
		int tm_min  = (FirstIndex->fileInfo.recordStartTime >> 5) & 0x3F;
		int tm_sec  = FirstIndex->fileInfo.recordStartTime & 0x1F;

		int tm_year = (FirstIndex->fileInfo.recordStartDate >> 9) & 0x7F;
		int tm_mon  = (FirstIndex->fileInfo.recordStartDate >> 5) & 0x0F;
		int tm_mday  = FirstIndex->fileInfo.recordStartDate & 0x1F;	

		sprintf(filename,"%04d%02d%02d%02d%02d%02d0%c.H264",tm_year+1980,tm_mon,tm_mday,tm_hour,tm_min,tm_sec*2,FirstIndex->fileInfo.alarmType+'a');
		printf("filename--------------->%s\n",filename);			
		return (char*)FirstIndex;
	}
	else
	{
		GetIndex = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(GetIndex == NULL)
		{
			return NULL;
		}
		tmpIndex = GetIndex;
		if(GetIndex == &gAVIndexList[gHeadIndex.lRootDirFileNum-1])
		{
			GetIndex = &gAVIndexList[1];
			//printf("============= %d =============\n",GetIndex->fileInfo.fileIndex);
			//printf("time : %d, %d\n",GetIndex->fileInfo.recordStartTimeStamp,tmpIndex->fileInfo.recordEndTimeStamp);
		}
		else 
		{
			GetIndex++;
		}

		if(GetIndex->fileInfo.filestate != NON_EMPTY_OK)
			return NULL;
		
		while(GetIndex->fileInfo.recordStartTimeStamp < tmpIndex->fileInfo.recordEndTimeStamp)
		{
			//ret_fd = 0;
			if(GetIndex == Get_Oldest_file())
			{
				return NULL;
			}
			printf("skip curr indext, return next indext.\n");
			tmpIndex = GetIndex;
			if(GetIndex == &gAVIndexList[gHeadIndex.lRootDirFileNum-1])
			{
				GetIndex = &gAVIndexList[1];
			}
			else
			{
				GetIndex++;
			}
		}
		
		if(time_lag != NULL)
		{
			*time_lag = GetIndex->fileInfo.recordEndTimeStamp - GetIndex->fileInfo.recordStartTimeStamp;
			printf("time lag ------- > %d\n",*time_lag);
		}
		if(first_timestamp != NULL)
			*first_timestamp = GetIndex->fileInfo.recordStartTimeStamp;
		
		int tm_hour = (GetIndex->fileInfo.recordStartTime >> 11) & 0x1F;
		int tm_min	= (GetIndex->fileInfo.recordStartTime >> 5) & 0x3F;
		int tm_sec	= GetIndex->fileInfo.recordStartTime & 0x1F;
		
		int tm_year = (GetIndex->fileInfo.recordStartDate >> 9) & 0x7F;
		int tm_mon	= (GetIndex->fileInfo.recordStartDate >> 5) & 0x0F;
		int tm_mday  = GetIndex->fileInfo.recordStartDate & 0x1F;	
		sprintf(filename,"%04d%02d%02d%02d%02d%02d0%c.H264",tm_year+1980,tm_mon,tm_mday,tm_hour,tm_min,tm_sec*2,GetIndex->fileInfo.alarmType+'a');
		printf("filename--------------->%s\n",filename);
		
		//ret_fd = GetIndex->fileInfo.fileIndex;
		
		return (char*)GetIndex;
	}
	
}

/*************************************
**返回值
** >0 找到断点位置，循环查找 
**************************************/
char* Mux_GetAllRecordFileTime(char* lastFd,unsigned int start_time,unsigned int end_time,RECORD_LIST *record_list)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}

	GosIndex* pGos_indexList = NULL;
	int lAviCount = 1;
	int IndexSum = 0;;
	unsigned int StartTimeStamp = 0;
	unsigned int EndTimeStamp = 0;
    if(NULL == gAVIndexList || (!StorageCheckSDExist()))
    {
        return NULL;
    }
	if(lastFd == NULL)
	{
		pGos_indexList = Get_Oldest_file();
		if(pGos_indexList == NULL)
		{
			return NULL;
		}
		lAviCount = 1;
		IndexSum = gHeadIndex.lRootDirFileNum;
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
			{
				if(pGos_indexList->fileInfo.recordStartTimeStamp >= start_time)
				{
					printf("get all file,start time %d\n",pGos_indexList->fileInfo.recordStartTimeStamp);
					break;
				}
			}
			if(pGos_indexList == &gAVIndexList[IndexSum-1])
			{
				pGos_indexList = &gAVIndexList[1];
			}
			else
			{
				pGos_indexList++;
			}
		}
		if(lAviCount == IndexSum) 
		{
			printf("there are no record file in this time!!\n");
			return NULL;
		}
	}
	else
	{
		pGos_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
	}
	
	if(pGos_indexList == NULL)
	{
		return NULL;
	}
	
	lAviCount = 1;
	IndexSum = gHeadIndex.lRootDirFileNum;
	StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
	EndTimeStamp = pGos_indexList->fileInfo.recordEndTimeStamp;
	
	GosIndex* pGos_indexList_next = NULL;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList == &gAVIndexList[IndexSum-1])
		{
			pGos_indexList = &gAVIndexList[1];
		}
		else
		{
			pGos_indexList++;
		}
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pGos_indexList->fileInfo.recordEndTimeStamp > end_time)
			{
				printf("pGos_indexList->fileInfo.recordEndTimeStamp >= end_time recordEndTimeStamp= %d \n",pGos_indexList->fileInfo.recordEndTimeStamp);
				break;
			}
			if(pGos_indexList->fileInfo.recordStartTimeStamp < EndTimeStamp)
			{
				printf("StartTimeStamp > lastEndTimeStamp: fd:%d s:%d   e:%d   EndTimeStamp:%d\n",
					pGos_indexList->fileInfo.fileIndex,pGos_indexList->fileInfo.recordStartTimeStamp,pGos_indexList->fileInfo.recordEndTimeStamp,EndTimeStamp);
				if(pGos_indexList == Get_Oldest_file())
				{
					break;
				}
				printf("skip curr indext, return next indext.\n");
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				if(pGos_indexList == &gAVIndexList[IndexSum-1])
				{
					return (char*)&gAVIndexList[1];
				}
				else
				{
					return (char*)(pGos_indexList+1);
				}
			}
			if((pGos_indexList->fileInfo.recordStartTimeStamp - EndTimeStamp) > MAX_INTERVAL_TIME)
			{
				//printf("start: %d  end: %d\n",StartTimeStamp,EndTimeStamp);
				
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				return (char*)pGos_indexList;
			}
			EndTimeStamp = pGos_indexList->fileInfo.recordEndTimeStamp;
		}
	}
	printf("find out all file!!!!!!!!!\n");
	
	record_list->StartTimeStamp = StartTimeStamp;
	record_list->EndTimeStamp = EndTimeStamp;
	return NULL;
}

//连续的录像只返回一个开始、结束时间，录像不连续则返回断点位置lastFd，并记录连续的录像时间
char* Mux_GetAllAlarmRecordFileTime(char* lastFd,unsigned int start_time,unsigned int end_time,RECORD_LIST *record_list)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}

	GosIndex* pGos_indexList = NULL;
	int lAviCount = 1;
	int IndexSum = 0;;
	unsigned int StartTimeStamp = 0;
	unsigned int EndTimeStamp = 0;
	unsigned int AlarmType = 0;
    if(NULL == gAVIndexList || (!StorageCheckSDExist()))
    {
        return NULL;
    }
	if(lastFd == NULL)
	{
		
		lAviCount = 1;
		IndexSum = gHeadIndex.lRootDirFileNum;
		pGos_indexList = Get_Oldest_file();
		if(pGos_indexList == NULL)
		{
			return NULL;
		}
		
		for(lAviCount;lAviCount < IndexSum;lAviCount++) //找到最早的报警录像文件
		{
			if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
			{
				if(pGos_indexList->fileInfo.recordStartTimeStamp >= start_time 
					&& pGos_indexList->fileInfo.alarmType > 0)
				{
					break;
				}
			}
			if(pGos_indexList == &gAVIndexList[IndexSum-1])
				pGos_indexList = &gAVIndexList[1];
			else
				pGos_indexList++;
		}

		if(lAviCount == IndexSum) //没有报警录像文件
		{
			printf("there is no alarm file in this time!!!\n");
			return NULL;
		}		
	}
	else
	{
		pGos_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(pGos_indexList == NULL || pGos_indexList->fileInfo.alarmType == 0)
		{
			return NULL;
		}
	}
	
	lAviCount = 1;
	IndexSum = gHeadIndex.lRootDirFileNum;
	StartTimeStamp = pGos_indexList->fileInfo.recordStartTimeStamp;
	EndTimeStamp = pGos_indexList->fileInfo.recordEndTimeStamp;
	AlarmType = pGos_indexList->fileInfo.alarmType;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList == &gAVIndexList[IndexSum-1])
		{
			pGos_indexList = &gAVIndexList[1];
		}
		else
		{
			pGos_indexList++;
		}
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK && pGos_indexList->fileInfo.alarmType > 0)
		{
			if(pGos_indexList->fileInfo.recordEndTimeStamp >= end_time)
			{
				break;
			}
			if(pGos_indexList->fileInfo.recordStartTimeStamp < EndTimeStamp)
			{
				printf("StartTimeStamp < lastEndTimeStamp\n");
				if(pGos_indexList == Get_Oldest_Alarm_file())
				{
					break;
				}
				printf("skip curr indext, return next indext.\n");
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				record_list->AlarmType = AlarmType;
				if(pGos_indexList == &gAVIndexList[IndexSum-1])
				{
					return (char*)&gAVIndexList[1];
				}
				else
				{
					return (char*)(pGos_indexList+1);
				}
			}
			if((pGos_indexList->fileInfo.recordStartTimeStamp - EndTimeStamp) > MAX_INTERVAL_TIME || pGos_indexList->fileInfo.alarmType != AlarmType)
			{
				//printf("start: %d  end: %d\n",StartTimeStamp,EndTimeStamp);
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				record_list->AlarmType = AlarmType;
				return (char*)pGos_indexList;
			}
			EndTimeStamp = pGos_indexList->fileInfo.recordEndTimeStamp;
		}
	}

	printf("find out all file!!!!!!!!!\n");
	record_list->StartTimeStamp = StartTimeStamp;
	record_list->EndTimeStamp = EndTimeStamp;
	record_list->AlarmType = AlarmType;
	return NULL;
}

//reference_time:参考时间 lastFd:上一次返回的文件索引描述符
char* Mux_RefreshRecordList(unsigned int reference_time,char* lastFd,RECORD_LIST *record_list)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}

	if(gAVIndexList == NULL || reference_time <= 0)
	{
		return NULL;
	}

	int lAviCount = 1;
	int IndexSum = 0;;
	GosIndex *pre_indexList; 	
	unsigned int time_stamp = 0;
	unsigned int StartTimeStamp = 0;
	unsigned int EndTimeStamp = 0;
	
	time_stamp = reference_time;//GetTimeStamp(reference_time);
	IndexSum = gHeadIndex.lRootDirFileNum;	
	lAviCount = 1;

	if(lastFd == NULL)
	{
		lAviCount = 1;
		IndexSum = gHeadIndex.lRootDirFileNum;
		pre_indexList = Get_Oldest_file();
		if(pre_indexList == NULL)
		{
			return NULL;
		}
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(pre_indexList->fileInfo.filestate == NON_EMPTY_OK)
			{
				if(pre_indexList->fileInfo.recordStartTimeStamp >= time_stamp) //找到需要刷新的位置
				{
					//printf("find the reflash pos ,fd = %d , %d ,%d!!!!!!!!!!!!!!!\n",pre_indexList->fileInfo.fileIndex,pre_indexList->fileInfo.recordStartTimeStamp,time_stamp);
					break;
				}
			}
			if(pre_indexList == &gAVIndexList[IndexSum-1])
				pre_indexList = &gAVIndexList[1];
			else
				pre_indexList++;
		}
		
		if(lAviCount == IndexSum)
		{
			printf("there is no new record file. return 0.\n");
			return NULL;
		}
	}
	else 
	{
		pre_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(pre_indexList == NULL)
		{
			return NULL;
		}
	}
	
	lAviCount = 1;
	IndexSum = gHeadIndex.lRootDirFileNum;
	StartTimeStamp = pre_indexList->fileInfo.recordStartTimeStamp;
	EndTimeStamp = pre_indexList->fileInfo.recordEndTimeStamp;
	
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pre_indexList == &gAVIndexList[IndexSum-1])
		{
			pre_indexList = &gAVIndexList[1];
		}
		else
		{
			pre_indexList++;
		}

		if(pre_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pre_indexList->fileInfo.recordStartTimeStamp < EndTimeStamp)
			{
				//printf("%d  %d,StartTimeStamp > lastEndTimeStamp\n",pre_indexList->fileInfo.recordStartTimeStamp,EndTimeStamp);
				if(pre_indexList == Get_Oldest_file())
				{
					break;
				}
				printf("skip curr indext, return next indext.\n");
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				if(pre_indexList == &gAVIndexList[IndexSum-1])
				{
					return (char*)&gAVIndexList[1];
				}
				else
				{
					return (char*)(pre_indexList+1);
				}
			}
			if((pre_indexList->fileInfo.recordStartTimeStamp - EndTimeStamp) > MAX_INTERVAL_TIME)
			{
				//printf("start: %d  end: %d\n",StartTimeStamp,EndTimeStamp);
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp	= EndTimeStamp;
				return (char*)pre_indexList;
			}
			EndTimeStamp = pre_indexList->fileInfo.recordEndTimeStamp;
		}
	}
	printf("find out all new file!!!!!!!!!\n");
	record_list->StartTimeStamp = StartTimeStamp;
	record_list->EndTimeStamp	= EndTimeStamp;
	return NULL;
}

//reference_time:参考时间 lastFd:上一次返回的索引描述符
char* Mux_RefreshAlarmRecordList(unsigned int reference_time,char* lastFd,RECORD_LIST *record_list)
{
	if(RemoveSdFlag == 1)
	{
		return NULL;
	}
	if(gAVIndexList == NULL || reference_time <= 0)
	{
		return NULL;
	}

	int lAviCount = 1;
	int IndexSum = 0;;
	GosIndex *pre_indexList; 	
	int time_stamp = 0;
	unsigned int StartTimeStamp = 0;
	unsigned int EndTimeStamp = 0;
	unsigned int AlarmType = 0;
	
	time_stamp = reference_time;//GetTimeStamp(reference_time);
	IndexSum = gHeadIndex.lRootDirFileNum;	
	lAviCount = 1;
	
	if(lastFd == NULL)
	{
		lAviCount = 1;
		IndexSum = gHeadIndex.lRootDirFileNum;
		pre_indexList = Get_Oldest_file();
		if(pre_indexList == NULL)
		{
			return NULL;
		}
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(pre_indexList->fileInfo.filestate == NON_EMPTY_OK && pre_indexList->fileInfo.alarmType > 0)
			{
				if(pre_indexList->fileInfo.recordStartTimeStamp >= time_stamp) //找到需要刷新的位置
				{
					break;
				}
			}
			if(pre_indexList == &gAVIndexList[IndexSum-1])
				pre_indexList = &gAVIndexList[1];
			else
				pre_indexList++;
		}
		
		if(lAviCount == IndexSum)
		{
			printf("there is no new alarm record file. return 0.\n");
			return NULL;
		}
	}
	else 
	{
		pre_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(pre_indexList == NULL)
		{
			return NULL;
		}
	}
	
	lAviCount = 1;
	IndexSum = gHeadIndex.lRootDirFileNum;
	StartTimeStamp = pre_indexList->fileInfo.recordStartTimeStamp;
	EndTimeStamp = pre_indexList->fileInfo.recordEndTimeStamp;
	AlarmType 	= pre_indexList->fileInfo.alarmType;
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pre_indexList == &gAVIndexList[IndexSum-1])
		{
			pre_indexList = &gAVIndexList[1];
		}
		else
		{
			pre_indexList++;
		}

		if(pre_indexList->fileInfo.filestate == NON_EMPTY_OK && pre_indexList->fileInfo.alarmType > 0)
		{
			if(pre_indexList->fileInfo.recordStartTimeStamp < EndTimeStamp)
			{
				if(pre_indexList == Get_Oldest_Alarm_file())
				{
					break;
				}
				printf("skip curr indext, return next indext.\n");
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				record_list->AlarmType		= AlarmType;
				if(pre_indexList == &gAVIndexList[IndexSum-1])
				{
					return (char*)&gAVIndexList[1];
				}
				else
				{
					return (char*)(pre_indexList+1);
				}
			}
			if((pre_indexList->fileInfo.recordStartTimeStamp - EndTimeStamp) > MAX_INTERVAL_TIME || pre_indexList->fileInfo.alarmType != AlarmType)
			{
				//printf("start: %d  end: %d\n",StartTimeStamp,EndTimeStamp);
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp	= EndTimeStamp;
				record_list->AlarmType		= AlarmType;
				return (char*)pre_indexList;
			}
			EndTimeStamp = pre_indexList->fileInfo.recordEndTimeStamp;
		}
	}
	printf("find out all file!!!!!!!!!\n");
	record_list->StartTimeStamp = StartTimeStamp;
	record_list->EndTimeStamp	= EndTimeStamp;
	record_list->AlarmType		= AlarmType;
	return NULL;
}

//flag: 0->更改当前文件报警类型 1->更改上一个文件报警类型
int Mux_SetLastFileAlarmType(char *CurrFp,int AlarmType,int flag)
{
	if(RemoveSdFlag == 1)
	{
		return -1;
	}
	if(gAVIndexList == NULL)
	{
		return -1;
	}

	if(CurrFp == NULL)
	{
		return -1;
	}
	GosIndex *pGos_indexList;
	GosIndex *pGos_Last_indexList;
	pGos_indexList = (GosIndex*)CurrFp;
	
	if(pGos_indexList == &gAVIndexList[1])
	{
		pGos_Last_indexList = &gAVIndexList[gHeadIndex.lRootDirFileNum-1];
	}
	else
	{
		pGos_Last_indexList = &gAVIndexList[pGos_indexList->fileInfo.fileIndex - 1];
	}

	if(flag == 0)
	{
		pGos_indexList->fileInfo.alarmType = AlarmType;
		printf("[record lib]: currfd -> %d , alarm type - > %d\n",pGos_Last_indexList->fileInfo.fileIndex,pGos_Last_indexList->fileInfo.alarmType);
		return 0;
	}
	else
	{	
		if(pGos_indexList->fileInfo.recordStartTimeStamp - pGos_Last_indexList->fileInfo.recordEndTimeStamp <= MAX_INTERVAL_TIME)
		{
			pGos_Last_indexList->fileInfo.alarmType = AlarmType;
			printf("[record lib]: lastfd -> %d , alarm type - > %d\n",pGos_Last_indexList->fileInfo.fileIndex,pGos_Last_indexList->fileInfo.alarmType);
			return 0;
		}
	}

	return -1;
}

//Mux_close之前调用
int Mux_SetTimeStamp(char* fd,unsigned int start_or_end,unsigned int time_stamp)
{
	if(RemoveSdFlag == 1)
	{
		return -1;
	}
	if(gAVIndexList == NULL)
	{
		return -1;
	}

	int lAviCount = 1;
	int IndexSum = 0;;
	GosIndex *indexList; 
	IndexSum = gHeadIndex.lRootDirFileNum;
	indexList = &gAVIndexList[1];

	if(fd == NULL || (start_or_end!=0 && start_or_end!=1) || time_stamp<=0)
	{
		return -1;
	}
	
	/*for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(indexList->fileInfo.fileIndex == fd)
		{
			break;
		}
		indexList ++;
	}
	if(lAviCount == IndexSum)
	{
		return -1;
	}*/
	indexList = (GosIndex*)fd;
	
	if(start_or_end == 1)
	{
		indexList->fileInfo.recordStartTimeStamp = time_stamp;
	}
	else if(start_or_end == 0)
	{
		indexList->fileInfo.recordEndTimeStamp = time_stamp;
	}
	
	return 0;
}

int Mux_Get_File_Size(const char* file_name)
{
	return Storage_Get_File_Size(file_name);
}

unsigned int Mux_Get_Oldest_Time()
{
	GosIndex* pGos_indexList = Get_Oldest_file();
	if(pGos_indexList == NULL)
	{
		printf("do not find Oldest Time!!!\n");
		return 0;
	}
	return pGos_indexList->fileInfo.recordStartTimeStamp;
}

long long Mux_lseek(int Fileindex,unsigned int offset,unsigned int whence)
{
	return storage_Lseek(Fileindex,offset,whence,fPart);
}

void *Gos_DiskManager_proc(void *p)
{
	pthread_detach(pthread_self());
	int flagInit = 1;
	RemoveSdFlag = 0;
	FormatSdFlag = 0;
	FILE* file_fd = NULL;
	FILE *Flagfp;	
	char flagFileName[128] = {0};
	while(1)
	{
		if(FormatSdFlag == 1)
		{
			printf("is formatting sd !!!!\n");
			sleep(1);
			continue;
		}
		if(!CheckSdIsMount())
		//if(!StorageCheckSDExist())
		{
			RemoveSdFlag = 1;
			if (fPart > 0 )
			{
				sleep(2);
				Storage_Close_All();
				FormatSdFlag = 0;
			}
			flagInit = 1;
			sleep(1);
			continue;
		}
		if(flagInit && CheckSdIsMount())
		{
			sprintf(flagFileName,"%s/%s",SDDirName,"1q2w3e4r5t.dat");
			if((Flagfp = fopen(flagFileName,"r+")) != NULL)
			{
				fclose(Flagfp);
				printf("SD Find 1q2w3e4r5t.dat exist,Don't to predistribution!!!\n");
			}
			else
			{
				Storage_Init(0);
				RemoveSdFlag = 0;
			}
			
			flagInit = 0;
		}	
		usleep(1000*1000);
	}

	pthread_exit(NULL);
}

#ifdef __cplusplus
}
#endif


