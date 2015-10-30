#include "client.h"
#include "constants.h"
#include "iostream"
#include "vector"
#include "string"
#include "fstream"
#include "sstream"
using namespace std;

extern void* AcceptConnections(void* _C);

int Client::get_pid() {
    return pid_;
}

int Client::get_master_fd() {
    return master_fd_;
}

int Client::get_primary_server_fd() {
    return primary_server_fd_;
}

int Client::get_master_port() {
    return master_port_;
}

int Client::get_server_listen_port(const int server_id) {
    return server_listen_port_[server_id];
}

int Client::get_my_chat_port() {
    return my_chat_port_;
}

int Client::get_my_listen_port() {
    return my_listen_port_;
}

void Client::set_pid(const int pid) {
    pid_ = pid;
}

void Client::set_master_fd(const int fd) {
    master_fd_ = fd;
}

int Client::set_primary_server_fd(const int fd) {
    primary_server_fd_ = fd;
}

/**
 * reads ports-file and populates port related vectors/maps
 * @return true is ports-file was read successfully
 */
bool Client::ReadPortsFile() {
    ifstream fin;
    fin.exceptions ( ifstream::failbit | ifstream::badbit );
    try {
        fin.open(kPortsFile.c_str());
        fin >> master_port_;

        int port;
        for (int i = 0; i < num_servers_; i++) {
            fin >> port;
            server_listen_port_[i] = port;
        }

        for (int i = 0; i < num_clients_; i++) {
            fin >> port;
            if(i == get_pid()) {
                my_listen_port_ = port;
                break;
            }
        }

        for (int i = 0; i < num_servers_; i++) {
            fin >> port;
            // server_paxos_port
            // client doesn't need this info
        }

        for (int i = 0; i < num_clients_; i++) {
            fin >> port;
            if(i == get_pid()) {
                my_chat_port_ = port;
                break;
            }
        }

        fin.close();
        return true;

    } catch (ifstream::failure e) {
        cout << e.what() << endl;
        if (fin.is_open()) fin.close();
        return false;
    }
}

/**
 * initialize data members and resize vectors
 * @param  pid process's self id
 * @param  num_servers number of servers
 * @param  num_clients number of clients
 */
void Client::Initialize(const int pid,
                        const int num_servers,
                        const int num_clients) {
    set_pid(pid);
    num_servers_ = num_servers;
    num_clients_ = num_clients;
    server_listen_port_.resize(num_servers_);
}

/**
 * creates a new thread
 * @param  f function pointer to be passed to the new thread
 * @param  arg arguments for the function f passed to the thread
 * @param  thread thread identifier for new thread
 */
void Client::CreateThread(void* (*f)(void* ), void* arg, pthread_t &thread) {
    if (pthread_create(&thread, NULL, f, arg)) {
        cout << "S" << get_pid() << ": ERROR: Unable to create thread" << endl;
        pthread_exit(NULL);
    }
}

int main(int argc, char *argv[]) {
    Client C;
    C.Initialize(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
    if (!C.ReadPortsFile())
        return 1;

    pthread_t accept_connections_thread;
    C.CreateThread(AcceptConnections, (void*)&C, accept_connections_thread);

    void* status;
    pthread_join(accept_connections_thread, &status);
    return 0;
}