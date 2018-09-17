
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

//pre-define
inline void Mark_FAT_Cluster (unsigned long cluster, unsigned long value, unsigned char* pFat);
unsigned char ChkSum (unsigned char *pFcbName);
void creatItem(char longNum,char *pFileName,char *de,unsigned char Chk);
void CreateFileItem(msdos_dir_entry *de,char *pFileName,
                unsigned long start, unsigned long filesize,
				unsigned char nClusterSize,char attribute);
void CreateLongFileItem(long_msdos_dir_entry *de,unsigned char *shortname,
						char *pFileName,unsigned long start,
						unsigned long filesize,unsigned char nClusterSize,
						char attribute);
int   FormatjpegDir(int fpart);
int   FormatParttion(int fpart, unsigned long filesize, unsigned long lAviNum,unsigned long eventlogSizeM);
int   Find_head_index(int fpart);
GosIndex* Get_Oldest_file();
GosIndex* Get_Oldest_Alarm_file();
GosIndex* Get_Index_Form_fd(unsigned int fd);

int   Storage_Write_gos_index(int fpart,enum RECORD_FILE_TYPE fileType);
int   Storage_Get_File_Size(const char *fileName);
int   StorageDeleteFile(int Fileindex);

int   Storage_Init(int mkfs_vfat);
int   Storage_Close_All();
char* Storage_Open(const char *fileName);
int   Storage_Close(char* Fileindex, char *fileName,int fpart);
int   Storage_Read(char* Fileindex,int offset,void *data,int dataSize,int fpart);
int   Storage_Write(char* Fileindex,const void *data,unsigned int dataSize,int fpart);
long long Storage_Lseek(int Fileindex,unsigned int offset,unsigned int whence,int fpart);
//////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C"{
#endif

int fPart = -1;						//磁盘句柄
CMtx LockFlag;               	//文件锁
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
}

void CreateLongFileItem(long_msdos_dir_entry *de,unsigned char *shortname,
						char *pFileName,unsigned long start,
						unsigned long filesize,unsigned char nClusterSize,
						char attribute)
{
	//长目录每项最多携带13个字符
	long_msdos_dir_entry *de_msdos = de;
	memset(de_msdos,0,sizeof(long_msdos_dir_entry));
    unsigned char Chk = ChkSum(shortname);

	//长目录每项最多携带13个字符
	creatItem(0x42,pFileName+13,de_msdos->long_msdos_dir[0],Chk);
	creatItem(0x01,pFileName,de_msdos->long_msdos_dir[1],Chk);
	
	CreateFileItem(&de_msdos->dir_entry,shortname,start,filesize,nClusterSize,attribute);
}


//自定义文件锁
void Storage_Lock()
{
	cmtx_enter(LockFlag);
}

