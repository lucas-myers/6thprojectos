#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>

using namespace std;

const int MAX_PROCESSES = 20;
const int PAGE_SIZE = 1024;
const int NUM_PAGES = 16;
const int NUM_FRAMES = 64;
const unsigned int DISK_DELAY = 14000000; // 14ms

struct Message {
    long mtype;
    int index;
    int address;
    int isWrite;
    int terminate;
};

struct PageTableEntry {
    int frame;
};

struct PCB {
    int occupied;
    int blocked;
    pid_t pid;
    PageTableEntry pageTable[NUM_PAGES];
};

struct Frame {
    int occupied;
    int process;
    int page;
    int dirty;
};

struct SimClock {
    unsigned int seconds;
    unsigned int nanoseconds;
};

struct BlockedRequest {
    int occupied;
    int processIndex;
    int address;
    int page;
    int isWrite;
    unsigned int unblockSeconds;
    unsigned int unblockNanoseconds;
};

int msgId = -1;

void cleanup() {
    if (msgId != -1) {
        msgctl(msgId, IPC_RMID, nullptr);
    }
}

void signalHandler(int sig) {
    cleanup();
    exit(1);
}

void incrementClock(SimClock& clock, unsigned int ns) {
    clock.nanoseconds += ns;

    while (clock.nanoseconds >= 1000000000) {
        clock.seconds++;
        clock.nanoseconds -= 1000000000;
    }
}

void addTime(unsigned int currentSec,
             unsigned int currentNano,
             unsigned int addNano,
             unsigned int& resultSec,
             unsigned int& resultNano) {
    resultSec = currentSec;
    resultNano = currentNano + addNano;

    while (resultNano >= 1000000000) {
        resultSec++;
        resultNano -= 1000000000;
    }
}

bool timeReached(SimClock clock, unsigned int sec, unsigned int nano) {
    return clock.seconds > sec ||
           (clock.seconds == sec && clock.nanoseconds >= nano);
}

void logBoth(ofstream& logFile, const string& message) {
    cout << message << endl;
    logFile << message << endl;
}

void initializePCB(PCB pcb[]) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        pcb[i].occupied = 0;
        pcb[i].blocked = 0;
        pcb[i].pid = -1;

        for (int j = 0; j < NUM_PAGES; j++) {
            pcb[i].pageTable[j].frame = -1;
        }
    }
}

void initializeFrameTable(Frame frameTable[]) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        frameTable[i].occupied = 0;
        frameTable[i].process = -1;
        frameTable[i].page = -1;
        frameTable[i].dirty = 0;
    }
}

void initializeBlockedQueue(BlockedRequest blockedQueue[]) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        blockedQueue[i].occupied = 0;
        blockedQueue[i].processIndex = -1;
        blockedQueue[i].address = 0;
        blockedQueue[i].page = -1;
        blockedQueue[i].isWrite = 0;
        blockedQueue[i].unblockSeconds = 0;
        blockedQueue[i].unblockNanoseconds = 0;
    }
}

int getOpenPCB(PCB pcb[]) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb[i].occupied == 0) {
            return i;
        }
    }

    return -1;
}

int countActiveProcesses(PCB pcb[]) {
    int count = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb[i].occupied) {
            count++;
        }
    }

    return count;
}

int countUnblockedProcesses(PCB pcb[]) {
    int count = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb[i].occupied && pcb[i].blocked == 0) {
            count++;
        }
    }

    return count;
}

int findFreeFrame(Frame frameTable[]) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frameTable[i].occupied == 0) {
            return i;
        }
    }

    return -1;
}

void freeProcessFrames(int processIndex, PCB pcb[], Frame frameTable[]) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frameTable[i].occupied && frameTable[i].process == processIndex) {
            frameTable[i].occupied = 0;
            frameTable[i].process = -1;
            frameTable[i].page = -1;
            frameTable[i].dirty = 0;
        }
    }

    for (int i = 0; i < NUM_PAGES; i++) {
        pcb[processIndex].pageTable[i].frame = -1;
    }
}

void removeBlockedRequestsForProcess(int processIndex, BlockedRequest blockedQueue[]) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (blockedQueue[i].occupied && blockedQueue[i].processIndex == processIndex) {
            blockedQueue[i].occupied = 0;
            blockedQueue[i].processIndex = -1;
            blockedQueue[i].address = 0;
            blockedQueue[i].page = -1;
            blockedQueue[i].isWrite = 0;
            blockedQueue[i].unblockSeconds = 0;
            blockedQueue[i].unblockNanoseconds = 0;
        }
    }
}

