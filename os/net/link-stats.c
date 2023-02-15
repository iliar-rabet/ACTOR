/*
 * Copyright (c) 2015, SICS Swedish ICT.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *
 * Authors: Simon Duquennoy <simonduq@sics.se>
 *          Atis Elsts <atis.elsts@edi.lv>
 */

#include "contiki.h"
#include "sys/clock.h"
#include "net/packetbuf.h"
#include "net/nbr-table.h"
#include "net/link-stats.h"
#include <stdio.h>
#include "arch/dev/radio/cc2420/cc2420.h"
#include "net/routing/rpl-lite/rpl-types.h"
#include "net/routing/rpl-lite/rpl-neighbor.h"
#include "lib/random.h"
#include "net/ipv6/simple-udp.h"
#include "dev/leds.h"

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "Link Stats"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Maximum value for the Tx count counter */
#define TX_COUNT_MAX                    32

/* Statistics with no update in FRESHNESS_EXPIRATION_TIMEOUT is not fresh */
#define FRESHNESS_EXPIRATION_TIME       (10 * 60 * (clock_time_t)CLOCK_SECOND)
/* Half time for the freshness counter */
#define FRESHNESS_HALF_LIFE             (15 * 60 * (clock_time_t)CLOCK_SECOND)
/* Statistics are fresh if the freshness counter is FRESHNESS_TARGET or more */
#define FRESHNESS_TARGET                 4
/* Maximum value for the freshness counter */
#define FRESHNESS_MAX                   16

/* EWMA (exponential moving average) used to maintain statistics over time */
#define EWMA_SCALE                     100
#define EWMA_ALPHA                      10
#define EWMA_BOOTSTRAP_ALPHA            25

/* ETX fixed point divisor. 128 is the value used by RPL (RFC 6551 and RFC 6719) */
#define ETX_DIVISOR                     LINK_STATS_ETX_DIVISOR
/* In case of no-ACK, add ETX_NOACK_PENALTY to the real Tx count, as a penalty */
#define ETX_NOACK_PENALTY               12
/* Initial ETX value */
#define ETX_DEFAULT                      2

/* Per-neighbor link statistics table */
NBR_TABLE(struct link_stats, link_stats);

/* Called at a period of FRESHNESS_HALF_LIFE */
struct ctimer periodic_timer;

#define MAX_UCB 105
#define LEARNING_FACTOR 5

int tx_p=7;
#ifdef PCRPL
static int multiplication;
static int losts_in_window;
static int pc_counter=0;
#endif
#if defined MABRPL
NBR_TABLE(struct rl_list, rl_table);
#endif
#if defined MABRPL
struct rl_list *
rl_from_lladdr(const linkaddr_t *lladdr)
{
  return nbr_table_get_from_lladdr(rl_table, lladdr);
}
#endif

#if defined MABRPL 
int floorSqrt(int x)
{
    // Base cases
    if (x == 0 || x == 1)
    return x;
 
    // Starting from 1, try all numbers until
    // i*i is greater than or equal to x.
    int i = 1, result = 1;
    while (result <= x)
    {
      i++;
      result = i * i;
    }
    return i - 1;
}

unsigned int Log2n(unsigned int n)
{
    return (n > 1) ? 1 + Log2n(n / 2) : 0;
}
 
int UCB_ind(int q,int t,int k){
  if(t==0 || k==0)
    return MAX_UCB;
  int l = Log2n(t);
  int f=floorSqrt(4*l/k);
  // printf("log(t):%d, floor(l/k)%d , k:%d\n",l,f,k);
  return q+f;
}
#endif

#if defined(MABRPL) && !defined sliding_UCB
int UCB(struct rl_list * rl_nbr){

  int max=0;
  int maxUCB=UCB_ind(rl_nbr->Q[max],rl_nbr->t, rl_nbr->k[max]);
  int i;
  int curr;
  for(i=0;i<8;i++){
    curr=UCB_ind(rl_nbr->Q[i],rl_nbr->t, rl_nbr->k[i]);
    // printf("i:%d, Q[i]:%d, t:%d, k:%d UCB:%d\n",i,rl_nbr->Q[i], rl_nbr->t, rl_nbr->k[i], curr);
    // if(rl_nbr->flag[i]==1)
    //   continue;
    if( curr > maxUCB ){
      max=i;
      maxUCB=curr;
    }

  }
  return max;
}
#endif 

