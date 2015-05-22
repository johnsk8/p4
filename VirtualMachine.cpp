/*  A virtual machine for ECS 150 with memory pools and FAT 16 functionality
    Filename: VirtualMachine.cpp
    Authors: John Garcia, Felix Ng

    In this version:
    VMStart -                           starting
    VMDirectoryOpen -                   not started      
    VMDirectoryClose -                  not started
    VMDirectoryRead -                   not started
    VMDirectoryRewind -                 not started
    VMDirectoryCurrent -                not started
    VMDirectoryChange -                 not started
    Threads Create/Delete -             NEEDS TO BE FIXED, NOT ALLOCATING PROPERLY

    In order to remove all system V messages: 
    1. ipcs //to see msg queue
    2. type this in cmd line: ipcs | grep q | awk '{print "ipcrm -q "$2""}' | xargs -0 bash -c
    3. ipcs //should be clear now

    In order to kill vm exec: killall -9 vm
*/

#include "VirtualMachine.h"
#include "Machine.h"
#include <vector>
#include <queue>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <iostream>
extern const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 1;
using namespace std;

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
}; //clas MPB - Memory Pool Block

//***************************************************************************//
//Global Variables & Functions
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

queue<TCB*> highPrio; //high priority queue
queue<TCB*> normPrio; //normal priority queue
queue<TCB*> lowPrio; //low priority queue

vector<TCB*> sleepList; //sleeping threads
vector<MB*> mutexSleepList; //sleeping mutexs

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

/*int main(int argc, char *argv[])
{
    //Given defaults
    int TicksMS = 100;
    int machineTicks = 100;
    TVMMemorySize heapSize = 0x1000000; //16 MiB
    TVMMemorySize sharedSize = 0x4000; //16 KiB
    char *fatMount = "fat.ima";

    int i = 1;
    while(i < argc)
    {
        if(strcmp(argv[i], "-t") == 0)
        {
            i++;
            if(i >= argc)
                break;

            if(sscanf(argv[i], "%d", &TicksMS) != 1) //read and make sure we read ticks if any in arg
            {
                fprintf(stderr, "Invalid parameter for -t of \"%s\".\n", argv[i]);
                return 1;
            }

            if(TicksMS <= 0) //make sure ticks are positive and not zero
            {
                fprintf(stderr, "Invalid parameter for -t. It must be positive.\n"); 
                return 1;
            }
        } //-t

        else if(strcmp(argv[i], "-m") == 0)
        {
            i++;
            if(i >= argc)
                break;

            if(sscanf(argv[i], "%d", &machineTicks) != 1) //ensures we read the machine ticks if any in arg
            {
                fprintf(stderr, "Invalid parameter for -m of \"%s\".\n", argv[i]);    
                return 1;
            }

            if(machineTicks <= 0) //make sure machine ticks are positive and not zero
            {
                fprintf(stderr, "Invalid parameter for -m. It must be positive.\n");    
                return 1;
            }
        } //-m

        else if(strcmp(argv[i], "-h") == 0)
        {
            i++;
            if(i >= argc)
                break;

            if(sscanf(argv[i], "%u", &heapSize) != 1) //ensures we read the heap size if any in arg
            {
                fprintf(stderr, "Invalid parameter for -h of \"%s\".\n", argv[i]);    
                return 1;
            }

            if(heapSize <= 0) //make sure heap size is positive and not zero
            {
                fprintf(stderr, "Invalid parameter for -h. It must be positive.\n");    
                return 1;
            }
        } //-h

        else if(strcmp(argv[i], "-s") == 0)
        {
            i++;
            if(i >= argc)
                break;

            if(sscanf(argv[i], "%u", &sharedSize) != 1) //ensures we read the shared size if any in arg
            {
                fprintf(stderr, "Invalid parameter for -s of \"%s\".\n", argv[i]);    
                return 1;
            }

            if(sharedSize <= 0) //make sure shared size is positive and not zero
            {
                fprintf(stderr, "Invalid parameter for -s. It must be positive.\n");    
                return 1;
            }
        } //-s

        else if(strcmp(argv[i], "-f") == 0)
        {
            i++;
            if(i >= argc)
                break;
            fatMount = argv[i]; //we got the fat file now
        } //-f

        else //nothing else after first arg so get out
            break;
    } //go through the arguments, read, and start

    if(i >= argc) //check to see if we went over
    {
        fprintf(stderr,"Syntax Error!\n");    
        return 1;
    }

    if(VMStart(TicksMS, heapSize, machineTicks, sharedSize, fatMount, argc - i, argv + i) != VM_STATUS_SUCCESS)
    {
        fprintf(stderr, "Virtual Machine failed to start! Oh my.\n");    
        return 1;
    }
    return 0;
} //main()*/

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

    //Read First Sector
    //uint8_t* fileImageData;
    //MachineFileOpen(mount, O_RDWR, 0644, FileCallBack, currentThread); //call to open fat file
    //MachineFileRead(currentThread->fileResult, fileImageData, 512, FileCallback, currentThread);

    if(VMMain == NULL) //fail to load module check
        return VM_STATUS_FAILURE;

    else //load successful
    {
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
        
        VMMain(argc, argv); //call to vmmain
        return VM_STATUS_SUCCESS;
    }
} //VMStart()

