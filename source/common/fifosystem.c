// SPDX-License-Identifier: Zlib
//
// Copyright (c) 2008-2015 Dave Murphy (WinterMute)
// Copyright (c) 2023 Antonio Niño Díaz

#include <nds/bios.h>
#include <nds/cothread.h>
#include <nds/fifocommon.h>
#include <nds/interrupts.h>
#include <nds/ipc.h>
#include <nds/system.h>

#include <stdlib.h>
#include <string.h>

#include "fifo_private.h"

// Maximum number of bytes that can be sent in a fifo message
#define FIFO_MAX_DATA_BYTES     128

// Number of words that can be stored temporarily while waiting to deque them
#ifdef ARM9
#define FIFO_BUFFER_ENTRIES     256
#else // ARM7
#define FIFO_BUFFER_ENTRIES     256
#endif

// The memory overhead of this library (per CPU) is:
//
//     16 + (NUM_CHANNELS * 32) + (FIFO_BUFFER_ENTRIES * 8)
//
// For 16 channels and 256 entries, this is 16 + 512 + 2048 = 2576 bytes of ram.
//
// Some padding may be added by the compiler, though.

// In the fifo_buffer[] array, this value means that there are no more values
// left to handle.
#define FIFO_BUFFER_TERMINATE   0xFFFF

// FIFO buffers format
// -------------------
//
// All entries in fifo_buffer are formed of two 32-bit values:
//
// 31 ... 28 | 27 ... 16 | 15 ... 0 || 31 ... 0
// ----------+-----------+----------++----------
// Control   | Extra     | Next     || Data
//
// - Control: "UNUSED" or "DATASTART".
// - Extra: Used for data messages, it specifies the size in bytes.
// - Next: Index of next block in the list. If "Next == FIFO_BUFFER_TERMINATE"
//   it means that is the end of the list.

// FIFO_BUFFER_ENTRIES * 8 bytes of global buffer space
vu32 fifo_buffer[FIFO_BUFFER_ENTRIES * 2];

#define FIFO_BUFFERCONTROL_UNUSED       0
#define FIFO_BUFFERCONTROL_DATASTART    5

#define FIFO_BUFFER_DATA(index) \
    fifo_buffer[(index) * 2 + 1]

// Mask used to extract the index in fifo_buffer[] of the next block
#define FIFO_BUFFER_NEXTMASK    0xFFFF

static inline u32 FIFO_BUFFER_GETNEXT(u32 index)
{
    return fifo_buffer[index * 2] & FIFO_BUFFER_NEXTMASK;
}

static inline u32 FIFO_BUFFER_GETEXTRA(u32 index)
{
    return (fifo_buffer[index * 2] >> 16) & 0xFFF;
}

static inline void FIFO_BUFFER_SETCONTROL(u32 index, u32 next, u32 control, u32 extra)
{
    fifo_buffer[index * 2] = (next & FIFO_BUFFER_NEXTMASK) |
                             (control << 28) | ((extra & 0xFFF) << 16);
}

static inline void FIFO_BUFFER_SETNEXT(u32 index, u32 next)
{
    fifo_buffer[index * 2] = (next & FIFO_BUFFER_NEXTMASK) |
                             (fifo_buffer[index * 2] & ~FIFO_BUFFER_NEXTMASK);
}

typedef struct fifo_queue {
    vu16 head;
    vu16 tail;
} fifo_queue;

fifo_queue fifo_address_queue[FIFO_NUM_CHANNELS];
fifo_queue fifo_data_queue[FIFO_NUM_CHANNELS];
fifo_queue fifo_value32_queue[FIFO_NUM_CHANNELS];

fifo_queue fifo_buffer_free = {0, FIFO_BUFFER_ENTRIES - 1};
fifo_queue fifo_send_queue = {FIFO_BUFFER_TERMINATE, FIFO_BUFFER_TERMINATE};
fifo_queue fifo_receive_queue = {FIFO_BUFFER_TERMINATE, FIFO_BUFFER_TERMINATE};

static comutex_t fifo_mutex[FIFO_NUM_CHANNELS];

vu32 fifo_freewords = FIFO_BUFFER_ENTRIES;

FifoAddressHandlerFunc fifo_address_func[FIFO_NUM_CHANNELS];
FifoValue32HandlerFunc fifo_value32_func[FIFO_NUM_CHANNELS];
FifoDatamsgHandlerFunc fifo_datamsg_func[FIFO_NUM_CHANNELS];
void *fifo_address_data[FIFO_NUM_CHANNELS];
void *fifo_value32_data[FIFO_NUM_CHANNELS];
void *fifo_datamsg_data[FIFO_NUM_CHANNELS];

