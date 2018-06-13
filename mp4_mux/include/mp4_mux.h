#ifndef __MP4_MUX_H__
#define __MP4_MUX_H__

#ifndef MAX_COUNT_BUFFER
#define MAX_COUNT_BUFFER 16
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "mp4_types.h"

//void Storage_Manager_Main();
extern void *Gos_DiskManager_proc(void *p);
int MP4Mux_Open(const char *filename);
int MP4Mux_GetVideoInfo(U8* pData, UINT size, UINT framerate, AM_VIDEO_INFO* h264_info);
ERR MP4Mux_OnInfo (AM_VIDEO_INFO *pvInfo,AM_AUDIO_INFO *paInfo);
//int MP4Mux_GetRecordTime();
//int Mux_GetAllRecordFileTime();
int MP4Mux_WriteVideoData(unsigned char *buf, int framesize, unsigned int timestamp, int frameType);
int MP4Mux_WriteAudioData(unsigned char *buf, int framesize, unsigned int timestamp);
void MP4Mux_Close();
//extern int Mux_open(const char *fileStype);
//extern int Mux_write(int Fileindex,const void *data,unsigned int dataSize);
extern long long Mux_lseek(int Fileindex,unsigned int offset,unsigned int whence);
//extern int Mux_close(int Fileindex,const char *fileName);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif //__MP4_MUXER_H__
