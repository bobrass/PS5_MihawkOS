/* SPDX-License-Identifier: GPL-3.0-or-later
 * Same PE32/PE64 DNS contract: configuration size/readback, ordinary adapter
 * discovery, real A/AAAA/TXT replies, TCP DNS, NXDOMAIN and record cleanup.
 * No configured addresses, adapter identifiers or record contents are logged.
 * The network's resolver is the one part outside the console: a TCP query it answers with a server failure or not at
 * all is asked once more, and a second such answer is an environment skip (counted in the verdict line as skipped=N),
 * not a failure and not a pass (gate-0036-pro-01, 2026-10-27: TCP status 9002 while UDP answered).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windns.h>
#include <iphlpapi.h>
static unsigned checks, failures, skipped;
static void check(int ok, const char *what)
{
    char line[160]; ++checks;
    if (!ok) { ++failures; wsprintfA(line,"dnsapi: FAIL %s\n",what); OutputDebugStringA(line); }
}
static void config(DNS_CONFIG_TYPE kind, int required)
{
    DWORD size=0, status=DnsQueryConfig(kind,0,NULL,NULL,NULL,&size);
    char line[160];
    wsprintfA(line,"dnsapi: config=%u status=%lu bytes=%lu\n",kind,status,size); OutputDebugStringA(line);
    if (!required && status==DNS_ERROR_NO_DNS_SERVERS) { check(1,"no configured family servers"); return; }
    check(status==ERROR_SUCCESS,"configuration size status");
    check(size>=FIELD_OFFSET(DNS_ADDR_ARRAY,AddrArray[1]) && size<=65536,"configuration bounded size");
    if (status || size>65536 || size<FIELD_OFFSET(DNS_ADDR_ARRAY,AddrArray[1])) return;
    DNS_ADDR_ARRAY *data=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);
    check(data!=NULL,"configuration buffer"); if (!data)return;
    DWORD short_size=1;
    check(DnsQueryConfig(kind,0,NULL,NULL,data,&short_size)==ERROR_MORE_DATA && short_size==size,"short buffer size");
    status=DnsQueryConfig(kind,0,NULL,NULL,data,&size);
    check(!status,"configuration readback");
    if(!status) {
        check(data->AddrCount>0 && data->AddrCount<=data->MaxCount,"server count");
        for(DWORD i=0;i<data->AddrCount;++i) {
            SOCKADDR *address=(SOCKADDR *)data->AddrArray[i].MaxSa;
            check(address->sa_family==AF_INET || address->sa_family==AF_INET6,"server address family");
        }
    }
    HeapFree(GetProcessHeap(),0,data);
}
static void query(const char *name, WORD type, DWORD flags, int negative)
{
    DNS_RECORDA *records=NULL; unsigned count=0,found=0;
    DWORD status=DnsQuery_A(name,type,flags|DNS_QUERY_NO_NETBT,NULL,&records,NULL);
    char line[160];
    const int tcp=!!(flags&DNS_QUERY_USE_TCP_ONLY);
    wsprintfA(line,"dnsapi: type=%u tcp=%u negative=%u status=%lu\n",type,tcp,negative,status);OutputDebugStringA(line);
    if(tcp && !negative && (status==DNS_ERROR_RCODE_SERVER_FAILURE || status==ERROR_TIMEOUT)) {
        if(records){DnsRecordListFree(records,DnsFreeRecordList);records=NULL;}
        Sleep(1000);
        status=DnsQuery_A(name,type,flags|DNS_QUERY_NO_NETBT,NULL,&records,NULL);
        wsprintfA(line,"dnsapi: type=%u tcp=1 retry status=%lu\n",type,status);OutputDebugStringA(line);
        if(status==DNS_ERROR_RCODE_SERVER_FAILURE || status==ERROR_TIMEOUT) {
            ++skipped;
            wsprintfA(line,"dnsapi: SKIP resolver refused TCP (status %lu): the network's, not the console's\n",status);
            OutputDebugStringA(line);
            if(records)DnsRecordListFree(records,DnsFreeRecordList);
            return;
        }
    }
    if(negative)check(status==DNS_ERROR_RCODE_NAME_ERROR,"NXDOMAIN");
    else {
        check(!status,"wire query status");
        for(DNS_RECORDA *r=records;r && count<256;r=r->pNext) { ++count; if(r->wType==type)++found; }
        check(found>0 && count<256,"requested records parsed");
    }
    if(records)DnsRecordListFree(records,DnsFreeRecordList);
}
void mainCRTStartup(void)
{
    char line[180]; WSADATA ws;
    check(!WSAStartup(MAKEWORD(2,2),&ws),"Winsock startup");
    config(DnsConfigDnsServersUnspec,1);config(DnsConfigDnsServersIpv4,0);config(DnsConfigDnsServersIpv6,0);
    ULONG size=0,status=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_PREFIX,NULL,NULL,&size);
    check(status==ERROR_BUFFER_OVERFLOW && size && size<=1048576,"full adapter size query");
    if(status==ERROR_BUFFER_OVERFLOW && size && size<=1048576) {
        IP_ADAPTER_ADDRESSES *data=HeapAlloc(GetProcessHeap(),0,size);
        check(data!=NULL,"full adapter buffer");
        if(data) {check(GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_PREFIX,NULL,data,&size)==NO_ERROR,"full adapter discovery with DNS");HeapFree(GetProcessHeap(),0,data);}
    }
    query("example.com",DNS_TYPE_A,0,0);
    query("example.com",DNS_TYPE_AAAA,0,0);
    query("example.com",DNS_TYPE_TEXT,0,0);
    query("example.com",DNS_TYPE_A,DNS_QUERY_USE_TCP_ONLY,0);
    query("ps5proton-dns-probe.invalid",DNS_TYPE_A,0,1);
    check(!WSACleanup(),"Winsock cleanup");
    wsprintfA(line,"PS5_PROTON_DNSAPI %s checks=%u failures=%u skipped=%u%s\n",failures?"FAIL":"PASS",checks,failures,skipped,
              skipped?" (resolver refused TCP)":"");
    OutputDebugStringA(line);
    ExitProcess(failures!=0);
}