// Set a callback to receive incoming address messages on a specific channel.
bool fifoSetAddressHandler(u32 channel, FifoAddressHandlerFunc newhandler, void *userdata)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    int oldIME = enterCriticalSection();

    fifo_address_func[channel] = newhandler;
    fifo_address_data[channel] = userdata;

    if (newhandler)
    {
        while (fifoCheckAddress(channel))
        {
            newhandler(fifoGetAddress(channel), userdata);
        }
    }

    leaveCriticalSection(oldIME);

    return true;
}

// Set a callback to receive incoming value32 messages on a specific channel.
bool fifoSetValue32Handler(u32 channel, FifoValue32HandlerFunc newhandler, void *userdata)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    int oldIME = enterCriticalSection();

    fifo_value32_func[channel] = newhandler;
    fifo_value32_data[channel] = userdata;

    if (newhandler)
    {
        while (fifoCheckValue32(channel))
        {
            newhandler(fifoGetValue32(channel), userdata);
        }
    }

    leaveCriticalSection(oldIME);

    return true;
}

// Set a callback to receive incoming data sequences on a specific channel.
bool fifoSetDatamsgHandler(u32 channel, FifoDatamsgHandlerFunc newhandler, void *userdata)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    int oldIME = enterCriticalSection();

    fifo_datamsg_func[channel] = newhandler;
    fifo_datamsg_data[channel] = userdata;

    if (newhandler)
    {
        while (fifoCheckDatamsg(channel))
        {
            int block = fifo_data_queue[channel].head;
            int n_bytes = FIFO_UNPACK_DATALENGTH(FIFO_BUFFER_DATA(block));
            newhandler(n_bytes, userdata);
            if (block == fifo_data_queue[channel].head)
                fifoGetDatamsg(channel, 0, 0);
        }
    }

    leaveCriticalSection(oldIME);

    return true;
}

static u32 fifo_allocBlock(void)
{
    if (fifo_freewords == 0)
        return FIFO_BUFFER_TERMINATE;

    u32 entry = fifo_buffer_free.head;
    fifo_buffer_free.head = FIFO_BUFFER_GETNEXT(fifo_buffer_free.head);
    FIFO_BUFFER_SETCONTROL(entry, FIFO_BUFFER_TERMINATE, FIFO_BUFFERCONTROL_UNUSED, 0);
    fifo_freewords--;
    return entry;
}

static u32 fifo_waitBlock(void)
{
    u32 block;
    do {
        block = fifo_allocBlock();
        if (block == FIFO_BUFFER_TERMINATE)
        {
            REG_IPC_FIFO_CR |= IPC_FIFO_SEND_IRQ;
            REG_IME = 1;
            swiIntrWait(0, IRQ_FIFO_EMPTY);
            REG_IME = 0;
        }
    } while (block == FIFO_BUFFER_TERMINATE);

    return block;
}

static void fifo_freeBlock(u32 index)
{
    FIFO_BUFFER_SETCONTROL(index, FIFO_BUFFER_TERMINATE, FIFO_BUFFERCONTROL_UNUSED, 0);
    FIFO_BUFFER_SETCONTROL(fifo_buffer_free.tail, index, FIFO_BUFFERCONTROL_UNUSED, 0);
    fifo_buffer_free.tail = index;
    fifo_freewords++;
}

bool fifoInternalSend(u32 firstword, u32 extrawordcount, u32 *wordlist)
{
    if (extrawordcount > 0 && wordlist == NULL)
        return false;

    if (fifo_freewords < extrawordcount + 1)
        return false;

    if (extrawordcount > (FIFO_MAX_DATA_BYTES / 4))
        return false;

    u32 count = 0;
    int oldIME = enterCriticalSection();

    u32 head = fifo_waitBlock();
    if (fifo_send_queue.head == FIFO_BUFFER_TERMINATE)
    {
        fifo_send_queue.head = head;
    }
    else
    {
        FIFO_BUFFER_SETNEXT(fifo_send_queue.tail, head);
    }
    FIFO_BUFFER_DATA(head) = firstword;
    fifo_send_queue.tail = head;

    while (count < extrawordcount)
    {
        u32 next = fifo_waitBlock();
        if (fifo_send_queue.head == FIFO_BUFFER_TERMINATE)
        {
            fifo_send_queue.head = next;
        }
        else
        {
            FIFO_BUFFER_SETNEXT(fifo_send_queue.tail,next);
        }
        FIFO_BUFFER_DATA(next) = wordlist[count];
        count++;
        fifo_send_queue.tail = next;
    }

    REG_IPC_FIFO_CR |= IPC_FIFO_SEND_IRQ;

    leaveCriticalSection(oldIME);

    return true;
}

