#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <queue>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

using namespace std;

const int MAX_PROCESSES = 20;
const int PAGE_SIZE = 1024;
const int NUM_PAGES = 16;
const int NUM_FRAMES = 64;

const long OSS_REPLY_TYPE = 1;

const unsigned int BILLION = 1000000000;
const unsigned int MEMORY_ACCESS_TIME = 100;
const unsigned int DISK_TIME = 14000000;
const unsigned int DIRTY_EXTRA_TIME = 14000000;
const unsigned int LOOP_INCREMENT = 10000000;

struct SimClock {
    unsigned int seconds;
    unsigned int nanoseconds;
};

struct Message {
    long mtype;
    int index;
    int address;
    int isWrite;
    int terminate;
};

struct PCB {
    bool occupied;
    bool blocked;
    bool messageOutstanding;
    pid_t pid;

    int pageTable[NUM_PAGES];

    int memoryReferences;
    int pageFaults;

    PCB() {
        occupied = false;
        blocked = false;
        messageOutstanding = false;
        pid = -1;
        memoryReferences = 0;
        pageFaults = 0;

        for (int i = 0; i < NUM_PAGES; i++) {
            pageTable[i] = -1;
        }
    }
};

struct Frame {
    bool occupied;
    int dirtyBit;
    int processIndex;
    int pageNumber;
    int generation;

    Frame() {
        occupied = false;
        dirtyBit = 0;
        processIndex = -1;
        pageNumber = -1;
        generation = 0;
    }
};

struct FIFOEntry {
    int frameNumber;
    int generation;
};

struct BlockedRequest {
    int processIndex;
    int address;
    int pageNumber;
    int isWrite;
    unsigned int readySec;
    unsigned int readyNano;
};

PCB processTable[MAX_PROCESSES];
Frame frameTable[NUM_FRAMES];

queue<FIFOEntry> fifoQueue;
queue<BlockedRequest> blockedQueue;

SimClock* simClock = nullptr;

int msgId = -1;
int shmId = -1;

ofstream logFile;

int totalLaunched = 0;
int activeChildren = 0;

int totalMemoryReferences = 0;
int totalReads = 0;
int totalWrites = 0;
int totalPageFaults = 0;

void cleanup();

void output(const string& text) {
    cout << text;
    if (logFile.is_open()) {
        logFile << text;
    }
}

void addTime(unsigned int ns) {
    simClock->nanoseconds += ns;

    while (simClock->nanoseconds >= BILLION) {
        simClock->seconds++;
        simClock->nanoseconds -= BILLION;
    }
}

bool timeReached(unsigned int currentSec, unsigned int currentNano,
                 unsigned int targetSec, unsigned int targetNano) {
    if (currentSec > targetSec) {
        return true;
    }

    if (currentSec == targetSec && currentNano >= targetNano) {
        return true;
    }

    return false;
}

void setFutureTime(unsigned int addNano,
                   unsigned int& futureSec,
                   unsigned int& futureNano) {
    futureSec = simClock->seconds;
    futureNano = simClock->nanoseconds + addNano;

    while (futureNano >= BILLION) {
        futureSec++;
        futureNano -= BILLION;
    }
}

void signalHandler(int sig) {
    cerr << "\noss: Caught signal " << sig << ". Cleaning up.\n";
    cleanup();
    exit(1);
}

void printHelp() {
    cout << "Usage:\n";
    cout << "./oss [-h] [-n proc] [-s simul] [-t timeLimitForChildren] "
         << "[-i fractionOfSecondToLaunchChildren] [-f logfile]\n";
}

void sendMessageToWorker(int index) {
    if (index < 0 || index >= MAX_PROCESSES) {
        return;
    }

    if (!processTable[index].occupied || processTable[index].blocked ||
        processTable[index].messageOutstanding) {
        return;
    }

    Message msg;
    msg.mtype = processTable[index].pid;
    msg.index = index;
    msg.address = 0;
    msg.isWrite = 0;
    msg.terminate = 0;

    if (msgsnd(msgId, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
        perror("oss msgsnd");
        return;
    }

    processTable[index].messageOutstanding = true;
}

void clearProcessMemory(int index) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frameTable[i].occupied && frameTable[i].processIndex == index) {
            frameTable[i].occupied = false;
            frameTable[i].dirtyBit = 0;
            frameTable[i].processIndex = -1;
            frameTable[i].pageNumber = -1;
            frameTable[i].generation++;
        }
    }

    for (int i = 0; i < NUM_PAGES; i++) {
        processTable[index].pageTable[i] = -1;
    }
}