#ifdef sliding_UCB
uint8_t sliding(struct rl_list * rl_nbr,const linkaddr_t * lladdr){
  uint8_t max=0;
  uint8_t maxUCB=UCB_ind(rl_nbr->Q[max],rl_nbr->t, rl_nbr->k[max]);
  uint8_t i,j,sum;
  uint8_t curr;
  for(i=0;i<TX_LEVELS;i++){
    sum=0;
    for(j=0;j<WIN_SIZE;j++){
      printf("%u ",sliding_window[i][j]);
      if(j>rl_nbr->k[i])
        continue;
      sum+=sliding_window[i][j];
    }
    printf("\n");
    uint8_t avg;
    if(rl_nbr->k[i]<WIN_SIZE)
       avg=sum/rl_nbr->k[i];
    else
      avg=sum/WIN_SIZE;
    curr=UCB_ind(sum/rl_nbr->k[i],rl_nbr->t, rl_nbr->k[i]);
    printf("i:%d, avg:%d, t:%d, k:%d UCB:%d\n",i,avg, rl_nbr->t, rl_nbr->k[i], curr);
    if(rl_nbr->flag[i]==1)
      continue;
    if( curr > maxUCB ){
      max=i;
      maxUCB=curr;
    }
  }
  return max;
}
#endif


#if defined MABRPL || defined PCRPL
// int trans(int x){
//   if (x < -80)
//     return 7;
//   if (x < -70)
//     return 5;
//   if (x < -50)
//     return 4;
//   if (x < -40)
//     return 3;    
//   if (x < -30)
//     return 2;
//   if (x < -20)
//     return 1;
//   if (x < -10)
//     return 0;
//   if (x < -5)
//     return 0;
//   return 7;
// }

int trans(int x){
  if (x < -80)
    return 5;
  if (x < -70)
    return 4;
  if (x < -50)
    return 3;
  if (x < -40)
    return 2;    
  if (x < -30)
    return 1;
  if (x < -20)
    return 0;
  if (x < -10)
    return 0;
  if (x < -5)
    return 0;
  return 5;
}
void MAB_init(struct  rl_list * rl_nbr,int rss){
  printf("trans:rss%d\n",rss);
  int tran;
  tran=trans(rss);
  int i;
  for(i=0;i<tran;i++){
    rl_nbr->k[i]=100;
  }

  for(i=tran;i<TX_LEVELS;i++){
    rl_nbr->Q[i]=100;
  }

  if(tran<=4){
    for(i=tran+2;i<TX_LEVELS;i++){
      rl_nbr->Q[i]=50;
      rl_nbr->k[i]=5;
    }
  }
}

void print_rss(){
    LOG_INFO_LLADDR(rpl_neighbor_get_lladdr(rpl_neighbor_select_best()));
    // printf(" best_parent_stats->rssi %d -> %d\n", best_parent_stats->rssi, trans(best_parent_stats->rssi));
}
#endif
#ifdef MABRPL
void print_rl(struct rl_list *rl_nbr){
  uint8_t i;
  // for(rl_nbr = nbr_table_head(rl_table); rl_nbr != NULL; rl_nbr = nbr_table_next(rl_table, rl_nbr)) {
      LOG_INFO_LLADDR(nbr_table_get_lladdr(rl_table, rl_nbr));
      LOG_INFO("\n");
      for(i=0;i<8;i++){
          LOG_INFO("k:%u Q:%u flag:%u, UCB:%d\n",rl_nbr->k[i],rl_nbr->Q[i],rl_nbr->flag[i],UCB_ind(rl_nbr->Q[i],rl_nbr->t, rl_nbr->k[i]));
      }
  // }
}
#endif
#if (defined MABRPL) || (defined PCRPL)

