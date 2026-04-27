#include <iostream>
#include <fstream>
#include <queue>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>

using namespace std;

const int MAX_TOTAL_PROCESSES = 20;
const int PCB_SIZE = 18;
const int PAGE_SIZE = 1024;
const int NUM_PAGES = 16;
const int NUM_FRAMES = 64;
const unsigned int BILLION = 1000000000;
const unsigned int CLOCK_INCREMENT = 10000000;
const unsigned int IDLE_INCREMENT = 100000;
const unsigned int DISPATCH_OVERHEAD = 1000;
const int MAX_LOG_LINES = 10000;

struct SimClock {
    unsigned int seconds;
    unsigned int nanoseconds;
};

struct PageTableEntry {
    int frame;
    int valid;
};

struct Frame {
    int occupied;
    int dirtyBit;
    int processIndex;
    int pageNumber;
};

struct PCB {
    int occupied;
    pid_t pid;
    int localPid;
    unsigned int startSeconds;
    unsigned int startNano;
    int blocked;
    PageTableEntry pageTable[NUM_PAGES];
};

struct Message {
    long mtype;
    int index;
    int address;
    int isWrite;
    int terminate;
};

int shmId = -1;
int msgId = -1;
SimClock* simClock = nullptr;

PCB processTable[PCB_SIZE];
Frame frameTable[NUM_FRAMES];
queue<int> readyQueue;

int totalChildren = 5;
int maxSimultaneous = 2;
double timeLimitForChildren = 3.0;
double launchInterval = 0.5;
string logFileName = "oss.log";

ofstream logFile;
int logLines = 0;

int launchedTotal = 0;
int finishedTotal = 0;
int runningNow = 0;

unsigned int nextLaunchSec = 0;
unsigned int nextLaunchNano = 0;

unsigned long long totalReads = 0;
unsigned long long totalWrites = 0;
unsigned long long totalRequests = 0;
unsigned long long totalPageFaults = 0;

void writeLog(const string& text) {
    cout << text;

    if (logLines < MAX_LOG_LINES) {
        logFile << text;
        logLines++;
    }
}

void addToTime(unsigned int& sec, unsigned int& nano, unsigned int addNano) {
    nano += addNano;

    while (nano >= BILLION) {
        nano -= BILLION;
        sec++;
    }
}

void advanceClock(unsigned int ns) {
    addToTime(simClock->seconds, simClock->nanoseconds, ns);
}

bool timeReached(unsigned int sec1, unsigned int nano1,
                 unsigned int sec2, unsigned int nano2) {
    if (sec1 > sec2) return true;
    if (sec1 == sec2 && nano1 >= nano2) return true;
    return false;
}

void cleanup() {
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
}

void killChildren() {
    for (int i = 0; i < PCB_SIZE; i++) {
        if (processTable[i].occupied && processTable[i].pid > 0) {
            kill(processTable[i].pid, SIGTERM);
        }
    }

    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
}

void signalHandler(int sig) {
    cerr << "\nOSS: caught signal " << sig << ", cleaning up.\n";
    killChildren();
    cleanup();
    exit(1);
}

void printUsage(const char* program) {
    cout << "Usage: " << program
         << " [-h] [-n proc] [-s simul] [-t timeLimitForChildren] "
         << "[-i fractionOfSecondToLaunchChildren] [-f logfile]\n";
}

void parseArguments(int argc, char* argv[]) {
    int opt;

    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'n':
                totalChildren = atoi(optarg);
                break;
            case 's':
                maxSimultaneous = atoi(optarg);
                break;
            case 't':
                timeLimitForChildren = atof(optarg);
                break;
            case 'i':
                launchInterval = atof(optarg);
                break;
            case 'f':
                logFileName = optarg;
                break;
            default:
                printUsage(argv[0]);
                exit(1);
        }
    }

    if (totalChildren < 1) totalChildren = 1;
    if (totalChildren > MAX_TOTAL_PROCESSES) totalChildren = MAX_TOTAL_PROCESSES;

    if (maxSimultaneous < 1) maxSimultaneous = 1;
    if (maxSimultaneous > PCB_SIZE) maxSimultaneous = PCB_SIZE;
}