void Storage_Unlock()
{
	cmtx_leave(LockFlag);
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
		LOGE_print("unable to allocate space in memory!!!");
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
		LOGE_print("write pChildDir error!!!");
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
		LOGE_print("write 2*sizeof(msdos_dir_entry) error!!!");
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
		LOGE_print("unable to allocate lRootFileSize in memory!!!");
		return -1;
	}
	memset(pDir, 0, lRootFileSize);
	
	gos_indexSize = sizeof(GosIndex)*gHeadIndex.lJpegFileNum;
	struct GosIndex* pGos_index = NULL; 
	if ((pGos_index = (struct GosIndex *)malloc(gos_indexSize)) == NULL)
	{
		free(pDir);
		LOGE_print("unable to allocate gos_indexSize in memory!!!");
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
		LOGE_print("write pDir error!!!");
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
		LOGE_print("write pGos_index error!!!");
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
		LOGE_print("write gHeadIndex error!!!");
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
		if(err == 0)
		{
			LOGI_print("disk_size=%lld errcode=%d",b , err);
			cblocks = ( b >> 9 );	//总共扇区数
		}
		else
		{
			err = ioctl(fpart, BLKGETSIZE, &sz);
			if(err == 0)
			{
				LOGI_print("disk_size=%ld errcode=%d",sz , err);
				cblocks = sz;
			}
			else
			{
				LOGE_print("get BLKGETSIZE error");
			}
		}
	
		LOGI_print("cblocks..000...111==%llu",cblocks);	
		//减去 10M 的空间不分区
		cblocks -= 10*1024*2; // 1kB = 2个扇区 得到所有需要分区的扇区
	}

	LOGI_print("cblocks..222!");	

	///获得文件的状态信息
	struct stat statbuf;
	if (fstat(fpart, &statbuf) < 0)
	{
		LOGE_print("fstat fpart ERROR ");	
		return -1;
	}
		
	if ( !S_ISBLK(statbuf.st_mode) )//如果文件是一个块设备，则S_ISBLK()返回真
	{
		statbuf.st_rdev = 0;///st_rdev;  
	}
	LOGI_print("cblocks..333! ");	

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

	LOGI_print("cblocks..444!");	

	//先给cluster_size赋一个粗值
	if ( VolSize <= 66600 )
	{
		LOGE_print("Volume too small for FAT32!" );
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

	LOGI_print("cblocks==%lld  lTotalSectors=%lu",cblocks ,lTotalSectors );
	
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
		LOGE_print("Attempting to create a too large file system!");
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
		LOGE_print("Too few blocks for viable file system!!");
		return -1;
	}
	
	//下面是填写fat32表和根目录文件。
	unsigned char* pFat = NULL; 
	unsigned char* pZeroFat = NULL;
	if ( ( pFat = ( unsigned char * )malloc(DEFAULT_SECTOR_SIZE) ) == NULL )
	{
		LOGE_print("nunable to allocate space for FAT image in memory!");
		return -1;
	}
	memset(pFat, 0, DEFAULT_SECTOR_SIZE);
	
	if ((pZeroFat = (unsigned char *)malloc(DEFAULT_SECTOR_SIZE)) == NULL)
	{
		LOGE_print("unable to allocate space for FAT image in memory!");
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
		LOGE_print("unable to allocate space for root directory in memory");
		return -1;
	}
	memset(pRootDir, 0, lRootFileSize);

	//分区上要有几个特定的文件
	unsigned long lStartClu = lClustersofRoot + 2;//数据区从二开始编簇号,数据区先放根目录，然后才放其它文件（在mbr中已经规定好了）

	//CreateFileItem(&pRootDir[0],"GosIndexDAT",lStartClu,10*1024*1024,mbs.cluster_size,0x20);
	char shortname[11] = {0};
	char longName[26] = {0};
	sprintf(shortname,"%s%s","gIndex","~1DAT");
	sprintf(longName,"%s","GosStorageManagerIndex.dat");
	
	CreateLongFileItem(&pRootDir[0],shortname,longName,lStartClu,10*1024*1024,mbs.cluster_size,0x20);

   	//有实际簇的个数得到的视频文件数
   	lClust32 -= (10*1024*1024)/(mbs.cluster_size * DEFAULT_SECTOR_SIZE);  //去掉10M的空间不分配
    //unsigned long lActualAviNum = (lClust32 - (lStartClu -2) - PIC_PARTITION_SIZE)/lClustersofFile;
	unsigned long clu = (FILE_MAX_LENTH) / (mbs.cluster_size * DEFAULT_SECTOR_SIZE);
	unsigned long lActualAviNum = lClust32  / clu;

	LOGI_print("clu=%d lActualAviNum=%d lClust32=%d",clu,lActualAviNum,lClust32);
	      		
	gHeadIndex.FAT1StartSector = DBR_RESERVED_SECTORS;
	gHeadIndex.RootStartSector = gHeadIndex.FAT1StartSector + 2*lFatLength32;
	gHeadIndex.lRootDirFileNum = lActualAviNum;  
	gHeadIndex.ClusterSize = mbs.cluster_size;
	gHeadIndex.HeadStartSector = lClustersofRoot*gHeadIndex.ClusterSize+gHeadIndex.RootStartSector;
	
	unsigned long all_gos_indexSize = sizeof(GosIndex)*gHeadIndex.lRootDirFileNum;
	struct GosIndex* pGos_index = NULL; 
	if ((pGos_index = (struct GosIndex *)malloc (all_gos_indexSize)) == NULL)
	{
		free(pFat);		
		free(pZeroFat);
		free(pRootDir);
		LOGE_print("unable to allocate space for root directory in memory");
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
		strcpy(&sz[6],"~1DAT");
		memset(longName,0,sizeof(longName));
		sprintf(longName,"%d%s",lAviCount + 1,".H264");
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
		LOGE_print("Out of memory!");
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
			LOGE_print("write pZeroFat ERROR");
			return -1;
		}
	}

    LOGI_print("cblocks..555!");		

	//写dbr
	lseek64(fpart,0,SEEK_SET);
	int count ;
	if(write(fpart,(char *)&mbs,DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
	{
		FREE();
		LOGE_print("write mbs ERROR");
		return -1;
	}
	
	//写文件信息
	lseek64(fpart,CF_LE_W(mbs.fat32.info_sector)*DEFAULT_SECTOR_SIZE,SEEK_SET);  
	if(write(fpart,(char *)pInfoSector,DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
	{
		FREE();
		LOGE_print("write pInfoSector ERROR");
		return -1;
	}

	LOGI_print( "cblocks..666!");		

	//写backup boot sector
	lseek64(fpart,CF_LE_W(mbs.fat32.backup_boot)*DEFAULT_SECTOR_SIZE,SEEK_SET);
	if(write(fpart,(char *)&mbs,DEFAULT_SECTOR_SIZE) != DEFAULT_SECTOR_SIZE)
	{
		FREE();
		LOGE_print("write mbs ERROR");
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
				LOGE_print("write pFatSector ERROR");
				return -1;
			}
		}	
	}

	LOGI_print("cblocks..777!");	
	//写根目录文件
	if (write(fpart,(char *)pRootDir, lRootFileSize) != (int)lRootFileSize)
	{
		FREE();
		LOGE_print("write pRootDir ERROR");
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
		LOGE_print("write all_gos_indexSize ERROR");
		return -1;
	}

	ppIndex = &pGos_index[0];
	for(lAviCount = 0;lAviCount < gHeadIndex.lRootDirFileNum - 1;lAviCount++)
	{
		ppIndex++;
	}	

	gHeadIndex.JpegStartEA  = lseek(fpart,0,SEEK_CUR);
	gHeadIndex.ChildStartCluster = ppIndex->startCluster + FILE_MAX_LENTH/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	gHeadIndex.ChildStartSector = ppIndex->DataSectorsNum + FILE_MAX_LENTH/DEFAULT_SECTOR_SIZE;
	gHeadIndex.ChildClusterListEA = ppIndex->CluSectorsNum* DEFAULT_SECTOR_SIZE 
		+  ppIndex->CluSectorsEA + FILE_MAX_LENTH /(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE)*4;

	gHeadIndex.ChildItemEA = ppIndex->DirSectorsNum*DEFAULT_SECTOR_SIZE + ppIndex->DirSectorsEA + LONG_DIR_ITEM_SIZE;

	FREE();
	fsync(fpart);

	LOGI_print( "cblocks..8888 do_success!");

	//更新索引标志
	gHeadIndex.FlagIndexHead = FLAG_INDEX_HEAD;
	gHeadIndex.CurrIndexPos = 0;
	offset = gHeadIndex.HeadStartSector * DEFAULT_SECTOR_SIZE;
	lseek64(fpart,offset,SEEK_SET);
	if (write(fpart,(char *)(&gHeadIndex), sizeof(HeadIndex)) != (int)sizeof(HeadIndex))
	{
		LOGE_print("write gHeadIndex error!!!");
		return -1;
	}
	fsync(fpart);

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
		LOGE_print("StartTimeStamp == 0");
		return NULL;
	}
	
	LOGI_print("oldest time is %d, fd = %d",StartTimeStamp,Start_pGos_indexList->fileInfo.fileIndex);
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
		LOGE_print("StartTimeStamp:%d Start_pGos_indexList:%p", StartTimeStamp, Start_pGos_indexList);
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
		LOGE_print("there is no alarm file in this time!!!");
		return NULL;
	}	
	LOGI_print("oldest alarm file time is %d, fd = %d",StartTimeStamp,Start_pGos_indexList->fileInfo.fileIndex);
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

	do
	{
		if( NULL == gAVIndexList || (!StorageCheckSDExist()))
		{
			LOGE_print("gAVIndexList:%p SDExist:0", gAVIndexList);
			return -1;
		}

		//写索引头
		Storage_Lock();
		lseek64(fpart,gHeadIndex.HeadStartSector * DEFAULT_SECTOR_SIZE,SEEK_SET);
		if (write(fpart,(char *)(&gHeadIndex), sizeof(HeadIndex)) != (int)sizeof(HeadIndex))
		{
			LOGE_print("write gHeadIndex error");
			break;
		}

		int err = 0;
	    switch(fileType)
	    {
	        case RECORD_FILE_MP4: 
			{
				all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
				if (write(fpart,(char *)(gMp4IndexList), all_gos_indexSize) != (int)all_gos_indexSize)
				{
					LOGE_print("write gMp4IndexList error");
					err = 1;;
				}
				break;
			}
	        case RECORD_FILE_JPG: 
			{
	            all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lJpegFileNum;
				lseek64(fpart,gHeadIndex.JpegStartEA,SEEK_SET);
				if (write(fpart,(char *)(gJpegIndexList), all_gos_indexSize) != (int)all_gos_indexSize)
				{
					LOGE_print("write gJpegIndexList error");
					err = 1;;
				}	
	            break;
			}
			case RECORD_FILE_H264:
			{
	            all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
				if (write(fpart,(char *)(gAVIndexList), all_gos_indexSize) != (int)all_gos_indexSize)
				{
					LOGE_print("write gAVIndexList error");
					err = 1;;
				}	
//				static FILE* s_pList = NULL;
//				s_pList = fopen(SDDirName"/index_list", "w");
//				if(s_pList != NULL) {
//					size_t wb = fwrite((char*)gAVIndexList, 1, all_gos_indexSize, s_pList);
//					if(wb != (size_t)all_gos_indexSize)
//					{
//						LOGE_print("fwrite gAVIndexList to s_pList error wb:%ld all_gos_indexSize:%lu", wb, all_gos_indexSize);
//					}
//				}
	            break;
			}

	        default: 
			{
				LOGE_print("fileType:%d error", fileType);
				err = 1;;
			}            
	    }

		if(err == 1)
		{
			break;
		}

		fsync(fpart);
		Storage_Unlock();
		
		return 0;
	}while(0);
	
	Storage_Unlock();
	return -1;
}

