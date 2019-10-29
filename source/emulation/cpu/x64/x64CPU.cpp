#include "boxedwine.h"
#include <SDL.h>

#ifdef BOXEDWINE_X64
#include "x64Ops.h"
#include "x64CPU.h"
#include "x64Asm.h"
#include "../../hardmmu/hard_memory.h"
#include "x64CodeChunk.h"
#include "../normal/normalCPU.h"

// hard to guage the benifit, seems like 1% to 3% with quake 2 and quake 3
bool x64CPU::hasBMI2 = true;
bool x64Intialized = false;

x64CPU::x64CPU() : nativeHandle(0), jmpBuf(NULL), endCond("x64CPU::endcond"), inException(false), restarting(false) {
    if (!x64Intialized) {
        x64Intialized = true;
        x64CPU::hasBMI2 = platformHasBMI2();
    }
}

typedef void (*StartCPU)();

void x64CPU::setSeg(U32 index, U32 address, U32 value) {
    CPU::setSeg(index, address, value);
    this->negSegAddress[index] = (U32)(-((S32)(this->seg[index].address)));
}

void x64CPU::run() {    
    while (true) {
        this->memOffset = this->thread->process->memory->id;
        this->negMemOffset = (U64)(-(S64)this->memOffset);
        for (int i=0;i<6;i++) {
            this->negSegAddress[i] = (U32)(-((S32)(this->seg[i].address)));
        }
		this->restarting = false;
        if (setjmp(this->runBlockJump)==0) {
            StartCPU start = (StartCPU)this->init();
            start();
#ifdef __TEST
            return;
#endif
        }
        if (this->inException) {
            this->inException = false;
        }
    }
}

void x64CPU::restart() {
	this->memOffset = this->thread->process->memory->id;
	this->negMemOffset = (U64)(-(S64)this->memOffset);
	for (int i = 0; i < 6; i++) {
		this->negSegAddress[i] = (U32)(-((S32)(this->seg[i].address)));
	}
	this->restarting = true;
    longjmp(this->runBlockJump, 1);
}

DecodedBlock* x64CPU::getNextBlock() {
    return NULL;
}

void* x64CPU::init() {
    X64Asm data(this);
    void* result;
    Memory* memory = this->thread->memory;
    x64CPU* cpu = this;

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(memory->executableMemoryMutex);

    data.pushNativeFlags();
    data.writeToRegFromValue(HOST_CPU, true, (U64)this, 8);
    data.writeToRegFromValue(HOST_MEM, true, cpu->memOffset, 8);

    data.writeToRegFromValue(HOST_SS, true, (U32)cpu->seg[SS].address, 4);
    data.writeToRegFromValue(HOST_DS, true, (U32)cpu->seg[DS].address, 4);

    data.setNativeFlags(this->flags, FMASK_TEST|DF);

    data.writeToRegFromValue(0, false, EAX, 4);
    data.writeToRegFromValue(1, false, ECX, 4);
    data.writeToRegFromValue(2, false, EDX, 4);
    data.writeToRegFromValue(3, false, EBX, 4);
    data.writeToRegFromValue(HOST_ESP, true, ESP, 4);
    data.writeToRegFromValue(5, false, EBP, 4);
    data.writeToRegFromValue(6, false, ESI, 4);
    data.writeToRegFromValue(7, false, EDI, 4);        
    
    data.calculatedEipLen = 1; // will force the long x64 chunk jump
    data.doJmp();
    X64CodeChunk* chunk = data.commit(true);
    result = chunk->getHostAddress();
    link(&data, chunk);
    this->pendingCodePages.clear();
    this->eipToHostInstruction = this->thread->memory->eipToHostInstruction;
    return result;
}

X64CodeChunk* x64CPU::translateChunk(X64Asm* parent, U32 ip) {
    X64Asm data1(this);
    data1.ip = ip;
    data1.startOfDataIp = ip;       
    data1.parent = parent;
    translateData(&data1);

    X64Asm data(this);
    data.ip = ip;
    data.startOfDataIp = ip;  
    data.calculatedEipLen = data1.ip - data1.startOfDataIp;
    data.parent = parent;
    translateData(&data, &data1);        
    S32 failedJumpOpIndex = this->preLinkCheck(&data);

    if (failedJumpOpIndex==-1) {
        X64CodeChunk* chunk = data.commit(false);
        link(&data, chunk);
        return chunk;
    } else {
        X64Asm data2(this);
        data2.ip = ip;
        data2.startOfDataIp = ip;       
        data2.parent = parent;
        data2.stopAfterInstruction = failedJumpOpIndex;
        translateData(&data2);

        X64Asm data3(this);
        data3.ip = ip;
        data3.startOfDataIp = ip;  
        data3.calculatedEipLen = data2.ip - data2.startOfDataIp;
        data3.parent = parent;
        data3.stopAfterInstruction = failedJumpOpIndex;
        translateData(&data3, &data2);

        X64CodeChunk* chunk = data3.commit(false);
        link(&data3, chunk);
        return chunk;
    }    
}