int getFreeFrame() {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (!frameTable[i].occupied) {
            return i;
        }
    }

    return -1;
}

int getFIFOFrame() {
    while (!fifoQueue.empty()) {
        FIFOEntry entry = fifoQueue.front();
        fifoQueue.pop();

        int frame = entry.frameNumber;

        if (frameTable[frame].occupied &&
            frameTable[frame].generation == entry.generation) {
            return frame;
        }
    }

    return -1;
}

int loadPageIntoFrame(int processIndex, int pageNumber, int isWrite) {
    int frame = getFreeFrame();

    if (frame == -1) {
        frame = getFIFOFrame();

        if (frame == -1) {
            output("oss: ERROR - no frame available for replacement\n");
            return -1;
        }

        int oldProcess = frameTable[frame].processIndex;
        int oldPage = frameTable[frame].pageNumber;

        output("oss: Clearing frame " + to_string(frame) +
               " and swapping out P" + to_string(oldProcess) +
               " page " + to_string(oldPage) + "\n");

        if (frameTable[frame].dirtyBit == 1) {
            output("oss: Dirty bit of frame " + to_string(frame) +
                   " is set. Adding extra time to the clock.\n");

            addTime(DIRTY_EXTRA_TIME);
        }

        if (oldProcess >= 0 && oldProcess < MAX_PROCESSES &&
            oldPage >= 0 && oldPage < NUM_PAGES) {
            processTable[oldProcess].pageTable[oldPage] = -1;
        }
    }

    frameTable[frame].occupied = true;
    frameTable[frame].dirtyBit = isWrite ? 1 : 0;
    frameTable[frame].processIndex = processIndex;
    frameTable[frame].pageNumber = pageNumber;
    frameTable[frame].generation++;

    processTable[processIndex].pageTable[pageNumber] = frame;

    FIFOEntry entry;
    entry.frameNumber = frame;
    entry.generation = frameTable[frame].generation;
    fifoQueue.push(entry);

    output("oss: Swapping in P" + to_string(processIndex) +
           " page " + to_string(pageNumber) +
           " into frame " + to_string(frame) + "\n");

    return frame;
}

void printMemoryLayout() {
    output("\nCurrent memory layout at time " +
           to_string(simClock->seconds) + ":" +
           to_string(simClock->nanoseconds) + " is:\n");

    output("Frame\tOccupied\tDirtyBit\tProcess\tPage\n");

    for (int i = 0; i < NUM_FRAMES; i++) {
        string occupied = frameTable[i].occupied ? "Yes" : "No";

        output(to_string(i) + "\t" +
               occupied + "\t\t" +
               to_string(frameTable[i].dirtyBit) + "\t\t" +
               to_string(frameTable[i].processIndex) + "\t" +
               to_string(frameTable[i].pageNumber) + "\n");
    }

    output("\nPage Tables:\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied) {
            output("P" + to_string(i) + " page table: [ ");

            for (int j = 0; j < NUM_PAGES; j++) {
                output(to_string(processTable[i].pageTable[j]) + " ");
            }

            output("]\n");
        }
    }

    output("\nBlocked Queue:\n");

    if (blockedQueue.empty()) {
        output("None\n");
    } else {
        queue<BlockedRequest> temp = blockedQueue;

        while (!temp.empty()) {
            BlockedRequest request = temp.front();
            temp.pop();

            output("P" + to_string(request.processIndex) +
                   " waiting for page " +
                   to_string(request.pageNumber) +
                   " until " +
                   to_string(request.readySec) + ":" +
                   to_string(request.readyNano) + "\n");
        }
    }

    output("\n");
}