int Storage_Get_File_Size(const char *fileName)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
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
		LOGE_print("filetype:%d", filetype);
		return -1;
	}
	unsigned int Checktime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;
	unsigned int Checkdate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.filestate == NON_EMPTY_OK)
		{
			if(pGos_indexList->fileInfo.recordStartDate == (Checkdate&0xffff)
			   && pGos_indexList->fileInfo.recordStartTime == (Checktime&0xffff))
			{
				return pGos_indexList->fileInfo.fileSize;
			}
		}
		pGos_indexList++;
	}
	return 0;
}

//删除文件
int StorageDeleteFile(int Fileindex)
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
		LOGE_print("gMp4IndexList:%p gJpegIndexList:%p or SDExist false", gMp4IndexList, gJpegIndexList);
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
				LOGE_print("FileType:%d", FileType);
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
				LOGE_print("write dir_entry ERROR");
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
		LOGE_print("lAviCount:%d == IndexSum:%d", lAviCount, IndexSum);
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

int StorageFatUpdate(int fpart, GosIndex* index)
{
	int i;
	__u64 fatOffset;
	
	//更新FAT
	LONG len = index->fileInfo.fileSize;
	int cuCount = len / (gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE); //这个文件占的簇数
	int allCount = FILE_MAX_LENTH/(gHeadIndex.ClusterSize*DEFAULT_SECTOR_SIZE);
	if(len % (gHeadIndex.ClusterSize * DEFAULT_SECTOR_SIZE) != 0)
	{
		cuCount += 1;  //不整除,分配多一簇
	}
	if(FILE_MAX_LENTH % (gHeadIndex.ClusterSize * DEFAULT_SECTOR_SIZE) != 0)
	{
		allCount +=1;  //不整除,分配多一簇
	}
	if(cuCount > allCount) //此情况不存在，write的时候已经保证不超出
	{
		LOGE_print("cuCount:%d > allCount:%d", cuCount, allCount);
		return -1;	 //文件长度大于预分配文件大小
	}

	fatOffset = index->CluSectorsNum * DEFAULT_SECTOR_SIZE + index->CluSectorsEA;

	int *pfat = NULL;
	if((pfat = (int *)malloc(allCount*sizeof(int))) == NULL)
	{
		LOGE_print("malloc error");
		return -1;
	}
	memset(pfat,0,allCount*sizeof(int));
	int *ptmp = &pfat[0];
	for(i=0; i<allCount; i++)
	{
		if(i==0 || i==cuCount)
		{
			*ptmp = 0XFFFFFFFF; // 1号表项
		}
		else if(i==cuCount)
		{
			*ptmp = 0X0FFFFFFF; // 文件结束于该簇
		}
		else
		{
			*ptmp = index->startCluster + i;
		}	
		ptmp++;
	}
	
	lseek64(fpart, fatOffset, SEEK_SET);
	if(write(fpart,(char *)pfat,allCount*sizeof(int)) != allCount*sizeof(int))
	{
		free(pfat);
		pfat = NULL;
		ptmp = NULL;
		LOGE_print("write pfat error");
		return	-1;
	}
	free(pfat);
	pfat = NULL;
	ptmp = NULL;

	return 0;
}


