#include <uapi/linux/ptrace.h>
#include <net/sock.h>
#include <bcc/proto.h>
#define KBUILD_MODNAME "foo"
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <net/netfilter/nf_tables.h>

#define LATENCY_SAMPLES 30
#define PAYLOAD_LEN 60

#define STATUS_CLIENT -1
#define STATUS_SERVER 1
#define STATUS_UNKNOWN 0

#define T_INCOMING 1
#define T_OUTGOING 0
#define T_UNKNOWN 2
#define T_STATUS_ON 1
#define T_STATUS_OFF 0


#define HTTP_CLIENT_PORT_MASKING
#define KILL_CONNECTION_DATA
#define BYPASS
#define REVERSE_BYPASS


//struct used to detect if a connection endpoint is server or client
//should be added to the hash ds, and removed on tcp state == closed
struct ipv4_endpoint_key_t {
  u32 addr;
  u16 port;
  u16 pad;
};

struct ipv6_endpoint_key_t {
  unsigned __int128 addr;
  u16 port;
  u16 pad;
  u32 pad2;
  u64 pad3;
};

struct endpoint_data_t {
  int16_t status; // -1 -> client, 0 -> unkknown, 1 -> server;
  int open_transactions; // count how many transactions are in flight
};

struct ipv4_key_t {
  u32 saddr;
  u32 daddr;
  u16 lport;
  u16 dport;
};

struct ipv6_key_t {
  unsigned __int128 saddr;
  unsigned __int128 daddr;
  u64 a;
  u32 b;
  u16 lport;
  u16 dport;
};

struct connection_data_t {
  u8 transaction_state; // 0 -> transaction off, 1 -> transaction ongoing
  u8 transaction_flow; // 1 -> incoming data, 0 -> outgoing data, 2 -> unknown
  u64 first_ts_in; // ts of first incoming packet of the transaction
  u64 last_ts_in; // ts of last incoming packet
  u64 first_ts_out; // ts of the first outgoing packet
  u64 last_ts_out; // ts of the last outgoing packet of the transaction
  u64 byte_tx; // bytes transmitted during the transaction
  u64 byte_rx; // bytes received during transaction
  char http_payload[PAYLOAD_LEN]; // String representation of the http request if available
};

struct ipv4_http_key_t {
  u32 saddr;
  u32 daddr;
  u16 lport;
  u16 dport;
  char http_payload[PAYLOAD_LEN];
};

struct ipv6_http_key_t {
  unsigned __int128 saddr;
  unsigned __int128 daddr;
  // remove padding to support 60 bytes of http_payload
  // u64 a;
  // u32 b;
  u16 lport;
  u16 dport;
  char http_payload[PAYLOAD_LEN];
};

struct summary_data_t {
  u64 latency[LATENCY_SAMPLES];
  u32 pid;
  u32 transaction_count;
  u64 byte_tx;
  u64 byte_rx;
  int16_t status;
};

struct currsock_t {
  u16 family;
  u32 saddr;
  u32 daddr;
  unsigned __int128 saddr6;
  unsigned __int128 daddr6;
  u16 lport;
  u16 dport;
  struct msghdr *msg;
};

BPF_HASH(ipv4_endpoints, struct ipv4_endpoint_key_t, struct endpoint_data_t);
BPF_HASH(ipv6_endpoints, struct ipv6_endpoint_key_t, struct endpoint_data_t);
BPF_HASH(ipv4_connections, struct ipv4_key_t, struct connection_data_t);
BPF_HASH(ipv6_connections, struct ipv6_key_t, struct connection_data_t);
BPF_HASH(ipv4_summary, struct ipv4_key_t, struct summary_data_t);
BPF_HASH(ipv6_summary, struct ipv6_key_t, struct summary_data_t);
BPF_HASH(ipv4_http_summary, struct ipv4_http_key_t, struct summary_data_t);
BPF_HASH(ipv6_http_summary, struct ipv6_http_key_t, struct summary_data_t);


BPF_HASH(set_state_cache, struct sock *, struct endpoint_data_t);
BPF_HASH(recv_cache, u64, struct currsock_t);

struct iptables_data_t {
  u32 saddr;
  u32 daddr;
  u16 lport;
  u16 dport;
  u32 pad;
  struct sk_buff *skb;
};

BPF_HASH(iptables_rewrite_cache_in, u64, struct iptables_data_t);
BPF_HASH(iptables_rewrite_cache_out, u64, struct iptables_data_t);
BPF_HASH(rewritten_rules, struct ipv4_endpoint_key_t, struct ipv4_endpoint_key_t);

struct iptables6_data_t {
  unsigned __int128 saddr;
  unsigned __int128 daddr;
//  u64 a;
  u32 b;
  u16 lport;
  u16 dport;
  struct sk_buff *skb;
};

BPF_HASH(iptables6_rewrite_cache_in, u64, struct iptables6_data_t);
BPF_HASH(iptables6_rewrite_cache_out, u64, struct iptables6_data_t);
BPF_HASH(rewritten_rules_6, struct ipv6_endpoint_key_t, struct ipv6_endpoint_key_t);