void* x64CPU::translateEipInternal(X64Asm* parent, U32 ip) {
    if (!this->big) {
        ip = ip & 0xFFFF;
    }
    U32 address = this->seg[CS].address+ip;
    void* result = this->thread->memory->getExistingHostAddress(address);

    if (!result) {
        X64CodeChunk* chunk = this->translateChunk(parent, ip);
        result = chunk->getHostAddress();
        chunk->makeLive();
    }
    return result;
}

#ifdef __TEST
void x64CPU::addReturnFromTest() {
    X64Asm data(this);
    data.addReturnFromTest();
    data.commit(true);
}
#endif

S32 x64CPU::preLinkCheck(X64Asm* data) {
    for (S32 i=0;i<data->todoJump.size();i++) {
        U32 eip = this->seg[CS].address+data->todoJump[i].eip;        
        U8 size = data->todoJump[i].offsetSize;

        if (size==4 && data->todoJump[i].sameChunk) {
            bool found = false;

            for (U32 ip=0;ip<data->ipAddressCount;ip++) {
                if (data->ipAddress[ip] == eip) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return data->todoJump[i].opIndex;
            }
        }
    }
    return -1;
}

void x64CPU::link(X64Asm* data, X64CodeChunk* fromChunk, U32 offsetIntoChunk) {
    U32 i;
    if (!fromChunk) {
        kpanic("x64CPU::link fromChunk missing");
    }
    for (i=0;i<data->todoJump.size();i++) {
        U32 eip = this->seg[CS].address+data->todoJump[i].eip;        
        U8* offset = (U8*)fromChunk->getHostAddress()+offsetIntoChunk+data->todoJump[i].bufferPos;
        U8 size = data->todoJump[i].offsetSize;

        if (size==4 && data->todoJump[i].sameChunk) {
            U8* host = (U8*)fromChunk->getHostFromEip(eip);
            if (!host) {
                kpanic("x64CPU::link can not link into the middle of an instruction");
            }
            data->write32Buffer(offset, (U32)(host - offset - 4));            
        } else if (size==4 && !data->todoJump[i].sameChunk) {
            U8* toHostAddress = (U8*)this->thread->memory->getExistingHostAddress(eip);

            if (!toHostAddress) {
                U8 op = 0xce;
                U32 hostIndex = 0;
                X64CodeChunk* chunk = X64CodeChunk::allocChunk(1, &eip, &hostIndex, &op, 1, eip-this->seg[CS].address, 1, false);
                chunk->makeLive();
                toHostAddress = (U8*)chunk->getHostAddress();            
            }
            X64CodeChunk* toChunk = this->thread->memory->getCodeChunkContainingHostAddress(toHostAddress);
            if (!toChunk) {
                kpanic("x64CPU::link to chunk missing");
            }
            X64CodeChunkLink* link = toChunk->addLinkFrom(fromChunk, eip, toHostAddress, offset, true);
            data->write32Buffer(offset, (U32)(toHostAddress - offset - 4));            
        } else if (size==8 && !data->todoJump[i].sameChunk) {
            U8* toHostAddress = (U8*)this->thread->memory->getExistingHostAddress(eip);

            if (!toHostAddress) {
                U8 op = 0xce;
                U32 hostIndex = 0;
                X64CodeChunk* chunk = X64CodeChunk::allocChunk(1, &eip, &hostIndex, &op, 1, eip-this->seg[CS].address, 1, false);
                chunk->makeLive();
                toHostAddress = (U8*)chunk->getHostAddress();            
            }
            X64CodeChunk* toChunk = this->thread->memory->getCodeChunkContainingHostAddress(toHostAddress);
            if (!toChunk) {
                kpanic("x64CPU::link to chunk missing");
            }
            X64CodeChunkLink* link = toChunk->addLinkFrom(fromChunk, eip, toHostAddress, offset, false);
            data->write64Buffer(offset, (U64)&link->toHostInstruction);
        } else {
            kpanic("x64CPU::link unexpected patch size");
        }
    }
    markCodePageReadOnly(data);
}