// Send an address (from mainram only) to the other cpu (on a specific channel)
// Addresses can be in the range of 0x02000000-0x02FFFFFF
bool fifoSendAddress(u32 channel, void *address)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    if (!FIFO_IS_ADDRESS_COMPATIBLE(address))
        return false;

    return fifoInternalSend(FIFO_PACK_ADDRESS(channel, address), 0, 0);
}

bool fifoSendValue32(u32 channel, u32 value32)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    u32 send_first, send_extra[1];

    if (FIFO_VALUE32_NEEDEXTRA(value32))
    {
        // The value doesn't fit in just one 32-bit message
        send_first = FIFO_PACK_VALUE32_EXTRA(channel);
        send_extra[0] = value32;
        return fifoInternalSend(send_first, 1, send_extra);
    }
    else
    {
        // The value fits in a 32-bit message
        send_first = FIFO_PACK_VALUE32(channel, value32);
        return fifoInternalSend(send_first, 0, 0);
    }
}

bool fifoSendDatamsg(u32 channel, u32 num_bytes, u8 *data_array)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    if (num_bytes == 0)
    {
        u32 send_first = FIFO_PACK_DATAMSG_HEADER(channel, 0);
        return fifoInternalSend(send_first, 0, NULL);
    }

    if (data_array == NULL)
        return false;

    if (num_bytes >= FIFO_MAX_DATA_BYTES) // TODO: Should this be ">"?
        return false;

    u32 num_words = (num_bytes + 3) >> 2;

    u32 buffer_array[num_words]; // TODO: This is a VLA, remove?

    if (fifo_freewords < num_words + 1)
        return false;

    buffer_array[num_words - 1] = 0; // zero out last few bytes before the copy
    memcpy(buffer_array, data_array, num_bytes);
    u32 send_first = FIFO_PACK_DATAMSG_HEADER(channel, num_bytes);
    return fifoInternalSend(send_first, num_words, buffer_array);
}

void *fifoGetAddress(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return NULL;

    int block = fifo_address_queue[channel].head;
    if (block == FIFO_BUFFER_TERMINATE)
        return NULL;

    int oldIME = enterCriticalSection();
    void *address = (void *)FIFO_BUFFER_DATA(block);
    fifo_address_queue[channel].head = FIFO_BUFFER_GETNEXT(block);
    fifo_freeBlock(block);
    leaveCriticalSection(oldIME);
    return address;
}

u32 fifoGetValue32(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return 0;

    int block = fifo_value32_queue[channel].head;
    if (block == FIFO_BUFFER_TERMINATE)
        return 0;

    int oldIME = enterCriticalSection();
    u32 value32 = FIFO_BUFFER_DATA(block);
    fifo_value32_queue[channel].head = FIFO_BUFFER_GETNEXT(block);
    fifo_freeBlock(block);
    leaveCriticalSection(oldIME);
    return value32;
}

int fifoGetDatamsg(u32 channel, int buffersize, u8 * destbuffer)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return -1;

    int block = fifo_data_queue[channel].head;
    if (block == FIFO_BUFFER_TERMINATE)
        return -1;

    int oldIME = enterCriticalSection();

    int num_bytes = FIFO_BUFFER_GETEXTRA(block);
    int num_words = (num_bytes + 3) >> 2;
    u32 buffer_array[num_words];

    int i,next;
    for (i = 0; i < num_words; i++)
    {
        buffer_array[i] = FIFO_BUFFER_DATA(block);
        next = FIFO_BUFFER_GETNEXT(block);
        fifo_freeBlock(block);
        block = next;
        if (block == FIFO_BUFFER_TERMINATE)
            break;
    }
    fifo_data_queue[channel].head = block;

    if (buffersize < num_bytes)
        num_bytes = buffersize;
    memcpy(destbuffer, buffer_array, num_bytes);

    leaveCriticalSection(oldIME);
    return num_bytes;
}

bool fifoCheckAddress(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    return fifo_address_queue[channel].head != FIFO_BUFFER_TERMINATE;
}

bool fifoCheckDatamsg(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    return fifo_data_queue[channel].head != FIFO_BUFFER_TERMINATE;
}

int fifoCheckDatamsgLength(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return -1;

    if (!fifoCheckDatamsg(channel))
        return -1;

    int block = fifo_data_queue[channel].head;
    return FIFO_BUFFER_GETEXTRA(block);
}

bool fifoCheckValue32(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return false;

    return fifo_value32_queue[channel].head != FIFO_BUFFER_TERMINATE;
}