static void safe_array_write(u32 idx, u64* array, u64 value) {
  #pragma clang loop unroll(full)
  for(int array_index = 0; array_index<LATENCY_SAMPLES; array_index++) {
    if(array_index == idx) {
      array[array_index] = value;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Trace tcp_set_state to capture new TCP connections and to detect closed    //
// ones to attribute and cleanup final TCP and HTTP transactions              //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int kprobe__tcp_set_state(struct pt_regs *ctx, struct sock *sk, int state) {
  u64 ts = bpf_ktime_get_ns();
  //get dport and lport
  int ret;
  u16 lport = sk->__sk_common.skc_num;
  u16 dport = sk->__sk_common.skc_dport;
  dport = ntohs(dport);

  //detect socket family and then detect tcp socket states
  u16 family = sk->__sk_common.skc_family;

  if(family == AF_INET) {
    u32 saddr = sk->__sk_common.skc_rcv_saddr;
    u32 daddr = sk->__sk_common.skc_daddr;

    if(state == TCP_SYN_SENT) {

      struct endpoint_data_t endpoint_value = {.status = STATUS_CLIENT, .open_transactions = 0};
      // I am a client trying to establish a connection
      set_state_cache.update(&sk, &endpoint_value);
    }

    if(state == TCP_ESTABLISHED) {
      // connection established, retrieve the sk and populate correctly the endpoint hashtable
      struct ipv4_endpoint_key_t endpoint_key = {.addr = saddr, .port = lport};
      struct endpoint_data_t endpoint_value;
      //check first if I am a client
      ret = bpf_probe_read(&endpoint_value, sizeof(endpoint_value), set_state_cache.lookup(&sk));
      if(ret == 0) {
        // I was a client
        set_state_cache.delete(&sk);
      } else {
        // I was a server
        ret = bpf_probe_read(&endpoint_value, sizeof(endpoint_value), ipv4_endpoints.lookup(&endpoint_key));
        if(ret != 0) {
          // I was a server never seen before
          endpoint_value.status = STATUS_SERVER;
          endpoint_value.open_transactions = 0;
          ret = 0;
        }
      }

      if(ret == 0) {
        ipv4_endpoints.update(&endpoint_key, &endpoint_value);

        // connection established, populate connection hashmap (this happens 2 times if connection between local processes)
        struct ipv4_key_t connection_key = {.saddr = saddr, .lport = lport, .daddr = daddr, .dport = dport};

        struct connection_data_t connection_data = {};
        connection_data.byte_rx = 0;
        connection_data.byte_tx = 0;
        connection_data.first_ts_in = ts;
        connection_data.last_ts_in = ts;
        connection_data.first_ts_out = ts;
        connection_data.last_ts_out = ts;
        connection_data.transaction_flow = T_UNKNOWN;
        connection_data.transaction_state = T_STATUS_OFF;

        ipv4_connections.update(&connection_key, &connection_data);
      }

    }

    if(state == TCP_CLOSE || state == TCP_FIN_WAIT1 || state == TCP_FIN_WAIT2 || state == TCP_CLOSING || state == TCP_TIME_WAIT || state == TCP_LAST_ACK || state == TCP_CLOSE_WAIT) {
      // socket closed, clean things
      struct ipv4_key_t connection_key = {.saddr = saddr, .lport = lport, .daddr = daddr, .dport = dport};
      struct connection_data_t * connection_data = ipv4_connections.lookup(&connection_key);
      //update the last pending transaction before leaving

      if(connection_data != NULL) {
        struct ipv4_endpoint_key_t endpoint_key = {.addr = saddr, .port = lport};
        struct endpoint_data_t * endpoint_data = ipv4_endpoints.lookup(&endpoint_key);

        if(endpoint_data != NULL) {

          if(connection_data->transaction_state == T_STATUS_ON
            && ((endpoint_data->status == STATUS_SERVER && connection_data->transaction_flow == T_OUTGOING)
              || (endpoint_data->status == STATUS_CLIENT && connection_data->transaction_flow == T_INCOMING))) {

            //if we are dealing with http, use the appropriate hashmap
            if(connection_data->http_payload[0] != '\0') {
#ifdef HTTP_CLIENT_PORT_MASKING
              struct ipv4_http_key_t http_key = {.saddr = saddr, .daddr = daddr, .lport = 0, .dport = 0};
              if(endpoint_data->status == STATUS_SERVER) {
                http_key.lport = lport;
              } else if (endpoint_data->status == STATUS_CLIENT){
                http_key.dport = dport;
              }
#else
              struct ipv4_http_key_t http_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};
#endif
              bpf_probe_read_str(&(http_key.http_payload), sizeof(http_key.http_payload), &(connection_data->http_payload));

              struct summary_data_t summary_data;
              ret = bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_http_summary.lookup(&http_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // check status and flow correctness
              if(endpoint_data->status == STATUS_SERVER) {
                //measuring latencies (response time for server)
                safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
                summary_data.status = STATUS_SERVER;
              } else if (endpoint_data->status == STATUS_CLIENT){
                //measuring latencies (overall time for client)
                safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
                summary_data.status = STATUS_CLIENT;
              }
              summary_data.transaction_count+= 1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv4_http_summary.update(&http_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv4_endpoint_key_t* nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                if(endpoint_data->status == STATUS_SERVER) {
                  http_key.saddr = nat_data->addr;
                  http_key.lport = nat_data->port;
                  http_key.daddr = endpoint_key.addr;
                  http_key.dport = endpoint_key.port;

                } else if (endpoint_data->status == STATUS_CLIENT) {
#ifdef HTTP_CLIENT_PORT_MASKING
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = 0;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = 0;
#else
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = endpoint_key.port;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = nat_data->port;
#endif
                }

                summary_data.status = STATUS_UNKNOWN;
                ipv4_http_summary.update(&http_key, &summary_data);
                rewritten_rules.delete(&endpoint_key);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                // reverse w.r.t. previous checks because if the endpoint is a
                // server, then we are looking at the client side
                if(endpoint_data->status == STATUS_SERVER) {
#ifdef HTTP_CLIENT_PORT_MASKING
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = 0;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = 0;
#else
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = endpoint_key.port;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = nat_data->port;
#endif
                } else if (endpoint_data->status == STATUS_CLIENT) {
                  http_key.saddr = nat_data->addr;
                  http_key.lport = nat_data->port;
                  http_key.daddr = endpoint_key.addr;
                  http_key.dport = endpoint_key.port;
                }

                summary_data.status = STATUS_UNKNOWN;
                ipv4_http_summary.update(&http_key, &summary_data);
                rewritten_rules.delete(&endpoint_key);
              }

              //remember to restore endpoint key!!!
              endpoint_key.addr = connection_key.saddr;
              endpoint_key.port = connection_key.lport;
#endif //BYPASS
            } else {

              struct summary_data_t summary_data = {};

              ret = bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_summary.lookup(&connection_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }
              // check status and flow correctness
              if(endpoint_data->status == STATUS_SERVER) {
                //measuring latencies (response time for server)
                safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
                summary_data.status = STATUS_SERVER;
              } else if (endpoint_data->status == STATUS_CLIENT){
                //measuring latencies (overall time for client)
                safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
                summary_data.status = STATUS_CLIENT;
              }
              summary_data.transaction_count+= 1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv4_summary.update(&connection_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv4_endpoint_key_t* nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {

                if(endpoint_data->status == STATUS_SERVER) {
                  connection_key.saddr = nat_data->addr;
                  connection_key.lport = nat_data->port;
                  connection_key.daddr = endpoint_key.addr;
                  connection_key.dport = endpoint_key.port;

                } else if (endpoint_data->status == STATUS_CLIENT) {
                  connection_key.saddr = endpoint_key.addr;
                  connection_key.lport = endpoint_key.port;
                  connection_key.daddr = nat_data->addr;
                  connection_key.dport = nat_data->port;
                }

                summary_data.status = STATUS_UNKNOWN;
                ipv4_summary.update(&connection_key, &summary_data);
                rewritten_rules.delete(&endpoint_key);
              }

              endpoint_key.addr = daddr;
              endpoint_key.port = dport;

              nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                // reverse w.r.t. previous checks because if the endpoint is a
                // server, then we are looking at the client side
                if(endpoint_data->status == STATUS_SERVER) {
                  connection_key.saddr = endpoint_key.addr;
                  connection_key.lport = endpoint_key.port;
                  connection_key.daddr = nat_data->addr;
                  connection_key.dport = nat_data->port;

                } else if (endpoint_data->status == STATUS_CLIENT) {
                  connection_key.saddr = nat_data->addr;
                  connection_key.lport = nat_data->port;
                  connection_key.daddr = endpoint_key.addr;
                  connection_key.dport = endpoint_key.port;
                }

                summary_data.status = STATUS_UNKNOWN;
                ipv4_summary.update(&connection_key, &summary_data);
                rewritten_rules.delete(&endpoint_key);
              }

              //remember to restore endpoint and connection key!!!
              endpoint_key.addr = saddr;
              endpoint_key.port = lport;
              connection_key.lport = lport;
              connection_key.dport = dport;
              connection_key.saddr = saddr;
              connection_key.daddr = daddr;
#endif //BYPASS
            }
          }
//          endpoint_data->open_transactions = endpoint_data->open_transactions - 1;
#ifdef KILL_CONNECTION_DATA
          if(endpoint_data->status == STATUS_CLIENT) {
            ipv4_endpoints.delete(&endpoint_key);
          }
#endif
        }
#ifdef KILL_CONNECTION_DATA
        ipv4_connections.delete(&connection_key);
#endif
      }

    }

  } else if (family == AF_INET6) {

    if(state == TCP_SYN_SENT) {

      struct endpoint_data_t endpoint_value = {.status = STATUS_CLIENT, .open_transactions = 0};
      // I am a client trying to establish a connection
      set_state_cache.update(&sk, &endpoint_value);
    }

    if(state == TCP_ESTABLISHED) {
      // connection established, retrieve the sk and populate correctly the endpoint hashtable
      struct ipv6_endpoint_key_t endpoint_key = {.port = lport};
      bpf_probe_read(&endpoint_key.addr, sizeof(endpoint_key.addr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);

      struct endpoint_data_t endpoint_value;
      //check first if I am a client
      ret = bpf_probe_read(&endpoint_value, sizeof(endpoint_value), set_state_cache.lookup(&sk));
      if(ret == 0) {
        // I was a client
        set_state_cache.delete(&sk);
      } else {
        // I was a server
        ret = bpf_probe_read(&endpoint_value, sizeof(endpoint_value), ipv6_endpoints.lookup(&endpoint_key));
        if(ret != 0) {
          // I was a server never seen before
          endpoint_value.status = STATUS_SERVER;
          endpoint_value.open_transactions = 0;
          ret = 0;
        }
      }

      if(ret == 0) {
        ipv6_endpoints.update(&endpoint_key, &endpoint_value);

        // connection established, populate connection hashmap (this happens 2 times if connection between local processes)
        struct ipv6_key_t connection_key = {.lport = lport, .dport = dport};
        bpf_probe_read(&connection_key.saddr, sizeof(connection_key.saddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
        bpf_probe_read(&connection_key.daddr, sizeof(connection_key.daddr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);

        struct connection_data_t connection_data = {};
        connection_data.byte_rx = 0;
        connection_data.byte_tx = 0;
        connection_data.first_ts_in = ts;
        connection_data.last_ts_in = ts;
        connection_data.first_ts_out = ts;
        connection_data.last_ts_out = ts;
        connection_data.transaction_flow = T_UNKNOWN;
        connection_data.transaction_state = T_STATUS_OFF;

        ipv6_connections.update(&connection_key, &connection_data);
      }

    }

    if(state == TCP_CLOSE || state == TCP_FIN_WAIT1 || state == TCP_FIN_WAIT2 || state == TCP_CLOSING || state == TCP_TIME_WAIT || state == TCP_LAST_ACK || state == TCP_CLOSE_WAIT) {

      // socket closed, clean things
      struct ipv6_key_t connection_key = {.lport = lport, .dport = dport};
      bpf_probe_read(&connection_key.saddr, sizeof(connection_key.saddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
      bpf_probe_read(&connection_key.daddr, sizeof(connection_key.daddr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);

      struct connection_data_t * connection_data = ipv6_connections.lookup(&connection_key);
      //update the last pending transaction before leaving

      if(connection_data != NULL) {
        struct ipv6_endpoint_key_t endpoint_key = {.addr = connection_key.saddr, .port = lport};
        struct endpoint_data_t * endpoint_data = ipv6_endpoints.lookup(&endpoint_key);

        if(endpoint_data != NULL) {

          if(connection_data->transaction_state == T_STATUS_ON
            && ((endpoint_data->status == STATUS_SERVER && connection_data->transaction_flow == T_OUTGOING)
              || (endpoint_data->status == STATUS_CLIENT && connection_data->transaction_flow == T_INCOMING))) {

            if(connection_data->http_payload[0] != '\0') {
#ifdef HTTP_CLIENT_PORT_MASKING
              struct ipv6_http_key_t http_key = {.saddr = connection_key.saddr, .daddr = connection_key.daddr, .lport = 0, .dport = 0};
              if(endpoint_data->status == STATUS_SERVER) {
                http_key.lport = lport;
              } else if (endpoint_data->status == STATUS_CLIENT){
                http_key.dport = dport;
              }
#else
              struct ipv6_http_key_t http_key = {.saddr = connection_key.saddr, .daddr = connection_key.daddr, .lport = lport, .dport = dport};
#endif
              bpf_probe_read_str(&(http_key.http_payload), sizeof(http_key.http_payload), &(connection_data->http_payload));

              struct summary_data_t summary_data;
              ret = bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_http_summary.lookup(&http_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // check status and flow correctness
              if(endpoint_data->status == STATUS_SERVER) {
                //measuring latencies (response time for server)
                safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
                summary_data.status = STATUS_SERVER;
              } else if (endpoint_data->status == STATUS_CLIENT){
                //measuring latencies (overall time for client)
                safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
                summary_data.status = STATUS_CLIENT;
              }
              summary_data.transaction_count+= 1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv6_http_summary.update(&http_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv6_endpoint_key_t* nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {

                if(endpoint_data->status == STATUS_SERVER) {
                  http_key.saddr = nat_data->addr;
                  http_key.lport = nat_data->port;
                  http_key.daddr = endpoint_key.addr;
                  http_key.dport = endpoint_key.port;

                } else if (endpoint_data->status == STATUS_CLIENT) {
#ifdef HTTP_CLIENT_PORT_MASKING
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = 0;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = 0;
#else
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = endpoint_key.port;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = nat_data->port;
#endif
                }

                summary_data.status = STATUS_UNKNOWN;
                ipv6_http_summary.update(&http_key, &summary_data);
                rewritten_rules_6.delete(&endpoint_key);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                // reverse w.r.t. previous checks because if the endpoint is a
                // server, then we are looking at the client side
                if(endpoint_data->status == STATUS_SERVER) {
#ifdef HTTP_CLIENT_PORT_MASKING
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = 0;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = 0;
#else
                  http_key.saddr = endpoint_key.addr;
                  http_key.lport = endpoint_key.port;
                  http_key.daddr = nat_data->addr;
                  http_key.dport = nat_data->port;
#endif

                } else if (endpoint_data->status == STATUS_CLIENT) {
                  http_key.saddr = nat_data->addr;
                  http_key.lport = nat_data->port;
                  http_key.daddr = endpoint_key.addr;
                  http_key.dport = endpoint_key.port;
                }

                summary_data.status = STATUS_UNKNOWN;
                ipv6_http_summary.update(&http_key, &summary_data);
                rewritten_rules_6.delete(&endpoint_key);
              }

              //remember to restore endpoint key!!!
              endpoint_key.addr = connection_key.saddr;
              endpoint_key.port = connection_key.lport;
#endif //BYPASS
            } else {

              struct summary_data_t summary_data = {};

              ret = bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_summary.lookup(&connection_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }
              // check status and flow correctness
              if(endpoint_data->status == STATUS_SERVER) {
                //measuring latencies (response time for server)
                safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
                summary_data.status = STATUS_SERVER;
              } else if (endpoint_data->status == STATUS_CLIENT){
                //measuring latencies (total time for server)
                safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
                summary_data.status = STATUS_CLIENT;
              }
              summary_data.transaction_count+= 1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv6_summary.update(&connection_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv6_endpoint_key_t* nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                if(endpoint_data->status == STATUS_SERVER) {
                  connection_key.saddr = nat_data->addr;
                  connection_key.lport = nat_data->port;
                  connection_key.daddr = endpoint_key.addr;
                  connection_key.dport = endpoint_key.port;

                } else if (endpoint_data->status == STATUS_CLIENT) {
                  connection_key.saddr = endpoint_key.addr;
                  connection_key.lport = endpoint_key.port;
                  connection_key.daddr = nat_data->addr;
                  connection_key.dport = nat_data->port;
                }
                summary_data.status = STATUS_UNKNOWN;
                ipv6_summary.update(&connection_key, &summary_data);
                rewritten_rules_6.delete(&endpoint_key);
              }

              bpf_probe_read(&endpoint_key.addr, sizeof(endpoint_key.addr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
              endpoint_key.port = dport;

              nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                // reverse w.r.t. previous checks because if the endpoint is a
                // server, then we are looking at the client side
                if(endpoint_data->status == STATUS_SERVER) {
                  connection_key.saddr = endpoint_key.addr;
                  connection_key.lport = endpoint_key.port;
                  connection_key.daddr = nat_data->addr;
                  connection_key.dport = nat_data->port;

                } else if (endpoint_data->status == STATUS_CLIENT) {
                  connection_key.saddr = nat_data->addr;
                  connection_key.lport = nat_data->port;
                  connection_key.daddr = endpoint_key.addr;
                  connection_key.dport = endpoint_key.port;
                }
                summary_data.status = STATUS_UNKNOWN;
                ipv6_summary.update(&connection_key, &summary_data);
                rewritten_rules_6.delete(&endpoint_key);
              }

              //remember to restore endpoint and connection key!!!
              bpf_probe_read(&endpoint_key.addr, sizeof(endpoint_key.addr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
              endpoint_key.port = lport;
              connection_key.lport = lport;
              connection_key.dport = dport;
              bpf_probe_read(&connection_key.saddr, sizeof(connection_key.saddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
              bpf_probe_read(&connection_key.daddr, sizeof(connection_key.daddr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
#endif //BYPASS
            }
          }
//          endpoint_data->open_transactions = endpoint_data->open_transactions - 1;
#ifdef KILL_CONNECTION_DATA
          if(endpoint_data->status == STATUS_CLIENT) {
            ipv6_endpoints.delete(&endpoint_key);
          }
#endif
        }
#ifdef KILL_CONNECTION_DATA
        ipv6_connections.delete(&connection_key);
#endif
      }
    }
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Trace tcp_sendmsg to retrieve data about socket, message pointer and size  //
// useful to collect transaction data for TCP and HTTP protocols              //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int kprobe__tcp_sendmsg(struct pt_regs *ctx, struct sock *sk, struct msghdr *msg, size_t size) {
  u64 ts = bpf_ktime_get_ns();

  u16 lport = sk->__sk_common.skc_num;
  u16 dport = sk->__sk_common.skc_dport;
  dport = ntohs(dport);

  u16 family = sk->__sk_common.skc_family;

  if (family == AF_INET) {
    u32 saddr = sk->__sk_common.skc_rcv_saddr;
    u32 daddr = sk->__sk_common.skc_daddr;

    //check if I am a server or a client
    struct ipv4_endpoint_key_t endpoint_key = {.addr = saddr, .port = lport};
    struct endpoint_data_t * endpoint_data = ipv4_endpoints.lookup(&endpoint_key);

    if(endpoint_data == NULL) {
      // skip detection in case of unknown endpoint for now
      return 0;
      //create endpoint if not in table
      // endpoint_data.status = STATUS_UNKNOWN;
      // endpoint_data.n_connections = 1;
      // ipv4_endpoints.update(&endpoint_key, &endpoint_data);
    }

    // create connection tuple
    struct ipv4_key_t connection_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};

    struct connection_data_t * connection_data;
    connection_data = ipv4_connections.lookup(&connection_key);
    // it should always be not null, but we need to check
    if(connection_data != NULL) {

      if(endpoint_data->status == STATUS_SERVER) {
        // I am the server and I am sending data
        // Either this is the first transfer back, or it is another transfer back
        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_INCOMING) {
            //this is the first outgoing message
            connection_data->first_ts_out = ts;
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
          } else if (connection_data->transaction_flow == T_OUTGOING) {
            // this is another outgoing message
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }
          connection_data->byte_tx += size;
        } else {
          // the transaction is off, maybe we are just seeing the end of an
          // untracked transaction, wait for further data
          connection_data->transaction_state = T_STATUS_OFF;
        }

      } else if (endpoint_data->status == STATUS_CLIENT) {
        //count transaction client side
        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_INCOMING) {
            // if we are a client sending data, then we are building a new transaction
            // commit the data and restart the thing again

//            endpoint_data->open_transactions = endpoint_data->open_transactions - 1;

            //if we are dealing with http, use the appropriate hashmap
            if(connection_data->http_payload[0] != '\0') {
#ifdef HTTP_CLIENT_PORT_MASKING
              struct ipv4_http_key_t http_key = {.saddr = saddr, .daddr = daddr, .lport = 0, .dport = dport};
#else
              struct ipv4_http_key_t http_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};
#endif
              bpf_probe_read_str(&(http_key.http_payload), sizeof(http_key.http_payload), &(connection_data->http_payload));

              struct summary_data_t summary_data;
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_http_summary.lookup(&http_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // measuring overall transaction time for client
              safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_CLIENT;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv4_http_summary.update(&http_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv4_endpoint_key_t* nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = endpoint_key.addr;
#ifndef HTTP_CLIENT_PORT_MASKING
                http_key.lport = endpoint_key.port;
                http_key.dport = nat_data->port;
#else
                http_key.lport = 0;
                http_key.dport = 0;
#endif
                http_key.daddr = nat_data->addr;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_http_summary.update(&http_key, &summary_data);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = nat_data->addr;
                http_key.lport = nat_data->port;
                http_key.daddr = endpoint_key.addr;
                http_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_http_summary.update(&http_key, &summary_data);
              }

              //remember to restore endpoint key!!!
              endpoint_key.addr = connection_key.saddr;
              endpoint_key.port = connection_key.lport;
#endif //BYPASS
            } else {
              struct summary_data_t summary_data;
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_summary.lookup(&connection_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // measuring overall transaction time for client
              safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_CLIENT;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv4_summary.update(&connection_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv4_endpoint_key_t* nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.lport = endpoint_key.port;
                connection_key.saddr = endpoint_key.addr;
                connection_key.daddr = nat_data->addr;
                connection_key.dport = nat_data->port;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_summary.update(&connection_key, &summary_data);
              }

              endpoint_key.addr = daddr;
              endpoint_key.port = dport;

              nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.saddr = nat_data->addr;
                connection_key.lport = nat_data->port;
                connection_key.daddr = endpoint_key.addr;
                connection_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_summary.update(&connection_key, &summary_data);
              }

              //remember to restore endpoint and connection key!!!
              endpoint_key.addr = saddr;
              endpoint_key.port = lport;
              connection_key.saddr = saddr;
              connection_key.lport = lport;
              connection_key.daddr = daddr;
              connection_key.dport = dport;
#endif //BYPASS
            }

            //clean connection_data
            connection_data->byte_rx = 0;
            connection_data->byte_tx = size;
            connection_data->first_ts_in = 0;
            connection_data->last_ts_in = 0;
            connection_data->first_ts_out = ts;
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
            connection_data->transaction_state = T_STATUS_ON;

//            endpoint_data->open_transactions++;

          } else if (connection_data->transaction_flow == T_OUTGOING) {
            connection_data->byte_tx += size;
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }

        } else {
          // transaction is off, but we have as client an outgoing message
          // set transaction as on!
          connection_data->byte_rx = 0;
          connection_data->byte_tx = size;
          connection_data->first_ts_in = 0;
          connection_data->last_ts_in = 0;
          connection_data->first_ts_out = ts;
          connection_data->last_ts_out = ts;
          connection_data->transaction_flow = T_OUTGOING;
          connection_data->transaction_state = T_STATUS_ON;

//          endpoint_data->open_transactions++;
        }

      } else {
        // if the status is unknown, we should wait to be sure it is a server
        // if it is a client, at the next connection of the same type we will
        // have further details thanks to kprobe__tcp_set_state
        return 0;
      }

      // ok, now read content of the message and see if it is an http request
      struct iov_iter iter = msg->msg_iter;
      //bpf_probe_read(&iter, sizeof(iter), &msg->msg_iter);
      struct iovec data_to_be_read;
      bpf_probe_read(&data_to_be_read, sizeof(data_to_be_read), iter.iov);

      if(data_to_be_read.iov_len >= 7) {
        char p[7];
        bpf_probe_read(&p, sizeof(p), data_to_be_read.iov_base);
        // check if the first bytes correspond to an HTTP request
        if (((p[0] == 'G') && (p[1] == 'E') && (p[2] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'O') && (p[2] == 'S') && (p[3] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'U') && (p[2] == 'T')) ||
          ((p[0] == 'D') && (p[1] == 'E') && (p[2] == 'L') && (p[3] == 'E') && (p[4] == 'T') && (p[5] == 'E')) ||
          ((p[0] == 'H') && (p[1] == 'E') && (p[2] == 'A') && (p[3] == 'D'))) {

          // here we are! retrieve the connection and upload the String
          bpf_probe_read_str(connection_data->http_payload, sizeof(connection_data->http_payload), data_to_be_read.iov_base);

          u8 clear = 0;
          #pragma clang loop unroll(full)
          for(int array_index = 0; array_index<PAYLOAD_LEN; array_index++) {
            if(connection_data->http_payload[array_index] == '?' || connection_data->http_payload[array_index] == '\r' || clear == 1) {
              connection_data->http_payload[array_index] = '\0';
              clear = 1;
            }
          }

        }
      }
    }

  } else if (family == AF_INET6) {
    // struct ipv6_key_t ipv6_key;
    // __builtin_memcpy(&ipv6_key.saddr, sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32, sizeof(ipv6_key.saddr));
    // __builtin_memcpy(&ipv6_key.daddr, sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32, sizeof(ipv6_key.daddr));
    // ipv6_key.lport = sk->__sk_common.skc_num;
    // dport = sk->__sk_common.skc_dport;
    // ipv6_key.dport = ntohs(dport);
    // //ipv6_send_bytes.increment(ipv6_key, 1);


    //check if I am a server or a client
    struct ipv6_endpoint_key_t endpoint_key = {.port = lport};
    bpf_probe_read(&endpoint_key.addr, sizeof(endpoint_key.addr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
    struct endpoint_data_t * endpoint_data = ipv6_endpoints.lookup(&endpoint_key);

    if(endpoint_data == NULL) {
      // skip detection in case of unknown endpoint for now
      return 0;
      //create endpoint if not in table
      // endpoint_data.status = STATUS_UNKNOWN;
      // endpoint_data.n_connections = 1;
      // ipv6_endpoints.update(&endpoint_key, &endpoint_data);
    }

    // create connection tuple

    struct ipv6_key_t connection_key = {.lport = lport, .dport = dport};
    bpf_probe_read(&connection_key.saddr, sizeof(connection_key.saddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
    bpf_probe_read(&connection_key.daddr, sizeof(connection_key.daddr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);

    struct connection_data_t * connection_data = ipv6_connections.lookup(&connection_key);
    // it should always be not null, but we need to check
    if(connection_data != NULL) {

      if(endpoint_data->status == STATUS_SERVER) {
        // I am the server and I am sending data
        // Either this is the first transfer back, or it is another transfer back
        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_INCOMING) {
            //this is the first outgoing message
            connection_data->first_ts_out = ts;
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
          } else if (connection_data->transaction_flow == T_OUTGOING) {
            // this is another outgoing message
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }
          connection_data->byte_tx += size;
        } else {
          // the transaction is off, maybe we are just seeing the end of an
          // untracked transaction, wait for further data
          connection_data->transaction_state = T_STATUS_OFF;
        }

      } else if (endpoint_data->status == STATUS_CLIENT) {
        //count transaction client side
        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_INCOMING) {
            // if we are a client sending data, then we are building a new transaction
            // commit the data and restart the thing again

//            endpoint_data->open_transactions--;

            //if we are dealing with http, use the appropriate hashmap
            if(connection_data->http_payload[0] != '\0') {
#ifdef HTTP_CLIENT_PORT_MASKING
              struct ipv6_http_key_t http_key = {.saddr = connection_key.saddr, .daddr = connection_key.daddr, .lport = 0, .dport = dport};
#else
              struct ipv6_http_key_t http_key = {.saddr = connection_key.saddr, .daddr = connection_key.daddr, .lport = lport, .dport = dport};
#endif
              bpf_probe_read_str(&(http_key.http_payload), sizeof(http_key.http_payload), &(connection_data->http_payload));

              struct summary_data_t summary_data;
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_http_summary.lookup(&http_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // measuring overall transaction time for client
              safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_CLIENT;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv6_http_summary.update(&http_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv6_endpoint_key_t* nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = endpoint_key.addr;
#ifndef HTTP_CLIENT_PORT_MASKING
                http_key.lport = endpoint_key.port;
                http_key.dport = nat_data->port;
#else
                http_key.lport = 0;
                http_key.dport = 0;
#endif
                http_key.daddr = nat_data->addr;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_http_summary.update(&http_key, &summary_data);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = nat_data->addr;
                http_key.lport = nat_data->port;
                http_key.daddr = endpoint_key.addr;
                http_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_http_summary.update(&http_key, &summary_data);
              }

              //remember to restore endpoint key!!!
              endpoint_key.addr = connection_key.saddr;
              endpoint_key.port = connection_key.lport;
#endif //BYPASS
            } else {
              struct summary_data_t summary_data;
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_summary.lookup(&connection_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              //measuring overall transaction time for client
              safe_array_write(idx, summary_data.latency, connection_data->last_ts_in - connection_data->first_ts_out);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_CLIENT;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv6_summary.update(&connection_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv6_endpoint_key_t* nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.lport = endpoint_key.port;
                connection_key.saddr = endpoint_key.addr;
                connection_key.daddr = nat_data->addr;
                connection_key.dport = nat_data->port;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_summary.update(&connection_key, &summary_data);
              }

              bpf_probe_read(&endpoint_key.addr, sizeof(endpoint_key.addr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
              endpoint_key.port = dport;

              nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.saddr = nat_data->addr;
                connection_key.lport = nat_data->port;
                connection_key.daddr = endpoint_key.addr;
                connection_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_summary.update(&connection_key, &summary_data);
              }

              //remember to restore endpoint and connection key!!!
              bpf_probe_read(&endpoint_key.addr, sizeof(endpoint_key.addr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
              endpoint_key.port = lport;
              bpf_probe_read(&connection_key.saddr, sizeof(connection_key.saddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
              connection_key.lport = lport;
              bpf_probe_read(&connection_key.daddr, sizeof(connection_key.daddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
              connection_key.dport = dport;
#endif //BYPASS
            }

            //clean connection_data
            connection_data->byte_rx = 0;
            connection_data->byte_tx = size;
            connection_data->first_ts_in = 0;
            connection_data->last_ts_in = 0;
            connection_data->first_ts_out = ts;
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
            connection_data->transaction_state = T_STATUS_ON;

//            endpoint_data->open_transactions++;

          } else if (connection_data->transaction_flow == T_OUTGOING) {
            connection_data->byte_tx += size;
            connection_data->last_ts_out = ts;
            connection_data->transaction_flow = T_OUTGOING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }

        } else {
          // transaction is off, but we have as client an outgoing message
          // set transaction as on!
          connection_data->byte_rx = 0;
          connection_data->byte_tx = size;
          connection_data->first_ts_in = 0;
          connection_data->last_ts_in = 0;
          connection_data->first_ts_out = ts;
          connection_data->last_ts_out = ts;
          connection_data->transaction_flow = T_OUTGOING;
          connection_data->transaction_state = T_STATUS_ON;

//          endpoint_data->open_transactions++;
        }

      } else {
        // if the status is unknown, we should wait to be sure it is a server
        // if it is a client, at the next connection of the same type we will
        // have further details thanks to kprobe__tcp_set_state
        return 0;
      }

      // ok, now read content of the message and see if it is an http request
      struct iov_iter iter = msg->msg_iter;
      //bpf_probe_read(&iter, sizeof(iter), &msg->msg_iter);
      struct iovec data_to_be_read;
      bpf_probe_read(&data_to_be_read, sizeof(data_to_be_read), iter.iov);

      if(data_to_be_read.iov_len >= 7) {
        char p[7];
        bpf_probe_read(&p, sizeof(p), data_to_be_read.iov_base);
        // check if the first bytes correspond to an HTTP request
        if (((p[0] == 'G') && (p[1] == 'E') && (p[2] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'O') && (p[2] == 'S') && (p[3] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'U') && (p[2] == 'T')) ||
          ((p[0] == 'D') && (p[1] == 'E') && (p[2] == 'L') && (p[3] == 'E') && (p[4] == 'T') && (p[5] == 'E')) ||
          ((p[0] == 'H') && (p[1] == 'E') && (p[2] == 'A') && (p[3] == 'D'))) {

          // here we are! retrieve the connection and upload the String
          bpf_probe_read(connection_data->http_payload, sizeof(connection_data->http_payload), data_to_be_read.iov_base);

          u8 clear = 0;
          #pragma clang loop unroll(full)
          for(int array_index = 0; array_index<PAYLOAD_LEN; array_index++) {
            if(connection_data->http_payload[array_index] == '?' || connection_data->http_payload[array_index] == '\r' || clear == 1) {
              connection_data->http_payload[array_index] = '\0';
              clear = 1;
            }
          }

        }
      }
    }
  }
  // else drop

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Trace tcp_recvmsg to store data about socket and message pointer to be     //
// analyzed together with the size read at function return, obtaining data    //
// for transaction flows for TCP and HTTP protocols                           //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int kprobe__tcp_recvmsg(struct pt_regs *ctx, struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len) {

  u64 pid = bpf_get_current_pid_tgid();

  struct currsock_t cache_data = {.msg = msg};
  cache_data.lport = sk->__sk_common.skc_num;

  cache_data.dport = sk->__sk_common.skc_dport;
  cache_data.dport = ntohs(cache_data.dport);
  cache_data.family = sk->__sk_common.skc_family;

  if (cache_data.family == AF_INET) {
    cache_data.saddr = sk->__sk_common.skc_rcv_saddr;
    cache_data.daddr = sk->__sk_common.skc_daddr;
  }
  else if(cache_data.family == AF_INET6) {
    bpf_probe_read(&cache_data.saddr6, sizeof(cache_data.saddr6), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
    bpf_probe_read(&cache_data.daddr6, sizeof(cache_data.daddr6), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
  }

  recv_cache.update(&pid, &cache_data);
  return 0;
}

int kretprobe__tcp_recvmsg(struct pt_regs *ctx) {
  u64 ts = bpf_get_current_pid_tgid();
  struct currsock_t * cache_data;
  cache_data = recv_cache.lookup(&ts);

  //recycle ts
  ts = bpf_ktime_get_ns();
  int copied = PT_REGS_RC(ctx);

  // lost information
  if(cache_data == NULL) {
    return 0;
  }

  u16 lport = cache_data->lport;
  u16 dport = cache_data->dport;
  u16 family = cache_data->family;

  if (copied <= 0){
    return 0;
  }

  if (family == AF_INET) {
    u32 saddr = cache_data->saddr;
    u32 daddr = cache_data->daddr;

    //check if I am a server or a client
    struct ipv4_endpoint_key_t endpoint_key = {.addr = saddr, .port = lport};
    struct endpoint_data_t * endpoint_data = ipv4_endpoints.lookup(&endpoint_key);

    if(endpoint_data == NULL) {
      // skip detection in case of unknown endpoint for now
      return 0;
      //create endpoint if not in table
      // endpoint_data.status = STATUS_UNKNOWN;
      // endpoint_data.n_connections = 1;
      // ipv4_endpoints.update(&endpoint_key, &endpoint_data);
    }


    // create connection tuple
    struct ipv4_key_t connection_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};

    struct connection_data_t * connection_data = ipv4_connections.lookup(&connection_key);

    // it should always be not null, but we need to check
    if(connection_data != NULL) {

      if(endpoint_data->status == STATUS_SERVER) {

        // I am the server and I am receiving data
        // Either this is the beginning of a transaction,
        // or it is another transfer to the server

        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_OUTGOING) {
            // this is the first incoming message
            // close the old transaction and start the new one

//            endpoint_data->open_transactions--;

            //if we are dealing with http, use the appropriate hashmap
            if(connection_data->http_payload[0] != '\0') {
#ifdef HTTP_CLIENT_PORT_MASKING
              struct ipv4_http_key_t http_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = 0};
#else
              struct ipv4_http_key_t http_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};
#endif
              bpf_probe_read_str(&(http_key.http_payload), sizeof(http_key.http_payload), &(connection_data->http_payload));

              struct summary_data_t summary_data;
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_http_summary.lookup(&http_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // measuring overall transaction time for client
              safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_SERVER;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv4_http_summary.update(&http_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv4_endpoint_key_t* nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = nat_data->addr;
                http_key.lport = nat_data->port;
                http_key.daddr = endpoint_key.addr;
                http_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_http_summary.update(&http_key, &summary_data);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = endpoint_key.addr;
                http_key.daddr = nat_data->addr;
#ifndef HTTP_CLIENT_PORT_MASKING
                http_key.dport = nat_data->port;
                http_key.lport = endpoint_key.port;
#else
                http_key.dport = 0;
                http_key.lport = 0;
#endif
                summary_data.status = STATUS_UNKNOWN;
                ipv4_http_summary.update(&http_key, &summary_data);
              }

              //remember to restore endpoint key!!!
              endpoint_key.addr = connection_key.saddr;
              endpoint_key.port = connection_key.lport;
#endif //BYPASS
            } else {
              struct summary_data_t summary_data = {};
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_summary.lookup(&connection_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }
              //measuring just response time for server
              safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_SERVER;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv4_summary.update(&connection_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv4_endpoint_key_t* nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.saddr = nat_data->addr;
                connection_key.lport = nat_data->port;
                connection_key.daddr = endpoint_key.addr;
                connection_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_summary.update(&connection_key, &summary_data);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.saddr = endpoint_key.addr;
                connection_key.daddr = nat_data->addr;
                connection_key.dport = nat_data->port;
                connection_key.lport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv4_summary.update(&connection_key, &summary_data);
              }

              //remember to restore endpoint and connection key!!!
              endpoint_key.addr = saddr;
              endpoint_key.port = lport;
              connection_key.saddr = saddr;
              connection_key.daddr = daddr;
              connection_key.dport = dport;
              connection_key.lport = lport;
#endif //BYPASS
            }

            //clean connection_data
            connection_data->byte_rx = copied;
            connection_data->byte_tx = 0;
            connection_data->first_ts_in = ts;
            connection_data->last_ts_in = ts;
            connection_data->first_ts_out = 0;
            connection_data->last_ts_out = 0;
            connection_data->transaction_flow = T_INCOMING;
            connection_data->transaction_state = T_STATUS_ON;

//            endpoint_data->open_transactions++;

          } else if (connection_data->transaction_flow == T_INCOMING) {
            // this is another incoming message
            connection_data->last_ts_in = ts;
            connection_data->byte_rx += copied;
            connection_data->transaction_flow = T_INCOMING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }
        } else {
          // the transaction is off, but this is the first incoming packet of
          // a new transaction, set it up!
          connection_data->byte_rx = copied;
          connection_data->byte_tx = 0;
          connection_data->first_ts_in = ts;
          connection_data->last_ts_in = ts;
          connection_data->first_ts_out = 0;
          connection_data->last_ts_out = 0;
          connection_data->transaction_flow = T_INCOMING;
          connection_data->transaction_state = T_STATUS_ON;

//          endpoint_data->open_transactions++;
        }

      } else if (endpoint_data->status == STATUS_CLIENT) {
        // I am the client and I am receiving data
        // Either this is the first receive back, or it is another receive back
        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_INCOMING) {
            //this is another incoming message
            connection_data->last_ts_in = ts;
            connection_data->transaction_flow = T_INCOMING;
          } else if (connection_data->transaction_flow == T_OUTGOING) {
            // this is the first incoming message
            connection_data->first_ts_in = ts;
            connection_data->last_ts_in = ts;
            connection_data->transaction_flow = T_INCOMING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }
          connection_data->byte_rx += copied;
        } else {
          // the transaction is off, maybe we are just seeing the end of an
          // untracked transaction, wait for further data
          connection_data->transaction_state = T_STATUS_OFF;
        }
      } else {
        // if the status is unknown, we should wait to be sure it is a server
        // if it is a client, at the next connection of the same type we will
        // have further details thanks to kprobe__tcp_set_state
        return 0;
      }

      // ok, now read content of the message and see if it is an http request
      struct msghdr * msg = cache_data->msg;
      struct iov_iter iter;
      bpf_probe_read(&iter, sizeof(iter), &msg->msg_iter);
      struct iovec data_to_be_read;
      bpf_probe_read(&data_to_be_read, sizeof(data_to_be_read), iter.iov);

      if(data_to_be_read.iov_len >= 7) {
        char p[7];
        bpf_probe_read(&p, sizeof(p), data_to_be_read.iov_base);
        // check if the first bytes correspond to an HTTP request
        if (((p[0] == 'G') && (p[1] == 'E') && (p[2] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'O') && (p[2] == 'S') && (p[3] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'U') && (p[2] == 'T')) ||
          ((p[0] == 'D') && (p[1] == 'E') && (p[2] == 'L') && (p[3] == 'E') && (p[4] == 'T') && (p[5] == 'E')) ||
          ((p[0] == 'H') && (p[1] == 'E') && (p[2] == 'A') && (p[3] == 'D'))) {

          // here we are! retrieve the connection and upload the String
          bpf_probe_read(connection_data->http_payload, sizeof(connection_data->http_payload), data_to_be_read.iov_base);

          u8 clear = 0;
          #pragma clang loop unroll(full)
          for(int array_index = 0; array_index<PAYLOAD_LEN; array_index++) {
            if(connection_data->http_payload[array_index] == '?' || connection_data->http_payload[array_index] == '\r' || clear == 1) {
              connection_data->http_payload[array_index] = '\0';
              clear = 1;
            }
          }

        }
      }
    }

  } else if (family == AF_INET6) {
    // struct ipv6_key_t ipv6_key;
    // __builtin_memcpy(&ipv6_key.saddr, sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32, sizeof(ipv6_key.saddr));
    // __builtin_memcpy(&ipv6_key.daddr, sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32, sizeof(ipv6_key.daddr));
    // ipv6_key.lport = sk->__sk_common.skc_num;
    // dport = sk->__sk_common.skc_dport;
    // ipv6_key.dport = ntohs(dport);
    // //ipv6_recv_bytes.increment(ipv6_key, 1);


    //check if I am a server or a client
    struct ipv6_endpoint_key_t endpoint_key = {.addr = cache_data->saddr6, .port = lport};
    struct endpoint_data_t * endpoint_data = ipv6_endpoints.lookup(&endpoint_key);

    if(endpoint_data == NULL) {
      // skip detection in case of unknown endpoint for now
      return 0;
      //create endpoint if not in table
      // endpoint_data.status = STATUS_UNKNOWN;
      // endpoint_data.n_connections = 1;
      // ipv6_endpoints.update(&endpoint_key, &endpoint_data);
    }


    // create connection tuple
    struct ipv6_key_t connection_key = {.saddr = cache_data->saddr6, .daddr = cache_data->daddr6, .lport = lport, .dport = dport};

    struct connection_data_t * connection_data = ipv6_connections.lookup(&connection_key);

    // it should always be not null, but we need to check
    if(connection_data != NULL) {
      if(endpoint_data->status == STATUS_SERVER) {

        // I am the server and I am receiving data
        // Either this is the beginning of a transaction,
        // or it is another transfer to the server

        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_OUTGOING) {
            // this is the first incoming message
            // close the old transaction and start the new one
//            endpoint_data->open_transactions--;

            //if we are dealing with http, use the appropriate hashmap
            if(connection_data->http_payload[0] != '\0') {
#ifdef HTTP_CLIENT_PORT_MASKING
              struct ipv6_http_key_t http_key = {.saddr = cache_data->saddr6, .daddr = cache_data->daddr6, .lport = lport, .dport = 0};
#else
              struct ipv6_http_key_t http_key = {.saddr = cache_data->saddr6, .daddr = cache_data->daddr6, .lport = lport, .dport = 0};
#endif
              bpf_probe_read_str(&(http_key.http_payload), sizeof(http_key.http_payload), &(connection_data->http_payload));

              struct summary_data_t summary_data;
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_http_summary.lookup(&http_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }

              // measuring overall transaction time for client
              safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_SERVER;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv6_http_summary.update(&http_key, &summary_data);

#ifdef BYPASS
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              struct ipv6_endpoint_key_t* nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = nat_data->addr;
                http_key.lport = nat_data->port;
                http_key.daddr = endpoint_key.addr;
                http_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_http_summary.update(&http_key, &summary_data);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                http_key.saddr = endpoint_key.addr;
                http_key.daddr = nat_data->addr;
#ifndef HTTP_CLIENT_PORT_MASKING
                http_key.dport = nat_data->port;
                http_key.lport = endpoint_key.port;
#else
                http_key.dport = 0;
                http_key.lport = 0;
#endif
                summary_data.status = STATUS_UNKNOWN;
                ipv6_http_summary.update(&http_key, &summary_data);
              }

              //remember to restore endpoint key!!!
              endpoint_key.addr = connection_key.saddr;
              endpoint_key.port = connection_key.lport;
#endif //BYPASS
            } else {
              struct summary_data_t summary_data = {};
              bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_summary.lookup(&connection_key));

              u32 idx = summary_data.transaction_count;
              if(summary_data.transaction_count > LATENCY_SAMPLES) {
                idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
              }
              //measuring just response time for server transaction
              safe_array_write(idx, summary_data.latency, connection_data->first_ts_out - connection_data->last_ts_in);
              summary_data.transaction_count+=1;
              summary_data.byte_rx += connection_data->byte_rx;
              summary_data.byte_tx += connection_data->byte_tx;
              summary_data.status = STATUS_SERVER;
              summary_data.pid = bpf_get_current_pid_tgid();
              ipv6_summary.update(&connection_key, &summary_data);

#ifdef BYPASS
              //
              //If there is a NAT in between, create an unknown transaction info with the mappings and the same key/value pairs
              //
              struct ipv6_endpoint_key_t* nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.saddr = nat_data->addr;
                connection_key.lport = nat_data->port;
                connection_key.daddr = endpoint_key.addr;
                connection_key.dport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_summary.update(&connection_key, &summary_data);
              }

              endpoint_key.addr = connection_key.daddr;
              endpoint_key.port = connection_key.dport;

              nat_data = rewritten_rules_6.lookup(&endpoint_key);
              if(nat_data != NULL) {
                connection_key.saddr = endpoint_key.addr;
                connection_key.daddr = nat_data->addr;
                connection_key.dport = nat_data->port;
                connection_key.lport = endpoint_key.port;
                summary_data.status = STATUS_UNKNOWN;
                ipv6_summary.update(&connection_key, &summary_data);
              }

              //remember to restore endpoint and connection key!!!
              endpoint_key.addr = cache_data->saddr6;
              endpoint_key.port = lport;
              connection_key.saddr = cache_data->saddr6;
              connection_key.daddr = cache_data->daddr6;
              connection_key.dport = dport;
              connection_key.lport = lport;
#endif //BYPASS
            }
            //clean connection_data
            connection_data->byte_rx = copied;
            connection_data->byte_tx = 0;
            connection_data->first_ts_in = ts;
            connection_data->last_ts_in = ts;
            connection_data->first_ts_out = 0;
            connection_data->last_ts_out = 0;
            connection_data->transaction_flow = T_INCOMING;
            connection_data->transaction_state = T_STATUS_ON;

//            endpoint_data->open_transactions++;

          } else if (connection_data->transaction_flow == T_INCOMING) {
            // this is another incoming message
            connection_data->last_ts_in = ts;
            connection_data->byte_rx += copied;
            connection_data->transaction_flow = T_INCOMING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }
        } else {
          // the transaction is off, but this is the first incoming packet of
          // a new transaction, set it up!
          connection_data->byte_rx = copied;
          connection_data->byte_tx = 0;
          connection_data->first_ts_in = ts;
          connection_data->last_ts_in = ts;
          connection_data->first_ts_out = 0;
          connection_data->last_ts_out = 0;
          connection_data->transaction_flow = T_INCOMING;
          connection_data->transaction_state = T_STATUS_ON;

//          endpoint_data->open_transactions++;
        }

      } else if (endpoint_data->status == STATUS_CLIENT) {
        // I am the client and I am receiving data
        // Either this is the first receive back, or it is another receive back

        if(connection_data->transaction_state == T_STATUS_ON) {
          if(connection_data->transaction_flow == T_INCOMING) {
            //this is another incoming message
            connection_data->last_ts_in = ts;
            connection_data->transaction_flow = T_INCOMING;
          } else if (connection_data->transaction_flow == T_OUTGOING) {
            // this is the first incoming message
            connection_data->first_ts_in = ts;
            connection_data->last_ts_in = ts;
            connection_data->transaction_flow = T_INCOMING;
          } else {
            // we do not know the flow status, keep it unknown till further info
            connection_data->transaction_flow = T_UNKNOWN;
          }
          connection_data->byte_rx += copied;
        } else {
          // the transaction is off, maybe we are just seeing the end of an
          // untracked transaction, wait for further data
          connection_data->transaction_state = T_STATUS_OFF;
        }

      } else {
        // if the status is unknown, we should wait to be sure it is a server
        // if it is a client, at the next connection of the same type we will
        // have further details thanks to kprobe__tcp_set_state
        return 0;
      }

      // ok, now read content of the message and see if it is an http request
      struct msghdr * msg = cache_data->msg;
      struct iov_iter iter;
      bpf_probe_read(&iter, sizeof(iter), &msg->msg_iter);
      struct iovec data_to_be_read;
      bpf_probe_read(&data_to_be_read, sizeof(data_to_be_read), iter.iov);

      if(data_to_be_read.iov_len >= 7) {
        char p[7];
        bpf_probe_read(&p, sizeof(p), data_to_be_read.iov_base);
        // check if the first bytes correspond to an HTTP request
        if (((p[0] == 'G') && (p[1] == 'E') && (p[2] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'O') && (p[2] == 'S') && (p[3] == 'T')) ||
          ((p[0] == 'P') && (p[1] == 'U') && (p[2] == 'T')) ||
          ((p[0] == 'D') && (p[1] == 'E') && (p[2] == 'L') && (p[3] == 'E') && (p[4] == 'T') && (p[5] == 'E')) ||
          ((p[0] == 'H') && (p[1] == 'E') && (p[2] == 'A') && (p[3] == 'D'))) {

          // here we are! retrieve the connection and upload the String
          bpf_probe_read(connection_data->http_payload, sizeof(connection_data->http_payload), data_to_be_read.iov_base);

          u8 clear = 0;
          #pragma clang loop unroll(full)
          for(int array_index = 0; array_index<PAYLOAD_LEN; array_index++) {
            if(connection_data->http_payload[array_index] == '?' || connection_data->http_payload[array_index] == '\r' || clear == 1) {
              connection_data->http_payload[array_index] = '\0';
              clear = 1;
            }
          }

        }
      }
    }
  }

  ts = bpf_get_current_pid_tgid();
  recv_cache.delete(&ts);
  return 0;
}

#ifdef BYPASS
////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Tracing input of IP layer to get IP:port rewriting on input flows          //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int kprobe__ip_rcv(struct pt_regs *ctx, struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev){

  u64 pid = bpf_get_current_pid_tgid();

  struct sk_buff skb_imported;
  bpf_probe_read(&skb_imported, sizeof(skb_imported), skb);

  struct iptables_data_t cache_data = {.skb = skb};

  struct iphdr *ip_header = (struct iphdr *)skb_network_header(&skb_imported);
  struct iphdr ip_header_local;
  bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

  if (ip_header_local.protocol == 6) {
    struct tcphdr *tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
    struct tcphdr tcp_header_local;
    bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

    cache_data.lport = (u16)ntohs(tcp_header_local.source);
    cache_data.dport = (u16)ntohs(tcp_header_local.dest);

    if(cache_data.lport == 0 || cache_data.dport == 0) {
      // skip not yet established connections
      return 0;
    }

    cache_data.saddr = ip_header_local.saddr;
    cache_data.daddr = ip_header_local.daddr;

    iptables_rewrite_cache_in.update(&pid, &cache_data);
  }

  return 0;

}

int kretprobe__ip_rcv(struct pt_regs *ctx){
  int return_value = PT_REGS_RC(ctx);
  u64 pid = bpf_get_current_pid_tgid();

  struct iptables_data_t * cache_data;
  cache_data = iptables_rewrite_cache_in.lookup(&pid);

  if(return_value == NET_RX_SUCCESS && cache_data != NULL) {
    struct sk_buff skb_imported;
    bpf_probe_read(&skb_imported, sizeof(skb_imported), cache_data->skb);

    struct iphdr *ip_header = (struct iphdr *)skb_network_header(&skb_imported);
    struct iphdr ip_header_local;
    bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

    u32 src_ip = ip_header_local.saddr;
    u32 dest_ip = ip_header_local.daddr;

    u16 src_port = 0;
    u16 dest_port = 0;
    if (ip_header_local.protocol == 6) {
      struct tcphdr *tcp_header;
      tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
      struct tcphdr tcp_header_local;
      bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

      src_port = (u16)ntohs(tcp_header_local.source);
      dest_port = (u16)ntohs(tcp_header_local.dest);

      if(src_port == 0 || dest_port == 0) {
        // skip not yet established connections
        return 0;
      }

#ifdef REVERSE_BYPASS
      // insert translated addresses into rewritten endpoints
      if(cache_data->saddr != src_ip || cache_data->lport != src_port){
        struct ipv4_endpoint_key_t key = {.addr = src_ip, .port = src_port};
        struct ipv4_endpoint_key_t value = {.addr = cache_data->saddr, .port = cache_data->lport};
        rewritten_rules.update(&key, &value);

      }
#endif

      if(cache_data->daddr != dest_ip || cache_data->dport != dest_port) {
        struct ipv4_endpoint_key_t key = {.addr = dest_ip, .port = dest_port};
        struct ipv4_endpoint_key_t value = {.addr = cache_data->daddr, .port = cache_data->dport};
        rewritten_rules.update(&key, &value);

      }
    }

    iptables_rewrite_cache_in.delete(&pid);
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Tracing output of IP layer to get IP:port rewriting on output flows        //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int kprobe__ip_output(struct pt_regs *ctx, struct net *net, struct sock *sk, struct sk_buff *skb){
  u64 pid = bpf_get_current_pid_tgid();

  struct sk_buff skb_imported;
  bpf_probe_read(&skb_imported, sizeof(skb_imported), skb);

  struct iptables_data_t cache_data = {.skb = skb};

  struct iphdr *ip_header = (struct iphdr *)skb_network_header(&skb_imported);
  struct iphdr ip_header_local;
  bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

  if (ip_header_local.protocol == 6) {
    struct tcphdr *tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
    struct tcphdr tcp_header_local;
    bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

    cache_data.lport = (u16)ntohs(tcp_header_local.source);
    cache_data.dport = (u16)ntohs(tcp_header_local.dest);

    if(cache_data.lport == 0 || cache_data.dport == 0) {
      // skip not yet established connections
      return 0;
    }

    cache_data.saddr = ip_header_local.saddr;
    cache_data.daddr = ip_header_local.daddr;

    iptables_rewrite_cache_out.update(&pid, &cache_data);
  }

  return 0;
}

int kretprobe__ip_output(struct pt_regs *ctx){
  int return_value = PT_REGS_RC(ctx);
  u64 pid = bpf_get_current_pid_tgid();

  struct iptables_data_t * cache_data;
  cache_data = iptables_rewrite_cache_out.lookup(&pid);

  if(return_value == NET_RX_SUCCESS && cache_data != NULL) {
    struct sk_buff skb_imported;
    bpf_probe_read(&skb_imported, sizeof(skb_imported), cache_data->skb);

    struct iphdr *ip_header = (struct iphdr *)skb_network_header(&skb_imported);
    struct iphdr ip_header_local;
    bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

    u32 src_ip = ip_header_local.saddr;
    u32 dest_ip = ip_header_local.daddr;

    u16 src_port = 0;
    u16 dest_port = 0;
    if (ip_header_local.protocol == 6) {
      struct tcphdr *tcp_header;
      tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
      struct tcphdr tcp_header_local;
      bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

      src_port = (u16)ntohs(tcp_header_local.source);
      dest_port = (u16)ntohs(tcp_header_local.dest);

      if(src_port == 0 || dest_port == 0) {
        // skip not yet established connections
        return 0;
      }

      // insert translated addresses into rewritten endpoints
      if(cache_data->saddr != src_ip || cache_data->lport != src_port){
        struct ipv4_endpoint_key_t value = {.addr = cache_data->saddr, .port = cache_data->lport};
        struct ipv4_endpoint_key_t key = {.addr = src_ip, .port = src_port};
        rewritten_rules.update(&key, &value);

      }

#ifdef REVERSE_BYPASS
      if(cache_data->daddr != dest_ip || cache_data->dport != dest_port) {
        struct ipv4_endpoint_key_t value = {.addr = cache_data->daddr, .port = cache_data->dport};
        struct ipv4_endpoint_key_t key = {.addr = dest_ip, .port = dest_port};
        rewritten_rules.update(&key, &value);

      }
#endif
    }

    iptables_rewrite_cache_out.delete(&pid);
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Tracing input of IPv6 layer to get IP:port rewriting on input flows        //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int kprobe__ipv6_rcv(struct pt_regs *ctx, struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev) {
  u64 pid = bpf_get_current_pid_tgid();

  struct sk_buff skb_imported;
  bpf_probe_read(&skb_imported, sizeof(skb_imported), skb);

  struct iptables6_data_t cache_data = {.skb = skb};

  struct ipv6hdr *ip_header = (struct ipv6hdr *)skb_network_header(&skb_imported);
  struct ipv6hdr ip_header_local;
  bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

  if (ip_header_local.nexthdr == 6) {
    struct tcphdr *tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
    struct tcphdr tcp_header_local;
    bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

    cache_data.lport = (u16)ntohs(tcp_header_local.source);
    cache_data.dport = (u16)ntohs(tcp_header_local.dest);

    if(cache_data.lport == 0 || cache_data.dport == 0) {
      // skip not yet established connections
      return 0;
    }

    bpf_probe_read(&cache_data.saddr, sizeof(cache_data.saddr), &ip_header_local.saddr.in6_u.u6_addr32);
    bpf_probe_read(&cache_data.daddr, sizeof(cache_data.daddr), &ip_header_local.daddr.in6_u.u6_addr32);

    iptables6_rewrite_cache_in.update(&pid, &cache_data);
  }

  return 0;
}

int kretprobe__ipv6_rcv(struct pt_regs *ctx){
  int return_value = PT_REGS_RC(ctx);
  u64 pid = bpf_get_current_pid_tgid();

  struct iptables6_data_t * cache_data;
  cache_data = iptables6_rewrite_cache_in.lookup(&pid);

  if(return_value == NET_RX_SUCCESS && cache_data != NULL) {
    struct sk_buff skb_imported;
    bpf_probe_read(&skb_imported, sizeof(skb_imported), cache_data->skb);

    struct ipv6hdr *ip_header = (struct ipv6hdr *)skb_network_header(&skb_imported);
    struct ipv6hdr ip_header_local;
    bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

    unsigned __int128 src_ip;// = ip_header_local.saddr.in6_u.u6_addr32;
    unsigned __int128 dest_ip;// = ip_header_local.daddr.in6_u.u6_addr32;
    bpf_probe_read(&src_ip, sizeof(src_ip), &ip_header_local.saddr.in6_u.u6_addr32);
    bpf_probe_read(&dest_ip, sizeof(dest_ip), &ip_header_local.daddr.in6_u.u6_addr32);

    u16 src_port = 0;
    u16 dest_port = 0;
    if (ip_header_local.nexthdr == 6) {
      struct tcphdr *tcp_header;
      tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
      struct tcphdr tcp_header_local;
      bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

      src_port = (u16)ntohs(tcp_header_local.source);
      dest_port = (u16)ntohs(tcp_header_local.dest);

      if(src_port == 0 || dest_port == 0) {
        // skip not yet established connections
        return 0;
      }

#ifdef REVERSE_BYPASS
      // insert translated addresses into rewritten endpoints
      if(cache_data->saddr != src_ip || cache_data->lport != src_port){
        struct ipv6_endpoint_key_t key = {.addr = src_ip, .port = src_port};
        struct ipv6_endpoint_key_t value = {.addr = cache_data->saddr, .port = cache_data->lport};
        rewritten_rules_6.update(&key, &value);
      }
#endif

      if(cache_data->daddr != dest_ip || cache_data->dport != dest_port) {
        struct ipv6_endpoint_key_t key = {.addr = dest_ip, .port = dest_port};
        struct ipv6_endpoint_key_t value = {.addr = cache_data->daddr, .port = cache_data->dport};
        rewritten_rules_6.update(&key, &value);
      }
    }

    iptables6_rewrite_cache_in.delete(&pid);
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Tracing output of IPv6 layer to get IP:port rewriting on output flows      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

int ip6_output(struct pt_regs *ctx, struct net *net, struct sock *sk, struct sk_buff *skb){
  u64 pid = bpf_get_current_pid_tgid();

  struct sk_buff skb_imported;
  bpf_probe_read(&skb_imported, sizeof(skb_imported), skb);

  struct iptables6_data_t cache_data = {.skb = skb};

  struct ipv6hdr *ip_header = (struct ipv6hdr *)skb_network_header(&skb_imported);
  struct ipv6hdr ip_header_local;
  bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

  if (ip_header_local.nexthdr == 6) {
    struct tcphdr *tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
    struct tcphdr tcp_header_local;
    bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

    cache_data.lport = (u16)ntohs(tcp_header_local.source);
    cache_data.dport = (u16)ntohs(tcp_header_local.dest);

    if(cache_data.lport == 0 || cache_data.dport == 0) {
      // skip not yet established connections
      return 0;
    }

    bpf_probe_read(&cache_data.saddr, sizeof(cache_data.saddr), &ip_header_local.saddr.in6_u.u6_addr32);
    bpf_probe_read(&cache_data.daddr, sizeof(cache_data.daddr), &ip_header_local.daddr.in6_u.u6_addr32);

    iptables6_rewrite_cache_out.update(&pid, &cache_data);
  }

  return 0;
}

int kretprobe__ip6_output(struct pt_regs *ctx) {
  int return_value = PT_REGS_RC(ctx);
  u64 pid = bpf_get_current_pid_tgid();

  struct iptables6_data_t * cache_data;
  cache_data = iptables6_rewrite_cache_out.lookup(&pid);

  if(return_value == NET_RX_SUCCESS && cache_data != NULL) {
    struct sk_buff skb_imported;
    bpf_probe_read(&skb_imported, sizeof(skb_imported), cache_data->skb);

    struct ipv6hdr *ip_header = (struct ipv6hdr *)skb_network_header(&skb_imported);
    struct ipv6hdr ip_header_local;
    bpf_probe_read(&ip_header_local, sizeof(ip_header_local), ip_header);

    unsigned __int128 src_ip;// = ip_header_local.saddr.in6_u.u6_addr32;
    unsigned __int128 dest_ip;// = ip_header_local.daddr.in6_u.u6_addr32;
    bpf_probe_read(&src_ip, sizeof(src_ip), &ip_header_local.saddr.in6_u.u6_addr32);
    bpf_probe_read(&dest_ip, sizeof(dest_ip), &ip_header_local.daddr.in6_u.u6_addr32);

    u16 src_port = 0;
    u16 dest_port = 0;
    if (ip_header_local.nexthdr == 6) {
      struct tcphdr *tcp_header;
      tcp_header = (struct tcphdr *)skb_transport_header(&skb_imported);
      struct tcphdr tcp_header_local;
      bpf_probe_read(&tcp_header_local, sizeof(tcp_header_local), tcp_header);

      src_port = (u16)ntohs(tcp_header_local.source);
      dest_port = (u16)ntohs(tcp_header_local.dest);

      if(src_port == 0 || dest_port == 0) {
        // skip not yet established connections
        return 0;
      }

      // insert translated addresses into rewritten endpoints
      if(cache_data->saddr != src_ip || cache_data->lport != src_port){
        struct ipv6_endpoint_key_t value = {.addr = cache_data->saddr, .port = cache_data->lport};
        struct ipv6_endpoint_key_t key = {.addr = src_ip, .port = src_port};
        rewritten_rules_6.update(&key, &value);

      }

#ifdef REVERSE_BYPASS
      if(cache_data->daddr != dest_ip || cache_data->dport != dest_port) {
        struct ipv6_endpoint_key_t value = {.addr = cache_data->daddr, .port = cache_data->dport};
        struct ipv6_endpoint_key_t key = {.addr = dest_ip, .port = dest_port};
        rewritten_rules_6.update(&key, &value);
      }
#endif
    }

    iptables6_rewrite_cache_out.delete(&pid);
  }
  return 0;
}

#endif //BYPASS
//
// /*
// * tcp_recvmsg() would be obvious to trace, but is less suitable because:
// * - we'd need to trace both entry and return, to have both sock and size
// * - misses tcp_read_sock() traffic
// */
// int kprobe__tcp_cleanup_rbuf(struct pt_regs *ctx, struct sock *sk, int copied) {
//   u64 ts = bpf_ktime_get_ns();
//   int ret;
//
//   u16 lport = sk->__sk_common.skc_num;
//   u16 dport = sk->__sk_common.skc_dport;
//   dport = ntohs(dport);
//
//   u16 family = sk->__sk_common.skc_family;
//   u64 *val, zero = 0;
//
//   if (copied <= 0){
//     return 0;
//   }
//
//   if (family == AF_INET) {
//     u32 saddr = sk->__sk_common.skc_rcv_saddr;
//     u32 daddr = sk->__sk_common.skc_daddr;
//
//     //check if I am a server or a client
//     struct ipv4_endpoint_key_t endpoint_key = {.addr = saddr, .port = lport};
//     struct endpoint_data_t endpoint_data = {.n_connections = 1, .status = STATUS_UNKNOWN};
//
//     ret = bpf_probe_read(&endpoint_data, sizeof(endpoint_data), ipv4_endpoints.lookup(&endpoint_key));
//     if(ret != 0) {
//       //create endpoint if not in table
//       ipv4_endpoints.update(&endpoint_key, &endpoint_data);
//     }
//
//
//     // create connection tuple
//     struct ipv4_key_t connection_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};
//
//
//     if(endpoint_data.status == STATUS_SERVER) {
//
//       // I am the server and I am receiving data
//       // Either this is the beginning of a transaction,
//       // or it is another transfer to the server
//       struct connection_data_t connection_data;
//       ret = bpf_probe_read(&connection_data, sizeof(connection_data), ipv4_connections.lookup(&connection_key));
//       // it should always be not null, but we need to check
//       if(ret == 0) {
//
//         if(connection_data.transaction_state == T_STATUS_ON) {
//           if(connection_data.transaction_flow == T_OUTGOING) {
//             // this is the first incoming message
//             // close the old transaction and start the new one
//             struct summary_data_t summary_data = {};
//             ret = bpf_probe_read(&summary_data, sizeof(summary_data), ipv4_summary.lookup(&connection_key));
//
//             u32 idx = summary_data.transaction_count;
//             if(summary_data.transaction_count > LATENCY_SAMPLES) {
//               idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
//             }
//             //measuring just response time for server
//             safe_array_write(idx, summary_data.latency, connection_data.first_ts_out - connection_data.last_ts_in);
//             summary_data.transaction_count+=1;
//             summary_data.byte_rx += connection_data.byte_rx;
//             summary_data.byte_tx += connection_data.byte_tx;
//             summary_data.status = STATUS_SERVER;
//             ipv4_summary.update(&connection_key, &summary_data);
//
//             //clean connection_data
//             connection_data.byte_rx = copied;
//             connection_data.byte_tx = 0;
//             connection_data.first_ts_in = ts;
//             connection_data.last_ts_in = ts;
//             connection_data.first_ts_out = 0;
//             connection_data.last_ts_out = 0;
//             connection_data.transaction_flow = T_INCOMING;
//             connection_data.transaction_state = T_STATUS_ON;
//             ipv4_connections.update(&connection_key, &connection_data);
//           } else if (connection_data.transaction_flow == T_INCOMING) {
//             // this is another incoming message
//             connection_data.last_ts_in = ts;
//             connection_data.byte_rx += copied;
//             connection_data.transaction_flow = T_INCOMING;
//             ipv4_connections.update(&connection_key, &connection_data);
//           } else {
//             // we do not know the flow status, keep it unknown till further info
//             connection_data.transaction_flow = T_UNKNOWN;
//           }
//         } else {
//           // the transaction is off, but this is the first incoming packet of
//           // a new transaction, set it up!
//           connection_data.byte_rx = copied;
//           connection_data.byte_tx = 0;
//           connection_data.first_ts_in = ts;
//           connection_data.last_ts_in = ts;
//           connection_data.first_ts_out = 0;
//           connection_data.last_ts_out = 0;
//           connection_data.transaction_flow = T_INCOMING;
//           connection_data.transaction_state = T_STATUS_ON;
//           ipv4_connections.update(&connection_key, &connection_data);
//         }
//       }
//
//
//     } else if (endpoint_data.status == STATUS_CLIENT) {
//       // I am the client and I am receiving data
//       // Either this is the first receive back, or it is another receive back
//       struct connection_data_t connection_data;
//       ret = bpf_probe_read(&connection_data, sizeof(connection_data), ipv4_connections.lookup(&connection_key));
//       // it should always be not null, but we need to check
//       if(ret == 0) {
//
//         if(connection_data.transaction_state == T_STATUS_ON) {
//           if(connection_data.transaction_flow == T_INCOMING) {
//             //this is another incoming message
//             connection_data.last_ts_in = ts;
//             connection_data.transaction_flow = T_INCOMING;
//           } else if (connection_data.transaction_flow == T_OUTGOING) {
//             // this is the first incoming message
//             connection_data.first_ts_in = ts;
//             connection_data.last_ts_in = ts;
//             connection_data.transaction_flow = T_INCOMING;
//           } else {
//             // we do not know the flow status, keep it unknown till further info
//             connection_data.transaction_flow = T_UNKNOWN;
//           }
//         } else {
//           // the transaction is off, maybe we are just seeing the end of an
//           // untracked transaction, wait for further data
//           connection_data.transaction_state = T_STATUS_OFF;
//         }
//         connection_data.byte_rx += copied;
//         ipv4_connections.update(&connection_key, &connection_data);
//       }
//
//     } else {
//       // if the status is unknown, we should wait to be sure it is a server
//       // if it is a client, at the next connection of the same type we will
//       // have further details thanks to kprobe__tcp_set_state
//       // return 0;
//     }
//
//
//
//   } else if (family == AF_INET6) {
//     // struct ipv6_key_t ipv6_key;
//     // __builtin_memcpy(&ipv6_key.saddr, sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32, sizeof(ipv6_key.saddr));
//     // __builtin_memcpy(&ipv6_key.daddr, sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32, sizeof(ipv6_key.daddr));
//     // ipv6_key.lport = sk->__sk_common.skc_num;
//     // dport = sk->__sk_common.skc_dport;
//     // ipv6_key.dport = ntohs(dport);
//     // //ipv6_recv_bytes.increment(ipv6_key, 1);
//
//     unsigned __int128 saddr;
//     unsigned __int128 daddr;
//
//     bpf_probe_read(&saddr, sizeof(saddr), sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
//     bpf_probe_read(&daddr, sizeof(daddr), sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
//
//     //check if I am a server or a client
//     struct ipv6_endpoint_key_t endpoint_key = {.addr = saddr, .port = lport};
//     struct endpoint_data_t endpoint_data = {.n_connections = 1, .status = STATUS_UNKNOWN};
//
//     ret = bpf_probe_read(&endpoint_data, sizeof(endpoint_data), ipv6_endpoints.lookup(&endpoint_key));
//     if(ret != 0) {
//       //create endpoint if not in table
//       ipv6_endpoints.update(&endpoint_key, &endpoint_data);
//     }
//
//
//     // create connection tuple
//     struct ipv6_key_t connection_key = {.saddr = saddr, .daddr = daddr, .lport = lport, .dport = dport};
//
//
//     if(endpoint_data.status == STATUS_SERVER) {
//
//       // I am the server and I am receiving data
//       // Either this is the beginning of a transaction,
//       // or it is another transfer to the server
//       struct connection_data_t connection_data;
//       ret = bpf_probe_read(&connection_data, sizeof(connection_data), ipv6_connections.lookup(&connection_key));
//       // it should always be not null, but we need to check
//       if(ret == 0) {
//
//         if(connection_data.transaction_state == T_STATUS_ON) {
//           if(connection_data.transaction_flow == T_OUTGOING) {
//             // this is the first incoming message
//             // close the old transaction and start the new one
//             struct summary_data_t summary_data = {};
//             ret = bpf_probe_read(&summary_data, sizeof(summary_data), ipv6_summary.lookup(&connection_key));
//
//             u32 idx = summary_data.transaction_count;
//             if(summary_data.transaction_count > LATENCY_SAMPLES) {
//               idx = bpf_get_prandom_u32() % LATENCY_SAMPLES;
//             }
//             //measuring just response time for server transaction
//             safe_array_write(idx, summary_data.latency, connection_data.first_ts_out - connection_data.last_ts_in);
//             summary_data.transaction_count+=1;
//             summary_data.byte_rx += connection_data.byte_rx;
//             summary_data.byte_tx += connection_data.byte_tx;
//             summary_data.status = STATUS_SERVER;
//             ipv6_summary.update(&connection_key, &summary_data);
//
//             //clean connection_data
//             connection_data.byte_rx = copied;
//             connection_data.byte_tx = 0;
//             connection_data.first_ts_in = ts;
//             connection_data.last_ts_in = ts;
//             connection_data.first_ts_out = 0;
//             connection_data.last_ts_out = 0;
//             connection_data.transaction_flow = T_INCOMING;
//             connection_data.transaction_state = T_STATUS_ON;
//             ipv6_connections.update(&connection_key, &connection_data);
//           } else if (connection_data.transaction_flow == T_INCOMING) {
//             // this is another incoming message
//             connection_data.last_ts_in = ts;
//             connection_data.byte_rx += copied;
//             connection_data.transaction_flow = T_INCOMING;
//             ipv6_connections.update(&connection_key, &connection_data);
//           } else {
//             // we do not know the flow status, keep it unknown till further info
//             connection_data.transaction_flow = T_UNKNOWN;
//           }
//         } else {
//           // the transaction is off, but this is the first incoming packet of
//           // a new transaction, set it up!
//           connection_data.byte_rx = copied;
//           connection_data.byte_tx = 0;
//           connection_data.first_ts_in = ts;
//           connection_data.last_ts_in = ts;
//           connection_data.first_ts_out = 0;
//           connection_data.last_ts_out = 0;
//           connection_data.transaction_flow = T_INCOMING;
//           connection_data.transaction_state = T_STATUS_ON;
//           ipv6_connections.update(&connection_key, &connection_data);
//         }
//       }
//
//
//     } else if (endpoint_data.status == STATUS_CLIENT) {
//       // I am the client and I am receiving data
//       // Either this is the first receive back, or it is another receive back
//       struct connection_data_t connection_data;
//       ret = bpf_probe_read(&connection_data, sizeof(connection_data), ipv6_connections.lookup(&connection_key));
//       // it should always be not null, but we need to check
//       if(ret == 0) {
//
//         if(connection_data.transaction_state == T_STATUS_ON) {
//           if(connection_data.transaction_flow == T_INCOMING) {
//             //this is another incoming message
//             connection_data.last_ts_in = ts;
//             connection_data.transaction_flow = T_INCOMING;
//           } else if (connection_data.transaction_flow == T_OUTGOING) {
//             // this is the first incoming message
//             connection_data.first_ts_in = ts;
//             connection_data.last_ts_in = ts;
//             connection_data.transaction_flow = T_INCOMING;
//           } else {
//             // we do not know the flow status, keep it unknown till further info
//             connection_data.transaction_flow = T_UNKNOWN;
//           }
//         } else {
//           // the transaction is off, maybe we are just seeing the end of an
//           // untracked transaction, wait for further data
//           connection_data.transaction_state = T_STATUS_OFF;
//         }
//         connection_data.byte_rx += copied;
//         ipv6_connections.update(&connection_key, &connection_data);
//       }
//
//     } else {
//       // if the status is unknown, we should wait to be sure it is a server
//       // if it is a client, at the next connection of the same type we will
//       // have further details thanks to kprobe__tcp_set_state
//       // return 0;
//     }
//
//
//   }
//   // else drop
//   struct sk_buff_head list = sk->sk_receive_queue;
//   //bpf_probe_read(&list, sizeof(list), &(sk->sk_receive_queue));
//
//   struct sk_buff *skb = skb_peek(&list);
//   //struct sk_buff *skb = sk->sk_backlog.head;
//   struct sk_buff skb_imported;
//   bpf_probe_read(&skb_imported, sizeof(skb_imported), skb);
//
//   struct iphdr    * iph;
//   struct tcphdr   * tcph;
//   char            * data;
//
//   struct iphdr iph_l;
//   struct tcphdr tcph_l;
//
//   //iph = ip_hdr(&skb_imported);
//   iph = (struct iphdr *) (skb_imported.head + skb_imported.network_header);
//
//   bpf_probe_read(&iph_l, sizeof(iph_l), iph);
//
//   tcph = (struct tcphdr *)((__u32 *)iph + iph_l.ihl);
//   bpf_probe_read(&tcph_l, sizeof(tcph_l), tcph);
//
//   data = (char *)((unsigned char *)tcph + (tcph_l.doff * 4));
//
//   u32 payload_len = ntohs(iph_l.tot_len) - (tcph_l.doff * 4) - (iph_l.ihl * 4);
//
//   char p[PAYLOAD_LEN];
//   if(payload_len >= PAYLOAD_LEN) {
//     ret = bpf_probe_read(p, sizeof(p), data);
//
//
//     struct rest_api_key_t tmp = {};
//     bpf_probe_read(tmp.http_payload, sizeof(tmp.http_payload), data);
//     u32* value;
//     u32 v = 0;
//
//     value = rest_api.lookup(&tmp);
//     if(value == NULL) {
//       v = 1;
//     } else {
//       v = *value +1;
//     }
//
//     rest_api.update(&tmp,&v);
//
//   } else if(payload_len > 7) {
//
//     ret = bpf_probe_read(p, 7, data);
//
//
//     struct rest_api_key_t tmp = {};
//     bpf_probe_read(tmp.http_payload, 7, data);
//     u32* value;
//     u32 v = 0;
//
//     value = rest_api.lookup(&tmp);
//     if(value == NULL) {
//       v = 1;
//     } else {
//       v = *value +1;
//     }
//
//     rest_api.update(&tmp,&v);
//   }
//
//
//
//
//   // struct tcphdr *tcph = (struct tcphdr *)(skb_network_header(&skb_imported) + ip_hdrlen(&skb_imported));
//   // struct tcphdr tcph_local;
//   // /*TCP header size*/
//   // ret = bpf_probe_read(&tcph_local, sizeof(tcph_local), tcph);
//   // if(ret != 0) {
//   //   return 0;
//   // }
//   // int tcph_len = tcph_local.doff * 4;
//   //
//   // /*get tcp payload */
//   // char *payload = (char *)tcph + tcph_len;
//   //
//   // char p[7] = {};
//   // ret = bpf_probe_read(p, sizeof(p), payload);
//   //
//   // if(ret != 0) {
//   //   return 0;
//   // }
//   //
//   // struct rest_api_key_t tmp = {};
//   // bpf_probe_read(tmp.http_payload, sizeof(p), payload);
//   // u32 value = 1;
//   // rest_api.update(&tmp,&value);
//
//
//   // char * tcp_hdr = skb_transport_header(&skb_imported);
//   // int tcp_offset = skb_transport_offset(&skb_imported);
//   // // int payload_offset = tcp_hdr + tcp_hdr * 4;
//   // char * to_read = tcp_hdr + (tcp_offset * 4);
//   //
//   // char p[7] = {};
//   // //skb_load_bytes(&skb_imported, payload_offset, p, 7);
//   //
//   // bpf_probe_read(p, sizeof(p), to_read);
//   //
//   // struct rest_api_key_t tmp = {};
//   // bpf_probe_read(tmp.http_payload, sizeof(p), to_read);
//   // u32 value = 1;
//   // rest_api.update(&tmp,&value);
//
//   // char p[7];
//   // int i = 0;
//   // for (i = 0; i < 7; i++) {
//   //   p[i] = load_byte(&skb_imported, payload_offset + i);
//   // }
//
//   if (((p[0] == 'G') && (p[1] == 'E') && (p[2] == 'T')) ||
//     ((p[0] == 'P') && (p[1] == 'O') && (p[2] == 'S') && (p[3] == 'T')) ||
//     ((p[0] == 'P') && (p[1] == 'U') && (p[2] == 'T')) ||
//     ((p[0] == 'D') && (p[1] == 'E') && (p[2] == 'L') && (p[3] == 'E') && (p[4] == 'T') && (p[5] == 'E')) ||
//     ((p[0] == 'H') && (p[1] == 'E') && (p[2] == 'A') && (p[3] == 'D'))) {
//
//     // struct rest_api_key_t tmp = {};
//     //bpf_probe_read(tmp.http_payload, sizeof(p), (skb+payload_offset));
//     //
//     // #pragma clang loop unroll(full)
//     // for(int array_index = 0; array_index<7; array_index++) {
//     //   tmp.http_payload[array_index] = p[array_index];
//     // }
//
//     // ret = bpf_skb_load_bytes(skb, payload_offset, &(tmp.http_payload), PAYLOAD_LEN);
//     //
//     // if(ret != 0) {
//     //   return -1;
//     // }
//     u32 value = 1;
//     // rest_api.update(&tmp,&value);
//     //bpf_trace_printk("%s", p);
//
//   }
//
//
//
//
//   return 0;
// }