int StorageDirEntryUpdate(int fpart, GosIndex* index, char *fileName)
{
	//更新目录结构
	struct long_msdos_dir_entry dir_entry; 
	char longName[25] = {0};
	char shortname[11] = {0};
	unsigned long start;
	__u64 fatOffset;

	if(fileName != NULL)
	{
		int tm_year,tm_mon,tm_mday;
		int tm_hour,tm_min,tm_sec;
		char zero;
		char filetype[32] = {0};
		char alarmtype;
	
		sscanf(fileName,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
			&tm_min,&tm_sec,&zero,&alarmtype,filetype);
	
		unsigned int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec/2;
		unsigned int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
		index->fileInfo.recordStartTime = ltime & 0xffff;
		index->fileInfo.recordStartDate = ldate & 0xffff;
		index->fileInfo.alarmType = alarmtype - 'a';
		sprintf(fileName,"%04d%02d%02d%02d%02d%02d0%c.H264",tm_year,tm_mon,tm_mday,tm_hour,tm_min,(tm_sec/2)*2,index->fileInfo.alarmType+'a');
		strcpy(longName,fileName);
		LOGD_print("close filename %s",fileName);
	}

	sprintf(shortname,"%06d%s", index->fileInfo.fileIndex, "~1DAT");
	start = index->startCluster;
	CreateLongFileItem(&dir_entry, shortname, longName, start, FILE_MAX_LENTH, gHeadIndex.ClusterSize, 0x20);
	
	dir_entry.dir_entry.time = index->fileInfo.recordStartTime;
	dir_entry.dir_entry.date = index->fileInfo.recordStartDate;
	dir_entry.dir_entry.ctime = dir_entry.dir_entry.time;
	dir_entry.dir_entry.cdate = dir_entry.dir_entry.date;
	dir_entry.dir_entry.adate = dir_entry.dir_entry.date;
	dir_entry.dir_entry.starthi = CT_LE_W(index->startCluster>>16);
	dir_entry.dir_entry.start = CT_LE_W(index->startCluster&0xffff);
	dir_entry.dir_entry.size = index->fileInfo.fileSize;

	fatOffset = index->DirSectorsNum * DEFAULT_SECTOR_SIZE + index->DirSectorsEA;
	lseek64(fpart, fatOffset, SEEK_SET);
	if(write(fpart,&dir_entry,sizeof(long_msdos_dir_entry)) != sizeof(long_msdos_dir_entry))
	{
		LOGE_print("write dir_entry error");
		return -1;
	}

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

	Storage_Lock();
	if(fPart > 0)
	{
		LOGW_print("fPart:%d", fPart);
		close(fPart);
		fPart = -1;
	}
	//FormatSdFlag = 1;
		
	int i;
	for(i=1;i<=4;i++)
	{
		sprintf(szPartName,"%s%d","/dev/mmcblk0p",i);
		fPart = open( szPartName, O_RDWR | O_LARGEFILE); //以大文件的方式打开
		if(fPart < 0 )
		{
			LOGE_print("fPart:%d open szPartName=%s Failed!!!", fPart, szPartName);
		}	
		else
		{
			LOGI_print("fPart:%d open szPartName=%s OK!!!", fPart, szPartName);
			break;
		}
	}
	
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
			LOGE_print("fPart:%d open szPartName=%s Failed!!!", fPart, szPartName);
			Storage_Unlock();
			return -1;
		}	
		else
		{
			LOGI_print("fPart:%d open szPartName=%s OK!!!", fPart, szPartName);
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
		int bRet = FormatParttion(fPart, FILE_MAX_LENTH, lAviNumtemp, lLogSizeMtemp );	
		if(bRet != 0)
		{
			LOGE_print("FormatParttion error!");
			Storage_Unlock();
			return -1;
		}
		memset(command,0,sizeof(command));
		sprintf(command,"mount -t vfat %s %s",szPartName,SDDirName);
		StoragepopenRead(command);
	}

	//分配内存给磁盘索引
	unsigned long all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
	if ((gAVIndexList = (struct GosIndex *)malloc (all_gos_indexSize)) == NULL)
	{
		LOGE_print("unable to allocate space for gAVIndexList in memory");
		Storage_Unlock();
		return -1;
	}
	LOGI_print("all_gos_indexSize %d",all_gos_indexSize);
	memset(gAVIndexList, 0, all_gos_indexSize);
	read(fPart,&gAVIndexList[0],all_gos_indexSize);
	
	FormatSdFlag = 0;
	LOGI_print( "storage init do_success!!!");
	Storage_Unlock();
	return 0;
}

int Storage_Close_All()
{
	Storage_Lock();
	FormatSdFlag = 1;

	if(fPart > 0)
	{
		LOGW_print("fPart:%d", fPart);
		close(fPart);
		fPart = -1;
	}
	else
	{
		LOGI_print("fPart:%d nothing opened", fPart);
		Storage_Unlock();
		return -1;
	}
	
	setMaxWriteSize(0);

	free(gAVIndexList);	
	gAVIndexList = NULL;	

	memset(&gHeadIndex,0,sizeof(HeadIndex));
	Storage_Unlock();           //非安全退出状态下一定要解锁
	LOGI_print("release all storage memory!!!");
	return 0;
}

//寻找可写文件的索引号(文件句柄)
char* Storage_Open(const char *fileName)
{
	int tm_year,tm_mon,tm_mday;
	int tm_hour,tm_min,tm_sec;
	char zero;
	char alarmtype;
	char filetype[32] = {0};

	Storage_Lock();
	GosIndex* handle_index;
	//是否回环，否则取下一个index
	if(gHeadIndex.CurrIndexPos >= gHeadIndex.lRootDirFileNum-1)
	{
		handle_index = &gAVIndexList[1];
	}
	else
	{
		handle_index = &gAVIndexList[gHeadIndex.CurrIndexPos+1];
	}

	sscanf(fileName,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,&alarmtype,filetype);
	unsigned int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec/2;
	unsigned int ldate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 

	//初始化index属性
	handle_index->fileInfo.recordStartTime = ltime & 0xffff;
	handle_index->fileInfo.recordStartDate = ldate & 0xffff;
	handle_index->fileInfo.filestate = OCCUPATION;
	handle_index->fileInfo.FileFpart = 0;
	handle_index->DataSectorsEA = 0;

	//更新全局标志
	oldStartTimeStap = handle_index->fileInfo.recordStartTimeStamp;
	oldEndTimeStap   = handle_index->fileInfo.recordEndTimeStamp;
	gHeadIndex.CurrIndexPos = handle_index->fileInfo.fileIndex;
	Storage_Unlock();
	
	LOGD_print("open CurrIndexPos DiskFd index=%d",handle_index->fileInfo.fileIndex);
	return (char*)handle_index;
}

