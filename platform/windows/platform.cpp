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
#include <winsock2.h>
#include "pixelformat.h"

LONGLONG PCFreq;
LONGLONG CounterStart;

void Platform::startMicroCounter()
{
    LARGE_INTEGER li;

    QueryPerformanceFrequency(&li);

    PCFreq = li.QuadPart;

    QueryPerformanceCounter(&li);
    CounterStart = li.QuadPart;
}

ULONGLONG Platform::getMicroCounter()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (li.QuadPart-CounterStart)*1000000/PCFreq;
}

ULONGLONG Platform::getSystemTimeAsMicroSeconds() {
    FILETIME tm;
    ULONGLONG t;

    GetSystemTimeAsFileTime( &tm );
    t = ((ULONGLONG)tm.dwHighDateTime << 32) | (ULONGLONG)tm.dwLowDateTime;
    t-=116444736000000000l;
    t/=10;
    /*
    if (!startTime) {
        startTime = t;
    } else {
        ULONGLONG diff = t - startTime;
        t = startTime+diff/20;
    }
    */
    return t;
}

void Platform::listNodes(const std::string& nativePath, std::vector<ListNodeResult>& results) {
    std::string path;
    WIN32_FIND_DATA findData;
    HANDLE hFind;

    path = nativePath+"\\*.*";;
    hFind = FindFirstFile(path.c_str(), &findData); 
    if(hFind != INVALID_HANDLE_VALUE)  { 		
        do  { 
            if (strcmp(findData.cFileName, ".") && strcmp(findData.cFileName, ".."))  {
                results.push_back(ListNodeResult(findData.cFileName, (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)!=0));
            }
        } while(FindNextFile(hFind, &findData)); 
        FindClose(hFind); 
    }
}

