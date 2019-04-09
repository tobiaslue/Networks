#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */


    /* Add your own data fields below this */
    // ...
    buffer_t* send_buffer;
    buffer_t* rec_buffer;
    int windowSize;

    int sndUna;
    int sndNxt;
    int rcvNxt;
};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    /* Do any other initialization you need here... */
    // ...
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // ...
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    r->windowSize = cc->window;

    r->sndUna = 1;
    r->sndNxt = 1;
    r->rcvNxt = 1;
    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...

    free(r);

}

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    if(n == 8){//Ack packet
      if(pkt->ackno == r->sndUna + 1){
        //printf("ack %d\n", pkt->ackno);
        r->sndUna++;
      }
    } else if (n == 12){//EOF
      rel_destroy(r);
      return;
    } else {//Data packet
      if(pkt->seqno < r->rcvNxt + r->windowSize){
        if(pkt->cksum == cksum(pkt->data, pkt->len -12)){//See if correct checksum
          buffer_insert(r->rec_buffer, pkt, 0);
          if(pkt->seqno == r->rcvNxt){//Send cumulative ack
            buffer_node_t *head = buffer_get_first(r->rec_buffer);
            buffer_node_t *curr = head;
            int num = r->rcvNxt;

            while(curr != NULL){//loop through buffer to find higest consecutive ack
              if(buffer_contains(r->rec_buffer, num)){
                num++;
              }
              curr = curr->next;
            }

            r->rcvNxt = num + 1;//get highest consequtive seqno in rec_buffer
            struct ack_packet *ack = xmalloc(sizeof(struct ack_packet));
            ack->cksum = 1;
            ack->len = 8;
            ack->ackno = r->rcvNxt;
            conn_sendpkt(r->c, ack, 8);//Send Ack Packet

          }


          rel_output(r);
        }

      }
    }
}

void
rel_read (rel_t *s)
{
    char *buf = xmalloc(12);
    int len = 0;
    packet_t *pck = xmalloc(sizeof(packet_t));

    if(s->sndNxt - s->sndUna < s->windowSize){//Still space in send Buffer?
      len = conn_input(s->c, buf, 500);
      if(len == -1){
        rel_destroy(s);
        return;
      }
      pck->cksum = cksum(buf, len);
      pck->len = 12 + len;
      pck->seqno = s->sndNxt;
      pck->ackno = 0;
      strcpy(pck->data, buf);//Make packet out of data
      struct timeval now;
      gettimeofday(&now, NULL);
      long nowMs = now.tv_sec * 1000 + now.tv_usec / 1000;
      //printf("nowMs %ld\n", nowMs);
      buffer_insert(s->send_buffer, pck, 0);//SendData
      s->sndNxt++;//Window shits one to left
      conn_sendpkt(s->c, pck, pck->len);
    }

    free(pck);


}

void
rel_output (rel_t *r)
{
    buffer_node_t *head = buffer_get_first(r->rec_buffer);
    buffer_node_t *curr = head;

    while(curr != NULL){//loop through buffer to find higest consecutive seqno
      if(curr->packet.seqno <= r->rcvNxt){
        char *data = xmalloc(12);
        strcpy(data, curr->packet.data);
        conn_output(r->c, curr->packet.data, curr->packet.len - 12);
        buffer_remove_first(r->rec_buffer);
      }
      curr = curr->next;
    }


}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL) {
      buffer_node_t *head = buffer_get_first(current->rec_buffer);
      buffer_node_t *curr = head;
      while(curr != NULL){//loop through buffer to find higest consecutive seqno
        struct timeval now;
        gettimeofday(&now, NULL);
        long nowMs = now.tv_sec * 1000 + now.tv_usec / 1000;

        if(curr->last_retransmit - nowMs > 200){
          conn_sendpkt(current->c, &curr->packet, 12);
          curr->last_retransmit = 0;
        }
        curr = curr->next;


      }
      current = rel_list->next;
    }
}
