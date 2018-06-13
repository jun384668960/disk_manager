#ifndef DISK_DEFINE_H
#define DISK_DEFINE_H

#if __BYTE_ORDER == __BIG_ENDIAN //(1)

#ifdef __le16_to_cpu //(2)

#define CF_LE_W	__le16_to_cpu
#define CF_LE_L	__le32_to_cpu
#define CT_LE_W	__cpu_to_le16
#define CT_LE_L	__cpu_to_le32
#else //(2)
#define CF_LE_W(v) ((((v) & 0xff) << 8) | (((v) >> 8) & 0xff))
#define CF_LE_L(v) (((unsigned)(v)>>24) | (((unsigned)(v)>>8)&0xff00) | \
               (((unsigned)(v)<<8)&0xff0000) | ((unsigned)(v)<<24))
#define CT_LE_W(v) CF_LE_W(v)
#define CT_LE_L(v) CF_LE_L(v)
#endif /* defined(__le16_to_cpu) */  //(2)
    
#else //(1)

#define CF_LE_W(v) (v)
#define CF_LE_L(v) (v)
#define CT_LE_W(v) (v)
#define CT_LE_L(v) (v)

#endif /* __BIG_ENDIAN */  //(1)

#define HDIO_GETGEO		        0x0301	/* get device geometry */
#define BOOTCODE_FAT32_SIZE	    420
#define BOOTCODE_SIZE		    448
#define DEFAULT_SECTOR_SIZE     512
#define DBR_RESERVED_SECTORS    32
#define DEFAULT_FAT_NUM         2
#define MSDOS_FAT32_SIGN        "FAT32   " 
#define BOOT_SIGN               0xAA55 
#define SECTORS_PER_BLOCK       ( BLOCK_SIZE / DEFAULT_SECTOR_SIZE )
#define MAX_CLUST_32            ((1 << 28) - 16) 
#define ATTR_VOLUME             8 

#define LONGLONG 	unsigned long long 
#define LONG 		unsigned long
#define __u8   		unsigned char
#define __u16  		unsigned short int
#define __u32  		unsigned int
#define __u64  		unsigned long long
#define BYTE   		unsigned char

typedef struct hd_geometry 
{
      unsigned char heads;
      unsigned char sectors;
      unsigned short cylinders;
      unsigned long start;
}hd_geometry;


typedef struct msdos_dir_entry //目录项结构,32个字节
{
    char	name[8], ext[3];	/* name and extension */
    __u8        attr;			/* attribute bits */
    __u8	lcase;			/* Case for base and extension */
    __u8	ctime_ms;		/* Creation time, milliseconds */
    __u16	ctime;			/* Creation time */
    __u16	cdate;			/* Creation date */
    __u16	adate;			/* Last access date */
    __u16	starthi;		/* high 16 bits of first cl. (FAT32) */
    __u16	time, date, start;	/* time, date and first cluster */
    __u32	size;			/* file size (in bytes) */
}msdos_dir_entry;

typedef struct long_msdos_dir_entry //长目录项结构,32*3个字节
{
	char long_msdos_dir[2][32];	   
	msdos_dir_entry dir_entry;    
}long_msdos_dir_entry;

typedef struct fat32_fsinfo //32个字节
{
  __u32		reserved1;	/* Nothing as far as I can tell */
  __u32		signature;	/* 0x61417272L */
  __u32		free_clusters;	/* Free cluster count.  -1 if unknown */
  __u32		next_cluster;	/* Most recently allocated cluster.
				 * Unused under Linux. */
  __u32		reserved2[4];
}fat32_fsinfo;


typedef struct msdos_volume_info   //26个字节
{
	__u8		drive_number;	/* BIOS drive number */
	__u8		RESERVED;	/* Unused */
	__u8		ext_boot_sign;	/* 0x29 if fields below exist (DOS 3.3+) */
	__u8		volume_id[4];	/* Volume ID number */
	__u8		volume_label[11];/* Volume label */
	__u8		fs_type[8];	/* Typically FAT12 or FAT16 */
}msdos_volume_info;

