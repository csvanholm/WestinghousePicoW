// Create a file named lwipopts.h in your source folder
#include "lwipopts_common.h"
#include <stdint.h>
// Optimize memory pools for SMTP (but maintain lwIP sanity checks)
// Note: MEMP_NUM_TCP_SEG must be at least TCP_SND_QUEUELEN (32) from examples_common
#define MEMP_NUM_ARP_QUEUE         10  
#define PBUF_POOL_SIZE             24 

#define TCPIP_THREAD_STACKSIZE    2048U

#define TCPIP_MBOX_SIZE            16      // Reduce mailbox size to save memory, but still allow for some buffering of messages to prevent deadlocks in the TCP/IP thread when the application is busy
#define DEFAULT_TCP_RECVMBOX_SIZE  16
#define DEFAULT_UDP_RECVMBOX_SIZE  16
#define MEMP_NUM_TCP_SEG           32
#define MEMP_NUM_UDP_PCB           8
#define MEMP_NUM_SYS_TIMEOUT       16


#define LWIP_SMTP                   1   // Explicitly enable SMTP in lwIP
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
#define ALTCP_MBEDTLS_CLIENT_PROFILE 1
#define ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS 0
#define LWIP_DNS                    1
#define LWIP_DNS_                   1
#define DNS_MAX_RETRIES             6   // Default is 4; extra retries absorb UDP packet loss on first query after TTL expiry
#define SNTP_SERVER_DNS             1  // Enable SNTP server name resolution, which allows us to specify "pool.ntp.org" instead of a hardcoded IP address for the NTP server. This is important for ensuring that we can synchronize time correctly even if the NTP server's IP address changes, which can happen with public NTP servers. If you find that SNTP synchronization is failing, you should check that DNS resolution is working correctly and that the NTP server name is being resolved to a valid IP address.
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_HTTPD                  1
#define LWIP_HTTPD_SSI              1
#define LWIP_HTTPD_CGI              1
#define LWIP_HTTPD_SSI_INCLUDE_TAG  0
#define HTTPD_FSDATA_FILE           "my_fsdata.c"

#undef LWIP_DEBUG
#define LWIP_DEBUG                  0

#ifdef __cplusplus
extern "C" {
#endif
void sntp_sync_callback(uint32_t sec, uint32_t us);
#ifdef __cplusplus
}
#endif
#define SNTP_SET_SYSTEM_TIME_US(sec, us) sntp_sync_callback((sec), (us))
// TLS requires memory, but not for certificate parsing (not implemented)
// 128KB heap is sufficient for SMTP over TLS without certificate verification
#define MEM_SIZE                 (32U * 1024U) //32
#define SMTP_CHECK_DATA  0  // to allow crlf in the email body, which is important for formatting the email correctly and ensuring that it is displayed properly by the email client. If you find that your email body is not being formatted correctly or that line breaks are not appearing as expected, you should check that SMTP_CHECK_DATA is set to 0 to allow for CRLF characters in the email body.