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
    buffer_t* send_buffer;
    buffer_t* rec_buffer;
    int windowSize;
    int timeout;
    int sndUna;
    int sndNxt;
    int rcvNxt;
    int eof_rcv;
    int eof_in;
    int all_ack;
    int all_write;

};
rel_t *rel_list;

void to_host(packet_t *pkt);
void to_network(packet_t *pkt);
void process_ack(rel_t *r, packet_t *pkt);
void process_data(rel_t *r, packet_t *pkt);
long get_time();
int try_destroy(rel_t *r);



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

    /* Do any other initialization you need here */
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;

    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    r->windowSize = cc->window;
    r->timeout = cc->timeout;
    r->sndUna = 1;
    r->sndNxt = 1;
    r->rcvNxt = 1;

    r->eof_in = 0;
    r->eof_rcv = 0;
    r->all_ack = 1;
    r->all_write = 1;

    return r;
}

void
rel_destroy (rel_t *r)
{
    fprintf(stderr, "destroy connection\n");
    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);

    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...
    free(r);

    /* Free any other allocated memory here */
}


void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{

    to_host(pkt);
    uint16_t cksum1 = pkt->cksum;
    pkt->cksum = 0;
    if(cksum1 != cksum((void *) pkt, pkt->len)) {
      fprintf(stderr, "corrupt packet received\n");
      return;
    }



    if(pkt->len < 12){
      process_ack(r, pkt);
    } else {
      process_data(r, pkt);
    }
}


void
rel_read (rel_t *s)
{
    char *buf = xmalloc(512);
    while(s->sndNxt - s->sndUna < s->windowSize){//Space in send_buffer?

      int num_bytes = conn_input(s->c, buf, 512);

      if(num_bytes < 0){
        s->eof_in = 1;
        conn_sendpkt(s->c, NULL, 0);
        return;
      } else{
        s->eof_in = 0;
      }

      if(num_bytes == 0){
        return;
      }

      packet_t *pkt = xmalloc(sizeof(packet_t));//Make packet out of data
      pkt->len = 12 + num_bytes;
      pkt->seqno = s->sndNxt;
      fprintf(stderr, "sent pkt %d\n", pkt->seqno);
      pkt->ackno = 0;
      memcpy(pkt->data, buf, num_bytes);
      //printf("Sent %d\n", pkt->seqno);
      pkt->cksum = 0;
      pkt->cksum = cksum((void *) pkt, num_bytes + 12);
      to_network(pkt);

      long now = get_time();
      buffer_insert(s->send_buffer, pkt, now);
      conn_sendpkt(s->c, pkt, num_bytes + 12);

      s->sndNxt++;
    }
    free(buf);
}

void
rel_output (rel_t *r)
{
    if(conn_bufspace(r->c) <= 0){
      return;
    }
    buffer_node_t *head = buffer_get_first(r->rec_buffer);
    buffer_node_t *current = head;

    while(current != NULL){
        packet_t pkt = current->packet;
        if(pkt.seqno == r->rcvNxt){
          buffer_node_t *head = buffer_get_first(r->rec_buffer);
          buffer_node_t *curr = head;
          int num = r->rcvNxt;
          while(curr != NULL){//loop through buffer to find higest consecutive ack
            if(buffer_contains(r->rec_buffer, htonl(num))){
              num++;

            }
            curr = curr->next;
          }
          conn_output(r->c, &pkt.data, pkt.len - 12);
          r->rcvNxt = num;//get highest consequtive seqno in rec_buffer
          struct ack_packet *ack = xmalloc(sizeof(struct ack_packet));
          ack->len = 8;
          ack->ackno = r->rcvNxt;
          ack->cksum = cksum((void *) ack, 8);
          to_network((packet_t *) ack);
          conn_sendpkt(r->c, (packet_t *) ack, 8);//Send Ack Packet
          buffer_remove_first(r->rec_buffer);
          if(buffer_size(r->rec_buffer) == 0){
            r->all_write = 1;
          } else{
            r->all_write = 0;
          }
          free(ack);
      }
      current = buffer_get_first(r->rec_buffer);
    }
}

void
rel_timer ()
{
  rel_t *current = rel_list;
  while (current != NULL) {
    buffer_node_t *head = buffer_get_first(current->send_buffer);
    buffer_node_t *curr = head;
    while(curr != NULL){//loop through buffer to find higest consecutive seqno
      long now = get_time();

      if(curr->last_retransmit - now > current->timeout){
        conn_sendpkt(current->c, &curr->packet, 12);
        curr->last_retransmit = now;
      }
      curr = curr->next;


    }
    if(try_destroy(current) == 1){
      rel_destroy(current);
    }else{
      current = rel_list->next;
    }
  }

}

/*Convert to host byte order*/
void
to_host(packet_t *pkt){
    pkt->len = ntohs(pkt->len);
    pkt->ackno = ntohl(pkt->ackno);
    if(pkt->len >= 12){
      pkt->seqno = ntohl(pkt->seqno);
    }
}

/*Convert to network byte order*/
void
to_network(packet_t *pkt){
  pkt->len = htons(pkt->len);
  pkt->ackno = htonl(pkt->ackno);
  if(pkt->len >= 12){
    pkt->seqno = htonl(pkt->seqno);
  }
}

/*Process Ack packet*/
/*Updates lowest seqno of outstanding frames and deletes packets out of send_buffer until received ack*/
void
process_ack(rel_t *r, packet_t *pkt){
    fprintf(stderr, "received ack %d\n", pkt->ackno);
    if(r->sndUna < pkt->ackno){
      r->sndUna = pkt->ackno;
    }
    buffer_remove(r->send_buffer, pkt->ackno);
    if(buffer_size(r->send_buffer) == 0){
      r->all_ack = 1;
    } else{
      r->all_ack = 0;
      rel_read(r);
    }
}

/*Process data packet*/
void
process_data(rel_t *r, packet_t *pkt){
    //printf("rcv %d\n", pkt->seqno);
    fprintf(stderr, "received pkt %d\n", pkt->seqno);
    if(pkt->seqno < r->rcvNxt + r->windowSize){
      if(pkt->len == 12){
        r->eof_rcv = 1;
        conn_output(r->c, NULL, 0);
      } else{
        r->eof_rcv = 0;
      }

      if(buffer_contains(r->rec_buffer, pkt->seqno) == 0){
        buffer_insert(r->rec_buffer, pkt, 0);

        rel_output(r);
      }
    }
}

/*Get Timestamp in milliseconds*/
long
get_time(){
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

int
try_destroy(rel_t *r){
  if(r->eof_in == 1 && r->eof_rcv == 1 && r->all_ack == 1 && r->all_write == 1){
    return 1;
  }
  return 0;
}
