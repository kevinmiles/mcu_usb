#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "mcu_usb.h"
#include "usb_endpoint.h"
#include "sync.h"
#include <lpc_tools/irq.h>

#include "usb_core.h"
#include "usb_queue.h"

usb_queue_t* endpoint_queues[NUM_USB_CONTROLLERS][12] = {};

#define USB_ENDPOINT_INDEX(endpoint_address) (((endpoint_address & 0xF) * 2) + ((endpoint_address >> 7) & 1))

static usb_queue_t* endpoint_queue(
        const USBEndpoint* const endpoint
) {
        uint32_t index = USB_ENDPOINT_INDEX(endpoint->address);
        if (endpoint_queues[endpoint->device->controller][index] == NULL) while (1);
        return endpoint_queues[endpoint->device->controller][index];
}

void usb_queue_init(
        usb_queue_t* const queue
) {
        uint32_t index = USB_ENDPOINT_INDEX(queue->endpoint->address);
        if (endpoint_queues[queue->endpoint->device->controller][index] != NULL) while (1);
        endpoint_queues[queue->endpoint->device->controller][index] = queue;

        usb_transfer_t* t = queue->free_transfers;
        for (unsigned int i=0; i < queue->pool_size - 1; i++, t++) {
                t->next = t+1;
                t->queue = queue;
        }
        t->next = NULL;
        t->queue = queue;
}



/* Allocate a transfer */
static usb_transfer_t* allocate_transfer(
        usb_queue_t* const queue
) {
        usb_transfer_t* transfer;
        if (queue->free_transfers == NULL)
        return NULL;
#ifdef CORE_M4
        bool aborted;
        do {
                transfer = (void *) __ldrex((uint32_t *) &queue->free_transfers);
                aborted = __strex((uint32_t) transfer->next, (uint32_t *) &queue->free_transfers);
        } while (aborted);
#else
        bool sts = irq_disable();
        transfer = queue->free_transfers;
        queue->free_transfers = transfer->next;
        irq_restore(sts);
#endif
        transfer->next = NULL;
        return transfer;
}

/* Place a transfer in the free list */
static void free_transfer(usb_transfer_t* const transfer)
{
        usb_queue_t* const queue = transfer->queue;
#ifdef CORE_M4        
        bool aborted;
        do {
                transfer->next = (void *) __ldrex((uint32_t *) &queue->free_transfers);
                aborted = __strex((uint32_t) transfer, (uint32_t *) &queue->free_transfers);
        } while (aborted);
#else
        bool sts = irq_disable();
        transfer->next = queue->free_transfers;
        queue->free_transfers = transfer;
        irq_restore(sts);
#endif
}

/* Add a transfer to the end of an endpoint's queue. Returns the old
 * tail or NULL is the queue was empty
 */
static usb_transfer_t* endpoint_queue_transfer(
        usb_transfer_t* const transfer
) {
        usb_queue_t* const queue = transfer->queue;
        transfer->next = NULL;
        if (queue->active != NULL) {
            usb_transfer_t* t = queue->active;
            while (t->next != NULL) t = t->next;
            t->next = transfer;
            return t;
        } else {
            queue->active = transfer;
            return NULL;
        }
}
                
static void usb_queue_flush_queue(usb_queue_t* const queue)
{
        irq_disable();

        while (queue->active) {
                usb_transfer_t* transfer = queue->active;
                queue->active = transfer->next;

                if (transfer->completion_cb) {
                        transfer->completion_cb(transfer->user_data, -1);
                }

                free_transfer(transfer);
        }
        irq_enable();
}

void usb_queue_flush_endpoint(const USBEndpoint* const endpoint)
{
        usb_queue_flush_queue(endpoint_queue(endpoint));
}