int addToBlockedQueue(BlockedRequest blockedQueue[],
                      int processIndex,
                      int address,
                      int page,
                      int isWrite,
                      SimClock clock) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (blockedQueue[i].occupied == 0) {
            blockedQueue[i].occupied = 1;
            blockedQueue[i].processIndex = processIndex;
            blockedQueue[i].address = address;
            blockedQueue[i].page = page;
            blockedQueue[i].isWrite = isWrite;

            addTime(clock.seconds,
                    clock.nanoseconds,
                    DISK_DELAY,
                    blockedQueue[i].unblockSeconds,
                    blockedQueue[i].unblockNanoseconds);

            return i;
        }
    }

    return -1;
}

void printMemoryLayout(ofstream& logFile,
                       PCB pcb[],
                       Frame frameTable[],
                       BlockedRequest blockedQueue[],
                       SimClock clock) {
    logBoth(logFile, "");
    logBoth(logFile, "Current memory layout at time " +
           to_string(clock.seconds) + ":" + to_string(clock.nanoseconds));

    logBoth(logFile, "Occupied DirtyBit Process Page");

    for (int i = 0; i < NUM_FRAMES; i++) {
        string occupied = frameTable[i].occupied ? "Yes" : "No";

        logBoth(logFile,
            "Frame " + to_string(i) + ": " +
            occupied + " " +
            to_string(frameTable[i].dirty) + " " +
            to_string(frameTable[i].process) + " " +
            to_string(frameTable[i].page)
        );
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb[i].occupied) {
            string line = "P" + to_string(i) + " page table: [ ";

            for (int j = 0; j < NUM_PAGES; j++) {
                line += to_string(pcb[i].pageTable[j].frame) + " ";
            }

            line += "]";
            logBoth(logFile, line);
        }
    }

    string blockedLine = "Blocked processes: ";

    bool anyBlocked = false;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (blockedQueue[i].occupied) {
            anyBlocked = true;
            blockedLine += "P" + to_string(blockedQueue[i].processIndex) + " ";
        }
    }

    if (!anyBlocked) {
        blockedLine += "None";
    }

    logBoth(logFile, blockedLine);
    logBoth(logFile, "");
}

