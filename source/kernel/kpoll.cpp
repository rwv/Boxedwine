/*
 *  Copyright (C) 2016  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"

#include "kpoll.h"
#include "kscheduler.h"

static void clearPollData(KPollData* data, U32 count) {
    KThread* thread = KThread::currentThread();
    for (U32 i = 0; i < count; i++, data++) {
        data->pFD->kobject->waitForEvents(thread->pollCond, 0);
    }
}

S32 internal_poll(KThread* thread, KPollData* data, U32 count, U32 timeout) {
    KPollData* firstData=data;

    while (true) {        
        S32 result = 0;
        BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(thread->pollCond);
        bool interrupted = !thread->inSignal && thread->interrupted;
        
        if (interrupted)
            thread->interrupted = false;
        
        data = firstData;
        // gather locks before we check the data so that we don't miss one
        for (U32 i=0;i<count;i++) {
            KFileDescriptor* fd = thread->process->getFileDescriptor(data->fd);
            if (fd) {
                fd->kobject->waitForEvents(thread->pollCond, data->events);
                data->pFD = fd;
                data++;
            } else {
                int ii = 0;
            }
        } 

        data = firstData;
        for (U32 i=0;i<count;i++) {
            KFileDescriptor* fd = data->pFD;
            data->revents = 0;
            if (fd) {
                if (!fd->kobject->isOpen()) {
                    data->revents = K_POLLHUP;
                } else {
                    if ((data->events & K_POLLPRI) && fd->kobject->isPriorityReadReady()) {
                        data->revents |= K_POLLPRI;
                    } else if ((data->events & K_POLLIN) != 0 && fd->kobject->isReadReady()) {
                        data->revents |= K_POLLIN;
                    } else if ((data->events & K_POLLOUT) != 0 && fd->kobject->isWriteReady()) {
                        data->revents |= K_POLLOUT;
                    }
                }
                if (data->revents!=0) {
                    result++;
                }
            }
            data++;
        }
        if (result>0) {	
            thread->condStartWaitTime = 0;
            clearPollData(firstData, count);
            return result;
        }
        if (timeout==0) {
            clearPollData(firstData, count);
            return 0;
        }	
        if (interrupted) {
            thread->condStartWaitTime = 0;
            clearPollData(firstData, count);
            return -K_EINTR;
        }
        if (!thread->condStartWaitTime) {
            thread->condStartWaitTime = KSystem::getMilliesSinceStart();
        } else {
            U32 diff = KSystem::getMilliesSinceStart()-thread->condStartWaitTime;
            if (diff>timeout) {
                thread->condStartWaitTime = 0;
                clearPollData(firstData, count);
                return 0;
            }
            timeout-=diff;
        }   
        if (timeout>0xF0000000) {
            BOXEDWINE_CONDITION_WAIT(thread->pollCond);
        } else {
            BOXEDWINE_CONDITION_WAIT_TIMEOUT(thread->pollCond, timeout);
        }
        clearPollData(firstData, count);
#ifdef BOXEDWINE_MULTI_THREADED
		if (KThread::currentThread()->terminating) {
			return -K_EINTR;
		}
        if (KThread::currentThread()->startSignal) {
            KThread::currentThread()->startSignal = false;
            return -K_CONTINUE;
        }
#endif        
    }
}

U32 kpoll(KThread* thread, U32 pfds, U32 nfds, U32 timeout) {
    U32 address = pfds;
    KMemory* memory = thread->memory;

    KPollData* pollData = new KPollData[nfds];

    for (U32 i=0;i<nfds;i++) {
        pollData[i].fd = memory->readd(address); address += 4;
        pollData[i].events = memory->readw(address); address += 2;
        pollData[i].revents = memory->readw(address); address += 2;
    }

    S32 result = internal_poll(thread, pollData, nfds, timeout);
    if (result >= 0) { 
        pfds+=6;
        for (U32 i=0;i<nfds;i++) {
            memory->writew(pfds, pollData[i].revents);
            pfds+=8;
        }
    }
    delete[] pollData;
    return result;
}


U32 kselect(KThread* thread, U32 nfds, U32 readfds, U32 writefds, U32 errorfds, U32 timeout) {
    S32 result = 0;
    int count = 0;
    U32 pollCount = 0;
    KMemory* memory = thread->memory;

    if (timeout == 0)
        timeout = 0x7FFFFFFF;
    else {
        timeout = memory->readd(timeout) * 1000 + memory->readd(timeout + 4) / 1000;
        if (timeout < NUMBER_OF_MILLIES_TO_SPIN_FOR_WAIT && nfds == 0) {
            return KThread::currentThread()->sleep(timeout);
        }
    }    

    KPollData* pollData = new KPollData[nfds];

    for (U32 i=0;i<nfds;) {
        U32 readbits = 0;
        U32 writebits = 0;
        U32 errorbits = 0;

        if (readfds!=0) {
            readbits = memory->readb(readfds + i / 8);
        }
        if (writefds!=0) {
            writebits = memory->readb(writefds + i / 8);
        }
        if (errorfds!=0) {
            errorbits = memory->readb(errorfds + i / 8);
        }
        for (U32 b = 0; b < 8 && i < nfds; b++, i++) {
            U32 mask = 1 << b;
            U32 r = readbits & mask;
            U32 w = writebits & mask;
            U32 e = errorbits & mask;
            if (r || w || e) {
                U32 events = 0;
                if (r)
                    events |= K_POLLIN;
                if (w)
                    events |= K_POLLHUP|K_POLLOUT;
                if (e)
                    events |= K_POLLERR;
                pollData[pollCount].events = events;
                pollData[pollCount].fd = i;
                pollCount++;
            }
        }
    }    
    result = internal_poll(thread, pollData, pollCount, timeout);
    if (result == -K_WAIT || result == -K_CONTINUE) {
        delete[] pollData;
        return result;
    }

    if (readfds)
        memory->memset(readfds, 0, (nfds + 7) / 8);
    if (writefds)
        memory->memset(writefds, 0, (nfds + 7) / 8);
    if (errorfds)
        memory->memset(errorfds, 0, (nfds + 7) / 8);

    if (result <= 0) {
        delete[] pollData;
        return result;
    }

    for (U32 i=0;i<pollCount;i++) {
        U32 found = 0;
        FD fd = pollData[i].fd;
        U32 revent = pollData[i].revents;

        if (readfds!=0 && ((revent & K_POLLIN) || (revent & K_POLLHUP))) {
            U8 v = memory->readb(readfds + fd / 8);
            v |= 1 << (fd % 8);
            memory->writeb(readfds + fd / 8, v);
            found = 1;
        }
        if (writefds!=0 && (revent & K_POLLOUT)) {
            U8 v = memory->readb(writefds + fd / 8);
            v |= 1 << (fd % 8);
            memory->writeb(writefds + fd / 8, v);
            found = 1;
        }
        if (errorfds!=0 && (revent & K_POLLERR)) {
            U8 v = memory->readb(errorfds + fd / 8);
            v |= 1 << (fd % 8);
            memory->writeb(errorfds + fd / 8, v);
            found = 1;
        }
        if (found) {
            count++;
        }
    }
    delete[] pollData;
    return count;
}