void terminateProcess(int index) {
    if (index < 0 || index >= MAX_PROCESSES || !processTable[index].occupied) {
        return;
    }

    pid_t childPid = processTable[index].pid;

    output("oss: P" + to_string(index) +
           " terminating at time " +
           to_string(simClock->seconds) + ":" +
           to_string(simClock->nanoseconds) + "\n");

    output("oss: P" + to_string(index) +
           " memory references: " +
           to_string(processTable[index].memoryReferences) +
           ", page faults: " +
           to_string(processTable[index].pageFaults) + "\n");

    if (processTable[index].memoryReferences > 0) {
        unsigned long long totalNano =
            static_cast<unsigned long long>(simClock->seconds) * BILLION +
            simClock->nanoseconds;

        double processEffectiveAccessTime =
            static_cast<double>(totalNano) /
            static_cast<double>(processTable[index].memoryReferences);

        output("oss: P" + to_string(index) +
               " effective memory access time: " +
               to_string(processEffectiveAccessTime) +
               " nanoseconds\n");
    }

    clearProcessMemory(index);

    processTable[index] = PCB();

    if (activeChildren > 0) {
        activeChildren--;
    }

    int status;
    waitpid(childPid, &status, 0);
}

void handleMemoryRequest(const Message& reply) {
    int index = reply.index;

    if (index < 0 || index >= MAX_PROCESSES) {
        return;
    }

    if (!processTable[index].occupied) {
        return;
    }

    processTable[index].messageOutstanding = false;

    if (reply.terminate == 1) {
        terminateProcess(index);
        return;
    }

    int address = reply.address;
    int pageNumber = address / PAGE_SIZE;
    int offset = address % PAGE_SIZE;

    if (pageNumber < 0 || pageNumber >= NUM_PAGES) {
        output("oss: Invalid memory address from P" + to_string(index) + "\n");
        sendMessageToWorker(index);
        return;
    }

    totalMemoryReferences++;
    processTable[index].memoryReferences++;

    if (reply.isWrite) {
        totalWrites++;
    } else {
        totalReads++;
    }

    string operation = reply.isWrite ? "write" : "read";

    output("oss: P" + to_string(index) +
           " requesting " + operation +
           " of address " + to_string(address) +
           " page " + to_string(pageNumber) +
           " offset " + to_string(offset) +
           " at time " +
           to_string(simClock->seconds) + ":" +
           to_string(simClock->nanoseconds) + "\n");

    int frame = processTable[index].pageTable[pageNumber];

    if (frame != -1) {
        if (reply.isWrite) {
            frameTable[frame].dirtyBit = 1;
        }

        output("oss: Address " + to_string(address) +
               " is in frame " + to_string(frame) +
               ". Giving data to P" + to_string(index) + ".\n");

        addTime(MEMORY_ACCESS_TIME);

        sendMessageToWorker(index);
    } else {
        output("oss: Address " + to_string(address) +
               " is not in a frame. Page fault.\n");

        totalPageFaults++;
        processTable[index].pageFaults++;

        processTable[index].blocked = true;

        BlockedRequest request;
        request.processIndex = index;
        request.address = address;
        request.pageNumber = pageNumber;
        request.isWrite = reply.isWrite;

        setFutureTime(DISK_TIME, request.readySec, request.readyNano);

        blockedQueue.push(request);

        output("oss: P" + to_string(index) +
               " blocked for disk I/O until " +
               to_string(request.readySec) + ":" +
               to_string(request.readyNano) + "\n");
    }
}

void handleBlockedQueue() {
    while (!blockedQueue.empty()) {
        BlockedRequest request = blockedQueue.front();

        if (!timeReached(simClock->seconds,
                         simClock->nanoseconds,
                         request.readySec,
                         request.readyNano)) {
            break;
        }

        blockedQueue.pop();

        int index = request.processIndex;

        if (index < 0 || index >= MAX_PROCESSES || !processTable[index].occupied) {
            continue;
        }

        int frame = loadPageIntoFrame(index, request.pageNumber, request.isWrite);

        if (frame != -1) {
            output("oss: Page fault completed for P" +
                   to_string(index) +
                   ". Address " +
                   to_string(request.address) +
                   " is now in frame " +
                   to_string(frame) + ".\n");

            processTable[index].blocked = false;

            sendMessageToWorker(index);
        }
    }
}

bool allActiveProcessesBlocked() {
    bool foundActive = false;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied) {
            foundActive = true;

            if (!processTable[i].blocked) {
                return false;
            }
        }
    }

    return foundActive;
}

