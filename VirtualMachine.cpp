/*  A virtual machine for ECS 150 with memory pools and FAT 16 functionality
    Filename: VirtualMachine.cpp
    Authors: John Garcia, Felix Ng

    In this version:
    VMStart -                   done
    BPB sector -                done
    FAT sector -                done
    ROOT sector -               done
    VMDirectoryOpen -           done
    VMDirectoryClose -          done
    VMDirectoryRead -           done
    VMDirectoryRewind -         done
    VMDirectoryCurrent -        done
    VMDirectoryChange -         done
    VMFileOpen -                done
    VMFileClose -               done
    VMFileRead -                started
    VMFileWrite -               started
    VMFileSeek -                started
    Thread Create/Delete -     allocating shared mem

    In order to remove all system V messages: 
    1. ipcs //to see msg queue
    2. type this in cmd line: ipcs | grep q | awk '{print "ipcrm -q "$2""}' | xargs -0 bash -c
    3. ipcs //should be clear now

    In order to kill vm exec: killall -9 vm
*/

#include "VirtualMachine.h"
#include "Machine.h"
#include <vector>
#include <map>
#include <queue>
#include <fcntl.h>
#include <iostream>
#include <string.h>
#include <cstdio>
extern const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 1;
using namespace std;

//BPB
#define BPB_Size 36
#define BPB_NumFATS 2
#define ROOT_EntSz 32
#define BPB_BytsPerSec 2
#define BPB_BytsPerSecOffset 11
#define BPB_SecPerClus 1
#define BPB_SecPerClusOffset 13
#define BPB_RsvdSecCnt 2
#define BPB_RsvdSecCntOffset 14
#define BPB_RootEntCnt 2
#define BPB_RootEntCntOffset 17
#define BPB_TotSec16 2
#define BPB_TotSec16Offset 19
#define BPB_FATSz16 2
#define BPB_FATSz16Offset 22
#define BPB_TotSec32 4
#define BPB_TotSec32Offset 32

//DIR
#define ATTR_READ_ONLY      0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20
#define ATTR_LONG_NAME      0x0F //(ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