void initTables() {
    for (int i = 0; i < PCB_SIZE; i++) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].localPid = i;
        processTable[i].startSeconds = 0;
        processTable[i].startNano = 0;
        processTable[i].blocked = 0;

        for (int j = 0; j < NUM_PAGES; j++) {
            processTable[i].pageTable[j].frame = -1;
            processTable[i].pageTable[j].valid = 0;
        }
    }

    for (int i = 0; i < NUM_FRAMES; i++) {
        frameTable[i].occupied = 0;
        frameTable[i].dirtyBit = 0;
        frameTable[i].processIndex = -1;
        frameTable[i].pageNumber = -1;
    }
}

int getFreePCB() {
    for (int i = 0; i < PCB_SIZE; i++) {
        if (!processTable[i].occupied) {
            return i;
        }
    }

    return -1;
}

void removeFromPCB(int index) {
    processTable[index].occupied = 0;
    processTable[index].pid = 0;
    processTable[index].blocked = 0;

    for (int i = 0; i < NUM_PAGES; i++) {
        processTable[index].pageTable[i].frame = -1;
        processTable[index].pageTable[i].valid = 0;
    }
}

void setNextLaunchTime(double interval) {
    unsigned int ns = static_cast<unsigned int>(interval * BILLION);
    nextLaunchSec = simClock->seconds;
    nextLaunchNano = simClock->nanoseconds;
    addToTime(nextLaunchSec, nextLaunchNano, ns);
}

bool timeToLaunch() {
    return timeReached(simClock->seconds, simClock->nanoseconds,
                       nextLaunchSec, nextLaunchNano);
}

void launchWorker(int index) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        string indexStr = to_string(index);
        execl("./worker", "worker", indexStr.c_str(), (char*)nullptr);
        perror("execl");
        exit(1);
    }

    processTable[index].occupied = 1;
    processTable[index].pid = pid;
    processTable[index].localPid = index;
    processTable[index].startSeconds = simClock->seconds;
    processTable[index].startNano = simClock->nanoseconds;
    processTable[index].blocked = 0;

    for (int j = 0; j < NUM_PAGES; j++) {
        processTable[index].pageTable[j].frame = -1;
        processTable[index].pageTable[j].valid = 0;
    }

    readyQueue.push(index);

    writeLog("OSS: Generating process P" + to_string(index) +
             " PID " + to_string(pid) +
             " at time " +
             to_string(simClock->seconds) + ":" +
             to_string(simClock->nanoseconds) + "\n");
}

void freeProcessFrames(int index) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frameTable[i].occupied && frameTable[i].processIndex == index) {
            frameTable[i].occupied = 0;
            frameTable[i].dirtyBit = 0;
            frameTable[i].processIndex = -1;
            frameTable[i].pageNumber = -1;
        }
    }
}