static void fifo_queueBlock(fifo_queue *queue, int head, int tail)
{
    FIFO_BUFFER_SETNEXT(tail,FIFO_BUFFER_TERMINATE);
    if (queue->head == FIFO_BUFFER_TERMINATE )
    {
        queue->head = head;
        queue->tail = tail;
    }
    else
    {
        FIFO_BUFFER_SETNEXT(queue->tail,head);
        queue->tail = tail;
    }
}

static int processing = 0;

static void fifoInternalRecvInterrupt(void)
{
    u32 data;
    u32 block = FIFO_BUFFER_TERMINATE;

    // Get all available entries from the FIFO and save them in
    // fifo_receive_queue for processing.
    while (!(REG_IPC_FIFO_CR & IPC_FIFO_RECV_EMPTY))
    {
        block = fifo_allocBlock();

        // If there is no more space in fifo_buffer, stop saving blocks and
        // start processing them.
        if (block == FIFO_BUFFER_TERMINATE)
            break;

        FIFO_BUFFER_DATA(block) = REG_IPC_FIFO_RX;
        fifo_queueBlock(&fifo_receive_queue, block, block);
    }

    // This interrupt handler can be nested. This check makes sure that there is
    // only one level of nesting, and that it can only read data from the IPC
    // registers and save it to the FIFO receive queue.
    if (processing)
        return;

    processing = 1;

    while (fifo_receive_queue.head != FIFO_BUFFER_TERMINATE)
    {
        block = fifo_receive_queue.head;
        data = FIFO_BUFFER_DATA(block);

        u32 channel = FIFO_UNPACK_CHANNEL(data);

        if (FIFO_IS_SPECIAL_COMMAND(data))
        {
            uint32_t cmd = data & FIFO_SPECIAL_COMMAND_MASK;
#ifdef ARM9
            // Message sent from the ARM7 to the ARM9 to start a reset
            if (cmd == FIFO_ARM7_REQUESTS_ARM9_RESET)
                exit(0);
#endif

#ifdef ARM7
            // Message sent from the ARM9 to the ARM7 to start a reset
            if (cmd == FIFO_ARM9_REQUESTS_ARM7_RESET)
            {
                // Make sure that the two CPUs reset at the same time. The other
                // CPU reset function (located in the bootstub struct) is
                // responsible for issuing the same commands to ensure that both
                // CPUs are in sync and they reset at the same time.
                REG_IPC_SYNC = 0x100;
                while ((REG_IPC_SYNC & 0x0f) != 1);
                REG_IPC_SYNC = 0;
                swiSoftReset();
            }
#endif
        }
        else if (FIFO_IS_ADDRESS(data))
        {
            volatile void *address = FIFO_UNPACK_ADDRESS(data);

            fifo_receive_queue.head = FIFO_BUFFER_GETNEXT(block);
            if (fifo_address_func[channel])
            {
                fifo_freeBlock(block);
                REG_IME = 1;
                fifo_address_func[channel]((void *)address, fifo_address_data[channel]);
                REG_IME = 0;
            }
            else
            {
                FIFO_BUFFER_DATA(block) = (u32)address;
                fifo_queueBlock(&fifo_address_queue[channel], block, block);
            }
        }
        else if (FIFO_IS_VALUE32(data))
        {
            u32 value32;

            if (FIFO_UNPACK_VALUE32_NEEDEXTRA(data))
            {
                int next = FIFO_BUFFER_GETNEXT(block);

                // If the extra word hasn't been received, try later
                if (next == FIFO_BUFFER_TERMINATE)
                    break;

                fifo_freeBlock(block);
                block = next;
                value32 = FIFO_BUFFER_DATA(block);
            }
            else
            {
                value32 = FIFO_UNPACK_VALUE32_NOEXTRA(data);
            }

            // Increase read pointer
            fifo_receive_queue.head = FIFO_BUFFER_GETNEXT(block);

            if (fifo_value32_func[channel])
            {
                fifo_freeBlock(block);
                REG_IME = 1;
                fifo_value32_func[channel](value32, fifo_value32_data[channel]);
                REG_IME = 0;
            }
            else
            {
                FIFO_BUFFER_DATA(block) = value32;
                fifo_queueBlock(&fifo_value32_queue[channel], block, block);
            }
        }
        else if (FIFO_IS_DATA(data))
        {
            // Calculate the number of expected blocks
            int n_bytes = FIFO_UNPACK_DATALENGTH(data);
            int n_words = (n_bytes + 3) >> 2;

            // Count the number of available blocks
            int count = 0;
            int end = block;
            while (count < n_words && FIFO_BUFFER_GETNEXT(end) != FIFO_BUFFER_TERMINATE)
            {
                end = FIFO_BUFFER_GETNEXT(end);
                count++;
            }

            // If we haven't received enough blocks, try later
            if (count != n_words)
                break;

            // Advance pointer to the end of the blocks that form this message
            // TODO: Fix this! It can cause a use-after-free situation. If we
            // receive enough data messages while handling this one the data
            // will be overwritten.
            fifo_receive_queue.head = FIFO_BUFFER_GETNEXT(end);

            int tmp = FIFO_BUFFER_GETNEXT(block);
            fifo_freeBlock(block);

            FIFO_BUFFER_SETCONTROL(tmp, FIFO_BUFFER_GETNEXT(tmp),
                                   FIFO_BUFFERCONTROL_DATASTART, n_bytes);

            fifo_queueBlock(&fifo_data_queue[channel], tmp, end);
            if (fifo_datamsg_func[channel])
            {
                block = fifo_data_queue[channel].head;
                REG_IME = 1;
                // Call the handler and tell it the number of available bytes to
                // use. They need to be fetched and turned into a proper message
                // by calling fifoGetDatamsg().
                fifo_datamsg_func[channel](n_bytes, fifo_datamsg_data[channel]);
                REG_IME = 0;
                if (block == fifo_data_queue[channel].head)
                    fifoGetDatamsg(channel, 0, 0);
            }
        }
        else
        {
            fifo_receive_queue.head = FIFO_BUFFER_GETNEXT(block);
            fifo_freeBlock(block);
        }
    }

    processing = 0;
}

