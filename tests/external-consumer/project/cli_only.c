/** @file cli_only.c @brief External host linked only to the installed cog::cli. */
#include "installed_proof.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=5){fputs("installed_cli_only requires binary scratch URL CA\n",stderr);return 2;}
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return 1;
    char fenced[4096],routes[4096];
    int n=snprintf(fenced,sizeof(fenced),"%s/fenced-store",argv[2]);
    int m=snprintf(routes,sizeof(routes),"%s/route-store",argv[2]);
    int status=1;
    if(n>0 && m>0 && (size_t)n<sizeof(fenced) && (size_t)m<sizeof(routes) &&
       installed_fenced_store_proof(fenced)==0 &&
       installed_orchestrator_proof(argv[1],routes)==0 &&
       installed_real_app_proof(argv[2],argv[3],argv[4])==0)status=0;
    curl_global_cleanup();return status;
}