//更新fat表,目录以及索引
int Storage_Close(char* Fileindex,char *fileName,int fpart)
{	
	__u64 fatOffset;
	int ret;
	do
	{
		Storage_Lock();
		GosIndex* handle_index = (GosIndex*)Fileindex;

		handle_index->fileInfo.FileFpart = 0;  //文件连续读写结束了
		handle_index->DataSectorsEA = 0;

		if(handle_index->fileInfo.recordStartTimeStamp == oldStartTimeStap 
			|| handle_index->fileInfo.recordEndTimeStamp == oldEndTimeStap
			|| handle_index->fileInfo.recordStartTimeStamp <= 1514736000
			|| handle_index->fileInfo.recordEndTimeStamp <= 1514736000) //时间小于2010年1月1日
		{
			LOGE_print("record file timestap error[%d,%d]",handle_index->fileInfo.recordStartTimeStamp
				,handle_index->fileInfo.recordEndTimeStamp);
			
			handle_index->fileInfo.filestate = WRITE_OK;
			break;
		}

		ret = StorageFatUpdate(fpart, handle_index);
		if(ret != 0)
		{
			LOGE_print("StorageFatUpdate error");
			break;
		}
		ret = StorageDirEntryUpdate(fpart, handle_index, fileName);
		if(ret != 0)
		{
			LOGE_print("StorageDirEntryUpdate error");
			break;
		}
		
		handle_index->fileInfo.filestate = NON_EMPTY_OK;
		gHeadIndex.CurrIndexPos = handle_index->fileInfo.fileIndex;

		lseek64(fpart,gHeadIndex.HeadStartSector * DEFAULT_SECTOR_SIZE, SEEK_SET);
		if (write(fpart,(char *)(&gHeadIndex), sizeof(HeadIndex)) != (int)sizeof(HeadIndex))
		{
			LOGE_print("write gHeadIndex error");
			break;
		}

		unsigned long all_gos_indexSize = sizeof(GosIndex) * gHeadIndex.lRootDirFileNum;
		if (write(fpart,(char *)(gAVIndexList), all_gos_indexSize) != (int)all_gos_indexSize)
		{
			LOGE_print("write gAVIndexList error");
			break;
		}

		fsync(fpart);
		Storage_Unlock();
		
		return 0;
	}while(0);

	Storage_Unlock();
	return -1;
}

//读磁盘数据
int Storage_Read(char* Fileindex,int offset,void *data,int dataSize,int fpart)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return -1;
	}

	if( NULL == gAVIndexList || Fileindex == NULL)
	{
		LOGE_print("gAVIndexList:%p Fileindex:%p", gAVIndexList, Fileindex);
		return -1;
	}

	struct GosIndex *pGos_indexList;
	int lAviCount;
	int IndexSum;
	int datalen;
	int retlen = 0;

	pGos_indexList = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 0;
	
	pGos_indexList = (GosIndex*)Fileindex;
	//连续读
	if(offset >= pGos_indexList->fileInfo.fileSize)
	{
		LOGE_print("offset:%d >= fileSize:%d", offset, pGos_indexList->fileInfo.fileSize);
		return 0;
	}
	unsigned long long DataSectorsEA  = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE + offset;
	lseek64(fpart,DataSectorsEA,SEEK_SET); 
	
	datalen = pGos_indexList->DataSectorsNum * DEFAULT_SECTOR_SIZE;
	unsigned long all_read_size = DataSectorsEA - datalen;
	unsigned long remain_size = pGos_indexList->fileInfo.fileSize - all_read_size;
	if(remain_size == 0)
	{
		LOGE_print("file is read over.");
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
	//根据索引值找到对应文件的磁盘位置,然后写入
	GosIndex* handle_index = (GosIndex*)Fileindex;

	do
	{
		Storage_Lock();
		//判断是否连续写入，否则重置写偏移
		if(handle_index->fileInfo.FileFpart == 0)
		{
			handle_index->fileInfo.fileSize = 0;
			handle_index->DataSectorsEA = handle_index->DataSectorsNum * DEFAULT_SECTOR_SIZE;
		}
		handle_index->fileInfo.FileFpart = 1;
		
		//文件的长度不能超过预分配给每个文件的大小 
		if((handle_index->fileInfo.fileSize + dataSize) > FILE_MAX_LENTH)
		{
			LOGE_print("error fileSize:%d + dataSize:%d > maxfilesize:%d", handle_index->fileInfo.fileSize, dataSize, FILE_MAX_LENTH);
			break;
		}

		//写内容
		lseek64(fpart, handle_index->DataSectorsEA, SEEK_SET);
		unsigned int nlen = write(fpart, (char*)data, dataSize);
		if(nlen < 0 || nlen != dataSize)
		{
			LOGE_print("write error nlen = %d dataSize:%d", nlen, dataSize);
			break;
		}
		
		//更新写地址和当前文件长度
		handle_index->DataSectorsEA += nlen;
//		handle_index->fileInfo.fileSize += nlen;
		unsigned long long beyondSize;
		beyondSize = handle_index->DataSectorsEA - handle_index->DataSectorsNum * DEFAULT_SECTOR_SIZE;
		if(beyondSize > handle_index->fileInfo.fileSize)
		{
			handle_index->fileInfo.fileSize += (beyondSize - handle_index->fileInfo.fileSize); 
		}
		
		Storage_Unlock();
		return nlen;
	}while(0);

	Storage_Unlock();
	return -1;
}

long long Storage_Lseek(int Fileindex,unsigned int offset,unsigned int whence,int fpart)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return -1;
	}

	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p or SDExist false", gAVIndexList);
		return -1;
	}
	
	int lAviCount;
	int IndexSum;
	unsigned long long DataEA;
	struct GosIndex *pGos_indexList;

	pGos_indexList = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;
	lAviCount = 0;

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
		LOGE_print("lAviCount:%d == IndexSum:%d", lAviCount, IndexSum);
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
			LOGE_print("whence:%d error");
			return -1;
		}          
   }

	pGos_indexList->DataSectorsEA = DataEA;
	//文件偏移不能超过预分配给每个文件的大小
	if(offset > MP4_MAX_LENTH || offset > pGos_indexList->fileInfo.fileSize)
	{
		LOGE_print("offset:%d>MP4_MAX_LENTH:%d || offset:%d>fileSize:%lu", offset, MP4_MAX_LENTH, offset, pGos_indexList->fileInfo.fileSize);
		return -1;	
	}
	
	return lseek64(fpart,pGos_indexList->DataSectorsEA,whence);	
}