static void fifoInternalSendInterrupt(void)
{
    if ( fifo_send_queue.head == FIFO_BUFFER_TERMINATE )
    {
        // Disable send irq until there are messages to be sent
        REG_IPC_FIFO_CR &= ~IPC_FIFO_SEND_IRQ;
    }
    else
    {
        u32 head,next;

        head = fifo_send_queue.head;

        while (!(REG_IPC_FIFO_CR & IPC_FIFO_SEND_FULL))
        {
            next = FIFO_BUFFER_GETNEXT(head);
            REG_IPC_FIFO_TX = FIFO_BUFFER_DATA(head);
            fifo_freeBlock(head);
            head = next;

            // Check if there is nothing else to send
            if (head == FIFO_BUFFER_TERMINATE)
                break;
        }

        fifo_send_queue.head = head;
    }
}

bool fifoInit(void)
{
    int i;

    REG_IPC_FIFO_CR = IPC_FIFO_SEND_CLEAR | IPC_FIFO_RECV_EMPTY | IPC_FIFO_SEND_EMPTY;

    for (i = 0; i < FIFO_NUM_CHANNELS; i++)
    {
        fifo_address_queue[i].head = FIFO_BUFFER_TERMINATE;
        fifo_address_queue[i].tail = FIFO_BUFFER_TERMINATE;

        fifo_data_queue[i].head = FIFO_BUFFER_TERMINATE;
        fifo_data_queue[i].tail = FIFO_BUFFER_TERMINATE;

        fifo_value32_queue[i].head = FIFO_BUFFER_TERMINATE;
        fifo_value32_queue[i].tail = FIFO_BUFFER_TERMINATE;

        fifo_address_data[i] = fifo_value32_data[i] = fifo_datamsg_data[i] = 0;
        fifo_address_func[i] = 0;
        fifo_value32_func[i] = 0;
        fifo_datamsg_func[i] = 0;
    }

    for (i = 0; i < FIFO_BUFFER_ENTRIES - 1; i++)
    {
        FIFO_BUFFER_DATA(i) = 0;
        FIFO_BUFFER_SETCONTROL(i, i + 1, 0, 0);
    }

    FIFO_BUFFER_SETCONTROL(FIFO_BUFFER_ENTRIES - 1, FIFO_BUFFER_TERMINATE,
                           FIFO_BUFFERCONTROL_UNUSED, 0);

    irqSet(IRQ_FIFO_EMPTY, fifoInternalSendInterrupt);
    irqSet(IRQ_FIFO_NOT_EMPTY, fifoInternalRecvInterrupt);
    REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_RECV_IRQ;
    irqEnable(IRQ_FIFO_NOT_EMPTY | IRQ_FIFO_EMPTY);

    return true;
}

static comutex_t fifo_mutex[FIFO_NUM_CHANNELS];

void fifoMutexAcquire(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return;

    comutex_acquire(&fifo_mutex[channel]);
}

void fifoMutexRelease(u32 channel)
{
    if (channel >= FIFO_NUM_CHANNELS)
        return;

    comutex_release(&fifo_mutex[channel]);
}
