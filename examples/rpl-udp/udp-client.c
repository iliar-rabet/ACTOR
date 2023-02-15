#include <stdio.h>
#include <stdlib.h>
#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "net/link-stats.h"


#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  0
#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678

#define TOTAL_PKT 200
#define RATE 10
#define SEND_INTERVAL		  RATE*(CLOCK_SECOND)

#include "sys/energest.h"
#include "net/link-stats.h"

// static unsigned long to_seconds(uint64_t time)
// {
//   // printf("ENERGEST:%u\n",ENERGEST_SECOND); //32768
//   return (unsigned long)(time / ENERGEST_SECOND);
// }

static struct simple_udp_connection udp_conn;
static struct simple_udp_connection tpc_conn;
static int rx_count = 0;

// int first=1;
/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client");
PROCESS(energest_example_process, "Energest");
AUTOSTART_PROCESSES(&udp_client_process,&energest_example_process);
// AUTOSTART_PROCESSES(&udp_client_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{

  LOG_INFO("Received response '%.*s' from ", datalen, (char *) data);
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  rx_count++;


}

static void
tpc_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  LOG_INFO("TPC '%.*s' from ", datalen, (char *) data);
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");
  int x;
  x=atoi( (char *)data);
  // memcpy(&x,,sizeof(int));
  printf("x:%d\n",x);
  // if(first==1)
  //   req=x;
  // else 
  if(x > req)
     req=x;
  req=x;
}


/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  static int count;
  static char str[32];
  uip_ipaddr_t dest_ipaddr;
  
  PROCESS_BEGIN();
  // NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, 3);
  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);


  req=0;
  simple_udp_register(&tpc_conn, 50, NULL,
                      50, tpc_rx_callback);

  etimer_set(&periodic_timer, CLOCK_SECOND * 60);
  // max_txpower();
  while(count<TOTAL_PKT) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr) && NETSTACK_ROUTING.node_is_reachable()) {
      /* Send to DAG root */
      LOG_INFO("Sending request %u to ", count);
      LOG_INFO_6ADDR(&dest_ipaddr);
      LOG_INFO_("\n");
    
      // /* Print statistics every 10th TX */
      // if(count % 10 == 0) {
      //   LOG_INFO("Tx/Rx/MissedTx: %d, %d\n",
      //            count, rx_count);
      // }
      snprintf(str, sizeof(str), "hello %d", count);
      simple_udp_sendto(&udp_conn, str, strlen(str), &dest_ipaddr);
      count++;
    } 
    else {
      LOG_INFO("Not reachable yet\n");
    }

    /* Add some jitter */
    etimer_set(&periodic_timer, SEND_INTERVAL * 0.5
       + RATE*(random_rand() % CLOCK_SECOND));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

PROCESS_THREAD(energest_example_process, ev, data)
{
  static struct etimer periodic_timer;
  PROCESS_BEGIN();

  etimer_set(&periodic_timer, CLOCK_SECOND * 60);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
    etimer_reset(&periodic_timer);

    /* Update all energest times. */
    energest_flush();

    // printf(" CPU          %4lus LPM      %4lus DEEP LPM %4lus  Total time %lus\n",
    //        to_seconds(energest_type_time(ENERGEST_TYPE_CPU)),
    //        to_seconds(energest_type_time(ENERGEST_TYPE_LPM)),
    //        to_seconds(energest_type_time(ENERGEST_TYPE_DEEP_LPM)),
    //        to_seconds(ENERGEST_GET_TOTAL_TIME()));
    // printf(" Radio LISTEN %4lus TRANSMIT %lld \n",
    //        to_seconds(energest_type_time(ENERGEST_TYPE_LISTEN)),
    //        energest_type_time(ENERGEST_TYPE_TRANSMIT));
  }
  PROCESS_END();
}