//获取某月录像事件列表 (即获取SD卡中该月有哪些天是有录像的)
char *sGetMonthEventList(char *sMonthEventList)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return NULL;
	}

	int lAviCount;
	int IndexSum;
	int tm_year,tm_mon,tm_mday;
	char sList[32] = {0};
	struct GosIndex *pGos_indexList = NULL;

	if(NULL == gAVIndexList || NULL == sMonthEventList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p sMonthEventList:%p or SDExist false", gAVIndexList, sMonthEventList);
		return NULL;
	}
	
	DateList mDateList[DateListMax];
	int mDateListCounts = 0;
	memset(&mDateList,0,sizeof(DateList)*DateListMax);


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
	if(NULL == date || NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p date:%p or SDExist false", gAVIndexList, date);
		return NULL;
	}
	
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
	pGos_indexList = gAVIndexList; 
	IndexSum = gHeadIndex.lRootDirFileNum;	
	lAviCount = 0;	
	FileType = RECORD_FILE_H264; 

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
		pGos_indexList = gAVIndexList; 
		IndexSum = gHeadIndex.lRootDirFileNum;	
		lAviCount = 0;	
		FileType = RECORD_FILE_H264; 

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
	
	if(NULL == filename ||gAVIndexList == NULL || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p filename:%p or SDExist false", gAVIndexList, filename);
        return 0;
	}
	sscanf(filename,"%04d%02d%02d%02d%02d%02d%c%c%s",&tm_year,&tm_mon,&tm_mday,&tm_hour,
		&tm_min,&tm_sec,&zero,/*&recordtype,*/&alarmtype,/*&fileduration,*/filetype);

	recordDate = (tm_year - 1980) * 512 + tm_mon * 32 + tm_mday; 
	int ltime = tm_hour * 2048 + tm_min * 32 + tm_sec / 2;

	file_type = 0;
	
	pGos_indexList = gAVIndexList; 
	IndexSum = gHeadIndex.lRootDirFileNum;	
	lAviCount = 0;	
	FileType = RECORD_FILE_H264; 
	
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
		pGos_indexList = gAVIndexList; 
		IndexSum = gHeadIndex.lRootDirFileNum;	
		lAviCount = 0;	
		FileType = RECORD_FILE_H264; 

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

				sprintf(Item,"%s@%6.1f|",FileName,fileSize/(float)(1024*1024));
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
	
	return rltCount;
}

//开始下载指定录像文件(获取录像文件路径)
char *sGetRecordFullName(const char *sFileName, char *sFullName)
{
	if(NULL == sFileName || NULL == gMp4IndexList || NULL == gJpegIndexList 
		|| NULL == sFullName || (!StorageCheckSDExist()))
	{
		LOGE_print("gMp4IndexList:%p gJpegIndexList:%p sFileName:%p sFullName:%p or SDExist false"
			, gMp4IndexList, gJpegIndexList, sFileName, sFullName);
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
		LOGE_print("filetype:%d", filetype);
		return NULL;
	}
	
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.recordStartDate == (ldate&0xFFFF)
			&&pGos_indexList->fileInfo.recordStartTime == (ltime&0xFFFF))
		{
			sprintf(sFullName,"%s/%s",sDir,sFileName);
			return sFullName;
		}
		pGos_indexList++;
	}	

    return NULL;
}

int sDelRecord(const char *sFileName)
{
	if(NULL == gMp4IndexList || NULL == gJpegIndexList || NULL == sFileName || (!StorageCheckSDExist()))
	{
		LOGE_print("gMp4IndexList:%p gJpegIndexList:%p sFileName:%p or SDExist false"
			, gMp4IndexList, gJpegIndexList, sFileName);
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
		LOGE_print("filetype:%d", filetype);
		return -1;
	}
	
	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(pGos_indexList->fileInfo.recordStartDate == (ldate&0xFFFF)
			&&pGos_indexList->fileInfo.recordStartTime== (ltime&0xFFFF))
		{
			return StorageDeleteFile(pGos_indexList->fileInfo.fileIndex);	 
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
		//printf("offset=%llu lFileNum=%d",offset,lFileNum);
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
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return 0;
	}
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p or SDExist false", gAVIndexList);
		return 0;
	}

	int IndexSum;
	int RootDirFileUseCounts = 0;
	int JpegFileUseCounts = 0;
	struct GosIndex *pGos_indexList;

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

	return Usable;  //M
}

char* Mux_open(const char *fileName)
{
	Storage_Lock();
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p SDExist:0", gAVIndexList);
		Storage_Unlock();
		return NULL;
	}
	Storage_Unlock();
	
	if(fileName == NULL || RemoveSdFlag == 1)
	{
		LOGE_print("fileName:%p RemoveSdFlag:%d", fileName, RemoveSdFlag);
		return NULL;
	}
	return Storage_Open(fileName);
}

int Mux_close(char* Fileindex,char *fileName)
{
	Storage_Lock();
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p SDExist:0", gAVIndexList);
		Storage_Unlock();
		return -1;
	}
	Storage_Unlock();

	if(Fileindex == NULL || fileName == NULL || RemoveSdFlag == 1)
	{
		LOGE_print("Fileindex:%p fileName:%p RemoveSdFlag:%d", Fileindex, fileName, RemoveSdFlag);
		return -1;
	}
	return Storage_Close(Fileindex,fileName,fPart);
}