typedef struct _fat32
{
  __u32	fat32_length;	/* sectors/FAT */
  __u16	flags;		/* bit 8: fat mirroring, low 4: active fat */
  __u8	version[2];	/* major, minor filesystem version */
  __u32	root_cluster;	/* first cluster in root directory */
  __u16	info_sector;	/* filesystem info sector */
  __u16	backup_boot;	/* backup boot sector */
  __u16	reserved2[6];	/* Unused */
  msdos_volume_info vi;
  __u8	boot_code[BOOTCODE_FAT32_SIZE];//420个字节+54=474
}_fat32;

typedef struct _oldfat
{
	msdos_volume_info vi;//26个字节
	__u8	boot_code[BOOTCODE_SIZE];//448个字节+26=474
}_oldfat;

typedef union _fstype
{

  _fat32 fat32;
 _oldfat  oldfat;
  
}_fstype;

typedef struct msdos_boot_sector//512个字节
{
	__u8	        boot_jump[3];	/* Boot strap short or near jump */
	__u8          system_id[8];	/* Name - can be used to special case
				   partition manager volumes */
	__u8          sector_size[2];	/* bytes per logical sector */
	__u8          cluster_size;	/* sectors/cluster */
	__u16         reserved;	/* reserved sectors */
	__u8          fats;		/* number of FATs */
	__u8          dir_entries[2];	/* root directory entries */
	__u8          sectors[2];	/* number of sectors */
	__u8          media;		/* media code (unused) */
	__u16         fat_length;	/* sectors/FAT */
	__u16         secs_track;	/* sectors per track */
	__u16         heads;		/* number of heads */
	__u32         hidden;		/* hidden sectors (unused) */
	__u32         total_sect;	/* number of sectors (if sectors == 0) */
	_fat32 		  fat32;
	//_oldfat  	  oldfat;
	__u16		boot_sign;
} msdos_boot_sector;

//////////////////////////////////////////////////////////////////////////////////

#define LONG_MSDOS_DIR_SUPPORT 1   //支持长文件名

#if LONG_MSDOS_DIR_SUPPORT
//当前最大支持26个字符的长文件名(fat32最多支持255个字符，可扩容)
#define LONG_DIR_ITEM_SIZE     	(32*3)  
#else
#define LONG_DIR_ITEM_SIZE     	 32
#endif