int usb_transfer_schedule(
	const USBEndpoint* const endpoint,
	void* const data,
	const uint32_t maximum_length,
        const transfer_completion_cb completion_cb,
        void* const user_data
) {
        usb_queue_t* const queue = endpoint_queue(endpoint);
        usb_transfer_t* const transfer = allocate_transfer(queue);
        if (transfer == NULL) return -1;
        USBTransferDescriptor* const td = &transfer->td;

	// Configure the transfer descriptor
        td->next_dtd_pointer = USB_TD_NEXT_DTD_POINTER_TERMINATE;
        // td->capabilities.word = 0;
        // td->capabilities.total_bytes = maximum_length; //USB_TD_DTD_TOKEN_TOTAL_BYTES(maximum_length);
        // td->capabilities.int_on_complete = 1; //USB_TD_DTD_TOKEN_IOC;
        // td->capabilities.multiplier_override = 0; //USB_TD_DTD_TOKEN_MULTO(0);
        // td->capabilities.active = 1; //USB_TD_DTD_TOKEN_STATUS_ACTIVE;

	td->capabilities.word =
		  USB_TD_DTD_TOKEN_TOTAL_BYTES(maximum_length)
		| USB_TD_DTD_TOKEN_IOC
		| USB_TD_DTD_TOKEN_MULTO(0)
		| USB_TD_DTD_TOKEN_STATUS_ACTIVE
        	;
        
	td->buffer_pointer_page[0] =  (uint32_t)data;
	td->buffer_pointer_page[1] = ((uint32_t)data + 0x1000) & 0xfffff000;
	td->buffer_pointer_page[2] = ((uint32_t)data + 0x2000) & 0xfffff000;
	td->buffer_pointer_page[3] = ((uint32_t)data + 0x3000) & 0xfffff000;
	td->buffer_pointer_page[4] = ((uint32_t)data + 0x4000) & 0xfffff000;

        // Fill in transfer fields
        transfer->maximum_length = maximum_length;
        transfer->completion_cb = completion_cb;
        transfer->user_data = user_data;

        irq_disable();
        usb_transfer_t* tail = endpoint_queue_transfer(transfer);
        if (tail == NULL) {
                // The queue is currently empty, we need to re-prime
                usb_endpoint_schedule_wait(queue->endpoint, &transfer->td);
        } else {
                // The queue is currently running, try to append
                usb_endpoint_schedule_append(queue->endpoint, &tail->td, &transfer->td);
        }
        irq_enable();
        return 0;
}
	
int usb_transfer_schedule_block(
	const USBEndpoint* const endpoint,
	void* const data,
	const uint32_t maximum_length,
        const transfer_completion_cb completion_cb,
        void* const user_data
) {
        int ret;
        do {
                ret = usb_transfer_schedule(endpoint, data, maximum_length,
                                            completion_cb, user_data);
        } while (ret == -1);
        return 0;
}

int usb_transfer_schedule_ack(
	const USBEndpoint* const endpoint
) {
        return usb_transfer_schedule_block(endpoint, 0, 0, NULL, NULL);
}

/* Called when an endpoint might have completed a transfer */
void usb_queue_transfer_complete(USBEndpoint* const endpoint)
{
        usb_queue_t* const queue = endpoint_queue(endpoint);
        if (queue == NULL) while(1); // Uh oh
        usb_transfer_t* transfer = queue->active;

        while (transfer != NULL) {
                uint8_t status = transfer->td.capabilities.word;

                // Check for failures
                if (   status & USB_TD_DTD_TOKEN_STATUS_HALTED
                    || status & USB_TD_DTD_TOKEN_STATUS_BUFFER_ERROR
                    || status & USB_TD_DTD_TOKEN_STATUS_TRANSACTION_ERROR) {
                        // TODO: Uh oh, do something useful here
                        while (1);
                }

                // Still not finished
                if (status & USB_TD_DTD_TOKEN_STATUS_ACTIVE) 
                        break;

                // Advance the head. We need to do this before invoking the completion
                // callback as it might attempt to schedule a new transfer
                queue->active = transfer->next;
                usb_transfer_t* next = transfer->next;

                // Invoke completion callback
                unsigned int total_bytes = transfer->td.capabilities.total_bytes;
                unsigned int transferred = transfer->maximum_length - total_bytes;
                if (transfer->completion_cb) {
                        transfer->completion_cb(transfer->user_data, transferred);
                }

                // Advance head and free transfer
                free_transfer(transfer);
                transfer = next;
        }
}

bool usb_queue_active(USBEndpoint *const endpoint)
{
        usb_queue_t* const queue = endpoint_queue(endpoint);
        if (!queue) {
                return 0;
        }
        usb_transfer_t* transfer = queue->active;
        return transfer != NULL;
}

uint32_t queue_free_space(USBEndpoint *const endpoint)
{
        usb_queue_t* const queue = endpoint_queue(endpoint);
        if (!queue) {
                return 0;
        }
        usb_transfer_t* transfer = queue->free_transfers;
        uint32_t count = 0;
        while(transfer) {
                count++;
                transfer = transfer->next;
        }
        return count;
}

int usb_queue_transferred_bytes(USBEndpoint* const endpoint)
{
        usb_queue_t* const queue = endpoint_queue(endpoint);
        if (queue == NULL) {
                return -1;
        }
        usb_transfer_t* transfer = queue->active;
        if (transfer == NULL) {
                //bool d = false;
                //while(!d);
                return -2;
        }
        unsigned int total_bytes = transfer->td.capabilities.total_bytes;
        int transferred = transfer->maximum_length - total_bytes;
        return transferred;
}