extern "C"
{
//***************************************************************************//
//Classes
//***************************************************************************//
class TCB
{
    public:
    TVMThreadID threadID; //to hold the threads ID
    TVMThreadPriority threadPrior; //for the threads priority
    TVMThreadState threadState; //for thread stack
    TVMMemorySize threadMemSize; //for stack size
    uint8_t *base; //this or another byte size type pointer for base of stack
    TVMThreadEntry threadEntry; //for the threads entry function
    void *vptr; //for the threads entry parameter
    SMachineContext SMC; //for the context to switch to/from the thread
    TVMTick ticker; //for the ticks that thread needs to wait
    int fileResult;//possibly need something to hold file return type
}; //class TCB - Thread Control Block

class MB
{
    public:
    TVMMutexID mutexID; //holds mutex ID
    TVMMutexIDRef mutexIDRef;
    TCB *ownerThread; //the owner for thread
    TVMTick ticker; //time
    queue<TCB*> highQ;
    queue<TCB*> medQ;
    queue<TCB*> lowQ;
}; //class MB - Mutex Block

class MPB
{
    public:
    TVMMemorySize MPsize; //size of memory pool
    TVMMemoryPoolID MPid; //memory pool id
    void *base; //pointer for base of stack
    uint8_t *spaceMap; //keep track of sizes and allocated spaces
}; //class MPB - Memory Pool Block

class BPB {
public:
    uint8_t *BPB; //first 512 bytes in first sector
    unsigned int FATSz;
    unsigned int ROOTSz;
    unsigned int bytesPerSector;
    unsigned int sectorsPerCluster;
    unsigned int reservedSectorCount;
    unsigned int rootEntityCount;
    unsigned int totalSectors16;
    unsigned int FATSz16;
    unsigned int totalSectors32;
}; //class BPB

class DirEntry {
public:
    uint8_t DLongFileName[VM_FILE_SYSTEM_MAX_PATH];
    uint8_t DShortFileName[VM_FILE_SYSTEM_SFN_SIZE];
    uint8_t DAttributes;
    uint16_t DIR_FstClusLO;
    uint32_t DSize;
    SVMDateTime DCreate;
    SVMDateTime DAccess;
    SVMDateTime DModify;
    uint32_t fd;
    int fdOffset;
};

class OpenDir {
public:
    vector<DirEntry*> entryList;
    vector<DirEntry*>::iterator entryItr;
    uint16_t dirDescriptor;
};
        
    //VirtualMachineUtils.c Functions
extern TVMStatus VMDateTime(SVMDateTimeRef curdatetime);
extern uint32_t VMStringLength(const char *str);
extern void VMStringCopy(char *dest, const char *src);
extern void VMStringCopyN(char *dest, const char *src, int32_t n);
extern void VMStringConcatenate(char *dest, const char *src);
extern TVMStatus VMFileSystemValidPathName(const char *name);
extern TVMStatus VMFileSystemIsRelativePath(const char *name);
extern TVMStatus VMFileSystemIsAbsolutePath(const char *name);
extern TVMStatus VMFileSystemGetAbsolutePath(char *abspath, const char *curpath, const char *destpath);
extern TVMStatus VMFileSystemPathIsOnMount(const char *mntpt, const char *destpath);
extern TVMStatus VMFileSystemDirectoryFromFullPath(char *dirname, const char *path);
extern TVMStatus VMFileSystemFileFromFullPath(char *filename, const char *path);
extern TVMStatus VMFileSystemConsolidatePath(char *fullpath, const char *dirname, const char *filename);
extern TVMStatus VMFileSystemSimplifyPath(char *simpath, const char *abspath, const char *relpath);
extern TVMStatus VMFileSystemRelativePath(char *relpath, const char *basepath, const char *destpath);


//***************************************************************************//
//Global Variables & Utility Functions
//***************************************************************************//

void pushThread(TCB*);
void pushMutex(MB*);
void Scheduler();
typedef void (*TVMMain)(int argc, char *argv[]); //function ptr
TVMMainEntry VMLoadModule(const char *module); //load module spec

TCB *idle = new TCB; //global idle thread
TCB *currentThread = new TCB; //global current running thread

vector<MB*> mutexList; //to hold mutexs
vector<TCB*> threadList; //global ptr list to hold threads
vector<MPB*> memPoolList; //global ptr list to hold memory pools
vector<uint16_t> FATTable; //global vector for fat tables

queue<TCB*> highPrio; //high priority queue
queue<TCB*> normPrio; //normal priority queue
queue<TCB*> lowPrio; //low priority queue

vector<TCB*> sleepList; //sleeping threads
vector<MB*> mutexSleepList; //sleeping mutexs

vector<DirEntry*> ROOT;
vector<OpenDir*> openDirList;
vector<DirEntry*> openFileList;
map<uint32_t, uint8_t*> loadedClus;

int FATfd;
BPB *BPB = new class BPB; //global fat var
unsigned int FirstRootSector;
unsigned int RootDirectorySectors;
unsigned int FirstDataSector;
unsigned int ClusterCount;

void AlarmCallBack(void *param, int result)
{
    //check threads if they are sleeping
    for(vector<TCB*>::iterator itr = sleepList.begin(); itr != sleepList.end(); ++itr)
    {
        if((*itr)->ticker > 0) //if still more ticks
            (*itr)->ticker--; //dec time
        else
        {
            (*itr)->threadState = VM_THREAD_STATE_READY; //set found thread to ready
            idle->threadState = VM_THREAD_STATE_WAITING; //set idle to wait
            pushThread(*itr); //place into its proper q
            sleepList.erase(itr); //remove it from sleep
            break;
        }
    }

    //check mutex if they are sleeping
    for(vector<MB*>::iterator itr = mutexSleepList.begin(); itr != mutexSleepList.end(); ++itr)
    {
        if((*itr)->ticker == VM_TIMEOUT_INFINITE) //if infinite, break iff ownerThread == NULL
        {
            if((*itr)->ownerThread == NULL)
            {
                idle->threadState = VM_THREAD_STATE_WAITING;
                pushMutex(*itr); //place into its proper mutex
                mutexSleepList.erase(itr); //remove it from sleep
                break;
            }
        } 

        else //finite
        {
            if((*itr)->ticker > 0 && (*itr)->ownerThread != NULL)
                (*itr)->ticker--; //dec time
            else
            {
                idle->threadState = VM_THREAD_STATE_WAITING;
                pushMutex(*itr);
                mutexSleepList.erase(itr);
                break;
            }
        }
    }
    Scheduler(); //make sure we schedule after call back
} //AlarmCallBack()

void FileCallBack(void *param, int result)
{ 
    ((TCB*)param)->fileResult = result; //store result aka fd
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    pushThread((TCB*)param);
} //FileCallBack()

void Skeleton(void* param)
{
    MachineEnableSignals();
    currentThread->threadEntry(param); //deal with thread
    VMThreadTerminate(currentThread->threadID); //terminate thread
} //Skeleton()

void idleFunction(void* TCBref)
{
    TMachineSignalState OldState; //a state
    MachineEnableSignals(); //start the signals
    while(1)
    {
        MachineSuspendSignals(&OldState);
        //usleep(10000);
        MachineResumeSignals(&OldState);
    } //this is idling while we are in the idle state
} //idleFunction()

void pushThread(TCB *myThread)
{
    if(myThread->threadPrior == VM_THREAD_PRIORITY_HIGH)
        highPrio.push(myThread); //push into high prio queue
    if(myThread->threadPrior == VM_THREAD_PRIORITY_NORMAL)
        normPrio.push(myThread); //push into norm prio queue
    if(myThread->threadPrior == VM_THREAD_PRIORITY_LOW)
        lowPrio.push(myThread); //push into low prio queue
} //pushThread()

void pushMutex(MB *myMutex)
{
    if(currentThread->threadPrior == VM_THREAD_PRIORITY_HIGH)
        myMutex->highQ.push(currentThread); //push into high q
    else if(currentThread->threadPrior == VM_THREAD_PRIORITY_NORMAL)
        myMutex->medQ.push(currentThread); //push into med q
    else if(currentThread->threadPrior == VM_THREAD_PRIORITY_LOW)
        myMutex->lowQ.push(currentThread); //push into low q
} //pushMutex()

TCB *findThread(TVMThreadID thread)
{
    for(vector<TCB*>::iterator itr = threadList.begin(); itr != threadList.end(); ++itr)
    {
        if((*itr)->threadID == thread)
            return (*itr); //thread does exist
    }
    return NULL; //thread does not exist
} //findThread()

MB *findMutex(TVMMutexID mutex)
{
    for(vector<MB*>::iterator itr = mutexList.begin(); itr != mutexList.end(); ++itr)
    {
        if((*itr)->mutexID == mutex)
            return *itr; //mutex exists
    }
    return NULL; //mutex does not exist
} //findMutex()

MPB *findMemoryPool(TVMMemoryPoolID memory)
{
    for(vector<MPB*>::iterator itr = memPoolList.begin(); itr != memPoolList.end(); ++itr)
    {
        if((*itr)->MPid == memory)
            return *itr; //memory id exists
    }
    return NULL; //memory id does not exist
} //findMemoryPool();

void removeFromMutex(TCB* myThread)
{
    //check and make sure not in any Mutex queues
    for(vector<MB*>::iterator itr = mutexList.begin(); itr != mutexList.end(); ++itr)
    {
        for(unsigned int i = 0; i < (*itr)->highQ.size(); i++)
        {
            if((*itr)->highQ.front() != myThread) //if not eq
                (*itr)->highQ.push((*itr)->highQ.front()); //then push into back if q
            (*itr)->highQ.pop(); //instead pop the found thread
        } //high q check

        for(unsigned int i = 0; i < (*itr)->medQ.size(); i++)
        {
            if((*itr)->medQ.front() != myThread)
                (*itr)->medQ.push((*itr)->medQ.front());
            (*itr)->medQ.pop();
        } //med q check

        for(unsigned int i = 0; i < (*itr)->lowQ.size(); i++)
        {
            if((*itr)->lowQ.front() != myThread)
                (*itr)->lowQ.push((*itr)->lowQ.front());
            (*itr)->lowQ.pop();
        } //low q check
    } //iterating through all mutex lists
} //removeFromMutex()

void Scheduler()
{
    if(currentThread->threadState == VM_THREAD_STATE_WAITING || 
        currentThread->threadState == VM_THREAD_STATE_DEAD)
    {
        TCB *newThread = new TCB;
        int flag = 0;
        if(!highPrio.empty())
        {
            newThread = highPrio.front();
            highPrio.pop();
            flag = 1;
        } //high prior check

        else if(!normPrio.empty())
        {
            newThread = normPrio.front();
            normPrio.pop();
            flag = 1;
        } //normal prior check

        else if(!lowPrio.empty())
        {
            newThread = lowPrio.front();
            lowPrio.pop();
            flag = 1;
        } //low prior check

        else
        {
            newThread = idle;
            flag = 1;
        } //instead just idle

        if(flag) //something in the queues
        {           
            TCB *oldThread = currentThread; //get cur threads tcb
            currentThread = newThread; //update current thread
            newThread->threadState = VM_THREAD_STATE_RUNNING; //set to running
            MachineContextSwitch(&(oldThread)->SMC, &(currentThread)->SMC); //switch contexts
        }
    } //if currentthread waiting or dead
} //Scheduler()

void scheduleMutex(MB *myMutex)
{
    if(myMutex->ownerThread == NULL) //check if no owner
    {
        if(!myMutex->highQ.empty())
        {
            myMutex->ownerThread = myMutex->highQ.front();
            myMutex->highQ.pop();
        } //high prior check

        else if(!myMutex->medQ.empty())
        {
            myMutex->ownerThread = myMutex->medQ.front();
            myMutex->medQ.pop();
        } //med prior check

        else if(!myMutex->lowQ.empty())
        {
            myMutex->ownerThread = myMutex->lowQ.front();
            myMutex->lowQ.pop();
        } //low prior check
    } //set owner to prior mutex 
} //scheduleMutex()

uint8_t* readSector(uint32_t sector) {
    void* sharedMem;
    uint8_t* sectorData;

    VMMemoryPoolAllocate(0, 512, &sharedMem);
    MachineFileSeek(FATfd, sector * 512, 0, FileCallBack, currentThread);
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler();

    MachineFileRead(FATfd, sharedMem, 512, FileCallBack, currentThread);
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler();

    sectorData = (uint8_t*)sharedMem;

    VMMemoryPoolDeallocate(0, &sharedMem);
    return sectorData;
}

uint8_t* readCluster(uint32_t dataCluster){
    uint32_t sector = FirstDataSector + (dataCluster - 2) * 2;
    uint8_t *clusterData = new uint8_t[1024];

    memcpy(clusterData, readSector(sector), 512);
    memcpy(&clusterData[512], readSector(sector + 1), 512);

    return clusterData;
}

int writeSector(uint32_t sector, uint8_t *sectorData) {
    void *sharedMem;
    VMMemoryPoolAllocate(0, 512, &sharedMem); //begin to allocate
    MachineFileSeek(FATfd, sector * 512, 0, FileCallBack, currentThread);
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler();

    memcpy(sharedMem, sectorData, 512); //mem copy from data to shared

    MachineFileWrite(FATfd, sharedMem, 512, FileCallBack, currentThread);
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler(); //now we schedule our threads

    VMMemoryPoolDeallocate(0, &sharedMem);
    return currentThread->fileResult;
}

void writeCluster(uint32_t dataCluster, uint8_t *clusterData) {
    uint32_t sector = FirstDataSector + (dataCluster - 2) * 2;

    writeSector(sector, clusterData);
    writeSector(sector, &clusterData[512]);
}

uint16_t* u8tou16(uint8_t *sector, uint32_t size){
    uint16_t *newArr = new uint16_t[size/2];
    for(uint32_t i = 0; i < size/2; i += 2)
        newArr[i/2] = sector[i+1] << 8 | sector[i];

    return newArr;
}

void toUpper(char *str) {
    do {
        if(*str >= 97 && *str <= 122)
            *str = *str - 32;
    } while(*str++);
}

SVMDateTime* parseDT(uint16_t rawDate, uint16_t rawTime) {
    SVMDateTime *newDT = new SVMDateTime;

    newDT->DYear    =  ((rawDate >> 9) & 0b01111111) + 1980;
    newDT->DMonth   =   (rawDate >> 5) & 0b00001111;
    newDT->DDay     =   (rawDate >> 0) & 0b00011111;

    newDT->DHour        =   (rawTime >> 11) & 0b00011111;
    newDT->DMinute      =   (rawTime >> 5)  & 0b00111111;
    newDT->DSecond      =  ((rawTime >> 0)  & 0b00011111) * 2;
    newDT->DHundredth   =   0;

    //cerr << (int)newDT->DMonth << "/" << (int)newDT->DDay << "/" << (int)newDT->DYear
    //     << " " << (int)newDT->DHour << ":" << (int)newDT->DMinute << ":" << (int)newDT->DSecond << endl;

    return newDT;
} // parseDT

unsigned int bytesToUnsigned(uint8_t* BPB, uint16_t offset, uint16_t size)
{
    unsigned int unsignedAccum = 0;
    for(unsigned int i = 0; i < size; i++)
        unsignedAccum += ((unsigned int)BPB[offset + i] << (i*8));

    return unsignedAccum;
} //bytesToUngned()

void dumpBPB() {
    cout << "BPB_BytsPerSec: " << BPB->bytesPerSector << endl;
    cout << "BPB_SecPerClus: " << BPB->sectorsPerCluster << endl;
    cout << "BPB_RsvdSecCnt: " << BPB->reservedSectorCount << endl;
    cout << "BPB_RootEntCnt: " << BPB->rootEntityCount << endl;
    cout << "BPB_TotSec16: " << BPB->totalSectors16 << endl;
    cout << "BPB_FATSz16: " << BPB->FATSz16 << endl;
    cout << "BPB_TotSec32: " << BPB->totalSectors32 << endl;
    cout << "FATSz16: " << BPB->FATSz << endl;
    cout << "ROOTSz16: " << BPB->ROOTSz << endl;
    cout << "FirstRootSector: " << FirstRootSector << endl;
    cout << "RootDirectorySectors: " << RootDirectorySectors << endl;
    cout << "FirstDataSector: " << FirstDataSector << endl;
    cout << "ClusterCount: " << ClusterCount << endl;
} // dumpBPB()

void dumpSector(uint8_t *sector, int width) {
    for(int j = 0; j < width; ++j) fprintf(stderr, "%2d ", j); printf("\n");
    for(int j = 0; j < 512; ++j) fprintf(stderr, "%02X ", sector[j]);
}

void dumpCluster(uint8_t *cluster, int width) {
    for(int j = 0; j < width; ++j) fprintf(stderr, "%2d ", j); printf("\n");
    for(int j = 0; j < 1024; ++j) fprintf(stderr, "%02X ", cluster[j]);
}

void dumpFAT() {    // dump FAT table
    for(int j = 0; j < 16; ++j) printf("%4d ", j); printf("\n");
    for(int j = 0; j < 256; ++j) printf("%04X ", FATTable[j]);
    fflush(stdout);
} // dumpFAT()

void dumpROOT() {
    int j = 0;
    for(vector<DirEntry*>::iterator itr = ROOT.begin(); itr != ROOT.end(); ++itr, ++j) {
        printf("%2d %8s attr: %02X clus: %04X size: %d\n", j, (*itr)->DShortFileName, 
            (*itr)->DAttributes, (*itr)->DIR_FstClusLO, (*itr)->DSize);
    }
}

int parseDirEnt(uint32_t sector, vector<DirEntry*> *outDirEnt) {
    uint8_t *rootSector = readSector(sector);   //dumpSector(rootSector, 32);

    for(uint32_t secOffset = 0; secOffset < 512; secOffset += 32) {      // 16 entries per sector
        if(rootSector[secOffset] == 0x00)        // stop, no more entries
            return -1;
        if(rootSector[secOffset + 11] == ATTR_LONG_NAME)    // skip longfile names
            continue;

        DirEntry *newEntry = new DirEntry;
        
        VMStringCopy((char*)newEntry->DLongFileName, "");
        VMStringCopyN((char*)newEntry->DShortFileName, (char*)&rootSector[secOffset], 11);  // store filename (short)
        newEntry->DSize = rootSector[secOffset + 31] << 24 | rootSector[secOffset + 30] 
            << 16 | rootSector[secOffset + 29] << 8 | rootSector[secOffset + 28];
        newEntry->DAttributes = rootSector[secOffset + 11];        // store attribute
        newEntry->DCreate.DHundredth = rootSector[secOffset + 13];
        newEntry->DCreate = *parseDT(rootSector[secOffset + 17] 
            << 8 | rootSector[secOffset + 16], rootSector[secOffset + 15] << 8 | rootSector[secOffset + 14]);
        newEntry->DAccess = *parseDT(rootSector[secOffset + 19] << 8 | rootSector[secOffset + 18], 0);
        newEntry->DModify = *parseDT(rootSector[secOffset + 25] 
            << 8 | rootSector[secOffset + 24], rootSector[secOffset + 23] << 8 | rootSector[secOffset + 22]);

        newEntry->DIR_FstClusLO = rootSector[secOffset + 27] << 8 | rootSector[secOffset + 26]; // store fstClusLO

        outDirEnt->push_back(newEntry);    // save

    } // for each entry

    return 1;
}

void dismountFAT()
{
    cerr << "writing clusters" << endl;
    // for every modified cluster, write out
    for(map<uint32_t, uint8_t*>::iterator itr = loadedClus.begin(); itr != loadedClus.end(); ++itr)
    {   
        writeCluster(itr->first, itr->second);
    }

    cerr << "writing FAT" << endl;
    //update FAT table
    int j = 0;
    uint32_t sector = 1;
    uint8_t *fatSector = new uint8_t[512];
    for(vector<uint16_t>::iterator itr = FATTable.begin(); itr != FATTable.end(); ++itr)
    {
        fatSector[j++] = (*itr) & 0xFF;
        fatSector[j++] = (*itr) >> 8;

        if(j == 512)
        {
            j = 0;
            writeSector(sector, fatSector);
            ++sector;
        }
    }

    //update ROOT dir
    /*for(uint32_t i = 0; i < RootDirectorySectors; ++i) {
            uint32_t sector = i + FirstRootSector;       // starts at

            if(writeDirEnt(sector, &ROOT) == -1)
                break;
    }*/
    cerr << "closing fd" << endl;
} 

//***************************************************************************//
//The Virtual Machine Starter!
//***************************************************************************//

TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, 
    TVMMemorySize sharedsize, const char *mount, int argc, char *argv[])
{
    TVMMain VMMain = VMLoadModule(argv[0]); //load the module
    uint8_t *sharedBase = (uint8_t*)MachineInitialize(tickms, sharedsize); //initialize machine
    useconds_t usec = tickms * 1000; //usec in microseconds
    MachineRequestAlarm(usec, (TMachineAlarmCallback)AlarmCallBack, NULL); //starts the alarm tick
    MachineEnableSignals(); //start the signals


    if(VMMain == NULL) //fail to load module check
        return VM_STATUS_FAILURE;

    //THREADS
    uint8_t *stack = new uint8_t[0x100000]; //array of threads treated as a stack
    idle->threadID = 0; //idle thread first in array of threads
    idle->threadState = VM_THREAD_STATE_DEAD;
    idle->threadPrior = VM_THREAD_PRIORITY_LOW;
    idle->threadEntry = idleFunction;
    idle->base = stack;
    MachineContextCreate(&(idle)->SMC, Skeleton, NULL, stack, 0x100000); //context for idle

    TCB *VMMainTCB = new TCB; //start main thread
    VMMainTCB->threadID = 1; //main is second in array of threads
    VMMainTCB->threadPrior = VM_THREAD_PRIORITY_NORMAL;
    VMMainTCB->threadState = VM_THREAD_STATE_RUNNING;
    currentThread = VMMainTCB; //current thread is now main

    threadList.push_back(idle); //push into pos 0
    threadList.push_back(VMMainTCB); //push into pos 1

    //MEMORY POOLS
    MPB *sharedMPB = new MPB;
    sharedMPB->MPsize = sharedsize; 
    sharedMPB->MPid = 0; //shared pool id is 0
    sharedMPB->base = sharedBase; //allocate for sharedsize
    sharedMPB->spaceMap = new uint8_t[sharedsize/64]; //NOT DONE

    uint8_t *base = new uint8_t[heapsize];
    MPB *VMMainMPB = new MPB;
    VMMainMPB->MPsize = heapsize; 
    VMMainMPB->MPid = VM_MEMORY_POOL_ID_SYSTEM; //mem pool id is 1
    VMMainMPB->base = base; //allocate for heapsize
    VMMainMPB->spaceMap = new uint8_t[heapsize/64 + (heapsize % 64 > 0)]; //map creation

    memPoolList.push_back(sharedMPB); //push sharedmemblock into poolList[0]
    memPoolList.push_back(VMMainMPB); //push main into mem pool list[1]
    
    //FAT STUFF
    MachineFileOpen(mount, O_RDWR, 0644, FileCallBack, currentThread); //call to open fat file
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler();
    
    FATfd = currentThread->fileResult;  // fileResult holds fd

    BPB->BPB = readSector(0);       // sector 0 has BPB
    BPB->bytesPerSector = bytesToUnsigned(BPB->BPB, BPB_BytsPerSecOffset, BPB_BytsPerSec);
    BPB->sectorsPerCluster = bytesToUnsigned(BPB->BPB, BPB_SecPerClusOffset, BPB_SecPerClus);
    BPB->reservedSectorCount = bytesToUnsigned(BPB->BPB, BPB_RsvdSecCntOffset, BPB_RsvdSecCnt);
    BPB->rootEntityCount = bytesToUnsigned(BPB->BPB, BPB_RootEntCntOffset, BPB_RootEntCnt);
    BPB->totalSectors16 = bytesToUnsigned(BPB->BPB, BPB_TotSec16Offset, BPB_TotSec16);
    BPB->FATSz16 = bytesToUnsigned(BPB->BPB, BPB_FATSz16Offset, BPB_FATSz16);
    BPB->totalSectors32 = bytesToUnsigned(BPB->BPB, BPB_TotSec32Offset, BPB_TotSec32);
    BPB->FATSz = BPB_NumFATS * BPB->FATSz16;
    BPB->ROOTSz = BPB->rootEntityCount * ROOT_EntSz / 512; //handout said divide by 512

    FirstRootSector = BPB->reservedSectorCount + BPB_NumFATS * BPB->FATSz16;
    RootDirectorySectors = (BPB->rootEntityCount * 32)/512;
    FirstDataSector = FirstRootSector + RootDirectorySectors;
    ClusterCount = (BPB->totalSectors32 - FirstDataSector)/BPB->sectorsPerCluster;
    //dumpBPB();

    // for all FAT sectors
    for(uint32_t i = 0; i < BPB->FATSz16; ++i) {
        uint32_t sector = i + 1;
        uint8_t *fatSector = readSector(sector); // convert to 2 byte

        for(int j = 0; j < 256; j += 2)
            FATTable.push_back(fatSector[j+1] << 8 | fatSector[j]);           // store raw fat table
    }
    //dumpFAT();

    // for all root sectors
    for(uint32_t i = 0; i < RootDirectorySectors; ++i) {
        uint32_t sector = i + FirstRootSector;       // starts at

        if(parseDirEnt(sector, &ROOT) == -1)
            break;
    }   
    //dumpROOT();

    VMMain(argc, argv); //call to vmmain
    dismountFAT(); //DISMOUNT FILE******
    return VM_STATUS_SUCCESS;
} //VMStart()

