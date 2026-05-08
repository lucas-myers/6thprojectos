#include <iostream>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>

using namespace std;

const int PAGE_SIZE = 1024;
const int NUM_PAGES = 16;
const long OSS_REPLY_TYPE = 1;

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

bool timeReached(unsigned int currentSec,
                 unsigned int currentNano,
                 unsigned int targetSec,
                 unsigned int targetNano) {
    if (currentSec > targetSec) {
        return true;
    }

    if (currentSec == targetSec && currentNano >= targetNano) {
        return true;
    }

    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "worker: missing process index\n";
        return 1;
    }

    int localIndex = atoi(argv[1]);

    int timeLimit = 3;

    if (argc >= 3) {
        timeLimit = atoi(argv[2]);
    }

    if (timeLimit <= 0) {
        timeLimit = 1;
    }

    srand(static_cast<unsigned int>(getpid() ^ time(nullptr)));

    key_t msgKey = ftok(".", 75);

    if (msgKey == -1) {
        perror("worker ftok message");
        return 1;
    }

    int msgId = msgget(msgKey, 0666);

    if (msgId == -1) {
        perror("worker msgget");
        return 1;
    }

    key_t shmKey = ftok(".", 76);

    if (shmKey == -1) {
        perror("worker ftok shared memory");
        return 1;
    }

    int shmId = shmget(shmKey, sizeof(SimClock), 0666);

    if (shmId == -1) {
        perror("worker shmget");
        return 1;
    }

    SimClock* simClock = static_cast<SimClock*>(shmat(shmId, nullptr, 0));

    if (simClock == reinterpret_cast<SimClock*>(-1)) {
        perror("worker shmat");
        return 1;
    }

    int randomLife = 1 + rand() % timeLimit;

    unsigned int endSec = simClock->seconds + randomLife;
    unsigned int endNano = simClock->nanoseconds;

    while (true) {
        Message msg;

        // Wait until oss sends this worker a turn.
        if (msgrcv(msgId,
                   &msg,
                   sizeof(Message) - sizeof(long),
                   getpid(),
                   0) == -1) {
            perror("worker msgrcv");
            shmdt(simClock);
            return 1;
        }

        Message reply;
        reply.mtype = OSS_REPLY_TYPE;
        reply.index = localIndex;
        reply.terminate = 0;
        reply.address = 0;
        reply.isWrite = 0;

        if (timeReached(simClock->seconds,
                        simClock->nanoseconds,
                        endSec,
                        endNano)) {
            reply.terminate = 1;
        } else {
            int page = rand() % NUM_PAGES;
            int offset = rand() % PAGE_SIZE;

            reply.address = page * PAGE_SIZE + offset;

            // Bias toward reads: about 70% reads, 30% writes.
            reply.isWrite = (rand() % 100 < 30) ? 1 : 0;
        }

        if (msgsnd(msgId,
                   &reply,
                   sizeof(Message) - sizeof(long),
                   0) == -1) {
            perror("worker msgsnd");
            shmdt(simClock);
            return 1;
        }

        if (reply.terminate) {
            break;
        }
    }

    shmdt(simClock);

    return 0;
}