void advanceClockToNextBlockedRequest() {
    if (!blockedQueue.empty()) {
        BlockedRequest request = blockedQueue.front();

        simClock->seconds = request.readySec;
        simClock->nanoseconds = request.readyNano;

        output("oss: All active processes are blocked. Advancing clock to " +
               to_string(simClock->seconds) + ":" +
               to_string(simClock->nanoseconds) + "\n");
    }
}

void launchChild(int timeLimitForChildren) {
    // Important fix:
    // Use totalLaunched as the PCB/process number so a run like -n 2 -s 1
    // creates P0 first and P1 second instead of reusing P0 after it exits.
    int index = totalLaunched;

    if (index < 0 || index >= MAX_PROCESSES) {
        output("oss: Maximum process table size reached. Cannot launch more children.\n");
        return;
    }

    if (processTable[index].occupied) {
        output("oss: ERROR - selected process slot is already occupied.\n");
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("oss fork");
        return;
    }

    if (pid == 0) {
        string indexArg = to_string(index);
        string timeArg = to_string(timeLimitForChildren);

        execl("./worker",
              "worker",
              indexArg.c_str(),
              timeArg.c_str(),
              nullptr);

        perror("oss execl");
        exit(1);
    }

    processTable[index] = PCB();
    processTable[index].occupied = true;
    processTable[index].blocked = false;
    processTable[index].messageOutstanding = false;
    processTable[index].pid = pid;

    totalLaunched++;
    activeChildren++;

    output("oss: Launched P" + to_string(index) +
           " with PID " + to_string(pid) +
           " at time " +
           to_string(simClock->seconds) + ":" +
           to_string(simClock->nanoseconds) + "\n");

    sendMessageToWorker(index);
}

void sendMessagesToReadyProcesses() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied &&
            !processTable[i].blocked &&
            !processTable[i].messageOutstanding) {
            sendMessageToWorker(i);
        }
    }
}

void receiveWorkerReplies() {
    while (true) {
        Message reply;

        if (msgrcv(msgId,
                   &reply,
                   sizeof(Message) - sizeof(long),
                   OSS_REPLY_TYPE,
                   IPC_NOWAIT) == -1) {
            if (errno == ENOMSG) {
                return;
            } else {
                perror("oss msgrcv");
                return;
            }
        }

        handleMemoryRequest(reply);
    }
}

void printFinalStats() {
    output("\nFinal Statistics:\n");
    output("Total memory references: " + to_string(totalMemoryReferences) + "\n");
    output("Total reads: " + to_string(totalReads) + "\n");
    output("Total writes: " + to_string(totalWrites) + "\n");
    output("Total page faults: " + to_string(totalPageFaults) + "\n");

    double pageFaultPercent = 0.0;

    if (totalMemoryReferences > 0) {
        pageFaultPercent =
            static_cast<double>(totalPageFaults) /
            static_cast<double>(totalMemoryReferences) * 100.0;
    }

    output("Page fault percentage: " + to_string(pageFaultPercent) + "%\n");

    if (totalMemoryReferences > 0) {
        unsigned long long totalNano =
            static_cast<unsigned long long>(simClock->seconds) * BILLION +
            simClock->nanoseconds;

        double effectiveAccessTime =
            static_cast<double>(totalNano) /
            static_cast<double>(totalMemoryReferences);

        output("Effective memory access time: " +
               to_string(effectiveAccessTime) +
               " nanoseconds\n");
    }
}

void cleanup() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied && processTable[i].pid > 0) {
            kill(processTable[i].pid, SIGTERM);
            waitpid(processTable[i].pid, nullptr, 0);
            processTable[i] = PCB();
        }
    }

    activeChildren = 0;

    if (simClock != nullptr) {
        shmdt(simClock);
        simClock = nullptr;
    }

    if (shmId != -1) {
        shmctl(shmId, IPC_RMID, nullptr);
        shmId = -1;
    }

    if (msgId != -1) {
        msgctl(msgId, IPC_RMID, nullptr);
        msgId = -1;
    }

    if (logFile.is_open()) {
        logFile.close();
    }
}