void x64CPU::markCodePageReadOnly(X64Asm* data) {
    U32 pageStart = (data->startOfDataIp+this->seg[CS].address) >> K_PAGE_SHIFT;
    U32 pageEnd = (data->startOfDataIp+this->seg[CS].address) >> K_PAGE_SHIFT;
    S32 pageCount = pageEnd-pageStart+1;

#ifndef __TEST
    for (int i=0;i<pageCount;i++) {        
        pendingCodePages.push_back(pageStart+i);        
    }
#endif    
}

void x64CPU::makePendingCodePagesReadOnly() {
    for (int i=0;i<this->pendingCodePages.size();i++) {
        // the chunk could cross a page and be a mix of dynamic and non dynamic code
        if (this->thread->memory->dynamicCodePageUpdateCount[this->pendingCodePages[i]]!=MAX_DYNAMIC_CODE_PAGE_COUNT) {
            ::makeCodePageReadOnly(this->thread->memory, this->pendingCodePages[i]);
        }
    }
    this->pendingCodePages.clear();
}

void* x64CPU::translateEip(U32 ip) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);

    void* result = translateEipInternal(NULL, ip);
    makePendingCodePagesReadOnly();
    return result;
}

void x64CPU::translateInstruction(X64Asm* data, X64Asm* firstPass) {
    data->startOfOpIp = data->ip;        
#ifdef _DEBUG
    //data->logOp(data->ip);
    // just makes debugging the asm output easier
#ifndef __TEST
    data->writeToMemFromValue(data->ip, HOST_CPU, true, -1, false, 0, CPU_OFFSET_EIP, 4, false);
#endif
#endif    
    if (data->dynamic) {
        data->addDynamicCheck(false);
    } else {
#ifdef _DEBUG
        data->addDynamicCheck(true);
#endif
    }    
    while (1) {  
        data->op = data->fetch8();            
        data->inst = data->baseOp + data->op;        
        if (!x64Decoder[data->inst](data)) {                
            break;
        }            
    }
    data->tmp1InUse = false;
    data->tmp2InUse = false;
    data->tmp3InUse = false;
}

void x64CPU::translateData(X64Asm* data, X64Asm* firstPass) {
    Memory* memory = data->cpu->thread->memory;

    U32 codePage = (data->ip+data->cpu->seg[CS].address) >> K_PAGE_SHIFT;
    if (this->thread->memory->dynamicCodePageUpdateCount[codePage]==MAX_DYNAMIC_CODE_PAGE_COUNT) {
        data->dynamic = true;
    }
    while (1) {  
        U32 address = data->cpu->seg[CS].address+data->ip;
        void* hostAddress = this->thread->memory->getExistingHostAddress(address);
        if (hostAddress) {
            data->jumpTo(data->ip);
            break;
        }
        if (firstPass) {
            U32 nextEipLen = firstPass->calculateEipLen(data->ip+data->cpu->seg[CS].address);
            U32 page = (data->ip+data->cpu->seg[CS].address+nextEipLen) >> K_PAGE_SHIFT;

            if (page!=codePage) {
                codePage = page;
                if (data->dynamic) {                    
                    if (this->thread->memory->dynamicCodePageUpdateCount[codePage] == MAX_DYNAMIC_CODE_PAGE_COUNT) {
                        // continue to cross from my dynamic page into another dynamic page
                    } else {
                        // we will continue to emit code that will self check for modified code, even though the page we spill into is not dynamic
                    }
                } else {
                    if (this->thread->memory->dynamicCodePageUpdateCount[codePage] == MAX_DYNAMIC_CODE_PAGE_COUNT) {
                        // we crossed a page boundry from a non dynamic page to a dynamic page
                        data->dynamic = true; // the instructions from this point on will do their own check
                    } else {
                        // continue to cross from one non dynamic page into another non dynamic page
                    }
                }
            }
        }
        data->mapAddress(address, data->bufferPos);
        U32 page = address >> K_PAGE_SHIFT;
        translateInstruction(data, firstPass);
        if (data->done) {
            break;
        }
        if (data->stopAfterInstruction!=-1 && data->ipAddressCount==data->stopAfterInstruction) {
            break;
        }
        data->resetForNewOp();
    }     
}

static U8 fetchByte(U32 *eip) {
    return readb((*eip)++);
}