void sendMessageToWorker(pid_t pid, int index) {
    Message msg;
    msg.mtype = pid;       // worker waits on getpid()
    msg.index = index;
    msg.address = 0;
    msg.isWrite = 0;
    msg.terminate = 0;

    if (msgsnd(msgId, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
        perror("oss msgsnd");
    }
}

void loadBlockedPage(BlockedRequest& request,
                     PCB pcb[],
                     Frame frameTable[],
                     ofstream& logFile,
                     SimClock clock) {
    int processIndex = request.processIndex;
    int page = request.page;

    int freeFrame = findFreeFrame(frameTable);

    if (freeFrame == -1) {
        // Day 3 temporary replacement.
        // FIFO replacement will be added later.
        freeFrame = 0;

        int oldProcess = frameTable[freeFrame].process;
        int oldPage = frameTable[freeFrame].page;

        if (oldProcess != -1 && oldPage != -1) {
            pcb[oldProcess].pageTable[oldPage].frame = -1;
        }

        logBoth(logFile,
            "oss: No free frames, temporarily replacing frame 0"
        );
    }

    frameTable[freeFrame].occupied = 1;
    frameTable[freeFrame].process = processIndex;
    frameTable[freeFrame].page = page;
    frameTable[freeFrame].dirty = 0;

    pcb[processIndex].pageTable[page].frame = freeFrame;

    logBoth(logFile,
        "oss: Page fault complete for P" + to_string(processIndex) +
        ". Loaded page " + to_string(page) +
        " into frame " + to_string(freeFrame) +
        " at time " + to_string(clock.seconds) +
        ":" + to_string(clock.nanoseconds)
    );

    if (request.isWrite) {
        frameTable[freeFrame].dirty = 1;

        logBoth(logFile,
            "oss: Dirty bit of frame " +
            to_string(freeFrame) + " set"
        );
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    int totalChildren = 5;
    int maxSimultaneous = 2;
    int timeLimit = 3;
    int launchInterval = 100000000;
    string logFileName = "oss.log";

    int opt;

    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                cout << "Usage: ./oss [-h] [-n proc] [-s simul] [-t timeLimit] [-i interval] [-f logfile]\n";
                return 0;

            case 'n':
                totalChildren = atoi(optarg);
                break;

            case 's':
                maxSimultaneous = atoi(optarg);
                break;

            case 't':
                timeLimit = atoi(optarg);
                break;

            case 'i':
                launchInterval = atoi(optarg);
                break;

            case 'f':
                logFileName = optarg;
                break;

            default:
                cerr << "Invalid option\n";
                return 1;
        }
    }

    ofstream logFile(logFileName);

    if (!logFile) {
        cerr << "Could not open log file\n";
        return 1;
    }

    key_t msgKey = ftok(".", 75);
    if (msgKey == -1) {
        perror("oss ftok");
        return 1;
    }

    msgId = msgget(msgKey, IPC_CREAT | 0666);
    if (msgId == -1) {
        perror("oss msgget");
        return 1;
    }

    PCB pcb[MAX_PROCESSES];
    Frame frameTable[NUM_FRAMES];
    BlockedRequest blockedQueue[MAX_PROCESSES];
    SimClock simClock;

    initializePCB(pcb);
    initializeFrameTable(frameTable);
    initializeBlockedQueue(blockedQueue);

    simClock.seconds = 0;
    simClock.nanoseconds = 0;

    int launched = 0;

    int totalRequests = 0;
    int totalReads = 0;
    int totalWrites = 0;
    int totalPageFaults = 0;

    unsigned int nextLaunchSeconds = 0;
    unsigned int nextLaunchNanoseconds = 0;

    unsigned int nextPrintSeconds = 0;
    unsigned int nextPrintNanoseconds = 500000000;

    while (launched < totalChildren || countActiveProcesses(pcb) > 0) {
        incrementClock(simClock, 10000);

        while (waitpid(-1, nullptr, WNOHANG) > 0) {
            // Worker termination is handled when oss receives terminate message.
        }

        // Check blocked queue to see if any page fault has finished.
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (blockedQueue[i].occupied &&
                timeReached(simClock,
                            blockedQueue[i].unblockSeconds,
                            blockedQueue[i].unblockNanoseconds)) {

                int processIndex = blockedQueue[i].processIndex;

                if (processIndex >= 0 &&
                    processIndex < MAX_PROCESSES &&
                    pcb[processIndex].occupied) {

                    loadBlockedPage(blockedQueue[i],
                                    pcb,
                                    frameTable,
                                    logFile,
                                    simClock);

                    pcb[processIndex].blocked = 0;

                    sendMessageToWorker(pcb[processIndex].pid, processIndex);
                }

                blockedQueue[i].occupied = 0;
                blockedQueue[i].processIndex = -1;
                blockedQueue[i].address = 0;
                blockedQueue[i].page = -1;
                blockedQueue[i].isWrite = 0;
                blockedQueue[i].unblockSeconds = 0;
                blockedQueue[i].unblockNanoseconds = 0;
            }
        }

        bool canLaunchNow =
            simClock.seconds > nextLaunchSeconds ||
            (simClock.seconds == nextLaunchSeconds &&
             simClock.nanoseconds >= nextLaunchNanoseconds);

        if (launched < totalChildren &&
            countActiveProcesses(pcb) < maxSimultaneous &&
            canLaunchNow) {

            int index = getOpenPCB(pcb);

            if (index != -1) {
                pid_t pid = fork();

                if (pid == -1) {
                    perror("fork");
                    cleanup();
                    return 1;
                }

                if (pid == 0) {
                    execl("./worker", "./worker", to_string(index).c_str(), nullptr);
                    perror("execl");
                    exit(1);
                }

                pcb[index].occupied = 1;
                pcb[index].blocked = 0;
                pcb[index].pid = pid;

                for (int i = 0; i < NUM_PAGES; i++) {
                    pcb[index].pageTable[i].frame = -1;
                }

                logBoth(logFile,
                    "oss: Launching P" + to_string(index) +
                    " with pid " + to_string(pid) +
                    " at time " + to_string(simClock.seconds) +
                    ":" + to_string(simClock.nanoseconds)
                );

                sendMessageToWorker(pid, index);

                launched++;

                nextLaunchNanoseconds += launchInterval;

                while (nextLaunchNanoseconds >= 1000000000) {
                    nextLaunchSeconds++;
                    nextLaunchNanoseconds -= 1000000000;
                }
            }
        }

        Message msg;

        if (msgrcv(msgId, &msg, sizeof(Message) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            int processIndex = msg.index;

            if (processIndex < 0 ||
                processIndex >= MAX_PROCESSES ||
                pcb[processIndex].occupied == 0) {
                continue;
            }

            if (msg.terminate) {
                logBoth(logFile,
                    "oss: P" + to_string(processIndex) +
                    " terminating at time " +
                    to_string(simClock.seconds) + ":" +
                    to_string(simClock.nanoseconds)
                );

                freeProcessFrames(processIndex, pcb, frameTable);
                removeBlockedRequestsForProcess(processIndex, blockedQueue);

                pcb[processIndex].occupied = 0;
                pcb[processIndex].blocked = 0;
                pcb[processIndex].pid = -1;
            } else {
                totalRequests++;

                if (msg.isWrite) {
                    totalWrites++;
                } else {
                    totalReads++;
                }

                int page = msg.address / PAGE_SIZE;
                int frame = pcb[processIndex].pageTable[page].frame;

                string action = msg.isWrite ? "write" : "read";

                logBoth(logFile,
                    "oss: P" + to_string(processIndex) +
                    " requesting " + action +
                    " of address " + to_string(msg.address) +
                    " at time " + to_string(simClock.seconds) +
                    ":" + to_string(simClock.nanoseconds)
                );

                if (frame == -1) {
                    totalPageFaults++;

                    logBoth(logFile,
                        "oss: Address " + to_string(msg.address) +
                        " is not in memory, page fault"
                    );

                    int blockedIndex = addToBlockedQueue(blockedQueue,
                                                         processIndex,
                                                         msg.address,
                                                         page,
                                                         msg.isWrite,
                                                         simClock);

                    if (blockedIndex != -1) {
                        pcb[processIndex].blocked = 1;

                        logBoth(logFile,
                            "oss: P" + to_string(processIndex) +
                            " blocked until time " +
                            to_string(blockedQueue[blockedIndex].unblockSeconds) +
                            ":" +
                            to_string(blockedQueue[blockedIndex].unblockNanoseconds)
                        );
                    } else {
                        logBoth(logFile,
                            "oss: ERROR blocked queue is full"
                        );
                    }

                    // Do NOT send message back yet.
                    // Worker stays blocked on msgrcv until page fault completes.
                    continue;
                }

                logBoth(logFile,
                    "oss: Address " + to_string(msg.address) +
                    " is already in frame " + to_string(frame)
                );

                if (msg.isWrite) {
                    frameTable[frame].dirty = 1;

                    logBoth(logFile,
                        "oss: Dirty bit of frame " +
                        to_string(frame) + " set"
                    );
                }

                incrementClock(simClock, 100);

                sendMessageToWorker(pcb[processIndex].pid, processIndex);
            }
        }

        // If every active process is blocked, move clock forward faster.
        // This prevents oss from spinning forever while all workers wait.
        if (countActiveProcesses(pcb) > 0 && countUnblockedProcesses(pcb) == 0) {
            incrementClock(simClock, 1000000);
        }

        bool printNow =
            simClock.seconds > nextPrintSeconds ||
            (simClock.seconds == nextPrintSeconds &&
             simClock.nanoseconds >= nextPrintNanoseconds);

        if (printNow) {
            printMemoryLayout(logFile, pcb, frameTable, blockedQueue, simClock);

            nextPrintNanoseconds += 500000000;

            while (nextPrintNanoseconds >= 1000000000) {
                nextPrintSeconds++;
                nextPrintNanoseconds -= 1000000000;
            }
        }
    }

    logBoth(logFile, "");
    logBoth(logFile, "Statistics:");
    logBoth(logFile, "Total memory requests: " + to_string(totalRequests));
    logBoth(logFile, "Total reads: " + to_string(totalReads));
    logBoth(logFile, "Total writes: " + to_string(totalWrites));
    logBoth(logFile, "Total page faults: " + to_string(totalPageFaults));

    double pageFaultPercent = 0.0;

    if (totalRequests > 0) {
        pageFaultPercent = (static_cast<double>(totalPageFaults) /
                            static_cast<double>(totalRequests)) * 100.0;
    }

    logBoth(logFile, "Page fault percentage: " + to_string(pageFaultPercent) + "%");

    logBoth(logFile, "");
    logBoth(logFile, "oss: All children finished. Cleaning up.");

    cleanup();

    return 0;
}