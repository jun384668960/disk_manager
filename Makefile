INC_DIR := disk_manager mp4_mux utils
INC := $(patsubst %,-I%/include,$(INC_DIR))

SRC := $(wildcard *.cpp *.c $(patsubst %,%/*.cpp,$(INC_DIR)) $(patsubst %,%/*.c,$(INC_DIR)))
OBJ := $(patsubst %.cpp,%.obj,$(patsubst %.c,%.o,$(SRC)))
DEP := $(patsubst %.obj,%.dep,$(patsubst %.o,%.d,$(OBJ)))

COMPILE_PREFIX := arm-hisiv300-linux-
CC := $(COMPILE_PREFIX)gcc
CFLAGS := $(INC) -O0 -DBSD=1 -fPIC
#CFLAGS := $(INC) -O0 -Wall -DBSD=1 -fPIC
CXX := $(COMPILE_PREFIX)c++
CXXFLAGS := $(CFLAGS)
LINK := $(COMPILE_PREFIX)c++ -o
LIBRARY_LINK := $(COMPILE_PREFIX)c++ -shared -o
LIBRARY_LINK_STATIC := $(COMPILE_PREFIX)ar cr
LIBRARY_LINK_OPTS =	
STRIP := $(COMPILE_PREFIX)strip

TARGET := libmp4lib.a

.PHONY : all clean

all : $(TARGET)

%.o : %.c
	$(CC) $(CFLAGS) -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -c $< -o $@

%.obj : %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -MF"$(@:%.obj=%.dep)" -MT"$(@:%.obj=%.dep)" -c $< -o $@

$(TARGET) : $(OBJ)
	$(LIBRARY_LINK_STATIC) $@ $^

clean:
	@rm -rf $(OBJ) $(DEP) $(TARGET)

sinclude $(DEP)