DecodedOp* x64CPU::getOp(U32 eip, bool existing) {
    if (this->big) {
        eip+=this->seg[CS].address;
    } else {
        eip=this->seg[CS].address + (eip & 0xFFFF);
    }        
    if (!existing || (this->eipToHostInstruction[eip >> K_PAGE_SHIFT] && this->eipToHostInstruction[eip >> K_PAGE_SHIFT][eip & K_PAGE_MASK])) {
        THREAD_LOCAL static DecodedBlock* block;
        if (!block) {
            block = new DecodedBlock();
        }
        decodeBlock(fetchByte, eip, this->big, 4, 64, 1, block);
        return block->op;
    }
    return NULL;
}

U64 x64CPU::handleCodePatch(U64 rip, U32 address, U64 rsi, U64 rdi, std::function<void(DecodedOp*)> doSyncFrom, std::function<void(DecodedOp*)> doSyncTo) {
#ifndef __TEST
    // only one thread at a time can update the host code pages and related date like opToAddressPages
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);
#endif
    // get the emulated eip of the op that corresponds to the host address where the exception happened
    X64CodeChunk* chunk = this->thread->memory->getCodeChunkContainingHostAddress((void*)rip);
    this->eip.u32 = chunk->getEipThatContainsHostAddress((void*)rip, NULL, NULL)-this->seg[CS].address;

    // get the emulated op that caused the write
    DecodedOp* op = this->getOp(this->eip.u32, true);
    if (op) {             
        // change permission of the page so that we can write to it
        U32 len = instructionInfo[op->inst].writeMemWidth/8;
        X64AsmCodeMemoryWrite w(this);        
        static DecodedBlock b;
        DecodedBlock::currentBlock = &b;
        b.next1 = &b;
        b.next2 = &b;
        // do the write
        op->pfn = NormalCPU::getFunctionForOp(op);
        op->next = DecodedOp::alloc();
        op->next->inst = Done;
        op->next->pfn = NormalCPU::getFunctionForOp(op->next);
        doSyncFrom(op);

        if (this->flags & DF) {
            this->df = -1;
        } else {
            this->df = 1;
        }
        // for string instruction, we modify (add memory offset and segment) rdi and rsi so that the native string instruction can be used, this code will revert it back to the original values
        // uses si
        if (op->inst==Lodsb || op->inst==Lodsw || op->inst==Lodsd) {
            THIS_ESI=(U32)(rsi - this->memOffset - this->seg[op->base].address);
            // doesn't write            
        }
        // uses di (Examples: diablo 1 will trigger this in the middle of the Stosd when creating a new game)
        else if (op->inst==Stosb || op->inst==Stosw || op->inst==Stosd ||
            op->inst==Scasb || op->inst==Scasw || op->inst==Scasd) {
            THIS_EDI=(U32)(rdi - this->memOffset - this->seg[ES].address);
            if (instructionInfo[op->inst].writeMemWidth) {
                w.invalidateStringWriteToDi(op->repNotZero || op->repZero, instructionInfo[op->inst].writeMemWidth/8);
            }
        }
        // uses si and di
        else if (op->inst==Movsb || op->inst==Movsw || op->inst==Movsd ||
            op->inst==Cmpsb || op->inst==Cmpsw || op->inst==Cmpsd) {
            THIS_ESI=(U32)(rsi - this->memOffset - this->seg[op->base].address);
            THIS_EDI=(U32)(rdi - this->memOffset - this->seg[ES].address);
            if (instructionInfo[op->inst].writeMemWidth) {
                w.invalidateStringWriteToDi(op->repNotZero || op->repZero, instructionInfo[op->inst].writeMemWidth/8);
            }
        } else {
            w.invalidateCode(address, len);
        }  
        FILE* f = (FILE*)this->logFile;
        this->logFile = NULL;       
        op->pfn(this, op);   
        this->logFile = f;        
        doSyncTo(op);

        // eip was ajusted after running this instruction                        
        U32 a = this->getEipAddress();
        if (!this->eipToHostInstruction[a >> K_PAGE_SHIFT] || !this->eipToHostInstruction[a >> K_PAGE_SHIFT][a & K_PAGE_MASK]) {
            this->translateEip(this->eip.u32);
        }
        U64 result = (U64)this->eipToHostInstruction[a >> K_PAGE_SHIFT][a & K_PAGE_MASK];
        if (result==0) {
            kpanic("x64::handleCodePatch failed to translate code");
        }
        op->dealloc(true);
        return result;
    } else {                        
        kpanic("Threw an exception from a host location that doesn't map to an emulated instruction");
    }
    return 0;
}