int Mux_write(char* Fileindex,const void *data,unsigned int dataSize)
{	
	Storage_Lock();
	if(NULL == gAVIndexList || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p SDExist:0", gAVIndexList);
		Storage_Unlock();
		return -1;
	}
	Storage_Unlock();

	if(Fileindex == NULL || data == NULL || dataSize <= 0 || RemoveSdFlag == 1)
	{
		LOGE_print("Fileindex:%p data:%p dataSize:%d RemoveSdFlag:%d", Fileindex, data, dataSize, RemoveSdFlag);
		return -1;
	}
	
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
		LOGE_print("gAVIndexList:%p or SDExist false", gAVIndexList);
		return -1;
	}
	tmpIndex = gAVIndexList;
	IndexSum = gHeadIndex.lRootDirFileNum;

	for(lAviCount;lAviCount < IndexSum;lAviCount++)
	{
		if(tmpIndex->fileInfo.filestate == NON_EMPTY_OK)
		{
			LOGI_print("fd:%d  start: %d   end : %d  alarmType : %d",tmpIndex->fileInfo.fileIndex,tmpIndex->fileInfo.recordStartTimeStamp,
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
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return NULL;
	}
	GosIndex * GetIndex = NULL;
	GosIndex * FirstIndex = NULL;
	GosIndex * tmpIndex = NULL;
	GosIndex * nextIndex = NULL;

	if(gAVIndexList == NULL || filename == NULL || (!StorageCheckSDExist()))
	{
		LOGE_print("gAVIndexList:%p filename:%p or SDExist false", gAVIndexList, filename);
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
			LOGE_print("lAviCount:%d = IndexSum:%d", lAviCount, IndexSum);
			return NULL;
		}
		LOGI_print("got it ,fd: %d, start: %d, end: %d",FirstIndex->fileInfo.fileIndex,FirstIndex->fileInfo.recordStartTimeStamp,FirstIndex->fileInfo.recordEndTimeStamp);
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
		LOGI_print("filename %s",filename);			
		return (char*)FirstIndex;
	}
	else
	{
		GetIndex = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(GetIndex == NULL)
		{
			LOGE_print("GetIndex == NULL");
			return NULL;
		}
		tmpIndex = GetIndex;
		if(GetIndex == &gAVIndexList[gHeadIndex.lRootDirFileNum-1])
		{
			GetIndex = &gAVIndexList[1];
		}
		else 
		{
			GetIndex++;
		}

		if(GetIndex->fileInfo.filestate != NON_EMPTY_OK)
		{
			LOGE_print("filestate != NON_EMPTY_OK");
			return NULL;
		}
		
		while(GetIndex->fileInfo.recordStartTimeStamp < tmpIndex->fileInfo.recordEndTimeStamp)
		{
			if(GetIndex == Get_Oldest_file())
			{
				LOGE_print("GetIndex:%p == Get_Oldest_file()", GetIndex);
				return NULL;
			}
			LOGI_print("skip curr indext, return next indext.");
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
			LOGI_print("time lag %d",*time_lag);
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
		LOGI_print("filename:%s",filename);
		
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
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return NULL;
	}

	GosIndex* pGos_indexList = NULL;
	int lAviCount = 1;
	int IndexSum = 0;;
	unsigned int StartTimeStamp = 0;
	unsigned int EndTimeStamp = 0;
    if(NULL == gAVIndexList || (!StorageCheckSDExist()))
    {
    	LOGE_print("gAVIndexList:%p or SDExist false", gAVIndexList);
        return NULL;
    }
	if(lastFd == NULL)
	{
		pGos_indexList = Get_Oldest_file();
		if(pGos_indexList == NULL)
		{
			LOGE_print("pGos_indexList:%p", pGos_indexList);
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
					LOGI_print("get all file,start time %d",pGos_indexList->fileInfo.recordStartTimeStamp);
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
			LOGE_print("there are no record file in this time!!");
			return NULL;
		}
	}
	else
	{
		pGos_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
	}
	
	if(pGos_indexList == NULL)
	{
		LOGE_print("pGos_indexList:%p", pGos_indexList);
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
				LOGI_print("pGos_indexList->fileInfo.recordEndTimeStamp >= end_time recordEndTimeStamp= %d",pGos_indexList->fileInfo.recordEndTimeStamp);
				break;
			}
			if(pGos_indexList->fileInfo.recordStartTimeStamp < EndTimeStamp)
			{
				LOGI_print("StartTimeStamp > lastEndTimeStamp: fd:%d s:%d   e:%d   EndTimeStamp:%d",
					pGos_indexList->fileInfo.fileIndex,pGos_indexList->fileInfo.recordStartTimeStamp,pGos_indexList->fileInfo.recordEndTimeStamp,EndTimeStamp);
				if(pGos_indexList == Get_Oldest_file())
				{
					break;
				}
				printf("skip curr indext, return next indext.");
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
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				return (char*)pGos_indexList;
			}
			EndTimeStamp = pGos_indexList->fileInfo.recordEndTimeStamp;
		}
	}
	LOGI_print("find out all file!");
	
	record_list->StartTimeStamp = StartTimeStamp;
	record_list->EndTimeStamp = EndTimeStamp;
	return NULL;
}