//***************************************************************************//
//Directory Functions
//***************************************************************************//

TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor)
{return 0;} //VMDirectoryOpen()

TVMStatus VMDirectoryClose(int dirdescriptor)
{return 0;} //VMDirectoryClose()

TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent)
{return 0;} //VMDirectoryRead()

TVMStatus VMDirectoryRewind(int dirdescriptor)
{return 0;} //VMDirectoryRewind()

TVMStatus VMDirectoryCurrent(char *abspath)
{return 0;} //VMDirectoryCurrent()

TVMStatus VMDirectoryChange(const char *path)
{return 0;} //VMDirectoryChange()

//***************************************************************************//
//MemoryPool Functions
//***************************************************************************//

TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
{
    TMachineSignalState OldState; //local variable to suspend
    MachineSuspendSignals(&OldState); //suspend signals

    if(base == NULL || memory == NULL || size == 0) //invalid check
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t curr = 0;
    uint32_t slots = size/64  + (size % 64 > 0); //to know the amount of slots we need
    MPB *myMemPool = findMemoryPool(VM_MEMORY_POOL_ID_SYSTEM); //this is the main mem pool, head honcho

    for(int i = 0; i < myMemPool->MPsize/64 ; i++)
    {
        if(myMemPool->spaceMap[i] == 0) //if this slot empty, check neighboring slots for empty
        {           
            curr++;
            if(curr == slots) //if enough slots are open
            {     
                for(int j = 0 ; j < slots; j++)
                {
                    myMemPool->spaceMap[i - j] = memPoolList.size(); //size becomes poolID
                    curr = (i - j) * 64; //curr becomes offset of base
                }   
                break; //get out once youre done allocating spaces
            }
            continue; //move on if not there yet
        }
        curr = 0; //reset so we can find the next slot
    } //going through our map to find open slots to allocate memory

    MPB *newMemPool = new MPB;
    newMemPool->base = (uint8_t*)myMemPool->base + curr; // base gets mainMemPool base + offset
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

    //void *stack; //array of threads treated as a stack
    //VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, (uint32_t)memsize, &stack); //allocate pool for thread

    uint8_t *stack = new uint8_t[memsize]; //array of threads treated as a stack
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

    MachineFileOpen(filename, flags, mode, FileCallBack, currentThread);
    
    currentThread->threadState = VM_THREAD_STATE_WAITING; //set to wait
    Scheduler(); //now we schedule threads so that we can let other threads work

    *filedescriptor = currentThread->fileResult; //fd get the file result

    MachineResumeSignals(&OldState); //resume signals
    if(currentThread->fileResult < 0) //check for failure
        return VM_STATUS_FAILURE;
    return VM_STATUS_SUCCESS;
} //VMFileOpen()

TVMStatus VMFileClose(int filedescriptor)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

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

    uint32_t read = 0; //to keep track of how much we have read
    char *localData = new char[*length]; //local var to copy data to/from
    void *sharedBase; //temp address to allocate memory

    if(*length > 512)
    {
        VMMemoryPoolAllocate(0, 512, &sharedBase); //begin to allocate with 512 bytes
        for(uint32_t i = 0; i < *length/512; ++i)
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

    MachineResumeSignals(&OldState); //resume signals
    if(currentThread->fileResult < 0) //check for failure
        return VM_STATUS_FAILURE;

    return VM_STATUS_SUCCESS;
} //VMFileRead()

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    if(data == NULL || length == NULL) //invalid input
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t written = 0; //to keep track of how much we have written
    char *localData = new char[*length]; //local var to copy data to/from
    memcpy(localData, data, *length); //we cope first
    void *sharedBase; //temp address to allocate memory

    if(*length > 512)
    {
        VMMemoryPoolAllocate(0, 512, &sharedBase); //begin to allocate
        for(uint32_t i = 0; i < *length/512; ++i)
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

    MachineResumeSignals(&OldState); //resume signals
    return VM_STATUS_SUCCESS;
} //VMFileWrite() 

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
{
    TMachineSignalState OldState; //local variable to suspend signals
    MachineSuspendSignals(&OldState); //suspend signals

    MachineFileSeek(filedescriptor, offset, whence, FileCallBack, currentThread);

    currentThread->threadState = VM_THREAD_STATE_WAITING;
    Scheduler();

    *newoffset = currentThread->fileResult; //set newoffset to file result

    MachineResumeSignals(&OldState); //resume signals
    if(currentThread->fileResult < 0) //check for failure
        return VM_STATUS_FAILURE;
    return VM_STATUS_SUCCESS;
} //VMFileSeek()
} //extern "C"