U64 x64CPU::handleChangedUnpatchedCode(U64 rip) {
#ifndef __TEST
    // only one thread at a time can update the host code pages and related date like opToAddressPages
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);
#endif
                        
    unsigned char* hostAddress = (unsigned char*)rip;
    X64CodeChunk* chunk = this->thread->memory->getCodeChunkContainingHostAddress(hostAddress);
    if (!chunk) {
        /* some debug code
        U32 toEip = *(((U32*)hostAddress)+2);
        KThread* thread = KThread::currentThread();
        Memory* memory = thread->memory;

        for (U32 i=0;i<K_NUMBER_OF_PAGES;i++) {
            X64CodeChunk* chunk = memory->hostCodeChunks[i];
            while (chunk) {
                if (chunk->hasLinkTo(hostAddress)) {
                    int ii=0;
                }
                if (chunk->hasLinkToEip(toEip)) {
                    int ii=0;
                }
                chunk = chunk->getNext();
            }
        }
        */
        kpanic("x64CPU::handleChangedUnpatchedCode: could not find chunk");
    }
    U32 startOfEip = chunk->getEipThatContainsHostAddress(hostAddress, NULL, NULL);
    if (!chunk->isDynamicAware() || !chunk->retranslateSingleInstruction(this, hostAddress)) {        
        chunk->deallocAndRetranslate();   
    }
    U64 result = (U64)this->thread->memory->getExistingHostAddress(startOfEip);
    if (result==0) {
        result = (U64)this->translateEip(startOfEip-this->seg[CS].address);
    }
    if (result==0) {
        kpanic("x64CPU::handleChangedUnpatchedCode failed to translate code in exception");
    }
    return result;
}

U64 x64CPU::handleMissingCode(U64 r8, U64 r9, U32 inst) {
    U32 page = (U32)r8;
    U32 offset = (U32)r9;

    this->translateEip(((page << K_PAGE_SHIFT) | offset) - this->seg[CS].address);  
    if (inst==0xCA148B4F) {
        return (U64)(this->eipToHostInstruction[page]);
    } else {
        return (U64)(this->eipToHostInstruction[page][offset]);
    }
}

U64 x64CPU::handleIllegalInstruction(U64 rip) {    
    if (*((U8*)rip)==0xce) {            
        return this->handleChangedUnpatchedCode(rip);
    } 
    if (*((U8*)rip)==0xcd) { 
        // free'd chunks are filled in with 0xcd, if this one is free'd, it is possible another thread replaced the chunk
        // while this thread jumped to it and this thread waited in the critical section at the top of this function.
        void* host = this->thread->memory->getExistingHostAddress(this->eip.u32+this->seg[CS].address);
        if (host) {
            return (U64)host;
        } else {
            kpanic("x64CPU::handleIllegalInstruction tried to run code in a free'd chunk");
        }
    }
    return 0;
}

U32 dynamicCodeExceptionCount;

U64 x64CPU::handleAccessException(U64 rip, U64 address, bool readAddress, U64 rsi, U64 rdi, U64 r8, U64 r9, U64* r10, std::function<void(DecodedOp*)> doSyncFrom, std::function<void(DecodedOp*)> doSyncTo) {
    U32 inst = *((U32*)rip);
    if (address==0x377032 || rip==0x3000738c1e) {
        int ii=0;
    }
    if (inst==0x0A8B4566 || inst==0xCA148B4F) { // if these constants change, update handleMissingCode too     
        // rip is not adjusted so we don't need to check for stack alignment
        *r10 = this->handleMissingCode(r8, r9, inst);
        return 0;
    } else if (inst==0xcdcdcdcd) {
        // this thread was waiting on the critical section and the thread that was currently in this handler removed the code we were running
        void* host = this->thread->memory->getExistingHostAddress(this->eip.u32+this->seg[CS].address);
        if (host) {
            return (U64)host;
        } else {
            kpanic("x64CPU::handleAccessException tried to run code in a free'd chunk");
            return 0;
        }
    } else {          
        // check if the emulated memory caused the exception
        if ((address & 0xFFFFFFFF00000000l) == this->thread->memory->id) {                
            U32 emulatedAddress = (U32)address;
            
            // check if emulated memory that caused the exception is a page that has code
            if (this->thread->memory->nativeFlags[emulatedAddress>>K_PAGE_SHIFT] & NATIVE_FLAG_CODEPAGE_READONLY) {                    
                dynamicCodeExceptionCount++;                    
                return this->handleCodePatch(rip, emulatedAddress, rsi, rdi, doSyncFrom, doSyncTo);                    
            }
        }   
#ifdef _DEBUG
        void* fromHost = this->thread->memory->getExistingHostAddress(this->fromEip);
        X64CodeChunk* chunk = this->thread->memory->getCodeChunkContainingHostAddress((void*)rip);
#endif
        doSyncFrom(NULL);
        this->thread->seg_mapper((U32)address, readAddress, !readAddress);
        doSyncTo(NULL);
        U64 result = (U64)this->translateEip(this->eip.u32); 
        if (result==0) {
            kpanic("x64CPU::handleAccessException failed to translate code");
        }
        return result;
    }
}