int main(int argc, char* argv[]) {
    int totalToLaunch = 5;
    int maxSimultaneous = 2;
    int timeLimitForChildren = 3;
    double launchInterval = 0.5;
    string logFileName = "oss.log";

    int option;

    while ((option = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (option) {
            case 'h':
                printHelp();
                return 0;
            case 'n':
                totalToLaunch = atoi(optarg);
                break;
            case 's':
                maxSimultaneous = atoi(optarg);
                break;
            case 't':
                timeLimitForChildren = atoi(optarg);
                break;
            case 'i':
                launchInterval = atof(optarg);
                break;
            case 'f':
                logFileName = optarg;
                break;
            default:
                printHelp();
                return 1;
        }
    }

    if (totalToLaunch < 1) {
        totalToLaunch = 1;
    }

    if (totalToLaunch > MAX_PROCESSES) {
        totalToLaunch = MAX_PROCESSES;
    }

    if (maxSimultaneous < 1) {
        maxSimultaneous = 1;
    }

    if (maxSimultaneous > MAX_PROCESSES) {
        maxSimultaneous = MAX_PROCESSES;
    }

    if (timeLimitForChildren < 1) {
        timeLimitForChildren = 1;
    }

    if (launchInterval < 0.0) {
        launchInterval = 0.0;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGALRM, signalHandler);
    alarm(60);

    logFile.open(logFileName);

    if (!logFile.is_open()) {
        cerr << "oss: could not open log file\n";
        return 1;
    }

    key_t msgKey = ftok(".", 75);

    if (msgKey == -1) {
        perror("oss ftok message");
        cleanup();
        return 1;
    }

    // Remove an old queue if one was left behind from a previous run.
    int oldMsgId = msgget(msgKey, 0666);
    if (oldMsgId != -1) {
        msgctl(oldMsgId, IPC_RMID, nullptr);
    }

    msgId = msgget(msgKey, IPC_CREAT | IPC_EXCL | 0666);

    if (msgId == -1) {
        perror("oss msgget");
        cleanup();
        return 1;
    }

    key_t shmKey = ftok(".", 76);

    if (shmKey == -1) {
        perror("oss ftok shared memory");
        cleanup();
        return 1;
    }

    // Remove old shared memory if one was left behind from a previous run.
    int oldShmId = shmget(shmKey, sizeof(SimClock), 0666);
    if (oldShmId != -1) {
        shmctl(oldShmId, IPC_RMID, nullptr);
    }

    shmId = shmget(shmKey, sizeof(SimClock), IPC_CREAT | IPC_EXCL | 0666);

    if (shmId == -1) {
        perror("oss shmget");
        cleanup();
        return 1;
    }

    simClock = static_cast<SimClock*>(shmat(shmId, nullptr, 0));

    if (simClock == reinterpret_cast<SimClock*>(-1)) {
        perror("oss shmat");
        cleanup();
        return 1;
    }

    simClock->seconds = 0;
    simClock->nanoseconds = 0;

    unsigned int launchIntervalNano =
        static_cast<unsigned int>(launchInterval * BILLION);

    unsigned int nextLaunchSec = 0;
    unsigned int nextLaunchNano = 0;

    unsigned int nextPrintSec = 0;
    unsigned int nextPrintNano = 500000000;

    time_t realStart = time(nullptr);

    output("oss: Starting memory management simulation\n");

    while (totalLaunched < totalToLaunch || activeChildren > 0) {
        bool realLimitReached = difftime(time(nullptr), realStart) >= 5.0;

        if (!realLimitReached &&
            totalLaunched < totalToLaunch &&
            activeChildren < maxSimultaneous &&
            timeReached(simClock->seconds,
                        simClock->nanoseconds,
                        nextLaunchSec,
                        nextLaunchNano)) {
            launchChild(timeLimitForChildren);
            setFutureTime(launchIntervalNano, nextLaunchSec, nextLaunchNano);
        }

        receiveWorkerReplies();

        handleBlockedQueue();

        if (allActiveProcessesBlocked()) {
            advanceClockToNextBlockedRequest();
            handleBlockedQueue();
        }

        sendMessagesToReadyProcesses();

        if (timeReached(simClock->seconds,
                        simClock->nanoseconds,
                        nextPrintSec,
                        nextPrintNano)) {
            printMemoryLayout();
            setFutureTime(500000000, nextPrintSec, nextPrintNano);
        }

        addTime(LOOP_INCREMENT);
    }

    printMemoryLayout();
    printFinalStats();

    output("\noss: Simulation complete. Cleaning up.\n");

    cleanup();

    return 0;
}
