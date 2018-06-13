#ifndef _DISK_UTILS_H_
#define _DISK_UTILS_H_

int GetTimeStamp(const char *str_time);
int CheckSdIsMount();
int StoragepopenRead(char *cmd);
int StorageCheckSDExist();
int sPopenCheckStringExist(char *cmd, char * cp_string);
void QuickSort(int a[], int left, int right);
#endif