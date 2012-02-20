#include "sockets.hpp"
#include <iostream>
#ifdef _WIN32
    #ifndef __WIN32
    #define __WIN32
    #endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace trankesbel;
using namespace std;

/* epoll() based handling */
#ifndef _WIN32
#include <sys/epoll.h>

SocketEvents::SocketEvents()
{
    epoll_desc = -1;
    prune_counter = 0;
}

SocketEvents::~SocketEvents()
{
    if (epoll_desc != -1) close(epoll_desc);
    epoll_desc = -1;
}

void SocketEvents::addSocket(WP<Socket> socket)
{
    if (epoll_desc == -1)
        epoll_desc = epoll_create(20);
    if (epoll_desc == -1)
        return;

    SP<Socket> s = socket.lock();
    if (!s) return;

    int raw_socket = s->getRawSocket();
    if (raw_socket == INVALID_SOCKET) return;

    struct epoll_event ee;
    memset(&ee, 0, sizeof(struct epoll_event));
    ee.data.fd = raw_socket;
    ee.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    int result = epoll_ctl(epoll_desc, EPOLL_CTL_ADD, raw_socket, &ee);
    if (result) return;

    target_sockets[raw_socket] = socket;
};

void SocketEvents::pruneTargetSockets()
{
    map<SOCKET, WP<Socket> >::iterator i1, target_sockets_end = target_sockets.end();
    for (i1 = target_sockets.begin(); i1 != target_sockets_end; ++i1)
    {
        if (!i1->second.lock())
        {
            target_sockets.erase(i1);
            --i1;
            target_sockets_end = target_sockets.end();
            continue;
        }
    }
}

void SocketEvents::forceEvent(SP<Socket> socket)
{
    map<int, WP<Socket> >::iterator i1 = target_sockets.find(socket->getRawSocket());
    if (i1 == target_sockets.end()) return;

    forced_events.insert(socket);
}

WP<Socket> SocketEvents::_getEvent(uint64_t timeout_nanoseconds, bool ignore_forced_events)
{
    if (prune_counter >= 200)
    {
        prune_counter = 0;
        pruneTargetSockets();
    }

    SP<Socket> result;
    while (!result && !forced_events.empty())
    {
        result = (*forced_events.begin()).lock();
        forced_events.erase(forced_events.begin());
    }
    if (result) return result;

    if (epoll_desc == -1) return SP<Socket>();
    if (target_sockets.empty()) return SP<Socket>();

    struct epoll_event ee[10];
    vector<epoll_event> ee_events;

    int result_i = 0;
    do
    {
        result_i = epoll_wait(epoll_desc, ee, 10, timeout_nanoseconds / 1000000);
        if (result_i == -1 && errno != EINTR)
            return SP<Socket>();
        else if (result_i == -1 && errno == EINTR)
            continue;

        for (int i1 = 0; i1 < result_i; ++i1)
            ee_events.push_back(ee[i1]);

        timeout_nanoseconds = 0;
    } while(result_i > 0);

    if (ee_events.size() == 0) return SP<Socket>();

    vector<epoll_event>::iterator i2, ee_events_end = ee_events.end();
    map<int, WP<Socket> >::iterator target_sockets_end = target_sockets.end();

    for (i2 = ee_events.begin(); i2 != ee_events_end; ++i2)
    {
        map<int, WP<Socket> >::iterator i1 = target_sockets.find(i2->data.fd);
        if (i1 != target_sockets_end)
            forced_events.insert(i1->second);
    }

    result = SP<Socket>();
    while (!result && !forced_events.empty())
    {
        result = (*forced_events.begin()).lock();
        forced_events.erase(forced_events.begin());
    }

    return result;
}

SP<Socket> SocketEvents::getEvent(uint64_t timeout_nanoseconds)
{
    return _getEvent(timeout_nanoseconds, false).lock();
}

#else
SocketEvents::SocketEvents()
{
    prune_counter = 0;
    event_objects = (WSAEVENT*) 0;
    event_sockets = (WP<Socket>*) 0;
    event_size = 0;
    event_size_allocated = 0;
}

