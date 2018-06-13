#ifndef _DISKMANANGER_H_
#define _DISKMANANGER_H_

typedef struct _RECORD_LIST
{
	unsigned int StartTimeStamp;
	unsigned int EndTimeStamp;
	unsigned int AlarmType;
}RECORD_LIST;

//public:
int Storage_Init(int mkfs_vfat);
int Storage_Close_All();

char* sGetMonthEventList(char *sMonthEventList);
char* sGetDayEventList(const char *date, unsigned int file_type, char *sDayEventList, int NMaxLength, unsigned int *filecounts);
int   sGetDayAssignTimeEventList(const char *filename, char *sDayEventList,int direction,unsigned int filecounts);
char* sGetRecordFullName(const char *sFileName, char *sFullName);
int   sDelRecord(const char *sFileName);
int   getMaxWriteSize();
int   setMaxWriteSize(int prams);

void  ChildDirSureSDCanWrite();
unsigned int GetDiskInfo_Usable();

///////////////////////////////////////////////////////////
char* Mux_open(const char *fileName);
int   Mux_close(char* Fileindex, char *fileName);
int   Mux_write(char* Fileindex,const void *data,unsigned int dataSize);
int   Mux_read(char* Fileindex,int offset,void *data,unsigned int dataSize);
int   Mux_Get_Sd_Remove_Flag();//1->sd卡被移除
int   Mux_Get_Sd_Format_Flag();//1->sd卡正在格式化
int   Mux_SetTimeStamp(char* fd,unsigned int start_or_end,unsigned int time_stamp);
char* Mux_GetFdFromTime(char* lastFd,char *filename,unsigned int timestamp,unsigned int *time_lag,unsigned int *first_timestamp);
char* Mux_GetAllRecordFileTime(char* lastFd,unsigned int start_time,unsigned int end_time,RECORD_LIST *record_list);
char* Mux_GetAllAlarmRecordFileTime(char* lastFd,unsigned int start_time,unsigned int end_time,RECORD_LIST *record_list);
char* Mux_RefreshRecordList(unsigned int reference_time,char* lastFd,RECORD_LIST *record_list);
char* Mux_RefreshAlarmRecordList(unsigned int reference_time,char* lastFd,RECORD_LIST *record_list);
int   Mux_SetLastFileAlarmType(char *CurrFp,int AlarmType,int flag);
int   Mux_SetTimeStamp(char* fd,unsigned int start_or_end,unsigned int time_stamp);
int   Mux_Get_File_Size(const char* file_name);
unsigned int Mux_Get_Oldest_Time();
long long	 Mux_lseek(int Fileindex,unsigned int offset,unsigned int whence);

///////////////////////////////////////////////////////////
void *Gos_DiskManager_proc(void *p);


#endif