U64 x64CPU::handleDivByZero(std::function<void(DecodedOp*)> doSyncFrom, std::function<void(DecodedOp*)> doSyncTo) {
    doSyncFrom(NULL);
    this->prepareException(EXCEPTION_DIVIDE, 0);
    doSyncTo(NULL);
    U64 result = (U64)this->translateEip(this->eip.u32); 
    if (result==0) {
        kpanic("x64CPU::handleDivByZero failed to translate code");
    }
    return result;
}

void OPCALL unscheduleCallback(CPU* cpu, DecodedOp* op) {
    jmp_buf* jmpBuf = ((x64CPU*)KThread::currentThread()->cpu)->jmpBuf;
    longjmp(*jmpBuf,1);
}


U64 x64CPU::startException(U64 address, bool readAddress, std::function<void(DecodedOp*)> doSyncFrom, std::function<void(DecodedOp*)> doSyncTo) {
    if (this->thread->exiting) {
        X64Asm data(this);
        data.callCallback((void*)unscheduleCallback);
        data.callCallback((void*)unscheduleCallback);        
        return (U64)data.commit(true)->getHostAddress();                
    }
    if (this->inException) {
        doSyncFrom(NULL);
        this->thread->seg_mapper((U32)address, readAddress, !readAddress);
        doSyncTo(NULL);
        U64 result = (U64)this->translateEip(this->eip.u32); 
        if (result==0) {
            kpanic("x64CPU::startException failed to translate code in exception");
        }
        return result;
    } 
    return 0;
}

extern U32 platformThreadCount;

void x64CPU::startThread() {
    jmp_buf jmpBuf;
    U32 threadId = thread->id;
    U32 processId = thread->process->id;

    KThread::setCurrentThread(thread);       

    // :TODO: hopefully this will eventually go away.  For now this prevents a signal from being generated which isn't handled yet
    SDL_Delay(50);   

    if (!setjmp(jmpBuf)) {
        this->jmpBuf = &jmpBuf;
        this->run();
    }
    thread->exited = true;
    while (thread->process) {
        SDL_Delay(10);
    }

    // in case we exited because of an exit syscall, but the process is also in the middle of an exit_group
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(KProcess::exitGroupMutex);

    BOXEDWINE_CONDITION_LOCK(this->endCond);
    BOXEDWINE_CONDITION_SIGNAL(this->endCond);
    BOXEDWINE_CONDITION_UNLOCK(this->endCond);

    delete thread;
    
    platformThreadCount--;
    if (platformThreadCount==0) {
        SDL_Event sdlevent;
        sdlevent.type = SDL_QUIT;        
        SDL_PushEvent(&sdlevent);
    }
}

void x64CPU::unscheduleThread() {
    thread->exiting = true;
    BoxedWineCondition* cond = thread->waitingCond;
    if (cond) {
        cond->signal();
    }    
    while (!thread->exited) {
        SDL_Delay(10);
    }
    // another thread in this process could have called exit while we are in the middle of an exit_group
    KProcess* process = thread->process;
    if (process) {
        process->removeThread(thread);
        thread->process = NULL;
    }
}

void addTimer(KTimer* timer) {
    kpanic("addTimer not implemented yet");
}

void removeTimer(KTimer* timer) {
    if (timer->active)
        kpanic("removeTimer not implemented yet");
}

void unscheduleThread(KThread* thread) {
    if (thread==KThread::currentThread())
        return;

    ((x64CPU*)thread->cpu)->unscheduleThread();
}

void unscheduleCurrentThread() {
    jmp_buf* jmpBuf = ((x64CPU*)KThread::currentThread()->cpu)->jmpBuf;
    longjmp(*jmpBuf,1);
}

#endif