SocketEvents::~SocketEvents()
{
    if (event_objects) 
    {
        size_t i1;
        for (i1 = 0; i1 < event_size; ++i1)
            if (event_objects[i1] != WSA_INVALID_EVENT)
                WSACloseEvent(event_objects[i1]);
        delete[] event_objects;
    }    
    if (event_sockets) delete[] event_sockets;
}

void SocketEvents::addSocket(WP<Socket> socket)
{
    SP<Socket> s = socket.lock();
    if (!s) return;

    WSAEVENT we = WSACreateEvent();
    if (we == WSA_INVALID_EVENT) return;

    SOCKET raw_socket = s->getRawSocket();

    int result = WSAEventSelect(raw_socket, we, FD_ACCEPT | FD_READ | FD_WRITE | FD_CLOSE);
    if (result == SOCKET_ERROR)
    {
        WSACloseEvent(we);
        return;
    }

    if (event_size >= event_size_allocated)
    {
        if (event_size_allocated == 0)
            event_size_allocated = 1;
        else
            event_size_allocated *= 2;

        WSAEVENT* new_array = new WSAEVENT[event_size_allocated];
        WP<Socket>* new_event_sockets = new WP<Socket>[event_size_allocated];

        if (event_size > 0)
        {
            memcpy(new_array, event_objects, event_size * sizeof(WSAEVENT));
            ui32 i1;
            for (i1 = 0; i1 < event_size; ++i1)
                new_event_sockets[i1] = event_sockets[i1];
        }
        if (event_sockets) delete[] event_sockets;
        if (event_objects) delete[] event_objects;
        event_sockets = new_event_sockets;
        event_objects = new_array;
    }

    event_objects[event_size] = we;
    event_sockets[event_size] = socket;
    ++event_size;

    target_sockets[raw_socket] = socket;
}

void SocketEvents::forceEvent(SP<Socket> socket)
{
    map<SOCKET, WP<Socket> >::iterator i1 = target_sockets.find(socket->getRawSocket());
    if (i1 == target_sockets.end()) return;

    forced_events.insert(socket);
}

void SocketEvents::pruneTargetSockets()
{
    map<SOCKET, WP<Socket> >::iterator i1, target_sockets_end = target_sockets.end();
    bool do_repeat;

    do
    {
        do_repeat = false;
        for (i1 = target_sockets.begin(); i1 != target_sockets_end; ++i1)
        {
            if (!i1->second.lock())
            {
                target_sockets.erase(i1);
                do_repeat = true;
                target_sockets_end = target_sockets.end();
                break;
            }
        }
    } while(do_repeat);

    size_t i2;
    for (i2 = 0; i2 < event_size; ++i2)
    {
        if (event_objects[i2] == WSA_INVALID_EVENT)
            continue;
        SP<Socket> s = event_sockets[i2].lock();
        if (!s)
        {
            WSACloseEvent(event_objects[i2]);
            event_objects[i2] = WSA_INVALID_EVENT;

            event_objects[i2] = event_objects[event_size-1];
            event_sockets[i2] = event_sockets[event_size-1];
            --event_size;
            --i2;
            continue;
        }
    }
}


SP<Socket> SocketEvents::getEvent(uint64_t timeout_nanoseconds)
{
    ++prune_counter;
    if (prune_counter >= 200)
    {
        pruneTargetSockets();
        prune_counter = 0;
    }

    if (event_size == 0) return SP<Socket>();

    SP<Socket> result_s;
    while (!result_s && !forced_events.empty())
    {
        result_s = (*forced_events.begin()).lock();
        forced_events.erase(forced_events.begin());
    }
    if (result_s) return result_s;

    DWORD result = WSAWaitForMultipleEvents(event_size, event_objects, FALSE, timeout_nanoseconds / 1000000, FALSE);
    if (result == WSA_WAIT_FAILED) 
        return SP<Socket>();

    DWORD index = result - WSA_WAIT_EVENT_0;
    if (index >= event_size) return SP<Socket>();

    WSAResetEvent(event_objects[index]);

    return event_sockets[index].lock();
}

#endif

