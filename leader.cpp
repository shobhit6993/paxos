#include "leader.h"
#include "server.h"
#include "constants.h"
#include "utilities.h"
#include "iostream"
#include "vector"
#include "string"
#include "fstream"
#include "sstream"
#include "unistd.h"
#include "signal.h"
#include "errno.h"
#include "sys/socket.h"
using namespace std;

typedef pair<int, Proposal> SPtuple;

#define DEBUG

#ifdef DEBUG
#  define D(x) x
#else
#  define D(x)
#endif // DEBUG

extern void* CommanderMode(void* _rcv_thread_arg);

Leader::Leader(Server* _S) {
    S = _S;

    set_ballot_num(Ballot(S->get_pid(), 0));
    set_leader_active(false);

    int num_servers = S->get_num_servers();

    commander_fd_.resize(num_servers, -1);
    scout_fd_.resize(num_servers, -1);
    replica_fd_.resize(num_servers, -1);
}

int Leader::get_commander_fd(const int server_id) {
    return commander_fd_[server_id];
}

int Leader::get_scout_fd(const int server_id) {
    return scout_fd_[server_id];
}

int Leader::get_replica_fd(const int server_id) {
    return replica_fd_[server_id];
}

Ballot Leader::get_ballot_num() {
    return ballot_num_;
}

bool Leader::get_leader_active() {
    return leader_active_;
}

void Leader::set_commander_fd(const int server_id, const int fd) {
    commander_fd_[server_id] = fd;
}

void Leader::set_scout_fd(const int server_id, const int fd) {
    scout_fd_[server_id] = fd;
}

void Leader::set_replica_fd(const int server_id, const int fd) {
    replica_fd_[server_id] = fd;
}

void Leader::set_ballot_num(const Ballot &ballot_num) {
    ballot_num_.id = ballot_num.id;
    ballot_num_.seq_num = ballot_num.seq_num;
}

void Leader::set_leader_active(const bool b) {
    leader_active_ = b;
}

/**
 * increments the value of ballot_num_
 */
void Leader::IncrementBallotNum() {
    Ballot b = get_ballot_num();
    b.seq_num++;
    set_ballot_num(b);
}

/**
 * thread entry function for leader
 * @param  _S pointer to server class object
 * @return    NULL
 */
void* LeaderEntry(void *_S) {
    Leader L((Server*)_S);

    // does not need accept threads since it does not listen to connections from anyone

    // sleep for some time to make sure accept threads of commanders,scouts,replica are running
    usleep(kGeneralSleep);
    usleep(kGeneralSleep);
    int primary_id = L.S->get_primary_id();
    if (L.ConnectToCommander(primary_id)) {
        D(cout << "SL" << L.S->get_pid() << ": Connected to commander of S"
          << primary_id << endl;)
    } else {
        D(cout << "SL" << L.S->get_pid() << ": ERROR in connecting to commander of S"
          << primary_id << endl;)
        return NULL;
    }

    if (L.ConnectToScout(primary_id)) {
        D(cout << "SL" << L.S->get_pid() << ": Connected to scout of S"
          << primary_id << endl;)
    } else {
        D(cout << "SL" << L.S->get_pid() << ": ERROR in connecting to scout of S"
          << primary_id << endl;)
        return NULL;
    }

    if (L.ConnectToReplica(primary_id)) { // same as R.S->get_pid()
        D(cout << "SL" << L.S->get_pid() << ": Connected to replica of S"
          << primary_id << endl;)
    } else {
        D(cout << "SL" << L.S->get_pid() << ": ERROR in connecting to replica of S"
          << primary_id << endl;)
        return NULL;
    }

    L.LeaderMode();
    return NULL;
}

/**
 * function for performing leader related job
 */
void Leader::LeaderMode()
{
    // scout
    pthread_t scout_thread;
    ScoutThreadArgument* arg = new ScoutThreadArgument;
    arg->SC = S->get_scout_object();
    arg->ball = get_ballot_num();
    CreateThread(ScoutMode, (void*)arg, scout_thread);
    
    char buf[kMaxDataSize];
    int num_bytes;
    int num_servers = S->get_num_servers();
    while (true) {  // always listen to messages from the acceptors
        if ((num_bytes = recv(get_replica_fd(S->get_primary_id()), buf, kMaxDataSize - 1, 0)) == -1)
        {
            D(cout << "SL" << S->get_pid() << ": ERROR in receving from primary replica" << endl;)
            // pthread_exit(NULL); //TODO: think about whether it should be exit or not
        } else if (num_bytes == 0) {     //connection closed
            D(cout << "SL" << S->get_pid() << ": ERROR Connection closed by primary replica" << endl;)
        } else {
            buf[num_bytes] = '\0';
            std::vector<string> message = split(string(buf), kMessageDelim[0]);
            for (const auto &msg : message)
            {
                std::vector<string> token = split(string(msg), kInternalDelim[0]);
                if (token[0] == kPropose)
                {
                    D(cout << "Leader receives PROPOSE" << "message" <<  endl;)
                    if (S->proposals_.find(stoi(token[1])) == S->proposals_.end())
                    {
                        S->proposals_[stoi(token[1])] = stringToProposal(token[2]);
                        if (get_leader_active())
                        {
                            // scout
                            // pthread_t scout_thread;
                            ScoutThreadArgument* arg = new ScoutThreadArgument;
                            arg->SC = S->get_scout_object();
                            arg->ball = get_ballot_num();;
                            CreateThread(ScoutMode, (void*)arg, scout_thread);
                        }
                    }
                }
                else if (token[0] == kAdopted)
                {
                    unordered_set<Triple> pvalues = stringToTripleSet(token[2]);
                    S->proposals_ = pairxor(S->proposals_, pmax(pvalues));

                    pthread_t commander_thread[S->proposals_.size()];
                    int i = 0;
                    for (auto it = S->proposals_.begin(); it != S->proposals_.end(); it++)
                    {
                        // commander
                        Commander C(S);
                        CommanderThreadArgument* arg = new CommanderThreadArgument;
                        arg->C = &C;
                        Triple tempt = Triple(get_ballot_num(), stoi(token[1]), S->proposals_[stoi(token[1])]);
                        arg->toSend = &tempt;
                        CreateThread(CommanderMode, (void*)arg, commander_thread[i]);
                        i++;
                    }
                    set_leader_active(true);
                }

                else if (token[0] == kPreEmpted)
                {
                    Ballot recvd_b = stringToBallot(token[1]);
                    if (recvd_b > get_ballot_num())
                    {
                        set_leader_active(false);
                        IncrementBallotNum();

                        // scout
                        // pthread_t scout_thread;
                        ScoutThreadArgument* arg = new ScoutThreadArgument;
                        arg->SC = S->get_scout_object();
                        arg->ball = get_ballot_num();
                        CreateThread(ScoutMode, (void*)arg, scout_thread);
                    }
                }
                else {    //other messages
                    D(cout << "SL" << S->get_pid() << "ERROR: Unexpected message received: " << msg << endl;)
                }
            }
        }
    }
}