#ifndef CONTIKI_TARGET_SKY
 radio_value_t translate_to_NRF(uint8_t t){
   switch (t)
   {
   case 7:
     return RADIO_TXPOWER_TXPOWER_0dBm;
   case 6:
    return RADIO_TXPOWER_TXPOWER_Neg4dBm;
    case 5:
    return RADIO_TXPOWER_TXPOWER_Neg8dBm;
    case 4:
    return RADIO_TXPOWER_TXPOWER_Neg12dBm;
    case 3:
    return RADIO_TXPOWER_TXPOWER_Neg16dBm;
    case 2:
    return RADIO_TXPOWER_TXPOWER_Neg20dBm;
    case 1:
    return RADIO_TXPOWER_TXPOWER_Neg30dBm;
    case 0:
    return RADIO_TXPOWER_TXPOWER_Neg40dBm;
   default:
     return RADIO_TXPOWER_TXPOWER_0dBm;
   }
return 0;
 }
 #endif

void max_txpower(){
  set_txpower(7);
}

void set_txpower(int new_tx){
  tx_p=new_tx;
  #ifdef CONTIKI_TARGET_SKY
    uint8_t tx_ls[8]={3, 7, 11, 15, 19, 23, 27, 31};
    cc2420_set_txpower(tx_ls[new_tx]);
  #else
    NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, translate_to_NRF(new_tx));
    leds_on(new_tx);
  #endif
}

void reset_txpower(){
  printf("after DIO setting %d\n",tx_p);
  set_txpower(tx_p);
}
#endif
/*---------------------------------------------------------------------------*/

#if defined(MABRPL) || defined(PCRPL)
static struct simple_udp_connection best_parent_conn;
void best_txpower(struct rl_list *rl_nbr){
  int new_tx;
  int uc;
  // rl_nbr = nbr_table_get_from_lladdr(rl_table, lladdr);
  rpl_nbr_t *best = NULL;
  best=rpl_neighbor_select_best();
  if(best == NULL){
    printf("BEST IS NULL again\n");
    return;
  }
  
  uc=UCB(rl_nbr);

  new_tx=(uc > req) ? uc:req;
  printf("req:%d , uc%d\n",req,uc);

  set_txpower(new_tx);  
  rl_nbr->txpower= new_tx;  
  printf("SET TO %d\n",new_tx);
  printf("in best tx %d , k=%d\n",new_tx,rl_nbr->k[rl_nbr->txpower]);
  if(rl_nbr->txpower != new_tx && rl_nbr->k[rl_nbr->txpower] > 2 && best != NULL ){
    printf("ASKING %d\n",new_tx);
    LOG_INFO_6ADDR(rpl_neighbor_get_ipaddr(best));
    LOG_INFO("\n");
    char buf[50];
    simple_udp_register(&best_parent_conn, 50, NULL, 50, NULL);
    sprintf(buf, "%d", new_tx);
    simple_udp_sendto(&best_parent_conn, buf, strlen(buf), rpl_neighbor_get_ipaddr(best));
  }
}