void printMemoryTables() {
    writeLog("\nOSS: Current memory layout at time " +
             to_string(simClock->seconds) + ":" +
             to_string(simClock->nanoseconds) + "\n");

    writeLog("Frame Table:\n");
    writeLog("Frame  Occupied  Dirty  Process  Page\n");

    for (int i = 0; i < NUM_FRAMES; i++) {
        ostringstream out;
        out << setw(5) << i << "  "
            << setw(8) << frameTable[i].occupied << "  "
            << setw(5) << frameTable[i].dirtyBit << "  "
            << setw(7) << frameTable[i].processIndex << "  "
            << setw(4) << frameTable[i].pageNumber << "\n";
        writeLog(out.str());
    }

    writeLog("Page Tables:\n");

    for (int i = 0; i < PCB_SIZE; i++) {
        if (!processTable[i].occupied) continue;

        ostringstream out;
        out << "P" << i << " page table: [ ";

        for (int j = 0; j < NUM_PAGES; j++) {
            out << processTable[i].pageTable[j].frame << " ";
        }

        out << "]\n";
        writeLog(out.str());
    }

    writeLog("\n");
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGALRM, signalHandler);
    alarm(5);

    parseArguments(argc, argv);

    logFile.open(logFileName.c_str(), ios::out | ios::trunc);
    if (!logFile) {
        cerr << "Failed to open log file.\n";
        return 1;
    }

    key_t shmKey = ftok(".", 65);
    if (shmKey == -1) {
        perror("ftok shm");
        return 1;
    }

    shmId = shmget(shmKey, sizeof(SimClock), IPC_CREAT | 0666);
    if (shmId == -1) {
        perror("shmget");
        return 1;
    }

    simClock = (SimClock*)shmat(shmId, nullptr, 0);
    if (simClock == (void*)-1) {
        perror("shmat");
        simClock = nullptr;
        cleanup();
        return 1;
    }

    simClock->seconds = 0;
    simClock->nanoseconds = 0;

    key_t msgKey = ftok(".", 75);
    if (msgKey == -1) {
        perror("ftok msg");
        cleanup();
        return 1;
    }

    msgId = msgget(msgKey, IPC_CREAT | 0666);
    if (msgId == -1) {
        perror("msgget");
        cleanup();
        return 1;
    }

    initTables();
    setNextLaunchTime(0.0);

    while (finishedTotal < totalChildren || runningNow > 0) {
        while (runningNow < maxSimultaneous &&
               launchedTotal < totalChildren &&
               timeToLaunch()) {

            int index = getFreePCB();

            if (index == -1) {
                break;
            }

            launchWorker(index);
            launchedTotal++;
            runningNow++;

            setNextLaunchTime(launchInterval);
            advanceClock(DISPATCH_OVERHEAD);
        }

        if (!readyQueue.empty()) {
            int index = readyQueue.front();
            readyQueue.pop();

            if (!processTable[index].occupied) {
                continue;
            }

            pid_t childPid = processTable[index].pid;

            Message dispatchMsg;
            dispatchMsg.mtype = childPid;
            dispatchMsg.index = index;
            dispatchMsg.address = 0;
            dispatchMsg.isWrite = 0;
            dispatchMsg.terminate = 0;

            writeLog("OSS: Dispatching P" + to_string(index) +
                     " at time " +
                     to_string(simClock->seconds) + ":" +
                     to_string(simClock->nanoseconds) + "\n");

            if (msgsnd(msgId, &dispatchMsg, sizeof(Message) - sizeof(long), 0) == -1) {
                perror("msgsnd");
                killChildren();
                cleanup();
                return 1;
            }

            Message replyMsg;

            if (msgrcv(msgId, &replyMsg, sizeof(Message) - sizeof(long), 1, 0) == -1) {
                perror("msgrcv");
                killChildren();
                cleanup();
                return 1;
            }

            if (replyMsg.terminate) {
                writeLog("OSS: Process P" + to_string(index) +
                         " is terminating at time " +
                         to_string(simClock->seconds) + ":" +
                         to_string(simClock->nanoseconds) + "\n");

                freeProcessFrames(index);
                waitpid(childPid, nullptr, 0);
                removeFromPCB(index);

                runningNow--;
                finishedTotal++;
            } else {
                totalRequests++;

                if (replyMsg.isWrite) {
                    totalWrites++;
                } else {
                    totalReads++;
                }

                int page = replyMsg.address / PAGE_SIZE;

                writeLog("OSS: P" + to_string(index) +
                         " requesting " +
                         (replyMsg.isWrite ? "write" : "read") +
                         " of address " + to_string(replyMsg.address) +
                         " page " + to_string(page) +
                         " at time " +
                         to_string(simClock->seconds) + ":" +
                         to_string(simClock->nanoseconds) + "\n");

               
                // Just log request and put process back in ready queue.
                readyQueue.push(index);
            }

            advanceClock(CLOCK_INCREMENT);
        } else {
            advanceClock(IDLE_INCREMENT);
        }
    }

    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }

    ostringstream report;
    report << "\nOSS: Final report\n";
    report << "OSS: Total processes launched: " << launchedTotal << "\n";
    report << "OSS: Total processes finished: " << finishedTotal << "\n";
    report << "OSS: Total memory requests: " << totalRequests << "\n";
    report << "OSS: Total reads: " << totalReads << "\n";
    report << "OSS: Total writes: " << totalWrites << "\n";
    report << "OSS: Total page faults: " << totalPageFaults << "\n";
    report << "OSS: Simulation finished at time "
           << simClock->seconds << ":" << simClock->nanoseconds << "\n";

    writeLog(report.str());

    printMemoryTables();

    cleanup();
    logFile.close();

    return 0;
}