int Platform::nativeSocketPair(S32 socks[2]) {
    union {
       struct sockaddr_in inaddr;
       struct sockaddr addr;
    } a;
    SOCKET listener;
    int e;
    socklen_t addrlen = sizeof(a.inaddr);
    DWORD flags = 0;
    int reuse = 1;

    if (socks == 0) {
      WSASetLastError(WSAEINVAL);
      return SOCKET_ERROR;
    }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) 
        return SOCKET_ERROR;

    memset(&a, 0, sizeof(a));
    a.inaddr.sin_family = AF_INET;
    a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.inaddr.sin_port = 0; 

    socks[0] = socks[1] = (U32)INVALID_SOCKET;
    do {
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,  (char*) &reuse, (socklen_t) sizeof(reuse)) == -1)
            break;
        if  (bind(listener, &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
            break;
        if  (getsockname(listener, &a.addr, &addrlen) == SOCKET_ERROR)
            break;
        if (listen(listener, 1) == SOCKET_ERROR)
            break;
        socks[0] = (U32)WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, flags);
        if (socks[0] == (U32)INVALID_SOCKET)
            break;
        if (connect(socks[0], &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
            break;
        socks[1] = (U32)accept(listener, NULL, NULL);
        if (socks[1] == INVALID_SOCKET)
            break;

        closesocket(listener);
        return 0;
    } while (0);

    e = WSAGetLastError();
    closesocket(listener);
    closesocket(socks[0]);
    closesocket(socks[1]);
    WSASetLastError(e);
    return SOCKET_ERROR;
}

int getPixelFormats(PixelFormat* pfd, int maxPfs) {
    PIXELFORMATDESCRIPTOR p;
    HDC hdc = GetDC(GetDesktopWindow());
    int count = DescribePixelFormat(hdc, 0, 0, NULL);
    int result = 1;
    int i;

    for (i=1;i<=count && result<maxPfs;i++) {
        DescribePixelFormat(hdc, i, sizeof(p), &p);
        if ((p.dwFlags & PFD_SUPPORT_OPENGL) && p.cColorBits<=32 && !(p.dwFlags & PFD_GENERIC_FORMAT)) {
            pfd[result].nSize = 40;
            pfd[result].nVersion = 1;
            pfd[result].dwFlags = p.dwFlags;
            pfd[result].iPixelType = p.iPixelType;
            pfd[result].cColorBits = p.cColorBits;
            pfd[result].cRedBits = p.cRedBits;
            pfd[result].cRedShift = p.cRedShift;
            pfd[result].cGreenBits = p.cGreenBits;
            pfd[result].cGreenShift = p.cGreenShift;
            pfd[result].cBlueBits = p.cBlueBits;
            pfd[result].cBlueShift = p.cBlueShift;
            pfd[result].cAlphaBits = p.cAlphaBits;
            pfd[result].cAlphaShift = p.cAlphaShift;
            pfd[result].cAccumBits = p.cAccumBits;
            pfd[result].cAccumRedBits = p.cAccumRedBits;
            pfd[result].cAccumGreenBits = p.cAccumGreenBits;
            pfd[result].cAccumBlueBits = p.cAccumBlueBits;
            pfd[result].cAccumAlphaBits = p.cAccumAlphaBits;
            pfd[result].cDepthBits = p.cDepthBits;
            pfd[result].cStencilBits = p.cStencilBits;
            pfd[result].cAuxBuffers = p.cAuxBuffers;
            pfd[result].iLayerType = p.iLayerType;
            pfd[result].bReserved = p.bReserved;
            pfd[result].dwLayerMask = p.dwLayerMask;
            pfd[result].dwVisibleMask = p.dwVisibleMask;
            pfd[result].dwDamageMask = p.dwDamageMask;
            result++;
            fprintf(stderr, "Pixel Format: %d bit (%d%d%d%d) %s:%s depth=%d stencil=%d accum=%d\n", (int)p.cColorBits, (int)p.cRedBits, (int)p.cBlueBits, (int)p.cGreenBits, (int)p.cAlphaBits, (p.dwFlags & K_PFD_GENERIC_FORMAT)?"not accelerated":"accelerated", (p.dwFlags & K_PFD_DOUBLEBUFFER)?"double buffered":"single buffered", (int)p.cDepthBits, (int)p.cStencilBits, (int)p.cAccumBits);
        }
    }
    if (result==1) {
        for (i=1;i<=count && result<maxPfs;i++) {
            DescribePixelFormat(hdc, i, sizeof(p), &p);
            if ((p.dwFlags & PFD_SUPPORT_OPENGL) && p.cColorBits<=32) {
                pfd[result].nSize = 40;
                pfd[result].nVersion = 1;
                pfd[result].dwFlags = p.dwFlags;
                pfd[result].iPixelType = p.iPixelType;
                pfd[result].cColorBits = p.cColorBits;
                pfd[result].cRedBits = p.cRedBits;
                pfd[result].cRedShift = p.cRedShift;
                pfd[result].cGreenBits = p.cGreenBits;
                pfd[result].cGreenShift = p.cGreenShift;
                pfd[result].cBlueBits = p.cBlueBits;
                pfd[result].cBlueShift = p.cBlueShift;
                pfd[result].cAlphaBits = p.cAlphaBits;
                pfd[result].cAlphaShift = p.cAlphaShift;
                pfd[result].cAccumBits = p.cAccumBits;
                pfd[result].cAccumRedBits = p.cAccumRedBits;
                pfd[result].cAccumGreenBits = p.cAccumGreenBits;
                pfd[result].cAccumBlueBits = p.cAccumBlueBits;
                pfd[result].cAccumAlphaBits = p.cAccumAlphaBits;
                pfd[result].cDepthBits = p.cDepthBits;
                pfd[result].cStencilBits = p.cStencilBits;
                pfd[result].cAuxBuffers = p.cAuxBuffers;
                pfd[result].iLayerType = p.iLayerType;
                pfd[result].bReserved = p.bReserved;
                pfd[result].dwLayerMask = p.dwLayerMask;
                pfd[result].dwVisibleMask = p.dwVisibleMask;
                pfd[result].dwDamageMask = p.dwDamageMask;
                result++;
                fprintf(stderr, "Pixel Format: %d bit (%d%d%d%d) %s:%s depth=%d stencil=%d accum=%d\n", p.cColorBits, p.cRedBits, p.cBlueBits, p.cGreenBits, p.cAlphaBits, (p.dwFlags & K_PFD_GENERIC_FORMAT)?"not accelerated":"accelerated", (p.dwFlags & K_PFD_DOUBLEBUFFER)?"double buffered":"single buffered", p.cDepthBits, p.cStencilBits, p.cAccumBits);
            }
        }
    }
    return result;
}

#ifdef BOXEDWINE_X64
bool platformHasBMI2() {
    int regs[4];

    __cpuidex(regs, 7, 0);
    if (regs[1] & (1 << 8)) {
        return true;
    }
    return false;
}
#endif