#endif
/*---------------------------------------------------------------------------*/
/* Returns the neighbor's link stats */
const struct link_stats *
link_stats_from_lladdr(const linkaddr_t *lladdr)
{
  return nbr_table_get_from_lladdr(link_stats, lladdr);
}
/*---------------------------------------------------------------------------*/
/* Returns the neighbor's address given a link stats item */
const linkaddr_t *
link_stats_get_lladdr(const struct link_stats *stat)
{
  return nbr_table_get_lladdr(link_stats, stat);
}
/*---------------------------------------------------------------------------*/
/* Are the statistics fresh? */
int
link_stats_is_fresh(const struct link_stats *stats)
{
  return (stats != NULL)
      && clock_time() - stats->last_tx_time < FRESHNESS_EXPIRATION_TIME
      && stats->freshness >= FRESHNESS_TARGET;
}
/*---------------------------------------------------------------------------*/
#if LINK_STATS_INIT_ETX_FROM_RSSI
uint16_t
guess_etx_from_rssi(const struct link_stats *stats)
{
  printf("guess %d\n",stats->rssi);
  if(stats != NULL) {
    if(stats->rssi == LINK_STATS_RSSI_UNKNOWN) {
      return ETX_DEFAULT * ETX_DIVISOR;
    } else {
      /* A rough estimate of PRR from RSSI, as a linear function where:
       *      RSSI >= -60 results in PRR of 1
       *      RSSI <= -90 results in PRR of 0
       * prr = (bounded_rssi - RSSI_LOW) / (RSSI_DIFF)
       * etx = ETX_DIVOSOR / ((bounded_rssi - RSSI_LOW) / RSSI_DIFF)
       * etx = (RSSI_DIFF * ETX_DIVOSOR) / (bounded_rssi - RSSI_LOW)
       * */
#define ETX_INIT_MAX 3
#define RSSI_HIGH -50
#define RSSI_LOW  -75
#define RSSI_DIFF (RSSI_HIGH - RSSI_LOW)
      uint16_t etx;
      int16_t bounded_rssi = stats->rssi;
      bounded_rssi = MIN(bounded_rssi, RSSI_HIGH);
      bounded_rssi = MAX(bounded_rssi, RSSI_LOW + 1);
      etx = RSSI_DIFF * ETX_DIVISOR / (bounded_rssi - RSSI_LOW);
      return MIN(etx, ETX_INIT_MAX * ETX_DIVISOR);
    }
  }
  return 0xffff;
}
#endif /* LINK_STATS_INIT_ETX_FROM_RSSI */
/*---------------------------------------------------------------------------*/
/* Packet sent callback. Updates stats for transmissions to lladdr */
void
link_stats_packet_sent(const linkaddr_t *lladdr, int status, int numtx)
{
  struct link_stats *stats;
#if !LINK_STATS_ETX_FROM_PACKET_COUNT
  uint16_t packet_etx;

  uint8_t ewma_alpha;

#endif /* !LINK_STATS_ETX_FROM_PACKET_COUNT */

  if(status != MAC_TX_OK && status != MAC_TX_NOACK && status != MAC_TX_QUEUE_FULL) {
    /* Do not penalize the ETX when collisions or transmission errors occur. */
    return;
  }

  stats = nbr_table_get_from_lladdr(link_stats, lladdr);
  if(stats == NULL) {
    /* If transmission failed, do not add the neighbor, as the neighbor might not exist anymore */
    if(status != MAC_TX_OK) {
      return;
    }

    /* Add the neighbor */
    stats = nbr_table_add_lladdr(link_stats, lladdr, NBR_TABLE_REASON_LINK_STATS, NULL);
    if(stats == NULL) {
      return; /* No space left, return */
    }
    stats->rssi = LINK_STATS_RSSI_UNKNOWN;
  }

  if(status == MAC_TX_QUEUE_FULL) {
#if LINK_STATS_PACKET_COUNTERS
    stats->cnt_current.num_queue_drops += 1;
#endif
    /* Do not penalize the ETX when the packet is dropped due to a full queue */
    return;
  }

  /* Update last timestamp and freshness */
  stats->last_tx_time = clock_time();
  stats->freshness = MIN(stats->freshness + numtx, FRESHNESS_MAX);

#if LINK_STATS_PACKET_COUNTERS
  /* Update paket counters */
  stats->cnt_current.num_packets_tx += numtx;
  if(status == MAC_TX_OK) {
    stats->cnt_current.num_packets_acked++;
  }
#endif

  /* Add penalty in case of no-ACK */
  if(status == MAC_TX_NOACK) {
    numtx += ETX_NOACK_PENALTY;
  }

#if LINK_STATS_ETX_FROM_PACKET_COUNT
  /* Compute ETX from packet and ACK count */
  /* Halve both counter after TX_COUNT_MAX */
  if(stats->tx_count + numtx > TX_COUNT_MAX) {
    stats->tx_count /= 2;
    stats->ack_count /= 2;
  }
  /* Update tx_count and ack_count */
  stats->tx_count += numtx;
  if(status == MAC_TX_OK) {
    stats->ack_count++;
  }
  /* Compute ETX */
  if(stats->ack_count > 0) {
    stats->etx = ((uint16_t)stats->tx_count * ETX_DIVISOR) / stats->ack_count;
  } else {
    stats->etx = (uint16_t)MAX(ETX_NOACK_PENALTY, stats->tx_count) * ETX_DIVISOR;
  }
#else /* LINK_STATS_ETX_FROM_PACKET_COUNT */
  /* Compute ETX using an EWMA */
  printf("numtx %d\n",numtx);
  /* ETX used for this update */
  packet_etx = numtx * ETX_DIVISOR;
  /* ETX alpha used for this update */

  ewma_alpha = link_stats_is_fresh(stats) ? EWMA_ALPHA : EWMA_BOOTSTRAP_ALPHA;

  if(stats->etx == 0) {
    /* Initialize ETX */
    stats->etx = packet_etx;
  } else {
    /* Compute EWMA and update ETX */
    #if !defined(MABRPL) && !defined(PCRPL)
    printf("not && not\n");
    stats->etx = ((uint32_t)stats->etx * (EWMA_SCALE - ewma_alpha) +
                  (uint32_t)packet_etx * ewma_alpha) / EWMA_SCALE;
    #endif
  }
  LOG_INFO("ETX:%d\n",stats->etx);
#endif /* LINK_STATS_ETX_FROM_PACKET_COUNT */

  // stats->txP=stats->etx*tx_ls[rl_nbr->txpower];
  /* Iliar's PC-RPL*/
#ifdef PCRPL

  if(rpl_dag_root_is_root()){     printf("ROOT\n"); return; }
  
  int new_tx=tx_p;
  
  if(pc_counter == 0){
    struct link_stats *best_parent_stats;  

    // print_rss();

    best_parent_stats = nbr_table_get_from_lladdr(link_stats, rpl_neighbor_get_lladdr(rpl_neighbor_select_best()));
    
    if(best_parent_stats == NULL)
      return;

    new_tx=trans(best_parent_stats->rssi);
    printf("INIT\n");
    LOG_INFO_LLADDR(rpl_neighbor_get_lladdr(rpl_neighbor_select_best()));
    printf(" best_parent_stats->rssi %d -> %d\n", best_parent_stats->rssi, trans(best_parent_stats->rssi));

  }
    
  pc_counter+=1;
  // printf("status:%d pc_counter:%d LIMIT:%d new_tx %d\n",status,pc_counter,LEARNING_FACTOR*multiplication, new_tx);
  if(status == MAC_TX_NOACK) {
    printf("LOST PACKET\n");
    printf("new_tx:%d old_tx:%d\n",new_tx,tx_p);
    pc_counter=1;
    losts_in_window=0;
    new_tx=tx_p+2;
    multiplication=multiplication*2;
    if(new_tx>7)
      new_tx=7;
  }
  else if(pc_counter==LEARNING_FACTOR*multiplication){
      if(losts_in_window==0){
        new_tx=tx_p-1;
        if(new_tx<req)
          new_tx=req;
        if(new_tx<1)
          new_tx=1;
      }
      printf("new_tx:%d old_tx:%d\n",new_tx,tx_p);
      pc_counter=1;
      losts_in_window=0;
  }
  else{
      printf("ewma_alpha:%d\n",ewma_alpha);
  }

  printf("SET TO %d\n",new_tx);
  tx_p=new_tx;
  set_txpower(tx_p); 
  // print_rss();
  #endif


/* Iliar's Multi-Armed Bandit*/
  #ifdef MABRPL
  struct rl_list *rl_nbr;
  struct link_stats *best_parent_stats;  
  
  // LOG_INFO("best:");
  // LOG_INFO_LLADDR(rpl_neighbor_get_lladdr(rpl_neighbor_select_best()));
  // LOG_INFO("\nlladdr:");
  // LOG_INFO_LLADDR(lladdr);
  // LOG_INFO("\n");

  if(rpl_dag_root_is_root())
  {
    printf("ROOT\n");  
    return;
  }
  else if (rpl_neighbor_select_best() == NULL){
    printf("best parent is NULL\n");  
    return;
  }
  const linkaddr_t *best_lladdr =rpl_neighbor_get_lladdr(rpl_neighbor_select_best());
  if (best_lladdr == NULL){
    printf("best lladdr is NULL\n");  
    return;
  }
  if (linkaddr_cmp(lladdr, best_lladdr) == 0) //linkaddr_cmp returns zero if the two are same
  {
    printf("dest is not to the best\n");  
    return;
  }

  printf("ewma_alpha:%d\n",ewma_alpha);

  printf("dest is best\n");
  rl_nbr = nbr_table_get_from_lladdr(rl_table, lladdr);
  if(rl_nbr == NULL) {
      printf("Add neighbor\n");
      /* Add the neighbor */
      rl_nbr = nbr_table_add_lladdr(rl_table, lladdr, NBR_TABLE_REASON_RL, NULL);
      memset(rl_nbr->k,0,8*sizeof(rl_nbr->k));
      memset(rl_nbr->Q,0,8*sizeof(rl_nbr->k));
      memset(rl_nbr->flag,0,8*sizeof(rl_nbr->flag));
      int j;
      for(j=0;j<8;j++){
        rl_nbr->k[j]=0;
        rl_nbr->Q[j]=0;
      }
      rl_nbr->txpower=7;
      rl_nbr->t=0;


      if(rpl_neighbor_select_best() != NULL){
      best_parent_stats = nbr_table_get_from_lladdr(link_stats, rpl_neighbor_get_lladdr(rpl_neighbor_select_best()));
      if(best_parent_stats != NULL)
        MAB_init(rl_nbr,best_parent_stats->rssi);
      // print_rl();
      }
      else{
        printf("BEST is null skipping MABinit \n");
        return;
      }
      
      #ifdef sliding_UCB
      int i,j;
      for(i=0;i<TX_LEVELS;i++)
        for(j=0;j<WIN_SIZE;j++)
          sliding_window[i][j]=0;
      #endif 
  }


  int reward;
  // printf("status:%d\n",status);
  if(status == MAC_TX_NOACK) {reward=0;   }
  if(status == MAC_TX_OK)    {reward=100; }

  LOG_INFO("reward %d\n",reward); 
  
  #ifdef discounted_UCB
    int alpha=15;
    if(rl_nbr->k[rl_nbr->txpower] ==0 )    
      rl_nbr->Q[rl_nbr->txpower] = reward;
    else  
      rl_nbr->Q[rl_nbr->txpower] = ( rl_nbr->Q[rl_nbr->txpower] * (100 - alpha) + reward * alpha) / 100;
  #endif

  #ifndef discounted_UCB
  if(rl_nbr->k[rl_nbr->txpower] ==0 ){
    rl_nbr->Q[rl_nbr->txpower] = reward;
  } 
  else {
    rl_nbr->Q[rl_nbr->txpower] = ((rl_nbr->Q[rl_nbr->txpower]) * rl_nbr->k[rl_nbr->txpower] + reward) / (rl_nbr->k[rl_nbr->txpower]+1);
  }
  #endif
  
  rl_nbr->k[rl_nbr->txpower]++;
  rl_nbr->t++;

  // if(rl_nbr->k[rl_nbr->txpower]>3)
  //   stats->etx = ETX_DIVISOR / (rl_nbr->Q[rl_nbr->txpower]+1) ;
  //   printf("%d\n",stats->etx);
  // else {
    // stats->etx = ((uint32_t)stats->etx * (EWMA_SCALE - ewma_alpha) +
    //                 (uint32_t) packet_etx * ewma_alpha) / EWMA_SCALE;
  // }

  best_txpower(rl_nbr);  
  print_rl(rl_nbr);
  // print_rss();

  printf("----------------------------------\n");

  #endif
}
/*---------------------------------------------------------------------------*/
/* Packet input callback. Updates statistics for receptions on a given link */
void
link_stats_input_callback(const linkaddr_t *lladdr)
{
  struct link_stats *stats;
  int16_t packet_rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);

  stats = nbr_table_get_from_lladdr(link_stats, lladdr);
  if(stats == NULL) {
    /* Add the neighbor */
    stats = nbr_table_add_lladdr(link_stats, lladdr, NBR_TABLE_REASON_LINK_STATS, NULL);
    if(stats == NULL) {
      return; /* No space left, return */
    }
    stats->rssi = LINK_STATS_RSSI_UNKNOWN;
  }

  if(stats->rssi == LINK_STATS_RSSI_UNKNOWN) {
    /* Initialize RSSI */
    stats->rssi = packet_rssi;
  } else {
    /* Update RSSI EWMA */
    stats->rssi = ((int32_t)stats->rssi * (EWMA_SCALE - EWMA_ALPHA) +
        (int32_t)packet_rssi * EWMA_ALPHA) / EWMA_SCALE;
  }

  if(stats->etx == 0) {
    /* Initialize ETX */
#if LINK_STATS_INIT_ETX_FROM_RSSI
    stats->etx = guess_etx_from_rssi(stats);
    LOG_INFO_LLADDR(link_stats_get_lladdr(stats));
    // printf(" GUESSING %d %d \n",stats->rssi,stats->etx);


#else /* LINK_STATS_INIT_ETX_FROM_RSSI */
    stats->etx = ETX_DEFAULT * ETX_DIVISOR;
#endif /* LINK_STATS_INIT_ETX_FROM_RSSI */
  }