#ifdef __cplusplus
extern "C"{
#endif

#define SDMOUNTPOINT      		"/opt/httpServer/lighttpd/htdocs/sd/ipc"
#define SDDirName         		"/opt/httpServer/lighttpd/htdocs/sd" 

#define FILE_MAX_LENTH     		(2*1024*1024)   	//云存储录6秒裸数据，包含音视频
#define MP4_MAX_LENTH     		(8*1024*1024)   	//MP4每个文件最大占8M
#define JPEG_MAX_LENTH     		(384*1024)        	//jpeg每个文件最大占384k

#define FLAG_INDEX_HEAD    		0XAAF0F0AB     		//索引头标志,读不到这个标志代表没有索引，开始预分配
#define MAX_INTERVAL_TIME 		1					//两个文件时间相差在1s内，则认为这两个文件连续
#define DateListMax             100
//索引的内容(index)
//--------------------------------------------
//HeadIndex | gMp4IndexList | gJpegIndexList | 
//--------------------------------------------

enum RECORD_FILE_TYPE 
{
    RECORD_FILE_TS,
    RECORD_FILE_MP4,
    RECORD_FILE_FLV,
    RECORD_FILE_JPG,
    RECORD_FILE_AVI,
    RECORD_FILE_H264,
    RECORD_FILE_MAX
};

char* pFileTypeString[RECORD_FILE_MAX]=
{
    "z.ts",
    ".mp4",
    ".flv",
    ".jpg",
    ".avi",
    ".H264"
};

//文件状态
enum RECORD_FILE_STATE  
{
	WRITE_OK = 0,		   	//空文件
	NON_EMPTY_OK,	   		//非空文件
	OCCUPATION,    			//文件被占用
	REST,			   		//无
};

enum RECORD_TYPE
{
    UNKNOWN_RECORD = 0,
    EVENT_RECORD,			//事件录像
    MANUAL_RECORD,			//手动录像
    SCHEDULE_RECORD,		//计划录像
};

enum ALARM_TYPE
{
    UNKNOWN = 0,
    VIDEO_MOTION,
    PIR_MOTION,
    PIR_VIDEO_MOTION, 
    AUDIO_MOTION, 
    IO_ALARM,
    LOW_TEMP_ALARM,
    HIGH_TEMP_ALARM,
    LOW_HUM_ALARM,
    HIGH_HUM_ALARM,
    LOW_WBGT_ALARM,
    HIGH_WBGT_ALARM,
    CALLING   
};

//文件信息
typedef struct FileInfo
{	
	__u16 fileIndex;						//文件句柄(索引号)
	__u16 FileFpart;						//文件预留标志
	//__u16 recordDuration;    				//录像时长(单位:秒)
	enum RECORD_FILE_STATE filestate;		//文件状态	
	//enum RECORD_TYPE recordType;			//录像类型(计划录像，手动录像，事件录像)
	enum ALARM_TYPE  alarmType;             //报警类型	
	//enum RECORD_FILE_TYPE fileType;         //文件类型
	__u16 recordStartDate;                  //录像开始日期
	__u16 recordStartTime;                  //录像开始时间
	__u32 recordStartTimeStamp;				//录像开始时间戳
	__u32 recordEndTimeStamp;				//录像结束时间戳
	LONG  fileSize;                 		//文件长度
}FileInfo;
/*	
	日期=(年份-1980)*512+月份*32+日  (2个字节)
	0x18字节0~4位是日期数；0x18字节5~7位和0x19字节0位是月份；0x19字节的1~7位为年号

	时间=小时*2048+分钟*32+秒/2   
	0x16字节的0~4位是以2秒为单位的量值；
	0x16字节的5~7位和0x17字节的0~2位是分钟；0x17字节的3~7位是小时   (2个字节)
*/
typedef struct DateList   //日期列表
{
	__u16 mDate;      	//日期
	__u16 mCounts;		//数量
}DateList;

typedef struct GosIndex   //磁盘索引 
{
	__u32 startCluster;				//起始簇
	__u32 CluSectorsNum;  			//簇所在的扇区
	__u32 CluSectorsEA;  			//簇所在的扇区内偏移
	__u64 DirSectorsNum;   			//目录所在的扇区
	__u32 DirSectorsEA;				//目录所在的扇区偏移
	__u64 DataSectorsNum;			//文件数据对应所在的起始扇区
	__u64 DataSectorsEA;          	//文件在磁盘中的偏移(文件的位置操作)
	FileInfo fileInfo;
}GosIndex;

typedef struct HeadIndex   //索引头
{
	LONG  FlagIndexHead;           	//索引头标志
	__u32 FAT1StartSector;		   	//fat1表起始扇区
	__u32 RootStartSector;			//根目录起始扇区
	__u64 ChildStartSector;      	//子目录起始扇区  
	__u32 ChildStartCluster;        //子目录起始簇
	__u64 ChildItemEA;              //子目录该条目所在的地址偏移
	__u64 ChildClusterListEA;       //子目录的簇链地址偏移
	__u32 lRootDirFileNum;			//mp4文件数量(包含索引文件)
	__u32 lJpegFileNum;             //jpeg文件数量
	__u32 HeadStartSector;   		//索引头在磁盘的起始扇区
	__u32 JpegStartEA;           	//jpeg索引的地址偏移
	__u32 CurrIndexPos;				//保存当前使用到的索引，方便下次直接顺序操作
	__u8  ClusterSize;              //簇的单位(扇区/簇)
}HeadIndex;


#ifdef __cplusplus
}
#endif

#endif
