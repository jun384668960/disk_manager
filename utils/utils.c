#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "utils.h"


int GetTimeStamp(const char *str_time)  
{  
    struct tm stm;  
    int iY, iM, iD, iH, iMin, iS;  
  
    memset(&stm,0,sizeof(stm));  
  
    sscanf(str_time,"%04d%02d%02d%02d%02d%02d",&iY,&iM,&iD,&iH,&iMin,&iS);
	
    stm.tm_year=iY-1900;  
    stm.tm_mon=iM-1;  
    stm.tm_mday=iD;  
    stm.tm_hour=iH;  
    stm.tm_min=iMin;  
    stm.tm_sec=iS;  
  
    printf("%d-%0d-%0d %0d:%0d:%0d\n", iY, iM, iD, iH, iMin, iS); 
  
    return mktime(&stm);  
}  


int CheckSdIsMount()
{
	if(StorageCheckSDExist())
	{
		if(sPopenCheckStringExist("cat /proc/mounts","/opt/httpServer/lighttpd/htdocs/sd") == 0)
			return 1;
		else
			return 0;
	}
	else
		return 0;
}

int StoragepopenRead(char *cmd)
{
    FILE *fp = NULL;
    if((fp = popen(cmd, "r")) == NULL)
    {
        printf("Fail to popen\n");
        return -1;
    }
    pclose(fp);

    return 0;
}

int StorageCheckSDExist()
{
    return (!access("/dev/mmcblk0", F_OK) || !access("/dev/mmcblk1", F_OK));
}

int sPopenCheckStringExist(char *cmd, char * cp_string)
{	
	if(NULL == cp_string)	
	{		
		return -1;	
	}		
	if(0 == strlen(cp_string))	
	{		
		return -1;	
	}	
	if(NULL == cmd)	
	{		
		return -1;	
	}		
	if(0 == strlen(cmd))	
	{		
		return -1;	
	}			    
	FILE *fp = NULL;    
	char buf[256]= {0};    
	if((fp = popen(cmd, "r")) == NULL)    
	{        
		return -1;    
	}       
	while(fgets(buf, sizeof(buf), fp) != NULL)    
	{		
		if(strstr(buf, cp_string))		
		{		    
			pclose(fp);			
			return 0;		
		}   
	}    
	pclose(fp);    
	return -1;
}

//øÏÀŸ≈≈–Ú
void QuickSort(int a[], int left, int right)
{
	int i = left;
	int j = right;
	int temp = a[i];
	if( left < right)
	{
		while(i < j)
		{
			while(a[j] <= temp && i < j)
			{
				j--;
			}
			a[i] = a[j];
			while(a[i] >= temp && i < j)
			{
				i++;
			}
			a[j] = a[i];
		}
		
		a[i] = temp;
		
		QuickSort(a, left, i - 1);
		QuickSort(a, j + 1, right);
	}
}