//连续的录像只返回一个开始、结束时间，录像不连续则返回断点位置lastFd，并记录连续的录像时间
char* Mux_GetAllAlarmRecordFileTime(char* lastFd,unsigned int start_time,unsigned int end_time,RECORD_LIST *record_list)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
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
    	LOGE_print("gAVIndexList:%p or SDExist=false", gAVIndexList);
        return NULL;
    }
	if(lastFd == NULL)
	{
		lAviCount = 1;
		IndexSum = gHeadIndex.lRootDirFileNum;
		pGos_indexList = Get_Oldest_file();
		if(pGos_indexList == NULL)
		{
			LOGE_print("pGos_indexList:%p", pGos_indexList);
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
			LOGE_print("there is no alarm file in this time!!");
			return NULL;
		}		
	}
	else
	{
		pGos_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(pGos_indexList == NULL || pGos_indexList->fileInfo.alarmType == 0)
		{
			LOGE_print("pGos_indexList:%p pGos_indexList->fileInfo.alarmType:%d", pGos_indexList, pGos_indexList->fileInfo.alarmType);
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
				LOGI_print("StartTimeStamp < lastEndTimeStamp");
				if(pGos_indexList == Get_Oldest_Alarm_file())
				{
					break;
				}
				LOGI_print("skip curr indext, return next indext.");
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
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp = EndTimeStamp;
				record_list->AlarmType = AlarmType;
				return (char*)pGos_indexList;
			}
			EndTimeStamp = pGos_indexList->fileInfo.recordEndTimeStamp;
		}
	}

	LOGI_print("find out all file!!");
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
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return NULL;
	}

	if(gAVIndexList == NULL || reference_time <= 0)
	{
		LOGE_print("gAVIndexList:%p reference_time:%d", gAVIndexList, reference_time);
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
			LOGE_print("pre_indexList:%p", pre_indexList);
			return NULL;
		}
		for(lAviCount;lAviCount < IndexSum;lAviCount++)
		{
			if(pre_indexList->fileInfo.filestate == NON_EMPTY_OK)
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
			LOGE_print("there is no new record file. return 0");
			return NULL;
		}
	}
	else 
	{
		pre_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(pre_indexList == NULL)
		{
			LOGE_print("pre_indexList:%p", pre_indexList);
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
				if(pre_indexList == Get_Oldest_file())
				{
					break;
				}
				LOGI_print("skip curr indext, return next indext.");
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
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp	= EndTimeStamp;
				return (char*)pre_indexList;
			}
			EndTimeStamp = pre_indexList->fileInfo.recordEndTimeStamp;
		}
	}
	LOGI_print("find out all new file!");
	record_list->StartTimeStamp = StartTimeStamp;
	record_list->EndTimeStamp	= EndTimeStamp;
	return NULL;
}

//reference_time:参考时间 lastFd:上一次返回的索引描述符
char* Mux_RefreshAlarmRecordList(unsigned int reference_time,char* lastFd,RECORD_LIST *record_list)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return NULL;
	}
	if(gAVIndexList == NULL || reference_time <= 0)
	{
		LOGE_print("gAVIndexList:%p reference_time:%d", gAVIndexList, reference_time);
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
			LOGE_print("pre_indexList == NULL");
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
			LOGE_print("there is no new alarm record file. return 0");
			return NULL;
		}
	}
	else 
	{
		pre_indexList = (GosIndex*)lastFd;//Get_Index_Form_fd(lastFd);
		if(pre_indexList == NULL)
		{
			LOGE_print("pre_indexList == NULL");
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
				printf("skip curr indext, return next indext.");
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
				record_list->StartTimeStamp = StartTimeStamp;
				record_list->EndTimeStamp	= EndTimeStamp;
				record_list->AlarmType		= AlarmType;
				return (char*)pre_indexList;
			}
			EndTimeStamp = pre_indexList->fileInfo.recordEndTimeStamp;
		}
	}
	LOGI_print("find out all file!!");
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
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
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

	Storage_Lock();
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
		LOGI_print("[record lib]: currfd -> %d , alarm type - > %d",pGos_Last_indexList->fileInfo.fileIndex,pGos_Last_indexList->fileInfo.alarmType);
		Storage_Unlock();
		return 0;
	}
	else
	{	
		if(pGos_indexList->fileInfo.recordStartTimeStamp - pGos_Last_indexList->fileInfo.recordEndTimeStamp <= MAX_INTERVAL_TIME)
		{
			pGos_Last_indexList->fileInfo.alarmType = AlarmType;
			LOGI_print("[record lib]: lastfd -> %d , alarm type - > %d",pGos_Last_indexList->fileInfo.fileIndex,pGos_Last_indexList->fileInfo.alarmType);
			Storage_Unlock();
			return 0;
		}
	}

	Storage_Unlock();
	return -1;
}

//Mux_close之前调用
int Mux_SetTimeStamp(char* fd,unsigned int start_or_end,unsigned int time_stamp)
{
	if(RemoveSdFlag == 1)
	{
		LOGE_print("error status, RemoveSdFlag:%d", RemoveSdFlag);
		return -1;
	}
	if(gAVIndexList == NULL)
	{
		return -1;
	}

	if(fd == NULL || (start_or_end!=0 && start_or_end!=1) || time_stamp<=0)
	{
		return -1;
	}
	
	GosIndex* indexList = (GosIndex*)fd;
	Storage_Lock();
	if(start_or_end == 1)
	{
		indexList->fileInfo.recordStartTimeStamp = time_stamp;
	}
	else if(start_or_end == 0)
	{
		indexList->fileInfo.recordEndTimeStamp = time_stamp;
	}
	Storage_Unlock();
	
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
		LOGE_print("do not find Oldest Time!!!");
		return 0;
	}
	return pGos_indexList->fileInfo.recordStartTimeStamp;
}

long long Mux_lseek(int Fileindex,unsigned int offset,unsigned int whence)
{
	return Storage_Lseek(Fileindex,offset,whence,fPart);
}

void *Gos_DiskManager_proc(void *p)
{
	pthread_detach(pthread_self());
	int flagInit = 1;
	RemoveSdFlag = 0;
	FormatSdFlag = 0;
	FILE *Flagfp;	
	char flagFileName[128] = {0};
	LockFlag = cmtx_create();
	
	while(1)
	{
		Storage_Lock();
		if(fPart == -1 && CheckSdIsMount())
		{
			sprintf(flagFileName,"%s/%s",SDDirName,"1q2w3e4r5t.dat");
			if((Flagfp = fopen(flagFileName,"r+")) != NULL)
			{
				fclose(Flagfp);
				Storage_Unlock();
				LOGW_print("SD Find 1q2w3e4r5t.dat exist,Don't to predistribution!!");
			}
			else
			{
				Storage_Unlock();
				//当没有过拔卡操作，且有强制格式化
				if(FormatSdFlag == 1 && RemoveSdFlag == 0)
				{
					Storage_Init(1);
				}
				else
				{
					Storage_Init(0);
				}
				
				RemoveSdFlag = 0;
			}
			
			flagInit = 0;
		}	
		else if(!CheckSdIsMount() && fPart > 0)//当卡拔出时，释放索引，并且标志拔卡动作
		{
			RemoveSdFlag = 1;
			Storage_Close_All();
		}
		
		Storage_Unlock();
		usleep(1000*1000);
	}

	cmtx_delete(LockFlag);
	pthread_exit(NULL);
}

#ifdef __cplusplus
}
#endif