//***************************************************************************//
//Directory Functions
//***************************************************************************//

TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(dirname == NULL || dirdescriptor == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    if(strcmp(dirname, "/") == 0) {
        OpenDir *newDir = new OpenDir;

        newDir->entryList = ROOT;
        newDir->entryItr = newDir->entryList.begin();
        newDir->dirDescriptor = *dirdescriptor = openDirList.size() + 3; // return dirdes
        
        openDirList.push_back(newDir);

        MachineResumeSignals(&OldState); //resume signals
        return VM_STATUS_SUCCESS;
    } // if root directory

    /*
    // look through root directory for this directory.
        for(vector<DirEntry*>::iterator itr = ROOT.begin(); itr != ROOT.end(); ++itr) {
                cerr << "location: " << hex << (*itr)->DIR_FstClusLO << endl;
        }
    
        DirEntry *newDir = new DirEntry;
        newDir->dirDescriptor = *dirdescriptor = openDirList.size() + 3;        // return dirdes
        newDir->pos = 0;

        openDirList.push_back(newDir);

    //if failure VM_STATUS_FAILURE
    */

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryOpen()

TVMStatus VMDirectoryClose(int dirdescriptor)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    // search for dirdescriptor
    for(vector<OpenDir*>::iterator itr = openDirList.begin(); itr != openDirList.end(); ++itr){
        if((*itr)->dirDescriptor == dirdescriptor) {
            openDirList.erase(itr);
            return VM_STATUS_SUCCESS;
        }
    }

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_FAILURE;
} //VMDirectoryClose()

TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(dirent == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    OpenDir *currDir = NULL;    //find dirdescriptor in open dir
    for(vector<OpenDir*>::iterator itr = openDirList.begin(); itr != openDirList.end(); ++itr) {
        if((*itr)->dirDescriptor == dirdescriptor) {
            currDir = *itr;
            break;
        }
    }

    vector<DirEntry*>::iterator itr = currDir->entryItr;
    DirEntry* currDirEntry = *itr;

    if(currDir->entryItr == currDir->entryList.end() || currDir == NULL)
        return VM_STATUS_FAILURE;

    // output
    VMStringCopy(dirent->DLongFileName, (char*)currDirEntry->DLongFileName);
    VMStringCopy(dirent->DShortFileName, (char*)currDirEntry->DShortFileName);
    dirent->DSize = currDirEntry->DSize;
    dirent->DAttributes = currDirEntry->DAttributes;
    dirent->DCreate = currDirEntry->DCreate;
    dirent->DAccess = currDirEntry->DAccess;
    dirent->DModify = currDirEntry->DModify;

    currDir->entryItr++;        // keep track

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryRead()

TVMStatus VMDirectoryRewind(int dirdescriptor)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    DirEntry *curDir = NULL;
    for(vector<DirEntry*>::iterator itr = openFileList.begin(); itr != openFileList.end(); ++itr)
    {
        if((*itr)->fd == dirdescriptor)
        {
            curDir = *itr;
            break;
        }
    } //loop to find dirdescriptor in open open files

    if(curDir == NULL)
        return VM_STATUS_FAILURE;

    curDir->fdOffset = 0; //reset here, place dir ptr back to beginning of the dir

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryRewind()

TVMStatus VMDirectoryCurrent(char *abspath)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(abspath == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    VMStringCopy(abspath, "/");

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryCurrent()

TVMStatus VMDirectoryChange(const char *path)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    char abspath[64], curpath[64];

    VMDirectoryCurrent(curpath);
    VMFileSystemGetAbsolutePath(abspath, curpath, path);

    if(strcmp(abspath, "/") != 0)
        return VM_STATUS_FAILURE;

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryChange()

TVMStatus VMDirectoryCreate(const char *dirname)        //  EXTRA CREDIT
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals
    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryCreate

TVMStatus VMDirectoryUnlink(const char *path)           // EXTRA CREDIT
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals
    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMDirectoryUnlink

//***************************************************************************//
//MemoryPool Functions
//***************************************************************************//

TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
{
    TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signals

    if(base == NULL || memory == NULL || size == 0) //invalid check
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MPB *newMemPool = new MPB;
    newMemPool->base = (uint8_t*)base; // base gets mainMemPool base + offset
    newMemPool->MPid = *memory = memPoolList.size(); //gets next size in list val
    newMemPool->MPsize = size;
    newMemPool->spaceMap = new uint8_t[size/64];
    memPoolList.push_back(newMemPool); //push it into the list of mem pools

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMMemoryPoolCreate()

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
{
    TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signals

    MPB *myMemPool = findMemoryPool(memory);
    if(myMemPool == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER; //mem does not exist

    vector<MPB*>::iterator itr;
    for(itr = memPoolList.begin(); itr != memPoolList.end(); ++itr)
    {
        if((*itr) == myMemPool) //specified mem pool does exist
        {
            for(uint32_t i = 0; i < myMemPool->MPsize/64; i++)
            {
                if(myMemPool->spaceMap[i] != 0)
                    return VM_STATUS_ERROR_INVALID_STATE; //theres something in there so cant
            }
            break; //then its empty and its okay to delete
        }
    } //iterate through list of memory pool

    memPoolList.erase(itr); //erase this from the memory pool

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMMemoryPoolDelete()

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
{
    TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signals

    MPB *myMemPool = findMemoryPool(memory);

    if(myMemPool == NULL || bytesleft == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER; //mem does not exist

    uint32_t freeCount = 0;

    for(uint32_t i = 0; i < myMemPool->MPsize/64; i++)
    {
        if(myMemPool->spaceMap[i] == 0) //if space available, then add it to chunks available       
            freeCount++; //update chunk available
    }

    *bytesleft = freeCount * 64; //return bytesleft

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMMemoryPoolQuery()

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
{
    TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signals

    MPB *myMemPool = findMemoryPool(memory);
    if(myMemPool == NULL || size == 0 || pointer == NULL) 
        return VM_STATUS_ERROR_INVALID_PARAMETER; //mem does not exist
    
    uint32_t slots = size/64 + (size % 64 > 0); //number of slots to allocate for
    uint32_t curr = 0; //offset

    for(uint32_t i = 0; i < (myMemPool->MPsize/64); i++)
    {
        if(myMemPool->spaceMap[i] == 0) //if this slot empty, check neighboring slots for empty
        {
            curr++;
            if(curr == slots) //enough slots are open
            {
                for(uint32_t j = 0 ; j < slots; j++)
                {
                    myMemPool->spaceMap[i - j] = slots; //place this at those slots open
                    curr = (i - j) * 64; //gives the position mapped to memory pool
                }

                *pointer = (uint8_t*)myMemPool->base + curr; //pointer now mapped to base plus offset
                MachineResumeSignals(&OldState); //resume signals
                return VM_STATUS_SUCCESS; //we allocated so we are done
            }
            continue; //move on if not there yet
        }
        curr = 0; //reset so we can find the next slot
    } //going through our map to find open slots to allocate memory

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
} //VMMemoryPoolAllocate()

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
{
   TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signalsMPB *myMemPool = findMemoryPool(memory);

    MPB *myMemPool = findMemoryPool(memory);
    if(myMemPool == NULL || pointer == NULL) 
        return VM_STATUS_ERROR_INVALID_PARAMETER; //mem does not exist

    //compare the ptr to base of mem pool
    uint32_t offset = *(uint8_t*)&pointer - *(uint8_t*)&myMemPool->base; //allocated part begins here
    uint32_t slots = myMemPool->spaceMap[offset/64]; //use offset to find out which spacemap slot we have to read

    for(uint32_t i = 0; i < slots; i++)
    {
        if(myMemPool->spaceMap[i + offset/64] == slots) //if exists
        {
            myMemPool->spaceMap[i + offset/64] = 0; //deallocate
            continue;
        }

        //if we went in here then the deallocation was a failure
        MachineResumeSignals(&OldState); //resume signals
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMMemoryPoolDeallocate()

//***************************************************************************//
//Thread Functions
//***************************************************************************//

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, 
    TVMThreadPriority prio, TVMThreadIDRef tid)
{
    TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signals

    if(entry == NULL || tid == NULL) //invalid
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    void *stack; //array of threads treated as a stack
    VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, (uint32_t)memsize, &stack); //allocate pool for thread

    TCB *newThread = new TCB; //start new thread
    newThread->threadEntry = entry;
    newThread->threadMemSize = memsize;
    newThread->threadPrior = prio;
    newThread->base = (uint8_t*)stack;
    newThread->threadState = VM_THREAD_STATE_DEAD;
    newThread->threadID = *tid = threadList.size();
    threadList.push_back(newThread); //store new thread into next pos of list
    
    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMThreadCreate()

TVMStatus VMThreadDelete(TVMThreadID thread)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals
    
    TCB *myThread = findThread(thread);
    if(myThread == NULL) //thread dne
        return VM_STATUS_ERROR_INVALID_ID;
    if(myThread->threadState != VM_THREAD_STATE_DEAD) //dead check
        return VM_STATUS_ERROR_INVALID_STATE;       

    //VMMemoryPoolDeallocate(VM_MEMORY_POOL_ID_SYSTEM, myThread->base); //deallocate this thread from pool
    removeFromMutex(myThread); //check if in any mutexs

    vector<TCB*>::iterator itr;
    for(itr = threadList.begin(); itr != threadList.end(); ++itr)
    {
        if((*itr) == myThread)
            break;
    } //iterate through threads to find it

    threadList.erase(itr); //now erase it

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMThreadDelete()

TVMStatus VMThreadActivate(TVMThreadID thread)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    TCB *myThread = findThread(thread); //call to find the thread ptr
    if(myThread == NULL) //check if thread exists
        return VM_STATUS_ERROR_INVALID_ID;
    if(myThread->threadState != VM_THREAD_STATE_DEAD) //if not dead, error
        return VM_STATUS_ERROR_INVALID_STATE;

    MachineContextCreate(&(myThread)->SMC, Skeleton, (myThread)->vptr, 
        (myThread)->base, (myThread)->threadMemSize); //create context here
    myThread->threadState = VM_THREAD_STATE_READY; //set current thread to running

    pushThread(myThread); //place thread into its proper place
    Scheduler(); //now we schedule the threads

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMThreadActivate()

TVMStatus VMThreadTerminate(TVMThreadID thread)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    TCB *myThread = findThread(thread);
    if(myThread == NULL) //check if thread exists
        return VM_STATUS_ERROR_INVALID_ID;
    if(myThread->threadState == VM_THREAD_STATE_DEAD) //dead state check
        return VM_STATUS_ERROR_INVALID_STATE;

    myThread->threadState = VM_THREAD_STATE_DEAD; //set to dead here

    //check and make sure not in thread queue
    for(unsigned int i = 0; i < highPrio.size(); i++)
    {
        if(highPrio.front() != myThread) //if not eq
            highPrio.push(highPrio.front()); //then place thread in back of q
        highPrio.pop(); //otherwise its the thread and pop it
    } //high prior check

    for(unsigned int i = 0; i < normPrio.size(); i++)
    {
        if(normPrio.front() != myThread)
            normPrio.push(normPrio.front());
        normPrio.pop();
    } //normal prior check

    for(unsigned int i = 0; i < lowPrio.size(); i++)
    {
        if(lowPrio.front() != myThread)
            lowPrio.push(lowPrio.front());
        lowPrio.pop();
    } //low prior check

    removeFromMutex(myThread); //make sure not in any mutexs
    Scheduler(); //now we schedule

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMThreadTerminate()

TVMStatus VMThreadID(TVMThreadIDRef threadref)
{
    if(threadref == NULL) //invalid
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    *threadref = currentThread->threadID; //set to current id

    return VM_STATUS_SUCCESS; //successful retrieval
} //VMThreadID()

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
{
    if(stateref == NULL) //invalid
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    
    vector<TCB*>::iterator itr;
    for(itr = threadList.begin(); itr != threadList.end(); ++itr)
    {
        if((*itr)->threadID == thread)
        {
            *stateref = (*itr)->threadState; //assign thread state here
            return VM_STATUS_SUCCESS;
        }
    } //iterate through the entire thread list until found thread id
    
    return VM_STATUS_ERROR_INVALID_ID; //thread does not exist
} //VMThreadState()

TVMStatus VMThreadSleep(TVMTick tick)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(tick == VM_TIMEOUT_INFINITE) //invalid
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    currentThread->threadState = VM_THREAD_STATE_WAITING; //set to wait for sleep
    currentThread->ticker = tick; //set tick as globaltick

    sleepList.push_back(currentThread); //put cur thread into sleep list so sleep
    Scheduler(); //now we schedule

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS; //success sleep after reaches zero
} //VMThreadSleep()

//***************************************************************************//
//Mutex Functions
//***************************************************************************//

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals 

    if(mutexref == NULL) //invalid
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MB *newMutex = new MB;
    newMutex->mutexID = mutexList.size(); //new mutexs get size of list for next pos
    mutexList.push_back(newMutex); //push it into next pos
    *mutexref = newMutex->mutexID; //set to id

    MachineResumeSignals(&OldState);
    return VM_STATUS_SUCCESS;
} //VMMutexCreate()

TVMStatus VMMutexDelete(TVMMutexID mutex)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    MB *myMutex = findMutex(mutex);
    if(myMutex == NULL) //mutex does not exist
        return VM_STATUS_ERROR_INVALID_ID;
    if(myMutex->ownerThread != NULL) //if not unlocked
        return VM_STATUS_ERROR_INVALID_STATE;

    vector<MB*>::iterator itr;
    for(itr = mutexList.begin(); itr != mutexList.end(); ++itr)
    {
        if((*itr) == myMutex)
            break;
    } //iterate through mutex list until found

    mutexList.erase(itr); //erase mutex from list

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMMutexDelete()

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(ownerref == NULL) //invalid
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MB *myMutex = findMutex(mutex);
    if(myMutex == NULL)
        return VM_STATUS_ERROR_INVALID_ID;

    if(myMutex->ownerThread == NULL)
        return VM_THREAD_ID_INVALID;

    *ownerref = myMutex->ownerThread->threadID; //set to owner ref from owner

    MachineResumeSignals(&OldState);
    return VM_STATUS_SUCCESS;
} //VMMutexQuery()

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    MB *myMutex = findMutex(mutex);
    if(myMutex == NULL)
        return VM_STATUS_ERROR_INVALID_ID;

    pushMutex(myMutex); //place it into its proper q

    //block timeout
    myMutex->ticker = timeout; //set time
    if(myMutex->ticker == VM_TIMEOUT_IMMEDIATE && myMutex->ownerThread != NULL)
        return VM_STATUS_FAILURE;

    if(myMutex->ticker > 0)
    {
        currentThread->threadState = VM_THREAD_STATE_WAITING;
        mutexSleepList.push_back(myMutex); //into the mutex sleeping list
        Scheduler(); //now we schedule threads
    } //then we start counting down the ticks

    if(myMutex->ownerThread != NULL)
        return VM_STATUS_FAILURE;

    scheduleMutex(myMutex); //now we schedule mutexs

    MachineResumeSignals(&OldState);
    return VM_STATUS_SUCCESS;
} //VMMutexAcquire()

TVMStatus VMMutexRelease(TVMMutexID mutex)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    MB *myMutex = findMutex(mutex);
    if(myMutex == NULL)
        return VM_STATUS_ERROR_INVALID_ID;
    if(myMutex->ownerThread != currentThread)
        return VM_STATUS_ERROR_INVALID_STATE;

    myMutex->ownerThread = NULL; //release the owner id
    scheduleMutex(myMutex); //now we schedule mutex

    MachineResumeSignals(&OldState);
    return VM_STATUS_SUCCESS;
} //VMMutexRelease()

//***************************************************************************//
//File Functions
//***************************************************************************//

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(filename == NULL || filedescriptor == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    char absPath[64], currPath[64], fileN[64];
    VMDirectoryCurrent(currPath);                                   // should be root '/'
    VMFileSystemGetAbsolutePath(absPath, currPath, filename);       // if filename is like /blah/yada
    
    if(strrchr(absPath, '/') - absPath != 0)      // checks if more than one /
        return VM_STATUS_FAILURE;

    VMFileSystemFileFromFullPath(fileN, absPath);
    toUpper(fileN); // make case insensitive

    if(VMStringLength(fileN) > 11)              // fail if long name
        return VM_STATUS_FAILURE;

    char padding[13];
    VMStringCopyN(padding, "              ", 11 - VMStringLength(fileN));
    VMStringConcatenate(fileN, padding);
    //cerr << "looking for file " << fileN << " in " << "/" << endl;

    DirEntry *newFile = NULL;
    for(vector<DirEntry*>::iterator itr = ROOT.begin(); itr != ROOT.end(); ++itr){
        if(strcmp((char*)(*itr)->DShortFileName, fileN) == 0) {
            newFile = (*itr);
            //cerr << fileN << " found!" << endl;
            break;
        }
    }

    if(newFile == NULL) { 
        //cerr << "not found" << endl;
        if((flags & O_CREAT) == O_CREAT) {          // create if necessary
            //cerr << "creating" << endl;
            MachineFileOpen(filename, flags, mode, FileCallBack, currentThread);
            currentThread->threadState = VM_THREAD_STATE_WAITING; //set to wait
            Scheduler(); //now we schedule threads so that we can let other threads work

            if(currentThread->fileResult < 0) //check for failure
                return VM_STATUS_FAILURE;
            
            newFile = new DirEntry;

            VMStringCopy((char*)newFile->DShortFileName, fileN);
            VMDateTime(&newFile->DCreate);

            // search for an open cluster
            int i = 0;
            for(vector<uint16_t>::iterator itr = FATTable.begin(); itr != FATTable.end(); ++itr, ++i){
                if((*itr) == 0) {
                    *itr = 0xFFFF;
                    newFile->DIR_FstClusLO = i;
                    //cout << "fstClusLO: " << i << endl;
                    break;
                }
            } 

            ROOT.push_back(newFile);

        } else if(flags != O_CREAT)        // check O_CREAT, create
            return VM_STATUS_FAILURE;
    }

    if(newFile->DAttributes == ATTR_DIRECTORY)
        return VM_STATUS_FAILURE;


    VMDateTime(&newFile->DAccess);  // set access time
    newFile->fd = *filedescriptor = openFileList.size() + 3;
    openFileList.push_back(newFile);

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMFileOpen()

TVMStatus VMFileClose(int filedescriptor)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    DirEntry *myFile = new DirEntry;
    for(vector<DirEntry*>::iterator itr = openFileList.begin(); itr != openFileList.end(); ++itr){
        if((*itr)->fd == (uint32_t)filedescriptor) {
            myFile = (*itr);
            break;
        }
    }

    VMDateTime(&myFile->DModify); //update date/time modified
    myFile = NULL;

    MachineFileClose(filedescriptor, FileCallBack, currentThread);
    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler(); //now we schedule our threads

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMFileClose()

TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(data == NULL || length == NULL) //invalid input
        return VM_STATUS_ERROR_INVALID_PARAMETER;

        
    if(filedescriptor < 3) {        // IF FD < 3 DO OLD STUFF
            uint32_t read = 0; //to keep track of how much we have read
            char *localData = new char[*length]; //local var to copy data to/from
            void *sharedBase; //temp address to allocate memory

            if(*length > 512)
            {
                VMMemoryPoolAllocate(0, 512, &sharedBase); //begin to allocate with 512 bytes
                for(uint32_t i = 0; i < (uint32_t)*length/512; ++i)
                {
                    MachineFileRead(filedescriptor, sharedBase, 512, FileCallBack, currentThread);

                    currentThread->threadState = VM_THREAD_STATE_WAITING;
                    Scheduler();

                    memcpy(&localData[i * 512], sharedBase, 512);
                    read += currentThread->fileResult;   
                } //while we still have 512 bytes we will then read
                VMMemoryPoolDeallocate(0, sharedBase); //deallcate once we are done
            }

            //else length < 512 or we do the remaining bytes
            uint32_t remaining = *length - read; //for remainders of *length % 512
            VMMemoryPoolAllocate(0, remaining, &sharedBase);
           
            MachineFileRead(filedescriptor, sharedBase, remaining, FileCallBack, currentThread);
            currentThread->threadState = VM_THREAD_STATE_WAITING;
            Scheduler();

            memcpy(&localData[read], sharedBase, remaining);
            read += currentThread->fileResult;
            
            memcpy(data, localData, read);

            delete localData; //delete it once we are done using it
            VMMemoryPoolDeallocate(0, sharedBase);
            *length = read; //set length to what we have read

            if(currentThread->fileResult < 0) //check for failure
                return VM_STATUS_FAILURE;
    } // if fd < 3
    else {  // IF FD > 3 DO NEW STUFF WITH CLUSTERS

        // find existing opened file
            DirEntry *openFile = NULL;
            for(vector<DirEntry*>::iterator itr = openFileList.begin(); itr != openFileList.end(); ++itr) {
                if((*itr)->fd == (uint32_t)filedescriptor) {
                    openFile = (*itr);
                    break;
                }
            }

            if(openFile == NULL)        // fail if file doesnt exist
                return VM_STATUS_FAILURE;

            int read = openFile->fdOffset; //offset will be -1 if EOF
            if(read == -1) {
                openFile->fdOffset = 0;
                return  VM_STATUS_FAILURE;
            }

            uint32_t curCluster = openFile->DIR_FstClusLO + read;
            char *localData = new char[*length]; //local var to copy data to/from
            uint8_t *localClus = new uint8_t[*length];

            //fprintf(stderr, "\n\nLength: %d\n", *length);

            if(*length > 1024) {    // if larger than a cluster
                for(uint32_t i = 0; i < (uint32_t)*length/1024; ++i) {

                    if(loadedClus.count(curCluster))    // if previously loaded, use that
                        localClus = loadedClus[curCluster];
                    else
                        localClus = readCluster(curCluster);

                    memcpy(&localData[i * 1024], localClus, 1024);
                    curCluster++;
                    read += 1024;
                }
            }
            uint32_t remaining = *length - read;

            if(loadedClus.count(curCluster))    // if previously loaded, use that
                localClus = loadedClus[curCluster];
            else
                localClus = readCluster(curCluster);
            memcpy(&localData[read], localClus, remaining);

            read += remaining;
            memcpy(data, localData, read);

            *length = read; //set length to what we have read

            //fprintf(stderr, "CurCluster: %X\n", curCluster);
            //fprintf(stderr, "nexClus: %X\n", FATTable[curCluster]);

            //dumpCluster(localClus, 32);
            
            if(FATTable[curCluster++] >= 0xFFF8)
                openFile->fdOffset = -1;        //EOF
            else
                openFile->fdOffset += read / 1024;

            //fprintf(stderr, "openFile->fdOffset: %d\n\n", openFile->fdOffset);
    }   //if fd > 3

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMFileRead()

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(data == NULL || length == NULL) //invalid input
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    if(filedescriptor < 3) {
            uint32_t written = 0; //to keep track of how much we have written
            char *localData = new char[*length]; //local var to copy data to/from
            memcpy(localData, data, *length); //we cope first
            void *sharedBase; //temp address to allocate memory

            if(*length > 512)
            {
                VMMemoryPoolAllocate(0, 512, &sharedBase); //begin to allocate
                for(uint32_t i = 0; i < (uint32_t)*length/512; ++i)
                {
                    memcpy(sharedBase, &localData[i * 512], 512);
                    
                    MachineFileWrite(filedescriptor, sharedBase, 512, FileCallBack, currentThread);

                    currentThread->threadState = VM_THREAD_STATE_WAITING;
                    Scheduler();

                    if(currentThread->fileResult < 0)
                        return VM_STATUS_FAILURE;

                    written += currentThread->fileResult;
                } //while we still have 512 bytes we will then write
                VMMemoryPoolDeallocate(0, sharedBase); //deallocate this once we are done
            }

            //else length < 512 or we do the remaining bytes
            uint32_t remaining = *length - written; //for remainders of *length % 512
            VMMemoryPoolAllocate(0, remaining, &sharedBase);

            memcpy(sharedBase, &localData[written], remaining);

            MachineFileWrite(filedescriptor, sharedBase, remaining, FileCallBack, currentThread);
            currentThread->threadState = VM_THREAD_STATE_WAITING;
            Scheduler();

            if(currentThread->fileResult < 0)
                return VM_STATUS_FAILURE;

            written += currentThread->fileResult;

            delete localData; //delete this once we have written
            VMMemoryPoolDeallocate(0, sharedBase);
            *length = written; //set length to what we have written
    } //if fd < 3
    else {
            DirEntry *openFile = NULL;
            for(vector<DirEntry*>::iterator itr = openFileList.begin(); itr != openFileList.end(); ++itr) {
                if((*itr)->fd == (uint32_t)filedescriptor) {
                    openFile = (*itr);
                    break;
                }
            }

            if(openFile == NULL)        // fail if file doesnt exist
                return VM_STATUS_FAILURE;

            int written = openFile->fdOffset; //offset will be -1 if EOF
            if(written == -1) {
                openFile->fdOffset = 0;
                return  VM_STATUS_FAILURE;
            }

            uint32_t curCluster = openFile->DIR_FstClusLO + written;
            char *localData = new char[*length]; //local var to copy data to/from
            uint8_t *localClus = new uint8_t[*length];

            memcpy(localData, data, *length); //we copy first

            if(*length > 1024) {      // for every cluster
                for(uint32_t i = 0; i < (uint32_t)*length/1024; ++i) {
                    memcpy(&localClus[i * 1024], &localData[i * 1024], 1024);
                    loadedClus[curCluster] = &localClus[i * 1024];
                    written += 1024;
                }
            }
            uint32_t remaining = *length - written;

            memcpy(&localClus[written], &localData[written], remaining);
            loadedClus[curCluster] = &localClus[written];
            written += remaining;

            *length = written; //set length to what we have written
            openFile->fdOffset += written / 1024;
    }

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMFileWrite() 

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    //cout << "offset: " << offset << endl;

    if(filedescriptor < 3)
    {
        MachineFileSeek(filedescriptor, offset, whence, FileCallBack, currentThread);
        currentThread->threadState = VM_THREAD_STATE_WAITING;
        Scheduler();

        *newoffset = currentThread->fileResult; //set newoffset to file result
    }

    else //fd >= 3
    {   
        MachineFileSeek(filedescriptor, *newoffset, whence, FileCallBack, currentThread);
        currentThread->threadState = VM_THREAD_STATE_WAITING;
        Scheduler();

        DirEntry *myFile = new DirEntry;
        for(vector<DirEntry*>::iterator itr = openFileList.begin(); itr != openFileList.end(); ++itr)
        {
            if((*itr)->fd == (uint32_t)filedescriptor)
            {
                myFile = (*itr);
                break;
            }
        }

        if(whence == 0) //SEEK_SET
        {
            //cout << "whence is 0" << endl;
            *newoffset = offset; //The offset is set to offset bytes.
        }

        else if(whence == 1) //SEEK_CUR
        {   
            //cout << "whence is 1" << endl;
            *newoffset = myFile->fd + offset; //The offset is set to its current location plus offset bytes.
        }

        else if(whence == 2) //SEEK_END
        {
            //cout << "whence is 2" << endl;
            *newoffset = myFile->DSize + offset; //The offset is set to the size of the file plus offset bytes.
        }

        else
            *newoffset = currentThread->fileResult; //set newoffset to file result
    }

    MachineResumeSignals(&OldState); //resume signals
    if(currentThread->fileResult < 0) //check for failure
        return VM_STATUS_FAILURE;
    return VM_STATUS_SUCCESS;
} //VMFileSeek()
} //extern "C"