#if LINK_STATS_PACKET_COUNTERS
  stats->cnt_current.num_packets_rx++;
#endif
}
/*---------------------------------------------------------------------------*/
#if LINK_STATS_PACKET_COUNTERS
/*---------------------------------------------------------------------------*/
static void
print_and_update_counters(void)
{
  struct link_stats *stats;

  for(stats = nbr_table_head(link_stats); stats != NULL;
      stats = nbr_table_next(link_stats, stats)) {

    struct link_packet_counter *c = &stats->cnt_current;

    LOG_INFO("num packets: tx=%u ack=%u rx=%u queue_drops=%u to=",
             c->num_packets_tx, c->num_packets_acked,
             c->num_packets_rx, c->num_queue_drops);
    LOG_INFO_LLADDR(link_stats_get_lladdr(stats));
    LOG_INFO_("\n");

    stats->cnt_total.num_packets_tx += stats->cnt_current.num_packets_tx;
    stats->cnt_total.num_packets_acked += stats->cnt_current.num_packets_acked;
    stats->cnt_total.num_packets_rx += stats->cnt_current.num_packets_rx;
    stats->cnt_total.num_queue_drops += stats->cnt_current.num_queue_drops;
    memset(&stats->cnt_current, 0, sizeof(stats->cnt_current));
  }
}
/*---------------------------------------------------------------------------*/
#endif /* LINK_STATS_PACKET_COUNTERS */
/*---------------------------------------------------------------------------*/
/* Periodic timer called at a period of FRESHNESS_HALF_LIFE */
static void
periodic(void *ptr)
{
  /* Age (by halving) freshness counter of all neighbors */
  struct link_stats *stats;
  ctimer_reset(&periodic_timer);
  for(stats = nbr_table_head(link_stats); stats != NULL; stats = nbr_table_next(link_stats, stats)) {
    stats->freshness >>= 1;
  }

#if LINK_STATS_PACKET_COUNTERS
  print_and_update_counters();
#endif
}
/*---------------------------------------------------------------------------*/
/* Resets link-stats module */
void
link_stats_reset(void)
{
  struct link_stats *stats;
  stats = nbr_table_head(link_stats);
  while(stats != NULL) {
    nbr_table_remove(link_stats, stats);
    stats = nbr_table_next(link_stats, stats);
  }
}
/*---------------------------------------------------------------------------*/
/* Initializes link-stats module */
void
link_stats_init(void)
{
  nbr_table_register(link_stats, NULL);
  #ifdef MABRPL
  nbr_table_register(rl_table, NULL);
  #endif
  ctimer_set(&periodic_timer, FRESHNESS_HALF_LIFE, periodic, NULL);

}
